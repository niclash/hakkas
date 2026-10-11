/*
 * The protocol dispatcher (02 §1-§3). See protocol/dispatcher.h.
 */
#include "protocol/dispatcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blocktypes/block_types.h"
#include "cJSON.h"
#include "protocol/messages.h"
#include "system/log.h"

/* Deferred bursts (see "Bursts" in the header). */
enum {
	PENDING_RUNTIME = 1u << 0,    /* runtime:getruntime after login */
	PENDING_COMPONENTS = 1u << 1, /* component:list */
};

struct ProtoConn {
	Protocol *p;
	const ProtoTransport *t;
	void *conn;
	AuthConn *ac;
	GraphsSubscriber sub;
	bool subscribed;
	bool closing; /* a close started or a send failed: send nothing more */
	unsigned pending;
	ProtoConn *prev, *next;
};

struct Protocol {
	Graphs *graphs;
	GraphFiles *files;
	Authorizer auth;
	char engine_id[CONFIG_ID_MAX + 1];
	char label[CONFIG_LABEL_MAX + 1];
	bool mirror; /* the log sink is registered */
	ProtoConn *conns;
	/* The current event's message, built once for every subscriber: valid
	 * while ev_cached and graphs->seq == ev_seq. */
	bool ev_cached;
	uint64_t ev_seq;
	char *ev_msg;
	/* Recursion guards of the log mirror. */
	bool in_send;
	bool mirroring;
};

/* A parsed envelope (02 §1); valid while its tree lives. */
typedef struct {
	const char *protocol;
	const char *command;
	const cJSON *payload;
	const char *secret;
} Envelope;

static const char *const protocols[] = {
	"environment", "runtime", "network", "graph", "component", "trace",
};

/* ---- Authorizer shorthands ---------------------------------------------------------- */

static uint32_t caps_of(const ProtoConn *c)
{
	const Authorizer *a = &c->p->auth;
	return a->ops->capabilities_for(a->self, c->ac);
}

/* ---- Sending ------------------------------------------------------------------------- */

/* Sends text (len bytes) unless c is closing. A failed send means the
 * connection is closing. */
static bool send_raw(ProtoConn *c, const char *text, size_t len)
{
	if (c->closing)
		return false;
	Protocol *p = c->p;
	bool was = p->in_send;
	p->in_send = true;
	bool ok = c->t->send_text(c->conn, text, len);
	p->in_send = was;
	if (!ok)
		c->closing = true;
	return ok;
}

static void close_conn(ProtoConn *c, uint16_t code, const char *reason)
{
	if (c->closing)
		return;
	c->closing = true;
	c->t->close(c->conn, code, reason);
}

/* Sends and frees msg, a message just built; NULL (out of memory) closes the
 * connection, since a reply would be missing. */
static void reply(ProtoConn *c, char *msg)
{
	if (msg == NULL) {
		log_error("protocol: out of memory, closing the connection");
		close_conn(c, WS_CLOSE_INTERNAL_ERROR, "out of memory");
		return;
	}
	send_raw(c, msg, strlen(msg));
	free(msg);
}

static void reply_error(ProtoConn *c, const char *message, const char *graph)
{
	reply(c, proto_msg_error(message, graph));
}

/* s cut to max bytes at a UTF-8 character boundary (s is valid UTF-8), with
 * "..." appended when cut, into buf (max + 4 bytes). */
static const char *cut(const char *s, size_t max, char *buf)
{
	size_t len = strlen(s);
	if (len <= max)
		return s;
	len = max;
	while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80)
		len--; /* s[len] continues a character: cut before it */
	memcpy(buf, s, len);
	memcpy(buf + len, "...", 4);
	return buf;
}

/* ---- Event delivery (01 §4, 02 §1.1, §2.5 item 6, §4.1) ----------------------------- */

static void drop_event_cache(Protocol *p)
{
	free(p->ev_msg);
	p->ev_msg = NULL;
	p->ev_cached = false;
}

static void on_event(void *ctx, const GraphEvent *ev)
{
	ProtoConn *c = ctx;
	Protocol *p = c->p;
	if (c->closing || caps_of(c) == 0)
		return;
	if (!p->ev_cached || p->ev_seq != p->graphs->seq) {
		free(p->ev_msg);
		p->ev_msg = proto_msg_event(ev);
		p->ev_seq = p->graphs->seq;
		p->ev_cached = true;
	}
	if (p->ev_msg != NULL)
		send_raw(c, p->ev_msg, strlen(p->ev_msg));
}

static void subscribe(ProtoConn *c)
{
	if (c->subscribed)
		return;
	graphs_subscriber_init(&c->sub, on_event, c); /* unscoped */
	graphs_subscribe(c->p->graphs, &c->sub);
	c->subscribed = true;
}

static void unsubscribe(ProtoConn *c)
{
	if (!c->subscribed)
		return;
	graphs_unsubscribe(c->p->graphs, &c->sub);
	c->subscribed = false;
}

bool protocol_conn_scope(ProtoConn *c, const char *graph_id)
{
	return graphs_subscriber_scope(&c->sub, graph_id);
}

/* ---- Log mirroring (05 §4) ------------------------------------------------------------ */

static void mirror_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)len;
	Protocol *p = ctx;
	if (p->in_send || p->mirroring)
		return;
	p->mirroring = true;
	char *msg = NULL;
	size_t msg_len = 0;
	for (ProtoConn *c = p->conns; c != NULL; c = c->next) {
		if (c->closing || caps_of(c) == 0)
			continue;
		if (msg == NULL) {
			msg = level == LOG_LEVEL_ERROR ? proto_msg_error(line, NULL)
						       : proto_msg_output(line);
			if (msg == NULL)
				break; /* out of memory: the line is not mirrored */
			msg_len = strlen(msg);
		}
		if (c->t->send_space(c->conn) >= msg_len)
			send_raw(c, msg, msg_len);
	}
	free(msg);
	p->mirroring = false;
}

/* ---- Bursts (02 §0 "Outgoing buffer") --------------------------------------------- */

/* The messages of one burst. */
typedef struct {
	char **msgs;
	size_t count;
	size_t cap;
	bool failed; /* out of memory */
} Burst;

static void burst_add(Burst *b, char *msg)
{
	if (b->failed || msg == NULL || b->count == b->cap) {
		b->failed = true;
		free(msg);
		return;
	}
	b->msgs[b->count++] = msg;
}

static void burst_free(Burst *b)
{
	for (size_t i = 0; i < b->count; i++)
		free(b->msgs[i]);
	free(b->msgs);
}

/* The component:component messages of every registered type and the
 * component:componentsready (02 §3.5), after a first message if not NULL. */
static bool burst_components(Burst *b, char *first)
{
	size_t types = block_types_count();
	b->cap = types + 2;
	b->msgs = malloc(b->cap * sizeof(*b->msgs));
	if (b->msgs == NULL) {
		free(first);
		return false;
	}
	if (first != NULL)
		burst_add(b, first);
	for (size_t i = 0; i < types; i++)
		burst_add(b, proto_msg_component(block_types_at(i)));
	burst_add(b, proto_msg_componentsready(types));
	return !b->failed;
}

enum burst_result {
	BURST_SENT,
	BURST_DEFERRED,
	BURST_FAILED, /* reported to the client, or the connection is closing */
};

/* Sends the burst whole if it fits; otherwise defers it as flag (made again
 * on drained) or, if the buffer is already empty, answers "Reply too
 * large". */
static enum burst_result burst_send(ProtoConn *c, Burst *b, unsigned flag)
{
	size_t need = 0;
	for (size_t i = 0; i < b->count; i++)
		need += strlen(b->msgs[i]) + PROTO_FRAME_OVERHEAD;
	if (need > c->t->send_space(c->conn)) {
		if (c->t->send_pending(c->conn) > 0) {
			c->pending |= flag;
			return BURST_DEFERRED;
		}
		reply_error(c, "Reply too large", NULL);
		return BURST_FAILED;
	}
	for (size_t i = 0; i < b->count; i++) {
		if (!send_raw(c, b->msgs[i], strlen(b->msgs[i])))
			return BURST_FAILED;
	}
	return BURST_SENT;
}

/* getruntime after login: runtime:runtime, the components, then a
 * network:status broadcast per graph (02 §3.2). */
static void send_runtime_burst(ProtoConn *c, uint32_t caps)
{
	Protocol *p = c->p;
	Burst b = {0};
	if (!burst_components(&b, proto_msg_runtime(p->engine_id, p->label, caps))) {
		burst_free(&b);
		reply(c, NULL);
		return;
	}
	enum burst_result r = burst_send(c, &b, PENDING_RUNTIME);
	burst_free(&b);
	if (r == BURST_SENT)
		graphs_status_all(p->graphs);
}

static void send_component_burst(ProtoConn *c)
{
	Burst b = {0};
	if (!burst_components(&b, NULL)) {
		burst_free(&b);
		reply(c, NULL);
		return;
	}
	burst_send(c, &b, PENDING_COMPONENTS);
	burst_free(&b);
}

/* ---- Payload fields ------------------------------------------------------------------- */

/* The string member key of payload, or NULL if absent or not a string. */
static const char *str_field(const cJSON *payload, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(payload, key);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* An optional string member: *out is its value, or NULL if absent or null.
 * False for any other type. */
static bool opt_str_field(const cJSON *payload, const char *key, const char **out)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(payload, key);
	*out = NULL;
	if (v == NULL || cJSON_IsNull(v))
		return true;
	if (!cJSON_IsString(v))
		return false;
	*out = v->valuestring;
	return true;
}

/* An error's "graph": id if it is a valid graph id, else none (02 §1). */
static const char *known_graph(const char *id)
{
	return graph_id_valid(id) ? id : NULL;
}

static void invalid_field(ProtoConn *c, const char *name, const char *graph)
{
	char text[64];
	snprintf(text, sizeof(text), "Invalid field: %s", name);
	reply_error(c, text, known_graph(graph));
}

/* ---- environment (02 §2.3, §3.1) --------------------------------------------------- */

static void send_challenge(ProtoConn *c, const AuthChallenge *ch)
{
	if (ch == NULL)
		reply_error(c, "Login failed", NULL);
	else
		reply(c, proto_msg_challenge(ch));
}

static void cmd_challenge(ProtoConn *c, const Envelope *env)
{
	const Authorizer *a = &c->p->auth;
	AuthChallenge ch;
	/* A missing or mistyped field is passed as NULL: "Login failed". */
	enum auth_result r = a->ops->challenge(a->self, c->ac, str_field(env->payload, "user"),
					       str_field(env->payload, "cnonce"), &ch);
	if (r == AUTH_OK)
		send_challenge(c, &ch);
	else if (r != AUTH_DELAYED) /* delayed: on_challenge replies */
		send_challenge(c, NULL);
}

static void cmd_login(ProtoConn *c, const Envelope *env)
{
	const Authorizer *a = &c->p->auth;
	AuthProof proof;
	enum auth_result r = a->ops->verify_proof(
		a->self, c->ac, str_field(env->payload, "user"), str_field(env->payload, "nonce"),
		str_field(env->payload, "proof"), &proof);
	if (r == AUTH_OK) {
		char secret[AUTH_SECRET_SIZE];
		if (a->ops->issue_secret(a->self, c->ac, secret)) {
			subscribe(c);
			reply(c, proto_msg_login(secret, &proof));
			memset(secret, 0, sizeof(secret));
			return;
		}
	}
	reply_error(c, "Login failed", NULL);
	if (r == AUTH_FAILED_CLOSE) /* 02 §2.5: 3 failed logins */
		close_conn(c, WS_CLOSE_POLICY, "too many failed logins");
}

static void cmd_logout(ProtoConn *c, const Envelope *env)
{
	(void)env;
	const Authorizer *a = &c->p->auth;
	a->ops->revoke_secret(a->self, c->ac);
	unsubscribe(c);
	reply(c, proto_msg_logout());
}

/* The authorizer's callbacks (from auth_tick). */
static void on_expired(void *user)
{
	ProtoConn *c = user;
	unsubscribe(c); /* 02 §2.5 item 3: stop event delivery */
	reply(c, proto_msg_logout());
}

static void on_challenge(void *user, const AuthChallenge *ch)
{
	send_challenge(user, ch);
}

static const AuthEvents auth_events = {on_expired, on_challenge};

/* ---- runtime (02 §3.2) ------------------------------------------------------------- */

static void cmd_getruntime(ProtoConn *c, const Envelope *env)
{
	(void)env;
	uint32_t caps = caps_of(c);
	if (caps == 0) /* before login: only runtime:runtime, no capabilities */
		reply(c, proto_msg_runtime(c->p->engine_id, c->p->label, 0));
	else
		send_runtime_burst(c, caps);
}

static void cmd_new_graph(ProtoConn *c, const Envelope *env)
{
	static const char *const keys[] = {"id", "name", "description", "icon"};
	const char *v[4];
	for (size_t i = 0; i < 4; i++) {
		if (!opt_str_field(env->payload, keys[i], &v[i])) {
			invalid_field(c, keys[i], i > 0 ? v[0] : NULL);
			return;
		}
	}
	GraphsMessage msg;
	if (graphs_new_graph(c->p->graphs, v[0], v[1], v[2], v[3], NULL, &msg) != GRAPH_OK)
		reply_error(c, msg.text, known_graph(v[0]));
}

static void cmd_delete_graph(ProtoConn *c, const Envelope *env)
{
	const char *id = str_field(env->payload, "id");
	const char *name = str_field(env->payload, "name");
	if (id == NULL || name == NULL) {
		invalid_field(c, id == NULL ? "id" : "name", id);
		return;
	}
	GraphsMessage msg;
	if (graph_files_delete(c->p->files, id, name, &msg) != GRAPH_OK)
		reply_error(c, msg.text, known_graph(id));
}

/* ---- component (02 §3.5) ----------------------------------------------------------- */

static void cmd_component_list(ProtoConn *c, const Envelope *env)
{
	(void)env;
	send_component_burst(c);
}

static void cmd_getsource(ProtoConn *c, const Envelope *env)
{
	const char *name = str_field(env->payload, "name");
	if (name == NULL) {
		invalid_field(c, "name", NULL);
		return;
	}
	if (block_types_get(name) == NULL) {
		char text[GRAPHS_MESSAGE_MAX];
		GraphRefusal r = {.name = name};
		graph_refusal_message(GRAPH_ERR_UNKNOWN_COMPONENT, &r, text, sizeof(text));
		reply_error(c, text, NULL);
		return;
	}
	/* A built-in type has no source; a composite's authoring JSON is plan
	 * task 21's. */
	reply(c, proto_msg_source(name, ""));
}

/* ---- trace (02 §3.6): no-ops ------------------------------------------------------- */

static void cmd_trace(ProtoConn *c, const Envelope *env)
{
	const char *graph = str_field(env->payload, "graph");
	if (graph == NULL) {
		invalid_field(c, "graph", NULL);
		return;
	}
	reply(c, proto_msg_trace(env->command, graph));
}

/* ---- Dispatch ------------------------------------------------------------------------ */

typedef struct {
	const char *protocol;
	const char *command;
	void (*fn)(ProtoConn *c, const Envelope *env);
} ProtoCommand;

/* The commands this layer runs; plan task 18 adds graph:* and network:*. */
static const ProtoCommand commands[] = {
	{"environment", "challenge", cmd_challenge},
	{"environment", "login", cmd_login},
	{"environment", "logout", cmd_logout},
	{"runtime", "getruntime", cmd_getruntime},
	{"runtime", "new_graph", cmd_new_graph},
	{"runtime", "delete_graph", cmd_delete_graph},
	{"component", "list", cmd_component_list},
	{"component", "getsource", cmd_getsource},
	{"trace", "start", cmd_trace},
	{"trace", "stop", cmd_trace},
	{"trace", "clear", cmd_trace},
	{"trace", "dump", cmd_trace},
};

static const ProtoCommand *find_command(const char *protocol, const char *command)
{
	for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
		if (strcmp(commands[i].protocol, protocol) == 0 &&
		    strcmp(commands[i].command, command) == 0)
			return &commands[i];
	}
	return NULL;
}

static void unknown_command(ProtoConn *c, const Envelope *env)
{
	char buf[PROTO_COMMAND_ECHO_MAX + 4];
	char text[sizeof(buf) + 64];
	snprintf(text, sizeof(text), "Unknown command: %s:%s", env->protocol,
		 cut(env->command, PROTO_COMMAND_ECHO_MAX, buf));
	reply_error(c, text, NULL);
}

static void dispatch(ProtoConn *c, const Envelope *env)
{
	const Authorizer *a = &c->p->auth;
	AuthCommandRule rule = auth_command_rule(env->protocol, env->command);
	if (rule.gated && !a->ops->validate_secret(a->self, c->ac, env->secret)) {
		reply_error(c, "Not authenticated", NULL);
		return;
	}
	if (!rule.known) {
		unknown_command(c, env);
		return;
	}
	if (!auth_permits(caps_of(c), &rule)) {
		char text[64];
		snprintf(text, sizeof(text), "Not permitted: %s:%s", env->protocol, env->command);
		reply_error(c, text, NULL);
		return;
	}
	const ProtoCommand *cmd = find_command(env->protocol, env->command);
	if (cmd == NULL)
		unknown_command(c, env); /* graph:*, network:*: plan task 18 */
	else
		cmd->fn(c, env);
}

static bool only_whitespace(const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
			return false;
	}
	return true;
}

/* The envelope of root (02 §1), or false if it is badly formatted. */
static bool parse_envelope(const cJSON *root, Envelope *env)
{
	if (!cJSON_IsObject(root))
		return false;
	const cJSON *proto = cJSON_GetObjectItemCaseSensitive(root, "protocol");
	const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(root, "command");
	const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
	const cJSON *secret = cJSON_GetObjectItemCaseSensitive(root, "secret");
	if (!cJSON_IsString(proto) || !cJSON_IsString(cmd) || !cJSON_IsObject(payload) ||
	    !cJSON_IsString(secret))
		return false;
	bool known = false;
	for (size_t i = 0; i < sizeof(protocols) / sizeof(protocols[0]); i++)
		known = known || strcmp(proto->valuestring, protocols[i]) == 0;
	if (!known)
		return false;
	*env = (Envelope){proto->valuestring, cmd->valuestring, payload, secret->valuestring};
	return true;
}

static void badly_formatted(ProtoConn *c, const char *text)
{
	char buf[PROTO_RAW_ECHO_MAX + 4];
	static const char prefix[] = "Badly formatted request: ";
	char msg[sizeof(prefix) + sizeof(buf)];
	/* text up to a U+0000 (cJSON strings end there), cut. */
	snprintf(msg, sizeof(msg), "%s%s", prefix, cut(text, PROTO_RAW_ECHO_MAX, buf));
	reply_error(c, msg, NULL);
}

void protocol_conn_message(ProtoConn *c, const char *text, size_t len)
{
	if (c == NULL || c->closing)
		return;
	Protocol *p = c->p;
	p->auth.ops->activity(p->auth.self, c->ac);

	const char *end = NULL;
	cJSON *root = cJSON_ParseWithLengthOpts(text, len, &end, false);
	if (root != NULL && !only_whitespace(end, (size_t)(text + len - end))) {
		cJSON_Delete(root); /* text after the object */
		root = NULL;
	}
	Envelope env;
	if (parse_envelope(root, &env))
		dispatch(c, &env);
	else
		badly_formatted(c, text);
	cJSON_Delete(root);
	drop_event_cache(p);
}

void protocol_conn_drained(ProtoConn *c)
{
	if (c == NULL || c->closing || c->pending == 0)
		return;
	unsigned pending = c->pending;
	c->pending = 0;
	if (pending & PENDING_RUNTIME) {
		uint32_t caps = caps_of(c);
		if (caps == 0) /* logged out meanwhile */
			reply(c, proto_msg_runtime(c->p->engine_id, c->p->label, 0));
		else
			send_runtime_burst(c, caps);
	}
	if ((pending & PENDING_COMPONENTS) && caps_of(c) != 0)
		send_component_burst(c);
	drop_event_cache(c->p);
}

/* ---- Sessions -------------------------------------------------------------------------- */

ProtoConn *protocol_conn_open(Protocol *p, const ProtoTransport *t, void *conn,
			      const char *peer_addr)
{
	ProtoConn *c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
	c->p = p;
	c->t = t;
	c->conn = conn;
	c->ac = p->auth.ops->open(p->auth.self, peer_addr, &auth_events, c);
	if (c->ac == NULL) {
		free(c);
		return NULL;
	}
	graphs_subscriber_init(&c->sub, on_event, c);
	c->next = p->conns;
	if (p->conns != NULL)
		p->conns->prev = c;
	p->conns = c;
	return c;
}

void protocol_conn_close(ProtoConn *c)
{
	if (c == NULL)
		return;
	Protocol *p = c->p;
	if (c->prev != NULL)
		c->prev->next = c->next;
	else
		p->conns = c->next;
	if (c->next != NULL)
		c->next->prev = c->prev;
	unsubscribe(c);
	p->auth.ops->close(p->auth.self, c->ac);
	free(c);
}

/* ---- The layer ------------------------------------------------------------------------- */

bool protocol_create(const ProtocolConfig *cfg, Protocol **out)
{
	*out = NULL;
	if (cfg == NULL || cfg->graphs == NULL || cfg->files == NULL || cfg->cfg == NULL ||
	    cfg->auth.ops == NULL || cfg->cfg->engine_id[0] == '\0') {
		log_error("protocol: invalid configuration");
		return false;
	}
	Protocol *p = calloc(1, sizeof(*p));
	if (p == NULL) {
		log_error("protocol: out of memory");
		return false;
	}
	p->graphs = cfg->graphs;
	p->files = cfg->files;
	p->auth = cfg->auth;
	snprintf(p->engine_id, sizeof(p->engine_id), "%s", cfg->cfg->engine_id);
	snprintf(p->label, sizeof(p->label), "%s", cfg->cfg->label);
	if (cfg->cfg->log.mirror) {
		if (!log_add_sink(mirror_sink, p)) {
			log_error("protocol: no free log sink for log.mirror");
			free(p);
			return false;
		}
		p->mirror = true;
	}
	*out = p;
	return true;
}

void protocol_destroy(Protocol *p)
{
	if (p == NULL)
		return;
	if (p->mirror)
		log_remove_sink(mirror_sink, p);
	while (p->conns != NULL)
		protocol_conn_close(p->conns);
	drop_event_cache(p);
	free(p);
}
