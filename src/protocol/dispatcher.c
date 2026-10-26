/*
 * The protocol dispatcher (02 §1-§3). See protocol/dispatcher.h.
 */
#include "protocol/dispatcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/type_files.h"
#include "blocktypes/block_types.h"
#include "blocktypes/composite_types.h"
#include "cJSON.h"
#include "graphs/graph_json.h"
#include "protocol/messages.h"
#include "system/log.h"

/* Deferred bursts (see "Bursts" in the header). */
enum {
	PENDING_RUNTIME = 1u << 0,    /* runtime:getruntime after login */
	PENDING_COMPONENTS = 1u << 1, /* component:list */
	PENDING_CONNECT = 1u << 2,    /* the graph:connect snapshot of connect_id */
	PENDING_DATA = 1u << 3,	      /* network:data of a dirty link-value watch */
	PENDING_ENV = 1u << 4,	      /* environment:values of a catch-up pass */
};

/* The deferred bursts, which take precedence over lossy messages. */
#define PENDING_BURSTS (PENDING_RUNTIME | PENDING_COMPONENTS | PENDING_CONNECT)

/* One network:edges subscription (02 §4.2): a live watch on the input
 * tgt_node.tgt_port of the connection's watch_graph. */
typedef struct {
	InputWatch w; /* first: the callbacks get &w */
	ProtoConn *c;
	Block *block; /* NULL once detached (the block was removed) */
	uint32_t input;
	/* A value did not fit the outgoing buffer: the input's value at the
	 * next on_drained is sent instead (PENDING_DATA). */
	bool dirty;
	/* The request's src and tgt, as given (one allocation, names). */
	const char *src_node, *src_port, *tgt_node, *tgt_port;
	char *names;
} EdgeWatch;

struct ProtoConn {
	Protocol *p;
	const ProtoTransport *t;
	void *conn;
	AuthConn *ac;
	GraphsSubscriber sub;
	bool subscribed;
	bool closing; /* a close started or a send failed: send nothing more */
	unsigned pending;
	char connect_id[GRAPH_ID_MAX + 1]; /* PENDING_CONNECT: the graph */
	/* The network:edges set (02 §4.2): watch_count watches on watch_graph. */
	EdgeWatch *watches;
	size_t watch_count;
	char *watch_graph;
	/* environment:watch (02 §3.7, plan task 76): every change up to
	 * env_sent has been sent. A catch-up pass (env_pass) sends the
	 * channels changed after env_sent, or all with a value (env_all), from
	 * env_cursor on; when it ends, everything up to env_pass_seq is sent. */
	bool env_watch;
	bool env_pass;
	bool env_all;
	bool env_flushed; /* the pass has sent a message */
	uint32_t env_sent;
	uint32_t env_pass_seq;
	uint32_t env_cursor;
	ProtoConn *prev, *next;
};

struct Protocol {
	Graphs *graphs;
	GraphFiles *files;
	Environment *env;
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
	"auth", "environment", "runtime", "network", "graph", "component", "trace",
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

/* Whether a lossy message of len bytes (network:data, a mirrored log line)
 * may go out now: no burst is deferred (it goes first, at on_drained) and it
 * leaves the reserve free for replies and events (decided, plan task 20
 * fix; 02 §4.2 "Back-pressure"). */
static bool lossy_fits(const ProtoConn *c, size_t len)
{
	if (c->pending & PENDING_BURSTS)
		return false;
	size_t reserve = c->t->send_capacity(c->conn) / PROTO_DATA_RESERVE_DIV;
	size_t space = c->t->send_space(c->conn);
	return space >= reserve && len <= space - reserve;
}

/* The bytes a lossy message may take now (lossy_fits). */
static size_t lossy_room(const ProtoConn *c)
{
	if (c->pending & PENDING_BURSTS)
		return 0;
	size_t reserve = c->t->send_capacity(c->conn) / PROTO_DATA_RESERVE_DIV;
	size_t space = c->t->send_space(c->conn);
	return space >= reserve ? space - reserve : 0;
}

/* Whether an on_drained will come: the buffer holds data, or a burst waits
 * for one. */
static bool drain_follows(const ProtoConn *c)
{
	return c->t->send_pending(c->conn) > 0 || (c->pending & PENDING_BURSTS);
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
		if (lossy_fits(c, msg_len))
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
	size_t need;  /* their bytes, PROTO_FRAME_OVERHEAD per message included */
	size_t limit; /* a message that would take need beyond it is not kept */
	bool over;    /* a message was dropped for the limit */
	bool failed;  /* out of memory */
} Burst;

#define BURST_NO_LIMIT SIZE_MAX

/* Adds msg (taken over; NULL is out of memory), unless the burst failed or
 * went over its limit already. */
static void burst_add(Burst *b, char *msg)
{
	if (msg == NULL)
		b->failed = true;
	if (b->failed || b->over) {
		free(msg);
		return;
	}
	size_t n = strlen(msg) + PROTO_FRAME_OVERHEAD;
	if (n > b->limit - b->need) {
		b->over = true;
		free(msg);
		return;
	}
	if (b->count == b->cap) {
		size_t cap = b->cap == 0 ? 16 : b->cap * 2;
		char **msgs = realloc(b->msgs, cap * sizeof(*msgs));
		if (msgs == NULL) {
			b->failed = true;
			free(msg);
			return;
		}
		b->msgs = msgs;
		b->cap = cap;
	}
	b->msgs[b->count++] = msg;
	b->need += n;
}

static void burst_free(Burst *b)
{
	for (size_t i = 0; i < b->count; i++)
		free(b->msgs[i]);
	free(b->msgs);
	*b = (Burst){0};
}

/* The component:component messages of every registered type and the
 * component:componentsready (02 §3.5), after a first message if not NULL. */
static bool burst_components(Burst *b, char *first)
{
	size_t types = block_types_count();
	b->limit = BURST_NO_LIMIT;
	if (first != NULL)
		burst_add(b, first);
	for (size_t i = 0; i < types; i++)
		burst_add(b, proto_msg_component(block_types_at(i)));
	burst_add(b, proto_msg_componentsready(types));
	return !b->failed;
}

/* Sends every message of b; false if the connection is closing. */
static bool burst_emit(ProtoConn *c, const Burst *b)
{
	for (size_t i = 0; i < b->count; i++) {
		if (!send_raw(c, b->msgs[i], strlen(b->msgs[i])))
			return false;
	}
	return true;
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
	if (b->need > c->t->send_space(c->conn)) {
		if (c->t->send_pending(c->conn) > 0) {
			c->pending |= flag;
			return BURST_DEFERRED;
		}
		reply_error(c, "Reply too large", NULL);
		return BURST_FAILED;
	}
	return burst_emit(c, b) ? BURST_SENT : BURST_FAILED;
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

static void edges_clear(ProtoConn *c);
static void env_unwatch(ProtoConn *c);

/* The session ended (logout, expiry): no more events, no link values
 * (02 §4.2, §3.7), no deferred snapshot. */
static void end_delivery(ProtoConn *c)
{
	unsubscribe(c);
	edges_clear(c);
	env_unwatch(c);
	c->pending &= ~PENDING_CONNECT;
}

/* ---- auth (02 §2.3, §3.1) ---------------------------------------------------------- */

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
	/* A missing or mistyped field is passed as NULL: "Login failed". A
	 * resume (02 §2.3.1) is a string "resume"; any other is a refusal. */
	const char *resume;
	if (!opt_str_field(env->payload, "resume", &resume)) {
		send_challenge(c, NULL);
		return;
	}
	enum auth_result r = a->ops->challenge(a->self, c->ac, str_field(env->payload, "user"),
					       str_field(env->payload, "cnonce"), resume, &ch);
	if (r == AUTH_OK)
		send_challenge(c, &ch);
	else if (r != AUTH_DELAYED) /* delayed: on_challenge replies */
		send_challenge(c, NULL);
}

static void cmd_login(ProtoConn *c, const Envelope *env)
{
	const Authorizer *a = &c->p->auth;
	AuthProof proof;
	const char *resume;
	/* A mistyped "resume" is a wrong one: the login fails and counts. */
	if (!opt_str_field(env->payload, "resume", &resume))
		resume = "";
	enum auth_result r = a->ops->verify_proof(
		a->self, c->ac, str_field(env->payload, "user"), str_field(env->payload, "nonce"),
		resume, str_field(env->payload, "proof"), &proof);
	if (r == AUTH_OK) {
		char secret[AUTH_SECRET_SIZE], next[AUTH_RESUME_SIZE];
		if (a->ops->issue_secret(a->self, c->ac, secret, next)) {
			subscribe(c);
			reply(c, proto_msg_login(secret, next, &proof));
			memset(secret, 0, sizeof(secret));
			memset(next, 0, sizeof(next));
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
	end_delivery(c);
	reply(c, proto_msg_logout());
}

/* The authorizer's callbacks (from auth_tick). */
static void on_expired(void *user)
{
	ProtoConn *c = user;
	end_delivery(c); /* 02 §2.5 item 3: stop event delivery */
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
	/* A component graph takes its type with it (02 §3.4.3). */
	const Graph *g = graphs_find(c->p->graphs, id);
	if (g != NULL && g->component) {
		if (type_files_delete_component(c->p->files, id, name, &msg) != GRAPH_FILES_OK)
			reply_error(c, msg.text, known_graph(id));
		return;
	}
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
	const BlockTypeDef *def = block_types_get(name);
	if (def == NULL) {
		char text[GRAPHS_MESSAGE_MAX];
		GraphRefusal r = {.name = name};
		graph_refusal_message(GRAPH_ERR_UNKNOWN_COMPONENT, &r, text, sizeof(text));
		reply_error(c, text, NULL);
		return;
	}
	if (!def->subgraph) {
		reply(c, proto_msg_source(name, "")); /* a built-in type has no source */
		return;
	}
	/* A composite type: its authoring JSON as a string (02 §3.5, 03 §5.1,
	 * in canonical form). */
	char *code = composite_types_source(def, NULL);
	reply(c, code != NULL ? proto_msg_source(name, code) : NULL);
	free(code);
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

/* ---- graph:* and network:* helpers (02 §3.3-§3.4) ---------------------------------- */

/* A refused command (02 §3.4.1): its message to the requester only, with
 * "graph" for a valid graph id. */
static void refused(ProtoConn *c, const GraphsMessage *msg, const char *graph)
{
	reply_error(c, msg->text, known_graph(graph));
}

/* The refusal st about graph (Invalid id, Unknown graph). */
static void refuse_graph(ProtoConn *c, enum graph_status st, const char *graph)
{
	GraphsMessage msg;
	GraphRefusal r = {.graph = graph};
	graph_refusal_message(st, &r, msg.text, sizeof(msg.text));
	refused(c, &msg, graph);
}

/* A required string member key: *out, or "Invalid field: <key>" (with
 * "graph" for a valid graph id) and false. */
static bool need_str(ProtoConn *c, const cJSON *payload, const char *key, const char *graph,
		     const char **out)
{
	*out = str_field(payload, key);
	if (*out == NULL) {
		invalid_field(c, key, graph);
		return false;
	}
	return true;
}

/* ---- environment (02 §3.7, plan task 62) ------------------------------------------- */

/* The running environment's description, NULL without one. */
static const HakkasEnvDescription *env_desc(const ProtoConn *c)
{
	return c->p->env != NULL ? environment_description(c->p->env) : NULL;
}

static void cmd_env_describe(ProtoConn *c, const Envelope *env)
{
	(void)env;
	const HakkasEnvDescription *d = env_desc(c);
	if (d == NULL)
		reply_error(c, "No environment", NULL);
	else
		reply(c, proto_msg_env_description(d));
}

static void cmd_getbindings(ProtoConn *c, const Envelope *env)
{
	(void)env;
	reply(c, proto_msg_bindings(c->p->graphs->rt));
}

/* bind / unbind {"graph","port","env","index"?} (port: a process port, plan
 * task 70): checked in the
 * order of the C API's hakkas_binding_add (02 §6): the fields, the process,
 * then the environment's refusals (04 §4). The event is the reply. */
static void bind_command(ProtoConn *c, const Envelope *env, bool bind)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph"), *port, *env_port;
	if (!need_str(c, pl, "graph", graph, &graph) || !need_str(c, pl, "port", graph, &port) ||
	    !need_str(c, pl, "env", graph, &env_port))
		return;
	const cJSON *index = cJSON_GetObjectItemCaseSensitive(pl, "index");
	uint32_t channel = 0;
	if (index != NULL && !cJSON_IsNull(index)) {
		double v = cJSON_IsNumber(index) ? index->valuedouble : 0;
		if (v < 1 || v > HAKKAS_ENV_COUNT_MAX || v != (double)(uint32_t)v) {
			invalid_field(c, "index", graph);
			return;
		}
		channel = (uint32_t)v;
	}
	if (!graph_id_valid(graph)) {
		refuse_graph(c, GRAPH_ERR_INVALID_ID, graph);
		return;
	}
	Graph *g = graphs_find(c->p->graphs, graph);
	if (g == NULL || g->component) {
		refuse_graph(c, g == NULL ? GRAPH_ERR_UNKNOWN_GRAPH : GRAPH_ERR_NOT_A_PROCESS,
			     graph);
		return;
	}
	if (c->p->env == NULL) {
		reply_error(c, "No environment", graph);
		return;
	}
	EnvBindingRequest req = {port, env_port, channel};
	char text[ENVIRONMENT_MESSAGE_MAX];
	enum environment_status st =
		bind ? environment_bind(c->p->env, g, &req, text, sizeof(text))
		     : environment_unbind(c->p->env, g, &req, text, sizeof(text));
	if (st != ENVIRONMENT_OK)
		reply_error(c, text, graph);
}

static void cmd_bind(ProtoConn *c, const Envelope *env)
{
	bind_command(c, env, true);
}

static void cmd_unbind(ProtoConn *c, const Envelope *env)
{
	bind_command(c, env, false);
}

/* ---- environment:watch, every channel's live value (02 §3.7, plan task 76) ---------- */

/* Entries of environment:values being joined. */
typedef struct {
	char *text;
	size_t len, cap;
} Entries;

static bool entries_add(Entries *e, const char *entry)
{
	size_t n = strlen(entry), need = e->len + (e->len > 0) + n;
	if (need > e->cap) {
		size_t cap = e->cap == 0 ? 256 : e->cap;
		while (cap < need)
			cap *= 2;
		char *t = realloc(e->text, cap);
		if (t == NULL)
			return false;
		e->text = t;
		e->cap = cap;
	}
	if (e->len > 0)
		e->text[e->len++] = ',';
	memcpy(e->text + e->len, entry, n);
	e->len += n;
	return true;
}

/* Sends the entries as one environment:values (they fit the room above the
 * reserve) and empties them; false if memory runs out. */
static bool entries_flush(ProtoConn *c, Entries *e)
{
	char *m = proto_msg_env_values(e->text != NULL ? e->text : "", e->len);
	if (m == NULL)
		return false;
	send_raw(c, m, strlen(m));
	free(m);
	e->len = 0;
	c->env_flushed = true;
	return true;
}

static void env_pass_begin(ProtoConn *c, bool all)
{
	c->env_pass = true;
	c->env_all = all;
	c->env_flushed = false;
	c->env_pass_seq = environment_seq(c->p->env);
	c->env_cursor = 0;
}

/* Runs c's catch-up pass from its cursor, in as many environment:values as
 * the room above the reserve takes, each filled up to it. When the room is
 * used up, it waits for on_drained (PENDING_ENV) and goes on from there.
 * When it ends, a channel that changed while it waited, before its cursor,
 * is sent by a further pass (which may send a channel the pass sent after
 * waiting once more). A pass that has sent nothing yet starts from the
 * current change number. The first pass after watch sends a message even
 * without any value. */
static void env_pass_run(ProtoConn *c)
{
	Environment *env = c->p->env;
	Entries e = {0};
	if (c->env_cursor == 0)
		c->env_pass_seq = environment_seq(env);
	while (c->env_pass && !c->closing) {
		uint32_t n = environment_channel_count(env);
		size_t room = lossy_room(c);
		while (c->env_cursor < n) {
			uint32_t ch = c->env_cursor;
			const char *id;
			uint32_t index;
			const Linkable *v;
			if (!environment_channel(env, ch, &id, &index, &v) ||
			    (!c->env_all && !environment_changed_after(env, ch, c->env_sent))) {
				c->env_cursor++;
				continue;
			}
			char *entry = proto_msg_env_value(id, index, v);
			if (entry == NULL)
				goto no_memory;
			size_t need =
				PROTO_ENV_VALUES_OVERHEAD + e.len + (e.len > 0) + strlen(entry);
			if (need > room && e.len > 0) {
				free(entry);
				if (!entries_flush(c, &e))
					goto no_memory;
				room = lossy_room(c);
				continue; /* this channel again, in the next message */
			}
			if (need > room) {
				free(entry);
				if (drain_follows(c)) {
					c->pending |= PENDING_ENV;
					free(e.text);
					return;
				}
				c->env_cursor++; /* too large for any buffer: dropped */
				continue;
			}
			bool ok = entries_add(&e, entry);
			free(entry);
			if (!ok)
				goto no_memory;
			c->env_cursor++;
		}
		bool first = c->env_all && !c->env_flushed;
		if ((e.len > 0 || first) && !entries_flush(c, &e))
			goto no_memory;
		c->env_sent = c->env_pass_seq;
		c->env_pass = false;
		c->env_all = false;
		if (environment_seq(env) != c->env_sent)
			env_pass_begin(c, false);
	}
	free(e.text);
	return;
no_memory:
	free(e.text);
	reply(c, NULL);
}

/* A channel's current value changed: one environment:values to every
 * watching connection that is up to date (any other sends it in its pass);
 * one whose message does not fit the room above the reserve starts a pass
 * at the next on_drained instead. */
static void on_env_change(void *ctx, uint32_t ch)
{
	Protocol *p = ctx;
	uint32_t seq = environment_seq(p->env);
	char *msg = NULL;
	size_t len = 0;
	for (ProtoConn *c = p->conns; c != NULL; c = c->next) {
		if (!c->env_watch || c->env_pass || c->closing)
			continue;
		if (msg == NULL) {
			const char *id;
			uint32_t index;
			const Linkable *v;
			if (!environment_channel(p->env, ch, &id, &index, &v))
				return;
			char *entry = proto_msg_env_value(id, index, v);
			msg = entry != NULL ? proto_msg_env_values(entry, strlen(entry)) : NULL;
			free(entry);
			if (msg == NULL) {
				log_error("protocol: out of memory, a live value not sent");
				return;
			}
			len = strlen(msg);
		}
		if (lossy_fits(c, len)) {
			send_raw(c, msg, len);
			c->env_sent = seq;
		} else if (drain_follows(c)) {
			env_pass_begin(c, false);
			c->pending |= PENDING_ENV;
		} else {
			c->env_sent = seq; /* too large for any buffer: dropped */
		}
	}
	free(msg);
}

static void env_unwatch(ProtoConn *c)
{
	c->env_watch = false;
	c->env_pass = false;
	c->pending &= ~PENDING_ENV;
}

/* watch {}: the echo, then every channel with a value (a pass sending all);
 * changes follow. Watching again starts over. */
static void cmd_env_watch(ProtoConn *c, const Envelope *env)
{
	(void)env;
	if (c->p->env == NULL || environment_description(c->p->env) == NULL) {
		reply_error(c, "No environment", NULL);
		return;
	}
	env_unwatch(c);
	reply(c, proto_msg_build("environment", "watch", cJSON_CreateObject()));
	if (c->closing)
		return;
	c->env_watch = true;
	env_pass_begin(c, true);
	env_pass_run(c);
}

static void cmd_env_unwatch(ProtoConn *c, const Envelope *env)
{
	(void)env;
	if (c->p->env == NULL || environment_description(c->p->env) == NULL) {
		reply_error(c, "No environment", NULL);
		return;
	}
	env_unwatch(c);
	reply(c, proto_msg_build("environment", "unwatch", cJSON_CreateObject()));
}

/* An edge endpoint {"node":String,"port":String}. */
typedef struct {
	const char *node;
	const char *port;
} Endpoint;

static bool endpoint_of(const cJSON *v, Endpoint *out)
{
	out->node = cJSON_IsObject(v) ? str_field(v, "node") : NULL;
	out->port = cJSON_IsObject(v) ? str_field(v, "port") : NULL;
	return out->node != NULL && out->port != NULL;
}

/* A required endpoint member key, as need_str. */
static bool need_endpoint(ProtoConn *c, const cJSON *payload, const char *key, const char *graph,
			  Endpoint *out)
{
	if (!endpoint_of(cJSON_GetObjectItemCaseSensitive(payload, key), out)) {
		invalid_field(c, key, graph);
		return false;
	}
	return true;
}

/* A member that is absent or JSON null. */
static const cJSON *opt_member(const cJSON *payload, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(payload, key);
	return cJSON_IsNull(v) ? NULL : v;
}

/* ---- graph (02 §3.4) ------------------------------------------------------------------- */

static void cmd_graph_list(ProtoConn *c, const Envelope *env)
{
	(void)env;
	graphs_status_all(c->p->graphs); /* a network:status broadcast per graph */
}

/* The graph:connect snapshot of g (02 §4.1.1): graph:clear, the blocks,
 * the graph ports (inports, then outports), the initials, then the edges,
 * each per block in creation order and input in port order. Retained
 * entries of placeholders are not live and are never sent. */
static void build_snapshot(Burst *b, const Graph *g)
{
	burst_add(b, proto_msg_graph_clear(g));
	for (uint32_t i = 0; i < g->block_count; i++)
		burst_add(b, proto_msg_addnode(g, g->blocks[i]));
	for (int dir = GRAPH_INPORT; dir <= GRAPH_OUTPORT; dir++) {
		for (uint32_t i = 0; i < g->port_count[dir]; i++)
			burst_add(b, proto_msg_addport(g, (enum graph_port_dir)dir,
						       &g->ports[dir][i]));
	}
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t i = 0; i < g->block_count; i++) {
			const Block *blk = g->blocks[i];
			for (uint32_t in = 0; in < blk->input_count; in++) {
				const Input *input = &blk->inputs[in];
				if (pass == 0 && input->source == INPUT_SOURCE_INITIAL)
					burst_add(b, proto_msg_addinitial(g, blk, in,
									  &input->initial));
				else if (pass == 1 && input->source == INPUT_SOURCE_EDGE)
					burst_add(b, proto_msg_addedge(g, input->src_block,
								       input->src_output, blk, in));
			}
		}
	}
}

/* graph:connect's re-scope and snapshot (02 §4.1, §4.1.1; decided, plan
 * task 18). The snapshot is built only up to the free outgoing space. If it
 * fits, the connection is re-scoped and the snapshot sent at the same
 * instant, so no event of the graph can come between them. If it does not
 * fit while the buffer holds data, it is deferred (PENDING_CONNECT) and made
 * again on drained; the scope stays as it was meanwhile, so events keep
 * arriving under the old scope, all before the snapshot's graph:clear. If
 * it does not fit an empty buffer, "Graph too large to send: <id>", and the
 * scope stays. */
static void send_snapshot(ProtoConn *c, const char *id)
{
	Graph *g = NULL;
	enum graph_status st = GRAPH_ERR_INVALID_ID;
	if (graph_id_valid(id)) {
		g = graphs_find(c->p->graphs, id);
		st = g != NULL ? GRAPH_OK : GRAPH_ERR_UNKNOWN_GRAPH;
	}
	if (st != GRAPH_OK) {
		refuse_graph(c, st, id);
		return;
	}
	Burst b = {.limit = c->t->send_space(c->conn)};
	build_snapshot(&b, g);
	if (b.failed) {
		burst_free(&b);
		reply(c, NULL);
		return;
	}
	if (b.over) {
		burst_free(&b);
		if (c->t->send_pending(c->conn) > 0) {
			memcpy(c->connect_id, id, strlen(id) + 1);
			c->pending |= PENDING_CONNECT;
		} else {
			char text[GRAPHS_MESSAGE_MAX];
			snprintf(text, sizeof(text), "Graph too large to send: %s", id);
			reply_error(c, text, id);
		}
		return;
	}
	protocol_conn_scope(c, id);
	burst_emit(c, &b);
	burst_free(&b);
}

static void cmd_connect(ProtoConn *c, const Envelope *env)
{
	const char *id;
	if (!need_str(c, env->payload, "id", NULL, &id))
		return;
	c->pending &= ~PENDING_CONNECT; /* a newer connect replaces a deferred one */
	send_snapshot(c, id);
}

/* graph:clear with "main": false (02 §3.4.3): create or empty a component
 * graph. The reply, to the requester only, is the graph's graph:clear. */
static void cmd_graph_clear(ProtoConn *c, const Envelope *env)
{
	const char *id;
	if (!need_str(c, env->payload, "id", NULL, &id))
		return;
	static const char *const keys[] = {"name", "library", "description", "icon"};
	const char *v[4];
	for (size_t i = 0; i < 4; i++) {
		if (!opt_str_field(env->payload, keys[i], &v[i])) {
			invalid_field(c, keys[i], id);
			return;
		}
	}
	/* Only component graphs: a process is created by runtime:new_graph. */
	if (!cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(env->payload, "main"))) {
		invalid_field(c, "main", id);
		return;
	}
	GraphsMessage msg;
	Graph *g;
	if (type_files_clear_component(c->p->files, id, v[0], v[2], v[3], &g, &msg) !=
	    GRAPH_FILES_OK) {
		reply_error(c, msg.text, known_graph(id));
		return;
	}
	reply(c, proto_msg_graph_clear(g));
}

static void cmd_rename(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *id = str_field(pl, "id"), *to;
	/* The request's "from" is not checked (02 §3.4). */
	if (!need_str(c, pl, "id", id, &id) || !need_str(c, pl, "to", id, &to))
		return;
	GraphsMessage msg;
	if (graphs_rename_graph(c->p->graphs, id, to, &msg) != GRAPH_OK)
		refused(c, &msg, id);
}

static void cmd_addnode(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph"), *id, *component;
	if (!need_str(c, pl, "id", graph, &id) || !need_str(c, pl, "graph", graph, &graph) ||
	    !need_str(c, pl, "component", graph, &component))
		return;
	BlockMetadata md;
	enum graph_status st = graph_json_to_metadata(opt_member(pl, "metadata"), &md);
	if (st == GRAPH_ERR_NO_MEMORY) {
		reply(c, NULL);
		return;
	}
	if (st != GRAPH_OK) {
		invalid_field(c, "metadata", graph);
		return;
	}
	GraphsMessage msg;
	if (graphs_add_node(c->p->graphs, graph, id, component, &md, &msg) != GRAPH_OK)
		refused(c, &msg, graph);
	block_metadata_release(&md);
}

static void cmd_removenode(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph"), *id;
	if (!need_str(c, pl, "id", graph, &id) || !need_str(c, pl, "graph", graph, &graph))
		return;
	GraphsMessage msg;
	if (graphs_remove_node(c->p->graphs, graph, id, &msg) != GRAPH_OK)
		refused(c, &msg, graph);
}

static void cmd_renamenode(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph"), *from, *to;
	if (!need_str(c, pl, "from", graph, &from) || !need_str(c, pl, "to", graph, &to) ||
	    !need_str(c, pl, "graph", graph, &graph))
		return;
	GraphsMessage msg;
	if (graphs_rename_node(c->p->graphs, graph, from, to, &msg) != GRAPH_OK)
		refused(c, &msg, graph);
}

/* changenode's patch merged into cur: GRAPH_OK, or a reply already sent
 * (an invalid patch, out of memory) for any other status. */
static enum graph_status node_change(ProtoConn *c, const BlockMetadata *cur, const cJSON *patch,
				     const char *graph, BlockChange *chg)
{
	enum graph_status st = graph_json_to_change(cur, patch, chg);
	if (st == GRAPH_ERR_NO_MEMORY)
		reply(c, NULL);
	else if (st != GRAPH_OK)
		invalid_field(c, "metadata", graph);
	return st;
}

static void cmd_changenode(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph"), *id;
	if (!need_str(c, pl, "id", graph, &id))
		return;
	const cJSON *patch = cJSON_GetObjectItemCaseSensitive(pl, "metadata");
	if (!cJSON_IsObject(patch)) {
		invalid_field(c, "metadata", graph);
		return;
	}
	if (!need_str(c, pl, "graph", graph, &graph))
		return;
	/* The patch's values are checked before the graph and node are (a
	 * payload field, 02 §1.2); the merge itself needs the node's
	 * metadata. */
	BlockMetadata none = block_metadata_default();
	BlockChange chg;
	if (node_change(c, &none, patch, graph, &chg) != GRAPH_OK)
		return;
	block_change_release(&chg);
	Graph *g = graph_id_valid(graph) ? graphs_find(c->p->graphs, graph) : NULL;
	Block *b = g != NULL && graph_id_valid(id) ? graph_find_block(g, id) : NULL;
	GraphsMessage msg;
	if (b == NULL) {
		/* Refused by the registry: Invalid id, Unknown graph or node. */
		BlockChange nothing = {0};
		if (graphs_change_node(c->p->graphs, graph, id, &nothing, &msg) != GRAPH_OK)
			refused(c, &msg, graph);
		return;
	}
	if (node_change(c, &b->metadata, patch, graph, &chg) != GRAPH_OK)
		return;
	if (graphs_change_node(c->p->graphs, graph, id, &chg, &msg) != GRAPH_OK)
		refused(c, &msg, graph);
	block_change_release(&chg);
}

/* Edge metadata (addedge: optional, changeedge: required) as model text in
 * *out: GRAPH_OK, or a reply already sent. */
static enum graph_status edge_metadata(ProtoConn *c, const cJSON *pl, bool required,
				       const char *graph, char **out)
{
	*out = NULL;
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(pl, "metadata");
	enum graph_status st = required && v == NULL ? GRAPH_ERR_INVALID_ARGUMENT
						     : graph_json_to_edge_metadata(v, out);
	if (st == GRAPH_ERR_NO_MEMORY)
		reply(c, NULL);
	else if (st != GRAPH_OK)
		invalid_field(c, "metadata", graph);
	return st;
}

static void cmd_addedge(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph");
	Endpoint src, tgt;
	char *md;
	if (!need_endpoint(c, pl, "src", graph, &src) ||
	    !need_endpoint(c, pl, "tgt", graph, &tgt) || !need_str(c, pl, "graph", graph, &graph) ||
	    edge_metadata(c, pl, false, graph, &md) != GRAPH_OK)
		return;
	GraphsMessage msg;
	if (graphs_add_edge(c->p->graphs, graph, src.node, src.port, tgt.node, tgt.port, md,
			    &msg) != GRAPH_OK)
		refused(c, &msg, graph);
	free(md);
}

static void cmd_removeedge(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph");
	Endpoint src, tgt;
	if (!need_endpoint(c, pl, "src", graph, &src) ||
	    !need_endpoint(c, pl, "tgt", graph, &tgt) || !need_str(c, pl, "graph", graph, &graph))
		return;
	GraphsMessage msg;
	if (graphs_remove_edge(c->p->graphs, graph, src.node, src.port, tgt.node, tgt.port,
			       &msg) != GRAPH_OK)
		refused(c, &msg, graph);
}

static void cmd_changeedge(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph");
	Endpoint src, tgt;
	char *md;
	if (!need_endpoint(c, pl, "src", graph, &src) ||
	    !need_endpoint(c, pl, "tgt", graph, &tgt) ||
	    edge_metadata(c, pl, true, graph, &md) != GRAPH_OK)
		return;
	if (!need_str(c, pl, "graph", graph, &graph)) {
		free(md);
		return;
	}
	GraphsMessage msg;
	if (graphs_change_edge(c->p->graphs, graph, src.node, src.port, tgt.node, tgt.port, md,
			       &msg) != GRAPH_OK)
		refused(c, &msg, graph);
	free(md);
}

/* The declared type of the input node.port of graph, or NULL if there is no
 * such input (the command is then refused by the registry, 02 §3.4.1). */
static const char *input_type(const Protocol *p, const char *graph, const char *node,
			      const char *port)
{
	const Graph *g = graph_id_valid(graph) ? graphs_find(p->graphs, graph) : NULL;
	const Block *b = g != NULL && graph_id_valid(node) ? graph_find_block(g, node) : NULL;
	int32_t i = b != NULL && !block_is_placeholder(b) ? block_input_index(b, port) : -1;
	return i >= 0 ? block_input_port(b, (uint32_t)i)->type : NULL;
}

static void cmd_addinitial(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph");
	Endpoint tgt;
	if (!need_endpoint(c, pl, "tgt", graph, &tgt))
		return;
	const cJSON *src = cJSON_GetObjectItemCaseSensitive(pl, "src");
	const cJSON *data = cJSON_IsObject(src) ? cJSON_GetObjectItemCaseSensitive(src, "data")
						: NULL;
	if (!cJSON_IsString(data) && !cJSON_IsNumber(data) && !cJSON_IsBool(data)) {
		invalid_field(c, "src", graph);
		return;
	}
	if (!need_str(c, pl, "graph", graph, &graph))
		return;
	/* Parsed for the target port's declared type (02 §3.4 "Initial value
	 * encoding"); without such an input any value does, the registry
	 * refuses the command. */
	Linkable v;
	enum graph_status st =
		graph_json_to_initial(data, input_type(c->p, graph, tgt.node, tgt.port), &v);
	if (st != GRAPH_OK) {
		reply(c, NULL); /* out of memory: data was checked above */
		return;
	}
	GraphsMessage msg;
	if (graphs_add_initial(c->p->graphs, graph, tgt.node, tgt.port, &v, &msg) != GRAPH_OK)
		refused(c, &msg, graph);
	linkable_clear(&v);
}

static void cmd_removeinitial(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph");
	Endpoint tgt;
	if (!need_endpoint(c, pl, "tgt", graph, &tgt) || !need_str(c, pl, "graph", graph, &graph))
		return;
	GraphsMessage msg;
	if (graphs_remove_initial(c->p->graphs, graph, tgt.node, tgt.port, &msg) != GRAPH_OK)
		refused(c, &msg, graph);
}

/* add{in,out}port {"graph","node","port","public"}: the broadcast calls the
 * public name "name" (02 §3.4). */
static void add_port(ProtoConn *c, const Envelope *env, enum graph_port_dir dir)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph"), *node, *port, *name;
	if (!need_str(c, pl, "graph", graph, &graph) || !need_str(c, pl, "node", graph, &node) ||
	    !need_str(c, pl, "port", graph, &port) || !need_str(c, pl, "public", graph, &name))
		return;
	GraphsMessage msg;
	if (graphs_add_port(c->p->graphs, graph, dir, name, node, port, &msg) != GRAPH_OK)
		refused(c, &msg, graph);
}

static void remove_port(ProtoConn *c, const Envelope *env, enum graph_port_dir dir)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph"), *name;
	if (!need_str(c, pl, "graph", graph, &graph) || !need_str(c, pl, "public", graph, &name))
		return;
	GraphsMessage msg;
	if (graphs_remove_port(c->p->graphs, graph, dir, name, &msg) != GRAPH_OK)
		refused(c, &msg, graph);
}

static void rename_port(ProtoConn *c, const Envelope *env, enum graph_port_dir dir)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph"), *from, *to;
	if (!need_str(c, pl, "graph", graph, &graph) || !need_str(c, pl, "from", graph, &from) ||
	    !need_str(c, pl, "to", graph, &to))
		return;
	GraphsMessage msg;
	if (graphs_rename_port(c->p->graphs, graph, dir, from, to, &msg) != GRAPH_OK)
		refused(c, &msg, graph);
}

static void cmd_addinport(ProtoConn *c, const Envelope *env)
{
	add_port(c, env, GRAPH_INPORT);
}

static void cmd_removeinport(ProtoConn *c, const Envelope *env)
{
	remove_port(c, env, GRAPH_INPORT);
}

static void cmd_renameinport(ProtoConn *c, const Envelope *env)
{
	rename_port(c, env, GRAPH_INPORT);
}

static void cmd_addoutport(ProtoConn *c, const Envelope *env)
{
	add_port(c, env, GRAPH_OUTPORT);
}

static void cmd_removeoutport(ProtoConn *c, const Envelope *env)
{
	remove_port(c, env, GRAPH_OUTPORT);
}

static void cmd_renameoutport(ProtoConn *c, const Envelope *env)
{
	rename_port(c, env, GRAPH_OUTPORT);
}

/* ---- Groups (02 §3.4.2; app/type_files.h) ------------------------------------------- */

/* A group's "metadata" {"description":String,"icon"?:String}: false if it is
 * no such object (icon may be null, the same as absent). */
static bool group_metadata(const cJSON *pl, const char **description, const char **icon)
{
	const cJSON *md = cJSON_GetObjectItemCaseSensitive(pl, "metadata");
	*description = cJSON_IsObject(md) ? str_field(md, "description") : NULL;
	*icon = NULL;
	return *description != NULL && opt_str_field(md, "icon", icon);
}

/* addgroup's "nodes": a non-empty array of strings, collected into *out (a
 * heap array, *count entries); false if it is none (*out NULL) or memory runs
 * out (*out NULL, *count non-zero). */
static bool group_nodes(const cJSON *pl, const char ***out, size_t *count)
{
	*out = NULL;
	*count = 0;
	const cJSON *nodes = cJSON_GetObjectItemCaseSensitive(pl, "nodes");
	const cJSON *e;
	if (!cJSON_IsArray(nodes) || nodes->child == NULL)
		return false;
	cJSON_ArrayForEach(e, nodes)
	{
		if (!cJSON_IsString(e))
			return false;
	}
	*count = (size_t)cJSON_GetArraySize(nodes);
	*out = malloc(*count * sizeof(**out));
	if (*out == NULL)
		return false;
	size_t i = 0;
	cJSON_ArrayForEach(e, nodes)
	{
		(*out)[i++] = e->valuestring;
	}
	return true;
}

static void cmd_addgroup(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph"), *name;
	if (!need_str(c, pl, "graph", graph, &graph) || !need_str(c, pl, "name", graph, &name))
		return;
	const char **nodes;
	size_t count;
	if (!group_nodes(pl, &nodes, &count)) {
		if (count > 0)
			reply(c, NULL); /* out of memory */
		else
			invalid_field(c, "nodes", graph);
		return;
	}
	CompositeSelection sel = {.name = name, .nodes = nodes, .node_count = count};
	if (!group_metadata(pl, &sel.description, &sel.icon)) {
		free(nodes);
		invalid_field(c, "metadata", graph);
		return;
	}
	GraphsMessage msg;
	if (type_files_add_group(c->p->files, graph, &sel, &msg) != GRAPH_FILES_OK)
		refused(c, &msg, graph);
	free(nodes);
}

static void cmd_removegroup(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph"), *name;
	if (!need_str(c, pl, "graph", graph, &graph) || !need_str(c, pl, "name", graph, &name))
		return;
	GraphsMessage msg;
	if (type_files_remove_group(c->p->files, graph, name, &msg) != GRAPH_FILES_OK)
		refused(c, &msg, graph);
}

static void cmd_renamegroup(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph"), *from, *to;
	if (!need_str(c, pl, "graph", graph, &graph) || !need_str(c, pl, "from", graph, &from) ||
	    !need_str(c, pl, "to", graph, &to))
		return;
	GraphsMessage msg;
	if (type_files_rename_group(c->p->files, graph, from, to, &msg) != GRAPH_FILES_OK)
		refused(c, &msg, graph);
}

static void cmd_changegroup(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph = str_field(pl, "graph"), *name, *description, *icon;
	if (!need_str(c, pl, "graph", graph, &graph) || !need_str(c, pl, "name", graph, &name))
		return;
	if (!group_metadata(pl, &description, &icon)) {
		invalid_field(c, "metadata", graph);
		return;
	}
	GraphsMessage msg;
	if (type_files_change_group(c->p->files, graph, name, description, icon, &msg) !=
	    GRAPH_FILES_OK)
		refused(c, &msg, graph);
}

/* ---- network (02 §3.3) ----------------------------------------------------------------- */

static void cmd_start(ProtoConn *c, const Envelope *env)
{
	const char *graph;
	if (!need_str(c, env->payload, "graph", NULL, &graph))
		return;
	GraphsMessage msg;
	if (graphs_start(c->p->graphs, graph, &msg) != GRAPH_OK)
		refused(c, &msg, graph);
}

static void cmd_stop(ProtoConn *c, const Envelope *env)
{
	const char *graph;
	if (!need_str(c, env->payload, "graph", NULL, &graph))
		return;
	GraphsMessage msg;
	if (graphs_stop(c->p->graphs, graph, &msg) != GRAPH_OK)
		refused(c, &msg, graph);
}

static void cmd_getstatus(ProtoConn *c, const Envelope *env)
{
	const char *graph;
	if (!need_str(c, env->payload, "graph", NULL, &graph))
		return;
	GraphsMessage msg;
	if (graphs_status(c->p->graphs, graph, &msg) != GRAPH_OK)
		refused(c, &msg, graph);
}

static void cmd_persist(ProtoConn *c, const Envelope *env)
{
	const char *graph;
	if (!need_str(c, env->payload, "graph", NULL, &graph))
		return;
	GraphsMessage msg;
	/* A component graph becomes its type (02 §3.4.3); a refusal goes to the
	 * requester, success is the component:component broadcast. */
	const Graph *g = graphs_find(c->p->graphs, graph);
	if (g != NULL && g->component) {
		if (type_files_persist_component(c->p->files, graph, &msg) != GRAPH_FILES_OK)
			refused(c, &msg, graph);
		return;
	}
	enum graph_files_status st = graph_files_persist(c->p->files, graph, &msg);
	/* An invalid or unknown graph is a refusal to the requester; a failed
	 * save was broadcast already (02 §3.3); success has no reply. */
	if (st == GRAPH_FILES_ERR_INVALID || st == GRAPH_FILES_ERR_NOT_FOUND)
		refused(c, &msg, graph);
}

/* ---- network:edges, link-value subscriptions (02 §4.2) ------------------------------- */

/* Sends the watch's network:data with v, if the session is live. A message
 * that does not fit the free space above the reserve, or comes while a burst
 * is deferred (lossy_fits), is not sent: the watch turns dirty and the
 * input's value at the next on_drained goes out instead (decided, plan task
 * 18: a value stream coalesces under back-pressure rather than closing the
 * connection); one that would not fit even an empty buffer above the
 * reserve is dropped. */
static void send_data(EdgeWatch *ew, const Linkable *v)
{
	ProtoConn *c = ew->c;
	if (c->closing || caps_of(c) == 0)
		return;
	char *m = proto_msg_data(c->watch_graph, ew->src_node, ew->src_port, ew->tgt_node,
				 ew->tgt_port, v);
	if (m == NULL) {
		reply(c, NULL);
		return;
	}
	size_t len = strlen(m);
	if (lossy_fits(c, len)) {
		ew->dirty = false;
		send_raw(c, m, len);
	} else if (c->t->send_pending(c->conn) > 0 || (c->pending & PENDING_BURSTS)) {
		ew->dirty = true; /* an on_drained follows */
		c->pending |= PENDING_DATA;
	} else {
		ew->dirty = false; /* too large for any buffer: dropped */
	}
	free(m);
}

static void watch_value(InputWatch *w, const Block *b, uint32_t input, const Linkable *value)
{
	(void)b;
	(void)input;
	send_data((EdgeWatch *)w, value);
}

static void watch_detached(InputWatch *w)
{
	((EdgeWatch *)w)->block = NULL; /* its block is gone (removed, or its graph) */
}

/* Ends every link-value subscription of c (a replacement, logout, expiry,
 * close). */
static void edges_clear(ProtoConn *c)
{
	for (size_t i = 0; i < c->watch_count; i++) {
		EdgeWatch *ew = &c->watches[i];
		if (ew->block != NULL)
			block_unwatch_input(ew->block, ew->input, &ew->w);
		free(ew->names);
	}
	free(c->watches);
	free(c->watch_graph);
	c->watches = NULL;
	c->watch_count = 0;
	c->watch_graph = NULL;
	c->pending &= ~PENDING_DATA;
}

/* The values of c's dirty watches, as they are now. */
static void send_dirty(ProtoConn *c)
{
	for (size_t i = 0; i < c->watch_count && !c->closing; i++) {
		EdgeWatch *ew = &c->watches[i];
		if (ew->dirty && ew->block != NULL)
			send_data(ew, block_input_value(ew->block, ew->input));
	}
}

static char *dup_str(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);
	if (d != NULL)
		memcpy(d, s, n);
	return d;
}

/* Fills ew's names from the pair (copied). False if memory runs out. */
static bool watch_names(EdgeWatch *ew, const Endpoint *src, const Endpoint *tgt)
{
	const char *parts[4] = {src->node, src->port, tgt->node, tgt->port};
	size_t len[4], total = 0;
	for (int i = 0; i < 4; i++)
		total += len[i] = strlen(parts[i]) + 1;
	ew->names = malloc(total);
	if (ew->names == NULL)
		return false;
	const char **out[4] = {&ew->src_node, &ew->src_port, &ew->tgt_node, &ew->tgt_port};
	char *p = ew->names;
	for (int i = 0; i < 4; i++) {
		memcpy(p, parts[i], len[i]);
		*out[i] = p;
		p += len[i];
	}
	return true;
}

/* network:edges (02 §4.2; decided, plan task 18): the request replaces the
 * connection's whole set. Every pair whose tgt is an input of the graph
 * becomes a watch; any other pair (no such graph, node or input) is
 * ignored. The payload is echoed first as network:edges, then each watch
 * sends the input's current value. */
static void cmd_edges(ProtoConn *c, const Envelope *env)
{
	const cJSON *pl = env->payload;
	const char *graph;
	if (!need_str(c, pl, "graph", NULL, &graph))
		return;
	const cJSON *edges = cJSON_GetObjectItemCaseSensitive(pl, "edges");
	bool valid = cJSON_IsArray(edges);
	const cJSON *e = valid ? edges->child : NULL;
	for (; e != NULL && valid; e = e->next) {
		Endpoint src, tgt;
		valid = cJSON_IsObject(e) &&
			endpoint_of(cJSON_GetObjectItemCaseSensitive(e, "src"), &src) &&
			endpoint_of(cJSON_GetObjectItemCaseSensitive(e, "tgt"), &tgt);
	}
	if (!valid) {
		invalid_field(c, "edges", graph);
		return;
	}
	edges_clear(c);
	cJSON *echo = cJSON_Duplicate(pl, true);
	reply(c, echo != NULL ? proto_msg_build("network", "edges", echo) : NULL);
	size_t n = (size_t)cJSON_GetArraySize(edges);
	if (c->closing || n == 0)
		return;
	c->watches = calloc(n, sizeof(*c->watches));
	c->watch_graph = dup_str(graph);
	if (c->watches == NULL || c->watch_graph == NULL) {
		edges_clear(c);
		reply(c, NULL);
		return;
	}
	Graph *g = graph_id_valid(graph) ? graphs_find(c->p->graphs, graph) : NULL;
	cJSON_ArrayForEach(e, edges)
	{
		Endpoint src, tgt;
		endpoint_of(cJSON_GetObjectItemCaseSensitive(e, "src"), &src);
		endpoint_of(cJSON_GetObjectItemCaseSensitive(e, "tgt"), &tgt);
		Block *b = g != NULL && graph_id_valid(tgt.node) ? graph_find_block(g, tgt.node)
								 : NULL;
		int32_t input = b != NULL ? block_input_index(b, tgt.port) : -1;
		if (input < 0)
			continue; /* no such tgt: ignored */
		EdgeWatch *ew = &c->watches[c->watch_count];
		if (!watch_names(ew, &src, &tgt)) {
			edges_clear(c);
			reply(c, NULL);
			return;
		}
		ew->w.on_value = watch_value;
		ew->w.on_detach = watch_detached;
		ew->c = c;
		ew->block = b;
		ew->input = (uint32_t)input;
		c->watch_count++;
		block_watch_input(b, ew->input, &ew->w); /* sends the current value */
		if (c->closing)
			return;
	}
}

/* ---- Dispatch ------------------------------------------------------------------------ */

typedef struct {
	const char *protocol;
	const char *command;
	void (*fn)(ProtoConn *c, const Envelope *env);
} ProtoCommand;

/* The commands this layer runs: every command of 02 §3. */
static const ProtoCommand commands[] = {
	{"auth", "challenge", cmd_challenge},
	{"auth", "login", cmd_login},
	{"auth", "logout", cmd_logout},
	{"runtime", "getruntime", cmd_getruntime},
	{"runtime", "new_graph", cmd_new_graph},
	{"runtime", "delete_graph", cmd_delete_graph},
	{"network", "start", cmd_start},
	{"network", "stop", cmd_stop},
	{"network", "getstatus", cmd_getstatus},
	{"network", "persist", cmd_persist},
	{"network", "edges", cmd_edges},
	{"graph", "list", cmd_graph_list},
	{"graph", "connect", cmd_connect},
	{"graph", "clear", cmd_graph_clear},
	{"graph", "rename", cmd_rename},
	{"graph", "addnode", cmd_addnode},
	{"graph", "removenode", cmd_removenode},
	{"graph", "renamenode", cmd_renamenode},
	{"graph", "changenode", cmd_changenode},
	{"graph", "addedge", cmd_addedge},
	{"graph", "removeedge", cmd_removeedge},
	{"graph", "changeedge", cmd_changeedge},
	{"graph", "addinitial", cmd_addinitial},
	{"graph", "removeinitial", cmd_removeinitial},
	{"graph", "addinport", cmd_addinport},
	{"graph", "removeinport", cmd_removeinport},
	{"graph", "renameinport", cmd_renameinport},
	{"graph", "addoutport", cmd_addoutport},
	{"graph", "removeoutport", cmd_removeoutport},
	{"graph", "renameoutport", cmd_renameoutport},
	{"graph", "addgroup", cmd_addgroup},
	{"graph", "removegroup", cmd_removegroup},
	{"graph", "renamegroup", cmd_renamegroup},
	{"graph", "changegroup", cmd_changegroup},
	{"environment", "describe", cmd_env_describe},
	{"environment", "getbindings", cmd_getbindings},
	{"environment", "bind", cmd_bind},
	{"environment", "unbind", cmd_unbind},
	{"environment", "watch", cmd_env_watch},
	{"environment", "unwatch", cmd_env_unwatch},
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
		unknown_command(c, env); /* not reached: every known command has one */
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
	if ((pending & PENDING_CONNECT) && caps_of(c) != 0 && !c->closing)
		send_snapshot(c, c->connect_id);
	if (pending & PENDING_DATA)
		send_dirty(c);
	if ((pending & PENDING_ENV) && c->env_watch && !c->closing)
		env_pass_run(c);
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
	edges_clear(c);
	env_unwatch(c);
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
	p->env = cfg->environment;
	p->auth = cfg->auth;
	if (p->env != NULL)
		environment_watch(p->env, on_env_change, p);
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
	if (p->env != NULL)
		environment_watch(p->env, NULL, NULL);
	while (p->conns != NULL)
		protocol_conn_close(p->conns);
	drop_event_cache(p);
	free(p);
}
