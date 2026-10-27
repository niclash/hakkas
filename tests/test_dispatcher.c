/*
 * The protocol dispatcher (protocol/dispatcher.h, protocol/messages.h; 02 §1-
 * §3.6, §4.1, 05 §4), driven in-process: fake connections capture every
 * message sent, and a fake Authorizer (02 §2.5 item 10) controls each
 * session. Covers every malformed envelope, secret gating, the capability
 * table per role, challenge/login/logout including the delayed challenge
 * and the close after failures, session expiry, getruntime before and after
 * login, new_graph/delete_graph broadcasts and refusals, component:list and
 * getsource, the trace no-ops, deferred bursts, log mirroring, a full SCRAM
 * login through the real authorizer with a test user made here; and (plan
 * task 18) every graph:* and network:* command with its broadcasts on
 * several connections and its refusals, scoping, the graph:connect snapshot
 * and the outgoing buffer, initial value encoding, network:edges with its
 * network:data stream, and the users file re-read at every challenge; and
 * (plan task 21b) the group commands with their broadcasts, refusals, type
 * files and the rollback of a failed save.
 */
#define _XOPEN_SOURCE 700

#include "test.h"

#include <ftw.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/graph_files.h"
#include "app/links_file.h"
#include "app/type_files.h"
#include "blocktypes/block_types.h"
#include "blocktypes/composite_types.h"
#include "cJSON.h"
#include "engine/version.h"
#include "graphs/graph_json.h"
#include "platform/pal.h"
#include "protocol/auth.h"
#include "protocol/dispatcher.h"
#include "protocol/messages.h"
#include "protocol/users.h"
#include "system/config.h"
#include "system/crypto.h"
#include "system/log.h"

/* ---- JSON helpers ------------------------------------------------------------------ */

/* text with every ' turned into ", in a rotating buffer. */
static const char *jq(const char *text)
{
	static char buf[4][4096];
	static int next;
	char *p = buf[next++ % 4];
	size_t i = 0;
	for (; text[i] != '\0' && i < sizeof(buf[0]) - 1; i++)
		p[i] = text[i] == '\'' ? '"' : text[i];
	p[i] = '\0';
	return p;
}

/* Whether actual and expected (with ' for ") are the same JSON value. */
static bool json_same(const char *actual, const char *expected)
{
	cJSON *a = cJSON_Parse(actual);
	cJSON *e = cJSON_Parse(jq(expected));
	bool eq = a != NULL && e != NULL && cJSON_Compare(a, e, true);
	cJSON_Delete(a);
	cJSON_Delete(e);
	return eq;
}

/* The same, showing both on a mismatch. */
static bool json_equal(const char *actual, const char *expected)
{
	bool eq = json_same(actual, expected);
	if (!eq)
		fprintf(stderr, "  got:      %s\n  expected: %s\n", actual ? actual : "(none)",
			jq(expected));
	return eq;
}

/* ---- Fake connections -------------------------------------------------------------- */

#define FAKE_MSGS 512

typedef struct {
	char *msgs[FAKE_MSGS];
	size_t count;
	size_t space;	/* what send_space says, and the largest message accepted */
	size_t pending; /* what send_pending says */
	size_t capacity; /* what send_capacity says */
	bool closed;
	uint16_t close_code;
	bool log_on_send; /* log a line from inside send_text (recursion test) */
	bool consume;	  /* each message takes its bytes from space */
	ProtoConn *pc;
} FakeConn;

static bool f_send_text(void *conn, const char *text, size_t len)
{
	FakeConn *f = conn;
	if (f->closed)
		return false;
	if (len > f->space) {
		f->closed = true; /* like the WebSocket server: a full buffer closes */
		return false;
	}
	CHECK(strlen(text) == len);
	/* One JSON object per message, never with a top-level secret (02 §1). */
	cJSON *m = cJSON_Parse(text);
	CHECK(cJSON_IsObject(m));
	CHECK(cJSON_GetObjectItemCaseSensitive(m, "secret") == NULL);
	cJSON_Delete(m);
	if (f->count < FAKE_MSGS) {
		f->msgs[f->count] = malloc(len + 1);
		memcpy(f->msgs[f->count], text, len + 1);
		f->count++;
	}
	if (f->log_on_send)
		log_warn("sending a message");
	if (f->consume)
		f->space -= len;
	return true;
}

static size_t f_send_space(void *conn)
{
	return ((FakeConn *)conn)->space;
}

static size_t f_send_capacity(void *conn)
{
	return ((FakeConn *)conn)->capacity;
}

static size_t f_send_pending(void *conn)
{
	return ((FakeConn *)conn)->pending;
}

static void f_close(void *conn, uint16_t code, const char *reason)
{
	FakeConn *f = conn;
	(void)reason;
	CHECK(!f->closed);
	f->closed = true;
	f->close_code = code;
}

static const ProtoTransport fake_transport = {f_send_text, f_send_space, f_send_capacity,
					      f_send_pending, f_close};

static void clear(FakeConn *f)
{
	for (size_t i = 0; i < f->count; i++)
		free(f->msgs[i]);
	f->count = 0;
}

static const char *msg(const FakeConn *f, size_t i)
{
	return i < f->count ? f->msgs[i] : NULL;
}

/* ---- The fake authorizer ----------------------------------------------------------- */

struct AuthConn {
	const AuthEvents *events;
	void *user;
	bool authenticated;
	bool expired; /* the session's deadline passed (the tick has not run) */
	uint32_t caps;
	char secret[AUTH_SECRET_SIZE];
};

static struct {
	bool open_fails;
	enum auth_result challenge_result;
	enum auth_result verify_result;
	bool issue_ok;
	enum user_role role;
	int activity;
	int opens, closes;
	bool saw_null_user, saw_null_cnonce, saw_null_nonce, saw_null_proof;
	char saw_resume[32]; /* "(null)" for NULL */
	int secrets;
} fake;

static AuthConn *fa_open(void *self, const char *peer, const AuthEvents *ev, void *user)
{
	(void)self;
	CHECK(peer != NULL);
	if (fake.open_fails)
		return NULL;
	AuthConn *c = calloc(1, sizeof(*c));
	c->events = ev;
	c->user = user;
	fake.opens++;
	return c;
}

static void fa_close(void *self, AuthConn *c)
{
	(void)self;
	fake.closes++;
	free(c);
}

static void fa_activity(void *self, AuthConn *c)
{
	(void)self;
	(void)c;
	fake.activity++;
}

static enum auth_result fa_challenge(void *self, AuthConn *c, const char *user, const char *cnonce,
				     const char *resume, AuthChallenge *out)
{
	(void)self;
	(void)c;
	fake.saw_null_user = user == NULL;
	fake.saw_null_cnonce = cnonce == NULL;
	snprintf(fake.saw_resume, sizeof(fake.saw_resume), "%s", resume ? resume : "(null)");
	if (fake.challenge_result == AUTH_OK) {
		out->resume = resume != NULL;
		snprintf(out->salt, sizeof(out->salt), "c2FsdHNhbHRzYWx0c2FsdA==");
		out->iterations = 4096;
		snprintf(out->nonce, sizeof(out->nonce), "%sSERVER", cnonce ? cnonce : "");
	}
	return fake.challenge_result;
}

static enum auth_result fa_verify(void *self, AuthConn *c, const char *user, const char *nonce,
				  const char *resume, const char *proof, AuthProof *out)
{
	(void)self;
	(void)c;
	snprintf(fake.saw_resume, sizeof(fake.saw_resume), "%s", resume ? resume : "(null)");
	fake.saw_null_user = user == NULL;
	fake.saw_null_nonce = nonce == NULL;
	fake.saw_null_proof = proof == NULL;
	if (fake.verify_result == AUTH_OK) {
		out->role = fake.role;
		out->capabilities = auth_role_capabilities(fake.role);
		snprintf(out->server_signature, sizeof(out->server_signature), "c2lnbmF0dXJl");
	}
	return fake.verify_result;
}

static bool fa_issue(void *self, AuthConn *c, char out[AUTH_SECRET_SIZE],
		     char resume[AUTH_RESUME_SIZE])
{
	(void)self;
	if (!fake.issue_ok)
		return false;
	snprintf(resume, AUTH_RESUME_SIZE, "resume%d", fake.secrets);
	c->authenticated = true;
	c->expired = false;
	c->caps = auth_role_capabilities(fake.role);
	snprintf(c->secret, sizeof(c->secret), "secret%d", fake.secrets++);
	memcpy(out, c->secret, AUTH_SECRET_SIZE);
	return true;
}

static bool fa_validate(void *self, AuthConn *c, const char *secret)
{
	(void)self;
	return c->authenticated && !c->expired && secret != NULL && strcmp(secret, c->secret) == 0;
}

static void fa_revoke(void *self, AuthConn *c)
{
	(void)self;
	c->authenticated = false;
}

static uint32_t fa_caps(void *self, const AuthConn *c)
{
	(void)self;
	return c->authenticated && !c->expired ? c->caps : 0;
}

static enum auth_state fa_state(void *self, const AuthConn *c)
{
	(void)self;
	return c->authenticated ? AUTH_STATE_AUTHENTICATED : AUTH_STATE_UNAUTHENTICATED;
}

static const AuthOps fake_ops = {fa_open,   fa_close,	 fa_activity, fa_challenge,
				 fa_verify, fa_issue,	 fa_validate, fa_revoke,
				 fa_caps,   fa_state};

/* The fake session behind a connection (the authorizer's per-connection
 * object is reached through the callbacks' user pointer: the ProtoConn). */
static AuthConn *last_auth_conn(void);

/* ---- Fixtures ---------------------------------------------------------------------- */

static char root[PAL_PATH_MAX];
static Runtime rt;
static Graphs gs;
static GraphFiles gf;
static Config cfg;
static Protocol *proto;
static AuthConn *fake_conns[8];
static size_t fake_conn_count;

static AuthConn *fa_open_tracked(void *self, const char *peer, const AuthEvents *ev, void *user)
{
	AuthConn *c = fa_open(self, peer, ev, user);
	if (c != NULL && fake_conn_count < 8)
		fake_conns[fake_conn_count++] = c;
	return c;
}

static AuthConn *last_auth_conn(void)
{
	return fake_conn_count > 0 ? fake_conns[fake_conn_count - 1] : NULL;
}

static AuthOps tracked_ops;
static int fixture;

/* The environment of setup_env (plan task 76): sensors ai (number, 1..2) and
 * di (bool), the actuator dq (bool). */
static const HakkasEnvPort env_out_ports[] = {
	{.id = "ai", .type = HAKKAS_ENV_NUMBER, .addressable = true, .count = 2},
	{.id = "di", .type = HAKKAS_ENV_BOOL},
};
static const HakkasEnvPort env_in_ports[] = {{.id = "dq", .type = HAKKAS_ENV_BOOL}};
static const HakkasEnvDescription env_desc = {
	.name = "fake", .out_ports = env_out_ports, .out_port_count = 2,
	.in_ports = env_in_ports, .in_port_count = 1};
static Environment env;
static Environment *with_env; /* setup_with opens it when set */
static HakkasEnvSink *env_sink;

static const HakkasEnvDescription *fe_describe(void *ctx)
{
	(void)ctx;
	return &env_desc;
}

static int fe_start(void *ctx, HakkasEnvSink *sink)
{
	(void)ctx;
	env_sink = sink;
	return 0;
}

static void fe_stop(void *ctx)
{
	(void)ctx;
}

static void fe_write(void *ctx, int port, int index, const HakkasValue *v)
{
	(void)ctx;
	(void)port;
	(void)index;
	(void)v;
}

static enum pal_err push_direct(WorkEntry *e)
{
	work_queue_push_entry(&rt.queue, e);
	return PAL_OK;
}

static int64_t fe_clock(void)
{
	return 0;
}

/* A sensor value, drained at once. */
static void sense(int port, int index, HakkasValue v)
{
	CHECK(hakkas_env_sink_push(env_sink, port, index, &v));
	runtime_drain(&rt, SIZE_MAX);
}

static void setup_with(Authorizer az, bool mirror)
{
	runtime_init(&rt);
	graphs_init(&gs, &rt, block_types_get, pal_random_bytes);
	char dir[PAL_PATH_MAX + 16];
	snprintf(dir, sizeof(dir), "%s/d%d", root, fixture++);
	CHECK(pal_storage_mkdir(dir) == PAL_OK);
	CHECK(graph_files_init(&gf, &gs, dir));
	ConfigDefaults d = {"/etc/hakkas", "/web", dir};
	CHECK(config_init(&cfg, &d) == CONFIG_OK);
	snprintf(cfg.engine_id, sizeof(cfg.engine_id), "engine-1");
	snprintf(cfg.label, sizeof(cfg.label), "Test Hakkas");
	cfg.log.mirror = mirror;
	if (with_env != NULL) {
		environment_init(with_env, fe_clock);
		HakkasEnvironment spi = {fe_describe, fe_start, fe_stop, fe_write, NULL, NULL};
		CHECK(environment_open(with_env, &rt, &spi, push_direct));
	}
	ProtocolConfig pc = {&gs, &gf, &cfg, az, with_env};
	CHECK(protocol_create(&pc, &proto));
	memset(&fake, 0, sizeof(fake));
	fake.challenge_result = AUTH_OK;
	fake.verify_result = AUTH_OK;
	fake.issue_ok = true;
	fake.role = USER_ROLE_EDITOR;
	fake_conn_count = 0;
}

static void setup_mirror(bool mirror)
{
	tracked_ops = fake_ops;
	tracked_ops.open = fa_open_tracked;
	setup_with((Authorizer){&tracked_ops, NULL}, mirror);
}

static void setup(void)
{
	setup_mirror(false);
}

static void teardown(void)
{
	protocol_destroy(proto);
	proto = NULL;
	if (with_env != NULL)
		environment_close(with_env);
	with_env = NULL;
	graphs_deinit(&gs);
	runtime_deinit(&rt);
}

static void conn_open(FakeConn *f)
{
	memset(f, 0, sizeof(*f));
	f->space = 1u << 20;
	f->capacity = 1u << 20;
	f->pc = protocol_conn_open(proto, &fake_transport, f, "192.0.2.7");
	CHECK(f->pc != NULL);
}

static void conn_close(FakeConn *f)
{
	protocol_conn_close(f->pc);
	f->pc = NULL;
	clear(f);
}

/* Deliver text (JSON with ' for ") as one client message. */
static void send_raw_text(FakeConn *f, const char *text)
{
	protocol_conn_message(f->pc, text, strlen(text));
}

static void sendq(FakeConn *f, const char *text)
{
	send_raw_text(f, jq(text));
}

/* A command with payload (JSON with ' for ") and secret. */
static void command(FakeConn *f, const char *protocol, const char *cmd, const char *payload,
		    const char *secret)
{
	char buf[2048];
	snprintf(buf, sizeof(buf),
		 "{\"protocol\":\"%s\",\"command\":\"%s\",\"payload\":%s,\"secret\":\"%s\"}",
		 protocol, cmd, jq(payload), secret);
	send_raw_text(f, buf);
}

/* The error message expected for text. */
static const char *error_json(const char *message)
{
	static char buf[4][1024];
	static int next;
	char *p = buf[next++ % 4];
	cJSON *e = cJSON_CreateObject();
	cJSON_AddStringToObject(e, "protocol", "network");
	cJSON_AddStringToObject(e, "command", "error");
	cJSON *pl = cJSON_AddObjectToObject(e, "payload");
	cJSON_AddStringToObject(pl, "stack", "");
	cJSON_AddStringToObject(pl, "message", message);
	char *t = cJSON_PrintUnformatted(e);
	snprintf(p, 1024, "%s", t);
	free(t);
	cJSON_Delete(e);
	return p;
}

/* Expect exactly one message on f, equal to expected; then clear. */
static void expect_one(FakeConn *f, const char *expected)
{
	CHECK(f->count == 1);
	CHECK(json_equal(msg(f, 0), expected));
	clear(f);
}

/* Expect a new graph's runtime:new_graph, equal to created, then its status,
 * unsaved (02 §3.3); then clear. */
static void expect_created(FakeConn *f, const char *created)
{
	CHECK(f->count == 2);
	CHECK(json_equal(msg(f, 0), created));
	CHECK(f->count == 2 && strstr(msg(f, 1), "\"command\":\"status\"") != NULL &&
	      strstr(msg(f, 1), "\"unsaved\":true}") != NULL);
	clear(f);
}

static void expect_error(FakeConn *f, const char *message)
{
	expect_one(f, error_json(message));
}

/* Log f in with the fake authorizer as role; returns its secret. */
static const char *login_as(FakeConn *f, enum user_role role)
{
	fake.role = role;
	command(f, "auth", "challenge", "{'user':'u','cnonce':'Y25vbmNl'}", "");
	command(f, "auth", "login", "{'user':'u','nonce':'n','proof':'p'}", "");
	CHECK(f->count == 2);
	clear(f);
	static char secret[16][AUTH_SECRET_SIZE];
	static int next;
	char *s = secret[next++ % 16];
	AuthConn *ac = last_auth_conn();
	for (size_t i = 0; i < fake_conn_count; i++) {
		if (fake_conns[i]->user == f->pc)
			ac = fake_conns[i];
	}
	snprintf(s, AUTH_SECRET_SIZE, "%s", ac->secret);
	return s;
}

static AuthConn *auth_of(const FakeConn *f)
{
	for (size_t i = 0; i < fake_conn_count; i++) {
		if (fake_conns[i]->user == f->pc)
			return fake_conns[i];
	}
	return NULL;
}

/* ---- Messages ---------------------------------------------------------------------- */

static void test_messages(void)
{
	char t[PROTO_TIME_SIZE];
	proto_msg_time(0, t);
	CHECK_STREQ(t, "1970-01-01T00:00:00Z");
	proto_msg_time(90999, t); /* floored to the second (02 §3.3) */
	CHECK_STREQ(t, "1970-01-01T00:01:30Z");
	proto_msg_time(951782400000ull + 3723000, t);
	CHECK_STREQ(t, "2000-02-29T01:02:03Z");
	proto_msg_time(86400000ull * 365, t);
	CHECK_STREQ(t, "1971-01-01T00:00:00Z");

	/* The special shapes, byte for byte (02 §1). */
	char *m = proto_msg_logout();
	CHECK_STREQ(m, "{\"protocol\":\"auth\",\"command\":\"logout\"}");
	free(m);
	m = proto_msg_componentsready(7);
	CHECK_STREQ(m, "{\"protocol\":\"component\",\"command\":\"componentsready\","
		       "\"payload\":7}");
	free(m);
	m = proto_msg_error("x", NULL);
	CHECK_STREQ(m, "{\"protocol\":\"network\",\"command\":\"error\",\"payload\":"
		       "{\"stack\":\"\",\"message\":\"x\"}}");
	free(m);
	m = proto_msg_error("y", "g");
	CHECK_STREQ(m, "{\"protocol\":\"network\",\"command\":\"error\",\"payload\":"
		       "{\"stack\":\"\",\"message\":\"y\",\"graph\":\"g\"}}");
	free(m);
	/* The ws glue exists with every callback. */
	const WsHandler *h = protocol_ws_handler();
	CHECK(h != NULL && h->on_open && h->on_message && h->on_close && h->on_drained);
}

/* ---- Envelope (02 §1) -------------------------------------------------------------- */

static void expect_badly(FakeConn *f, const char *raw)
{
	char text[1024];
	snprintf(text, sizeof(text), "Badly formatted request: %s", raw);
	expect_error(f, text);
}

static void test_envelope(void)
{
	setup();
	FakeConn f;
	conn_open(&f);
	static const char *const bad[] = {
		"hello",
		"",
		"[]",
		"42",
		"{}",
		"{'protocol':'runtime','command':'getruntime','payload':{}} x",
		"{'protocol':'runtime','command':'getruntime','payload':{}}",
		"{'protocol':'runtime','command':'getruntime','secret':''}",
		"{'protocol':'runtime','payload':{},'secret':''}",
		"{'command':'getruntime','payload':{},'secret':''}",
		"{'protocol':7,'command':'getruntime','payload':{},'secret':''}",
		"{'protocol':'foo','command':'getruntime','payload':{},'secret':''}",
		"{'protocol':'Runtime','command':'getruntime','payload':{},'secret':''}",
		"{'protocol':'runtime','command':1,'payload':{},'secret':''}",
		"{'protocol':'runtime','command':null,'payload':{},'secret':''}",
		"{'protocol':'runtime','command':'getruntime','payload':[],'secret':''}",
		"{'protocol':'runtime','command':'getruntime','payload':'x','secret':''}",
		"{'protocol':'runtime','command':'getruntime','payload':null,'secret':''}",
		"{'protocol':'runtime','command':'getruntime','payload':{},'secret':null}",
		"{'protocol':'runtime','command':'getruntime','payload':{},'secret':5}",
		"{'protocol':'runtime','command':'getruntime','payload':{},'secret':''",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		int before = fake.activity;
		sendq(&f, bad[i]);
		expect_badly(&f, jq(bad[i]));
		CHECK(fake.activity == before + 1); /* every message is activity */
	}
	/* Whitespace around the object is fine; extra members are ignored. */
	sendq(&f, " {'protocol':'trace','command':'start','payload':{'graph':'g'},'secret':'',"
		  "'requestId':3}\n ");
	expect_error(&f, "Not authenticated");

	/* A long raw text is cut to PROTO_RAW_ECHO_MAX bytes at a character
	 * boundary, "..." appended. */
	char raw[600];
	memset(raw, 'a', sizeof(raw) - 1);
	raw[sizeof(raw) - 1] = '\0';
	memcpy(raw + PROTO_RAW_ECHO_MAX - 1, "\xc3\xa9", 2); /* é across the cut */
	send_raw_text(&f, raw);
	char want[PROTO_RAW_ECHO_MAX + 8];
	memcpy(want, raw, PROTO_RAW_ECHO_MAX - 1);
	memcpy(want + PROTO_RAW_ECHO_MAX - 1, "...", 4);
	expect_badly(&f, want);
	/* Exactly PROTO_RAW_ECHO_MAX bytes: not cut. */
	raw[PROTO_RAW_ECHO_MAX] = '\0';
	memset(raw, 'b', PROTO_RAW_ECHO_MAX);
	send_raw_text(&f, raw);
	expect_badly(&f, raw);
	/* A U+0000 ends the echoed text. */
	protocol_conn_message(f.pc, "ab\0cd", 5);
	expect_badly(&f, "ab");
	CHECK(!f.closed);
	conn_close(&f);
	teardown();
}

/* ---- Secret gating (02 §2.5 item 5) ------------------------------------------------- */

static void test_secret_gating(void)
{
	setup();
	FakeConn f;
	conn_open(&f);
	/* Before login every gated command is refused, unknown ones included. */
	command(&f, "component", "list", "{}", "");
	expect_error(&f, "Not authenticated");
	command(&f, "graph", "merge", "{}", "");
	expect_error(&f, "Not authenticated");
	command(&f, "runtime", "foo", "{}", "x");
	expect_error(&f, "Not authenticated");
	/* auth is never gated: an unknown command is the generic error. */
	command(&f, "auth", "foo", "{}", "");
	expect_error(&f, "Unknown command: auth:foo");
	/* environment is gated (02 §3.7), the old login commands included. */
	command(&f, "environment", "challenge", "{'user':'u','cnonce':'Y25vbmNl'}", "");
	expect_error(&f, "Not authenticated");
	/* getruntime needs no secret. */
	command(&f, "runtime", "getruntime", "{}", "whatever");
	CHECK(f.count == 1);
	clear(&f);

	const char *secret = login_as(&f, USER_ROLE_VIEWER);
	command(&f, "trace", "clear", "{'graph':'g'}", secret);
	expect_one(&f, "{'protocol':'trace','command':'clear','payload':{'graph':'g'}}");
	/* A wrong or missing secret; the value is never echoed. */
	command(&f, "trace", "clear", "{'graph':'g'}", "wrong-secret-value");
	CHECK(f.count == 1 && strstr(msg(&f, 0), "wrong-secret-value") == NULL);
	expect_error(&f, "Not authenticated");
	command(&f, "trace", "clear", "{'graph':'g'}", "");
	expect_error(&f, "Not authenticated");
	/* Unknown commands with a valid secret: the generic error, a long
	 * command cut. */
	command(&f, "graph", "merge", "{}", secret);
	expect_error(&f, "Unknown command: graph:merge");
	char longcmd[200], want[200];
	memset(longcmd, 'c', 100);
	longcmd[100] = '\0';
	command(&f, "network", longcmd, "{}", secret);
	snprintf(want, sizeof(want), "Unknown command: network:%.*s...", PROTO_COMMAND_ECHO_MAX,
		 longcmd);
	expect_error(&f, want);
	/* An expired session (its deadline passed, the tick not run yet). */
	auth_of(&f)->expired = true;
	command(&f, "trace", "clear", "{'graph':'g'}", secret);
	expect_error(&f, "Not authenticated");
	/* A secret of another connection is invalid here (02 §2.5 item 2). */
	auth_of(&f)->expired = false;
	FakeConn g;
	conn_open(&g);
	command(&g, "trace", "clear", "{'graph':'g'}", secret);
	expect_error(&g, "Not authenticated");
	conn_close(&g);
	conn_close(&f);
	CHECK(fake.opens == fake.closes);
	teardown();
}

/* ---- Capabilities (02 §2.4) --------------------------------------------------------- */

static void test_capabilities(void)
{
	/* Each known command and the roles that may run it (bit per role:
	 * viewer, operator, editor, admin). */
	static const struct {
		const char *protocol, *command;
		unsigned roles;
	} table[] = {
		{"runtime", "new_graph", 0xC},	  {"runtime", "delete_graph", 0xC},
		{"component", "list", 0xF},	  {"component", "getsource", 0xF},
		{"trace", "start", 0xF},	  {"trace", "stop", 0xF},
		{"trace", "clear", 0xF},	  {"trace", "dump", 0xF},
		{"graph", "connect", 0xF},	  {"graph", "list", 0xF},
		{"graph", "addnode", 0xC},	  {"graph", "removenode", 0xC},
		{"graph", "rename", 0xC},	  {"graph", "addinitial", 0xC},
		{"graph", "renameoutport", 0xC},  {"graph", "addgroup", 0xC},
		{"network", "getstatus", 0xF},	  {"network", "edges", 0xF},
		{"network", "start", 0xE},	  {"network", "stop", 0xE},
		{"network", "persist", 0xC},	  {"component", "source", 0xC},
	};
	static const enum user_role roles[] = {USER_ROLE_VIEWER, USER_ROLE_OPERATOR,
					       USER_ROLE_EDITOR, USER_ROLE_ADMIN};
	for (size_t r = 0; r < 4; r++) {
		setup();
		FakeConn f;
		conn_open(&f);
		const char *secret = login_as(&f, roles[r]);
		for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
			command(&f, table[i].protocol, table[i].command,
				"{'graph':'nope','id':'nope','name':'nope'}", secret);
			char denied[128];
			snprintf(denied, sizeof(denied), "Not permitted: %s:%s",
				 table[i].protocol, table[i].command);
			bool allowed = (table[i].roles >> r) & 1u;
			bool refused = f.count == 1 && json_same(msg(&f, 0), error_json(denied));
			if (allowed == refused)
				fprintf(stderr, "  role %zu, %s:%s\n", r, table[i].protocol,
					table[i].command);
			CHECK(allowed != refused);
			clear(&f);
		}
		conn_close(&f);
		teardown();
	}
}

/* ---- auth (02 §2.3, §3.1) --------------------------------------------------------- */

static void test_login_flow(void)
{
	setup();
	FakeConn f;
	conn_open(&f);
	command(&f, "auth", "challenge", "{'user':'alice','cnonce':'Y25vbmNl'}", "");
	expect_one(&f, "{'protocol':'auth','command':'challenge','payload':"
		       "{'salt':'c2FsdHNhbHRzYWx0c2FsdA==','iterations':4096,"
		       "'nonce':'Y25vbmNlSERVER'}}");
	/* Key order, byte for byte. */
	command(&f, "auth", "challenge", "{'user':'alice','cnonce':'Y25vbmNl'}", "");
	CHECK_STREQ(msg(&f, 0), "{\"protocol\":\"auth\",\"command\":\"challenge\","
				"\"payload\":{\"salt\":\"c2FsdHNhbHRzYWx0c2FsdA==\",\"iterations\":"
				"4096,\"nonce\":\"Y25vbmNlSERVER\"}}");
	clear(&f);
	/* Mistyped or missing fields reach the authorizer as NULL. */
	fake.challenge_result = AUTH_FAILED;
	command(&f, "auth", "challenge", "{'user':5}", "");
	CHECK(fake.saw_null_user && fake.saw_null_cnonce);
	expect_error(&f, "Login failed");

	/* A resume (02 §2.3.1): the id reaches the authorizer, the reply has
	 * only the nonce; a mistyped id is refused without asking it. */
	fake.challenge_result = AUTH_OK;
	command(&f, "auth", "challenge", "{'user':'alice','cnonce':'Y25vbmNl'}", "");
	CHECK_STREQ(fake.saw_resume, "(null)");
	clear(&f);
	command(&f, "auth", "challenge",
		"{'user':'alice','cnonce':'Y25vbmNl','resume':'aWQ'}", "");
	CHECK_STREQ(fake.saw_resume, "aWQ");
	expect_one(&f, "{'protocol':'auth','command':'challenge','payload':"
		       "{'nonce':'Y25vbmNlSERVER'}}");
	fake.saw_resume[0] = '\0';
	command(&f, "auth", "challenge",
		"{'user':'alice','cnonce':'Y25vbmNl','resume':5}", "");
	CHECK_STREQ(fake.saw_resume, "");
	expect_error(&f, "Login failed");
	fake.challenge_result = AUTH_FAILED;

	/* Delayed: nothing now; the callback replies later. */
	fake.challenge_result = AUTH_DELAYED;
	command(&f, "auth", "challenge", "{'user':'alice','cnonce':'Y25vbmNl'}", "");
	CHECK(f.count == 0);
	AuthConn *ac = auth_of(&f);
	AuthChallenge ch = {false, "c2FsdA==", 7, "Y25vbmNlX"};
	ac->events->on_challenge(ac->user, &ch);
	expect_one(&f, "{'protocol':'auth','command':'challenge','payload':"
		       "{'salt':'c2FsdA==','iterations':7,'nonce':'Y25vbmNlX'}}");
	ac->events->on_challenge(ac->user, NULL);
	expect_error(&f, "Login failed");

	/* Login. */
	fake.role = USER_ROLE_OPERATOR;
	command(&f, "auth", "login", "{'user':'alice','nonce':'n','proof':'p'}", "");
	CHECK(!fake.saw_null_user && !fake.saw_null_nonce && !fake.saw_null_proof);
	CHECK_STREQ(fake.saw_resume, "(null)");
	CHECK(f.count == 1);
	CHECK_STREQ(msg(&f, 0),
		    "{\"protocol\":\"auth\",\"command\":\"login\",\"payload\":{\"secret\":"
		    "\"secret0\",\"role\":\"operator\",\"capabilities\":[\"protocol:component\","
		    "\"graph:readonly\",\"network:status\",\"network:data\",\"network:control\"],"
		    "\"server_signature\":\"c2lnbmF0dXJl\",\"resume\":\"resume0\"}}");
	clear(&f);
	CHECK(fa_caps(NULL, ac) != 0);

	/* Logout: no payload at all, back to unauthenticated. */
	command(&f, "auth", "logout", "{}", "");
	CHECK(f.count == 1);
	CHECK_STREQ(msg(&f, 0), "{\"protocol\":\"auth\",\"command\":\"logout\"}");
	clear(&f);
	command(&f, "trace", "clear", "{'graph':'g'}", "secret0");
	expect_error(&f, "Not authenticated");

	/* Failures: the secret not issued, a wrong proof, missing fields. */
	fake.issue_ok = false;
	command(&f, "auth", "login", "{'user':'alice','nonce':'n','proof':'p'}", "");
	expect_error(&f, "Login failed");
	fake.issue_ok = true;
	fake.verify_result = AUTH_FAILED;
	command(&f, "auth", "login", "{'nonce':7}", "");
	CHECK(fake.saw_null_user && fake.saw_null_nonce && fake.saw_null_proof);
	expect_error(&f, "Login failed");
	/* A resume's id reaches the authorizer; a mistyped one as "" (wrong). */
	command(&f, "auth", "login",
		"{'user':'alice','nonce':'n','resume':'aWQ','proof':'p'}", "");
	CHECK_STREQ(fake.saw_resume, "aWQ");
	expect_error(&f, "Login failed");
	command(&f, "auth", "login",
		"{'user':'alice','nonce':'n','resume':[],'proof':'p'}", "");
	CHECK_STREQ(fake.saw_resume, "");
	expect_error(&f, "Login failed");
	CHECK(!f.closed);
	/* The third failure closes with 1008 after the reply. */
	fake.verify_result = AUTH_FAILED_CLOSE;
	command(&f, "auth", "login", "{'user':'alice','nonce':'n','proof':'p'}", "");
	expect_error(&f, "Login failed");
	CHECK(f.closed && f.close_code == WS_CLOSE_POLICY);
	/* Nothing is answered after the close started. */
	command(&f, "runtime", "getruntime", "{}", "");
	CHECK(f.count == 0);
	conn_close(&f);

	/* The authorizer refusing a connection. */
	fake.open_fails = true;
	CHECK(protocol_conn_open(proto, &fake_transport, &f, "192.0.2.8") == NULL);
	teardown();
}

/* ---- Expiry (02 §2.5 item 3) -------------------------------------------------------- */

static void test_expiry(void)
{
	setup();
	FakeConn a, b;
	conn_open(&a);
	conn_open(&b);
	login_as(&a, USER_ROLE_EDITOR);
	const char *sb = login_as(&b, USER_ROLE_EDITOR);
	/* new_graph and the new graph's status (unsaved, 02 §3.3) each. */
	command(&b, "runtime", "new_graph", "{'id':'g1'}", sb);
	CHECK(a.count == 2 && b.count == 2);
	clear(&a);
	clear(&b);
	/* The deadline passed: no delivery even before the tick. */
	auth_of(&a)->expired = true;
	command(&b, "runtime", "new_graph", "{'id':'g2'}", sb);
	CHECK(a.count == 0 && b.count == 2);
	clear(&b);
	/* The tick ends it: logout, and no more events. */
	AuthConn *ac = auth_of(&a);
	ac->authenticated = false;
	ac->expired = false;
	ac->events->on_expired(ac->user);
	CHECK(a.count == 1);
	CHECK_STREQ(msg(&a, 0), "{\"protocol\":\"auth\",\"command\":\"logout\"}");
	clear(&a);
	command(&b, "runtime", "new_graph", "{'id':'g3'}", sb);
	CHECK(a.count == 0 && b.count == 2);
	clear(&b);
	/* A new login gets events again. */
	login_as(&a, USER_ROLE_VIEWER);
	command(&b, "runtime", "new_graph", "{'id':'g4'}", sb);
	CHECK(a.count == 2 && b.count == 2);
	conn_close(&a);
	conn_close(&b);
	teardown();
}

/* ---- runtime (02 §3.2) ------------------------------------------------------------- */

static void runtime_json(char *buf, size_t size, const char *caps)
{
	snprintf(buf, size,
		 "{\"protocol\":\"runtime\",\"command\":\"runtime\",\"payload\":{\"id\":"
		 "\"engine-1\",\"label\":\"Test Hakkas\",\"version\":\"%s\",\"allCapabilities\":"
		 "[\"protocol:runtime\",\"protocol:graph\",\"protocol:component\","
		 "\"graph:readonly\",\"network:status\",\"network:persist\",\"network:data\","
		 "\"network:control\",\"component:setsource\"],\"capabilities\":[%s],"
		 "\"type\":\"hakkas\",\"namespace\":"
		 "\"hakkas\",\"repository\":\"\",\"repositoryVersion\":\"\"}}",
		 hakkas_version(), caps);
}

/* The component:component message expected for def (02 §3.5). */
static const char *component_json(const BlockTypeDef *def)
{
	static char buf[4096];
	cJSON *p = graph_json_component(def);
	char *pl = cJSON_PrintUnformatted(p);
	snprintf(buf, sizeof(buf),
		 "{\"protocol\":\"component\",\"command\":\"component\",\"payload\":%s}", pl);
	free(pl);
	cJSON_Delete(p);
	return buf;
}

static void test_getruntime(void)
{
	setup();
	CHECK(graphs_new_graph(&gs, "g1", "One", "first", "cog", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_new_graph(&gs, "g2", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	FakeConn f, other, anon;
	conn_open(&f);
	conn_open(&other);
	conn_open(&anon);
	char want[2048];

	/* Before login: runtime:runtime only, no capabilities. */
	command(&f, "runtime", "getruntime", "{}", "");
	CHECK(f.count == 1);
	runtime_json(want, sizeof(want), "");
	CHECK_STREQ(msg(&f, 0), want);
	clear(&f);

	login_as(&other, USER_ROLE_VIEWER);
	login_as(&f, USER_ROLE_VIEWER);
	command(&f, "runtime", "getruntime", "{}", "");
	size_t types = block_types_count();
	CHECK(types > 10);
	CHECK(f.count == types + 4);
	runtime_json(want, sizeof(want),
		     "\"protocol:component\",\"graph:readonly\",\"network:status\","
		     "\"network:data\"");
	CHECK_STREQ(msg(&f, 0), want);
	for (size_t i = 0; i < types; i++)
		CHECK(json_equal(msg(&f, 1 + i), component_json(block_types_at(i))));
	char ready[128];
	snprintf(ready, sizeof(ready),
		 "{\"protocol\":\"component\",\"command\":\"componentsready\",\"payload\":%zu}",
		 types);
	CHECK_STREQ(msg(&f, types + 1), ready);
	static const char *const status[] = {
		"{'protocol':'network','command':'status','payload':{'graph':'g1','name':'One',"
		"'description':'first','uptime':0,'running':false,'started':false,'debug':false,"
		"'unsaved':true}}",
		"{'protocol':'network','command':'status','payload':{'graph':'g2','name':"
		"'<<new graph>>','description':'','uptime':0,'running':false,'started':false,"
		"'debug':false,'unsaved':true}}",
	};
	CHECK(json_equal(msg(&f, types + 2), status[0]));
	CHECK(json_equal(msg(&f, types + 3), status[1]));
	/* The status broadcast reaches every authenticated connection. */
	CHECK(other.count == 2);
	CHECK(json_equal(msg(&other, 0), status[0]));
	CHECK(json_equal(msg(&other, 1), status[1]));
	CHECK(anon.count == 0);
	clear(&f);
	clear(&other);

	/* Uptime of a started graph, in whole seconds. */
	runtime_set_now(&rt, 1000);
	CHECK(graphs_start(&gs, "g1", NULL) == GRAPH_OK);
	clear(&f);
	clear(&other);
	runtime_set_now(&rt, 63999);
	command(&f, "runtime", "getruntime", "{}", "");
	CHECK(json_equal(msg(&f, types + 2),
			 "{'protocol':'network','command':'status','payload':{'graph':'g1','name':"
			 "'One','description':'first','uptime':62,'running':true,'started':true,"
			 "'debug':false,'unsaved':true}}"));
	conn_close(&f);
	conn_close(&other);
	conn_close(&anon);
	teardown();
}

static bool file_exists(const char *id)
{
	char path[PAL_PATH_MAX];
	CHECK(graph_files_path(&gf, GRAPH_FILES_GRAPHS_DIR, id, path, sizeof(path)) ==
	      GRAPH_FILES_OK);
	return pal_storage_exists(path);
}

static void test_new_delete_graph(void)
{
	setup();
	FakeConn a, b, anon, viewer;
	conn_open(&a);
	conn_open(&b);
	conn_open(&anon);
	conn_open(&viewer);
	const char *sa = login_as(&a, USER_ROLE_EDITOR);
	const char *sb = login_as(&b, USER_ROLE_ADMIN);
	const char *sv = login_as(&viewer, USER_ROLE_VIEWER);

	/* Acknowledged only through the broadcast, sender included. */
	command(&a, "runtime", "new_graph", "{'id':'g1','name':'One','description':'d','icon':'i'}",
		sa);
	static const char *const created =
		"{'protocol':'runtime','command':'new_graph','payload':{'graph':'g1','name':'One',"
		"'description':'d','icon':'i'}}";
	expect_created(&a, created);
	expect_created(&b, created);
	expect_created(&viewer, created);
	CHECK(anon.count == 0);

	/* Defaults; null is the same as absent. */
	command(&a, "runtime", "new_graph", "{'name':null}", sa);
	CHECK(a.count == 2 && b.count == 2);
	cJSON *m = cJSON_Parse(msg(&a, 0));
	const cJSON *pl = cJSON_GetObjectItemCaseSensitive(m, "payload");
	const char *id = cJSON_GetObjectItemCaseSensitive(pl, "graph")->valuestring;
	CHECK(strlen(id) == 36);
	CHECK_STREQ(cJSON_GetObjectItemCaseSensitive(pl, "name")->valuestring, "<<new graph>>");
	CHECK_STREQ(cJSON_GetObjectItemCaseSensitive(pl, "description")->valuestring, "");
	CHECK_STREQ(cJSON_GetObjectItemCaseSensitive(pl, "icon")->valuestring, "");
	CHECK(graphs_find(&gs, id) != NULL);
	cJSON_Delete(m);
	clear(&a);
	clear(&b);
	clear(&viewer);

	/* Refusals reach the sender only. */
	command(&a, "runtime", "new_graph", "{'id':'g1'}", sa);
	expect_one(&a, "{'protocol':'network','command':'error','payload':{'stack':'','message':"
		       "'Graph exists: g1','graph':'g1'}}");
	CHECK(b.count == 0 && viewer.count == 0);
	command(&a, "runtime", "new_graph", "{'id':''}", sa);
	expect_error(&a, "Invalid id"); /* no valid id: no "graph" */
	command(&a, "runtime", "new_graph", "{'id':'g9','name':5}", sa);
	expect_one(&a, "{'protocol':'network','command':'error','payload':{'stack':'','message':"
		       "'Invalid field: name','graph':'g9'}}");
	command(&a, "runtime", "new_graph", "{'id':[]}", sa);
	expect_error(&a, "Invalid field: id");
	command(&viewer, "runtime", "new_graph", "{}", sv);
	expect_error(&viewer, "Not permitted: runtime:new_graph");
	CHECK(b.count == 0 && graphs_count(&gs) == 2);

	/* delete_graph: id and name required, the name must match. */
	command(&a, "runtime", "delete_graph", "{'id':'g1'}", sa);
	expect_one(&a, "{'protocol':'network','command':'error','payload':{'stack':'','message':"
		       "'Invalid field: name','graph':'g1'}}");
	command(&a, "runtime", "delete_graph", "{'name':'One'}", sa);
	expect_error(&a, "Invalid field: id");
	command(&a, "runtime", "delete_graph", "{'id':'g1','name':'Two'}", sa);
	expect_one(&a, "{'protocol':'network','command':'error','payload':{'stack':'','message':"
		       "'Graph name mismatch: g1','graph':'g1'}}");
	command(&a, "runtime", "delete_graph", "{'id':'zz','name':'Two'}", sa);
	expect_one(&a, "{'protocol':'network','command':'error','payload':{'stack':'','message':"
		       "'Unknown graph: zz','graph':'zz'}}");
	CHECK(b.count == 0 && viewer.count == 0);

	/* A started, persisted graph: network:stopped, then delete_graph, and
	 * its file is gone. */
	runtime_set_now(&rt, 2000);
	CHECK(graphs_start(&gs, "g1", NULL) == GRAPH_OK);
	static const char *const started =
		"{'protocol':'network','command':'started','payload':{'graph':'g1','time':"
		"'1970-01-01T00:00:02Z','started':true,'running':true,'debug':false}}";
	expect_one(&a, started);
	expect_one(&b, started);
	clear(&viewer);
	CHECK(graph_files_save(&gf, graphs_find(&gs, "g1")) == GRAPH_FILES_OK);
	CHECK(file_exists("g1"));
	runtime_set_now(&rt, 9500);
	command(&b, "runtime", "delete_graph", "{'id':'g1','name':'One'}", sb);
	static const char *const stopped =
		"{'protocol':'network','command':'stopped','payload':{'graph':'g1','time':"
		"'1970-01-01T00:00:02Z','uptime':7,'started':false,'running':false,'debug':false}}";
	static const char *const deleted =
		"{'protocol':'runtime','command':'delete_graph','payload':{'graph':'g1',"
		"'name':'One'}}";
	FakeConn *subs[] = {&a, &b, &viewer};
	for (size_t i = 0; i < 3; i++) {
		CHECK(subs[i]->count == 2);
		CHECK(json_equal(msg(subs[i], 0), stopped));
		CHECK(json_equal(msg(subs[i], 1), deleted));
		clear(subs[i]);
	}
	CHECK(anon.count == 0);
	CHECK(graphs_find(&gs, "g1") == NULL && !file_exists("g1"));

	/* A connection scoped to another graph still gets system-wide events
	 * (02 §4.1), but not that graph's network:started. */
	CHECK(protocol_conn_scope(viewer.pc, "elsewhere"));
	CHECK(!protocol_conn_scope(viewer.pc, ""));
	command(&a, "runtime", "new_graph", "{'id':'g5'}", sa);
	CHECK(viewer.count == 2); /* new_graph and its status, both system-wide */
	clear(&viewer);
	CHECK(graphs_start(&gs, "g5", NULL) == GRAPH_OK);
	CHECK(viewer.count == 0 && a.count == 3);
	clear(&a);
	clear(&b);

	conn_close(&a);
	conn_close(&b);
	conn_close(&anon);
	conn_close(&viewer);
	teardown();
}

/* ---- component (02 §3.5), trace (02 §3.6) ------------------------------------------ */

static void test_component(void)
{
	setup();
	FakeConn f, other;
	conn_open(&f);
	conn_open(&other);
	login_as(&other, USER_ROLE_VIEWER);
	const char *s = login_as(&f, USER_ROLE_VIEWER);
	command(&f, "component", "list", "{}", s);
	size_t types = block_types_count();
	CHECK(f.count == types + 1);
	for (size_t i = 0; i < types; i++)
		CHECK(json_equal(msg(&f, i), component_json(block_types_at(i))));
	char ready[128];
	snprintf(ready, sizeof(ready),
		 "{\"protocol\":\"component\",\"command\":\"componentsready\",\"payload\":%zu}",
		 types);
	CHECK_STREQ(msg(&f, types), ready);
	CHECK(other.count == 0); /* a reply, not a broadcast */
	clear(&f);

	command(&f, "component", "getsource", "{'name':'math/Add2'}", s);
	CHECK(f.count == 1);
	CHECK_STREQ(msg(&f, 0), "{\"protocol\":\"component\",\"command\":\"source\",\"payload\":"
				"{\"name\":\"math/Add2\",\"language\":\"json\",\"library\":"
				"\"hakkas\",\"code\":\"\",\"tests\":\"\"}}");
	clear(&f);
	command(&f, "component", "getsource", "{'name':'nope/Nope'}", s);
	expect_error(&f, "Unknown component: nope/Nope");
	command(&f, "component", "getsource", "{}", s);
	expect_error(&f, "Invalid field: name");
	command(&f, "component", "other", "{}", s);
	expect_error(&f, "Unknown command: component:other");
	conn_close(&f);
	conn_close(&other);
	teardown();
}

static void test_trace(void)
{
	setup();
	FakeConn f;
	conn_open(&f);
	const char *s = login_as(&f, USER_ROLE_VIEWER);
	command(&f, "trace", "start", "{'graph':'g','buffersize':10}", s);
	expect_one(&f, "{'protocol':'trace','command':'start','payload':{'graph':'g'}}");
	command(&f, "trace", "stop", "{'graph':'g'}", s);
	expect_one(&f, "{'protocol':'trace','command':'stop','payload':{'graph':'g'}}");
	command(&f, "trace", "clear", "{'graph':'g'}", s);
	expect_one(&f, "{'protocol':'trace','command':'clear','payload':{'graph':'g'}}");
	command(&f, "trace", "dump", "{'graph':'g'}", s);
	CHECK(f.count == 1);
	CHECK_STREQ(msg(&f, 0), "{\"protocol\":\"trace\",\"command\":\"dump\",\"payload\":"
				"{\"graph\":\"g\",\"type\":\"flowtrace.json\","
				"\"flowtrace\":\"\"}}");
	clear(&f);
	command(&f, "trace", "start", "{}", s);
	expect_error(&f, "Invalid field: graph");
	command(&f, "trace", "foo", "{'graph':'g'}", s);
	expect_error(&f, "Unknown command: trace:foo");
	conn_close(&f);
	teardown();
}

/* ---- Bursts (02 §0 "Outgoing buffer") ---------------------------------------------- */

static void test_bursts(void)
{
	setup();
	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	FakeConn f;
	conn_open(&f);
	const char *s = login_as(&f, USER_ROLE_VIEWER);
	size_t types = block_types_count();
	/* Does not fit now, the buffer holds data: deferred to drained. */
	f.space = 2000;
	f.pending = 100;
	command(&f, "component", "list", "{}", s);
	CHECK(f.count == 0 && !f.closed);
	command(&f, "runtime", "getruntime", "{}", s);
	CHECK(f.count == 0 && !f.closed);
	/* Another reply meanwhile goes out at once. */
	command(&f, "trace", "stop", "{'graph':'g'}", s);
	CHECK(f.count == 1);
	clear(&f);
	f.space = 1u << 20;
	f.pending = 0;
	protocol_conn_drained(f.pc);
	CHECK(f.count == (types + 3) + (types + 1));
	CHECK(strstr(msg(&f, 0), "\"command\":\"runtime\"") != NULL);
	CHECK(strstr(msg(&f, types + 1), "componentsready") != NULL);
	CHECK(strstr(msg(&f, types + 2), "\"command\":\"status\"") != NULL);
	CHECK(strstr(msg(&f, types + 3), "\"command\":\"component\"") != NULL);
	clear(&f);
	protocol_conn_drained(f.pc); /* nothing pending */
	CHECK(f.count == 0);
	/* Does not fit an empty buffer: "Reply too large". */
	f.space = 2000;
	command(&f, "component", "list", "{}", s);
	expect_error(&f, "Reply too large");
	CHECK(!f.closed);
	/* A broadcast that does not fit closes the connection (02 §0). */
	f.space = 20;
	CHECK(graphs_new_graph(&gs, "g2", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(f.closed && f.count == 0);
	conn_close(&f);
	teardown();
}

/* ---- Log mirroring (05 §4) ---------------------------------------------------------- */

static void test_mirror(void)
{
	enum log_level was = log_threshold();
	log_set_threshold(LOG_LEVEL_FINE);
	setup_mirror(true);
	FakeConn f, anon;
	conn_open(&f);
	conn_open(&anon);
	login_as(&f, USER_ROLE_VIEWER);
	log_info("hello %d", 1);
	CHECK(f.count == 1);
	cJSON *m = cJSON_Parse(msg(&f, 0));
	CHECK_STREQ(cJSON_GetObjectItemCaseSensitive(m, "protocol")->valuestring, "network");
	CHECK_STREQ(cJSON_GetObjectItemCaseSensitive(m, "command")->valuestring, "output");
	const cJSON *pl = cJSON_GetObjectItemCaseSensitive(m, "payload");
	CHECK_STREQ(cJSON_GetObjectItemCaseSensitive(pl, "type")->valuestring, "message");
	const char *text = cJSON_GetObjectItemCaseSensitive(pl, "message")->valuestring;
	CHECK(strncmp(text, "info test_dispatcher.c:", 23) == 0 && strstr(text, ": hello 1"));
	CHECK(cJSON_GetArraySize(pl) == 2);
	cJSON_Delete(m);
	clear(&f);
	log_fine("fine");
	log_warn("careful");
	CHECK(f.count == 2 && strstr(msg(&f, 1), "\"command\":\"output\""));
	clear(&f);
	log_error("broken");
	CHECK(f.count == 1);
	m = cJSON_Parse(msg(&f, 0));
	CHECK_STREQ(cJSON_GetObjectItemCaseSensitive(m, "command")->valuestring, "error");
	pl = cJSON_GetObjectItemCaseSensitive(m, "payload");
	CHECK_STREQ(cJSON_GetObjectItemCaseSensitive(pl, "stack")->valuestring, "");
	CHECK(strstr(cJSON_GetObjectItemCaseSensitive(pl, "message")->valuestring, ": broken"));
	CHECK(cJSON_GetObjectItemCaseSensitive(pl, "graph") == NULL);
	cJSON_Delete(m);
	clear(&f);
	CHECK(anon.count == 0); /* only authenticated connections */

	/* A line logged while sending is dropped: no recursion. */
	f.log_on_send = true;
	log_warn("outer");
	CHECK(f.count == 1);
	clear(&f);
	command(&f, "trace", "stop", "{'graph':'g'}", auth_of(&f)->secret);
	CHECK(f.count == 1 && strstr(msg(&f, 0), "\"trace\""));
	clear(&f);
	f.log_on_send = false;

	/* A line that does not fit is dropped; the connection stays open. */
	f.space = 30;
	log_warn("too long for the buffer");
	CHECK(f.count == 0 && !f.closed);
	/* So is one that would take the reserve (plan task 20 fix). */
	f.capacity = 4000;
	f.space = 4000 / PROTO_DATA_RESERVE_DIV + 20;
	log_warn("fits, but only into the reserve");
	CHECK(f.count == 0 && !f.closed);
	f.space = f.capacity = 1u << 20;
	conn_close(&f);
	conn_close(&anon);
	teardown();

	/* Off: nothing mirrored. */
	setup_mirror(false);
	conn_open(&f);
	login_as(&f, USER_ROLE_VIEWER);
	log_warn("not mirrored");
	CHECK(f.count == 0);
	conn_close(&f);
	teardown();
	log_set_threshold(was);
}

/* ---- The real authorizer (02 §2.3) ------------------------------------------------- */

static uint64_t clock_ms = 5000;

static uint64_t test_clock(void *ctx)
{
	(void)ctx;
	return clock_ms;
}

static uint8_t rnd;

static enum pal_err test_random(void *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		((uint8_t *)buf)[i] = rnd++;
	return PAL_OK;
}

static const char *payload_str(const char *message, const char *key)
{
	static char buf[4][256];
	static int next;
	char *p = buf[next++ % 4];
	p[0] = '\0';
	cJSON *m = cJSON_Parse(message);
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(
		cJSON_GetObjectItemCaseSensitive(m, "payload"), key);
	if (cJSON_IsString(v))
		snprintf(p, 256, "%s", v->valuestring);
	else if (cJSON_IsNumber(v))
		snprintf(p, 256, "%.0f", v->valuedouble);
	cJSON_Delete(m);
	return p;
}

/* The client side of 02 §2.3: a full challenge and login of user on f;
 * returns the secret ("" on failure). */
static const char *client_login(FakeConn *f, const char *user, const char *password)
{
	char ch[128];
	snprintf(ch, sizeof(ch), "{'user':'%s','cnonce':'MDEyMzQ1Njc4OWFiY2RlZg=='}", user);
	command(f, "auth", "challenge", ch, "");
	CHECK(f->count == 1);
	char salt64[64], nonce[256];
	snprintf(salt64, sizeof(salt64), "%s", payload_str(msg(f, 0), "salt"));
	snprintf(nonce, sizeof(nonce), "%s", payload_str(msg(f, 0), "nonce"));
	unsigned iterations = (unsigned)strtoul(payload_str(msg(f, 0), "iterations"), NULL, 10);
	CHECK(strncmp(nonce, "MDEyMzQ1Njc4OWFiY2RlZg==", 24) == 0 && strlen(nonce) == 48);
	clear(f);

	uint8_t salt[16], salted[32], ckey[32], stored[32], skey[32], csig[32], proof[32], ssig[32];
	size_t n = 0;
	CHECK(crypto_base64_decode(salt64, strlen(salt64), salt, sizeof(salt), &n) && n == 16);
	CHECK(crypto_pbkdf2_hmac_sha256(password, strlen(password), salt, 16, iterations, salted,
					32));
	CHECK(crypto_hmac_sha256(salted, 32, "Client Key", 10, ckey));
	CHECK(crypto_sha256(ckey, 32, stored));
	CHECK(crypto_hmac_sha256(salted, 32, "Server Key", 10, skey));
	char am[512];
	snprintf(am, sizeof(am), "%s,%s,%s,%u", user, nonce, salt64, iterations);
	CHECK(crypto_hmac_sha256(stored, 32, am, strlen(am), csig));
	for (int i = 0; i < 32; i++)
		proof[i] = ckey[i] ^ csig[i];
	CHECK(crypto_hmac_sha256(skey, 32, am, strlen(am), ssig));
	char proof64[64], ssig64[64], payload[512];
	crypto_base64_encode(proof, 32, proof64, sizeof(proof64));
	crypto_base64_encode(ssig, 32, ssig64, sizeof(ssig64));
	snprintf(payload, sizeof(payload), "{'user':'%s','nonce':'%s','proof':'%s'}", user, nonce,
		 proof64);
	command(f, "auth", "login", payload, "");
	CHECK(f->count == 1);
	static char secret[64];
	snprintf(secret, sizeof(secret), "%s", payload_str(msg(f, 0), "secret"));
	if (secret[0] != '\0') {
		CHECK_STREQ(payload_str(msg(f, 0), "server_signature"), ssig64);
		CHECK_STREQ(payload_str(msg(f, 0), "role"), "editor");
		CHECK(strstr(msg(f, 0), "\"capabilities\":[\"protocol:runtime\",\"protocol:graph\","
					"\"protocol:component\",\"graph:readonly\","
					"\"network:status\",\"network:persist\",\"network:data\","
					"\"network:control\",\"component:setsource\"]") != NULL);
		CHECK(strlen(secret) == AUTH_SECRET_LEN);
	}
	clear(f);
	return secret;
}

static void test_real_authorizer(void)
{
	static UserStore users;
	users_init(&users);
	CHECK(users_add(&users, "alice", USER_ROLE_EDITOR, "pencil", 6, 64, test_random) ==
	      USERS_OK);
	AuthConfig ac = {
		.users = &users,
		.server_secret = "dGVzdCBzZXJ2ZXIgc2VjcmV0IG9ubHkhIQ==",
		.idle_timeout_s = 10,
		.max_lifetime_s = 100,
		.clock = test_clock,
		.random_bytes = test_random,
	};
	Auth *auth = NULL;
	CHECK(auth_create(&ac, &auth));
	setup_with(auth_authorizer(auth), false);

	FakeConn f;
	conn_open(&f);
	char secret[64];
	snprintf(secret, sizeof(secret), "%s", client_login(&f, "alice", "pencil"));
	CHECK(secret[0] != '\0');
	command(&f, "component", "getsource", "{'name':'math/Add2'}", secret);
	CHECK(f.count == 1 && strstr(msg(&f, 0), "\"command\":\"source\""));
	clear(&f);
	command(&f, "component", "getsource", "{'name':'math/Add2'}", "AAAAAAAAAAAAAAAAAAAAAA");
	expect_error(&f, "Not authenticated");
	/* Messages keep the session alive (activity), then the idle timeout
	 * ends it: logout and no more delivery. */
	clock_ms += 8000;
	command(&f, "trace", "stop", "{'graph':'g'}", secret);
	clear(&f);
	clock_ms += 8000;
	auth_tick(auth, clock_ms);
	CHECK(f.count == 0);
	clock_ms += 3000;
	auth_tick(auth, clock_ms);
	CHECK(f.count == 1);
	CHECK_STREQ(msg(&f, 0), "{\"protocol\":\"auth\",\"command\":\"logout\"}");
	clear(&f);
	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(f.count == 0);
	command(&f, "trace", "stop", "{'graph':'g'}", secret);
	expect_error(&f, "Not authenticated");

	/* Three failed logins close the connection with 1008. */
	FakeConn g;
	conn_open(&g);
	for (int i = 0; i < 3; i++) {
		CHECK(client_login(&g, "alice", "wrong")[0] == '\0');
		CHECK(g.closed == (i == 2));
	}
	CHECK(g.close_code == WS_CLOSE_POLICY);
	conn_close(&g);
	conn_close(&f);
	teardown();
	auth_destroy(auth);
}

/* ---- graph:* and network:* (02 §3.3-§3.4, §4; plan task 18) ----------------------- */

/* A test type with an input of each port type (02 §3.4 "Initial value
 * encoding"). */
static int64_t types_compute(BlockCtx *ctx)
{
	(void)ctx;
	return -1;
}

static const PortDescriptor types_inputs[] = {
	{.name = "b", .type = "bool", .default_value = LINKABLE_BOOL_INIT(false)},
	{.name = "s", .type = "string", .default_value = LINKABLE_EMPTY_STRING_INIT},
	{.name = "n", .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
};

static const PortDescriptor types_outputs[] = {
	{.name = "out", .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
};

static const BlockTypeDef types_def = {
	.name = "test/Types",
	.inputs = types_inputs,
	.input_count = 3,
	.outputs = types_outputs,
	.output_count = 1,
	.compute = types_compute,
	.change_threshold = NAN,
};

/* Expect exactly the n messages expected (JSON with ' for "), in order, on
 * each of the nc connections, then clear them: the multi-client rule (05 §5,
 * 02 §1.1): the same event sequence everywhere. */
static void expect_all(FakeConn *const *conns, size_t nc, const char *const *expected, size_t n)
{
	for (size_t c = 0; c < nc; c++) {
		CHECK(conns[c]->count == n);
		for (size_t i = 0; i < n; i++)
			CHECK(json_equal(msg(conns[c], i), expected[i]));
		clear(conns[c]);
	}
}

/* The error message with "graph" (JSON with ' for "). */
static const char *graph_error_json(const char *message, const char *graph)
{
	static char buf[4][1024];
	static int next;
	char *p = buf[next++ % 4];
	char *m = proto_msg_error(message, graph);
	snprintf(p, 1024, "%s", m);
	free(m);
	return p;
}

/* Two editors (a, b) and a viewer (v), all logged in and connected to g1
 * ("One"), and an anonymous connection. */
typedef struct {
	FakeConn a, b, v, anon;
	const char *sa, *sb, *sv;
} Clients;

static void clients_open(Clients *k, const char *graph)
{
	conn_open(&k->a);
	conn_open(&k->b);
	conn_open(&k->v);
	conn_open(&k->anon);
	k->sa = login_as(&k->a, USER_ROLE_EDITOR);
	k->sb = login_as(&k->b, USER_ROLE_EDITOR);
	k->sv = login_as(&k->v, USER_ROLE_VIEWER);
	if (graph != NULL) {
		char payload[128];
		snprintf(payload, sizeof(payload), "{'id':'%s'}", graph);
		command(&k->a, "graph", "connect", payload, k->sa);
		command(&k->b, "graph", "connect", payload, k->sb);
		command(&k->v, "graph", "connect", payload, k->sv);
	}
	clear(&k->a);
	clear(&k->b);
	clear(&k->v);
}

static void clients_close(Clients *k)
{
	CHECK(k->anon.count == 0); /* nothing before login (02 §2.5 item 6) */
	conn_close(&k->a);
	conn_close(&k->b);
	conn_close(&k->v);
	conn_close(&k->anon);
}

/* A graph command from a, its events on a, b and v. */
static void edit(Clients *k, const char *cmd, const char *payload, const char *const *expected,
		 size_t n)
{
	command(&k->a, "graph", cmd, payload, k->sa);
	FakeConn *all[] = {&k->a, &k->b, &k->v};
	expect_all(all, 3, expected, n);
}

static void edit1(Clients *k, const char *cmd, const char *payload, const char *expected)
{
	edit(k, cmd, payload, &expected, 1);
}

static void test_graph_commands(void)
{
	setup();
	CHECK(graphs_new_graph(&gs, "g1", "One", NULL, NULL, NULL, NULL) == GRAPH_OK);
	Clients k;
	clients_open(&k, "g1");

	/* addnode: defaulted x/y, payload keys in 02 order (byte for byte). */
	command(&k.a, "graph", "addnode", "{'id':'n1','graph':'g1','component':'math/Add2'}",
		k.sa);
	static const char addnode_n1[] =
		"{\"protocol\":\"graph\",\"command\":\"addnode\",\"payload\":{\"graph\":\"g1\","
		"\"component\":\"math/Add2\",\"id\":\"n1\",\"metadata\":{\"x\":50,\"y\":50}}}";
	FakeConn *all[] = {&k.a, &k.b, &k.v};
	for (int i = 0; i < 3; i++) {
		CHECK(all[i]->count == 1);
		CHECK_STREQ(msg(all[i], 0), addnode_n1);
		clear(all[i]);
	}
	edit1(&k, "addnode",
	      "{'id':'n2','graph':'g1','component':'math/Add2','metadata':{'x':10.5,'y':-3,"
	      "'change_threshold':0.25,'label':'L'}}",
	      "{'protocol':'graph','command':'addnode','payload':{'graph':'g1','component':"
	      "'math/Add2','id':'n2','metadata':{'x':10.5,'y':-3,'change_threshold':0.25,"
	      "'label':'L'}}}");
	edit1(&k, "addnode",
	      "{'id':'t1','graph':'g1','component':'test/Types','metadata':{'y':7}}",
	      "{'protocol':'graph','command':'addnode','payload':{'graph':'g1','component':"
	      "'test/Types','id':'t1','metadata':{'x':50,'y':7}}}");
	edit1(&k, "addnode", "{'id':'n4','graph':'g1','component':'math/Add2','metadata':null}",
	      "{'protocol':'graph','command':'addnode','payload':{'graph':'g1','component':"
	      "'math/Add2','id':'n4','metadata':{'x':50,'y':50}}}");
	edit1(&k, "renamenode", "{'from':'n2','to':'n3','graph':'g1'}",
	      "{'protocol':'graph','command':'renamenode','payload':{'graph':'g1','from':'n2',"
	      "'to':'n3'}}");
	/* changenode: the full merged metadata. */
	edit1(&k, "changenode",
	      "{'id':'n3','metadata':{'x':1,'change_threshold':null,'label':null,'extra':[1,2]},"
	      "'graph':'g1'}",
	      "{'protocol':'graph','command':'changenode','payload':{'id':'n3','graph':'g1',"
	      "'metadata':{'x':1,'y':-3,'extra':[1,2]}}}");
	edit1(&k, "changenode", "{'id':'n3','metadata':{'y':null},'graph':'g1'}",
	      "{'protocol':'graph','command':'changenode','payload':{'id':'n3','graph':'g1',"
	      "'metadata':{'x':1,'y':50,'extra':[1,2]}}}");
	/* change_threshold is an ordinary opaque key (plan task 21c): any value
	 * is accepted, kept at the end, and has no effect on the block. */
	edit1(&k, "changenode", "{'id':'n3','metadata':{'change_threshold':-1},'graph':'g1'}",
	      "{'protocol':'graph','command':'changenode','payload':{'id':'n3','graph':'g1',"
	      "'metadata':{'x':1,'y':50,'extra':[1,2],'change_threshold':-1}}}");
	CHECK(block_change_threshold(graph_find_block(graphs_find(&gs, "g1"), "n3")) ==
	      RUNTIME_DEFAULT_CHANGE_THRESHOLD);
	edit1(&k, "changenode", "{'id':'n3','metadata':{'change_threshold':null},'graph':'g1'}",
	      "{'protocol':'graph','command':'changenode','payload':{'id':'n3','graph':'g1',"
	      "'metadata':{'x':1,'y':50,'extra':[1,2]}}}");
	/* Edges and their metadata. */
	edit1(&k, "addedge",
	      "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n3','port':'in1'},'graph':'g1',"
	      "'metadata':{'route':3}}",
	      "{'protocol':'graph','command':'addedge','payload':{'graph':'g1','src':{'node':"
	      "'n1','port':'out'},'tgt':{'node':'n3','port':'in1'},'metadata':{'route':3}}}");
	edit1(&k, "addedge",
	      "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n4','port':'in2'},'graph':'g1'}",
	      "{'protocol':'graph','command':'addedge','payload':{'graph':'g1','src':{'node':"
	      "'n1','port':'out'},'tgt':{'node':'n4','port':'in2'}}}");
	edit1(&k, "changeedge",
	      "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n3','port':'in1'},'metadata':"
	      "{'route':5,'secure':true},'graph':'g1'}",
	      "{'protocol':'graph','command':'changeedge','payload':{'graph':'g1','src':{'node':"
	      "'n1','port':'out'},'tgt':{'node':'n3','port':'in1'},'metadata':{'route':5,"
	      "'secure':true}}}");
	edit1(&k, "changeedge",
	      "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n3','port':'in1'},'metadata':"
	      "null,'graph':'g1'}",
	      "{'protocol':'graph','command':'changeedge','payload':{'graph':'g1','src':{'node':"
	      "'n1','port':'out'},'tgt':{'node':'n3','port':'in1'},'metadata':{}}}");
	edit1(&k, "removeedge",
	      "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n4','port':'in2'},'graph':'g1'}",
	      "{'protocol':'graph','command':'removeedge','payload':{'graph':'g1','src':{'node':"
	      "'n1','port':'out'},'tgt':{'node':'n4','port':'in2'}}}");
	/* Initials: data as a string; a replaced literal is a new addinitial. */
	edit1(&k, "addinitial", "{'tgt':{'node':'n1','port':'in1'},'src':{'data':5},'graph':'g1'}",
	      "{'protocol':'graph','command':'addinitial','payload':{'graph':'g1','src':{'data':"
	      "'5'},'tgt':{'node':'n1','port':'in1'}}}");
	edit1(&k, "addinitial",
	      "{'tgt':{'node':'n1','port':'in1'},'src':{'data':'2.5'},'graph':'g1'}",
	      "{'protocol':'graph','command':'addinitial','payload':{'graph':'g1','src':{'data':"
	      "'2.5'},'tgt':{'node':'n1','port':'in1'}}}");
	command(&k.a, "graph", "removeinitial", "{'tgt':{'node':'n1','port':'in1'},'graph':'g1'}",
		k.sa);
	static const char removeinitial[] =
		"{\"protocol\":\"graph\",\"command\":\"removeinitial\",\"payload\":{\"src\":{"
		"\"data\":\"2.5\"},\"tgt\":{\"node\":\"n1\",\"port\":\"in1\"},\"graph\":\"g1\"}}";
	for (int i = 0; i < 3; i++) {
		CHECK(all[i]->count == 1);
		CHECK_STREQ(msg(all[i], 0), removeinitial);
		clear(all[i]);
	}
	/* Graph ports: the request's "public" is the broadcast's "name". */
	command(&k.a, "graph", "addinport", "{'graph':'g1','node':'n1','port':'in2','public':'IN'}",
		k.sa);
	static const char addinport[] =
		"{\"protocol\":\"graph\",\"command\":\"addinport\",\"payload\":{\"graph\":\"g1\","
		"\"name\":\"IN\",\"node\":\"n1\",\"port\":\"in2\"}}";
	for (int i = 0; i < 3; i++) {
		CHECK(all[i]->count == 1);
		CHECK_STREQ(msg(all[i], 0), addinport);
		clear(all[i]);
	}
	edit1(&k, "addoutport", "{'graph':'g1','node':'n3','port':'out','public':'OUT'}",
	      "{'protocol':'graph','command':'addoutport','payload':{'graph':'g1','name':'OUT',"
	      "'node':'n3','port':'out'}}");
	edit1(&k, "renameinport", "{'graph':'g1','from':'IN','to':'IN2'}",
	      "{'protocol':'graph','command':'renameinport','payload':{'graph':'g1','from':'IN',"
	      "'to':'IN2'}}");
	edit1(&k, "renameoutport", "{'graph':'g1','from':'OUT','to':'OUT2'}",
	      "{'protocol':'graph','command':'renameoutport','payload':{'graph':'g1','from':"
	      "'OUT','to':'OUT2'}}");
	edit1(&k, "removeinport", "{'graph':'g1','public':'IN2'}",
	      "{'protocol':'graph','command':'removeinport','payload':{'graph':'g1','name':"
	      "'IN2'}}");
	edit1(&k, "removeoutport", "{'graph':'g1','public':'OUT2'}",
	      "{'protocol':'graph','command':'removeoutport','payload':{'graph':'g1','name':"
	      "'OUT2'}}");
	/* rename: the event's from is the actual name, not the request's. */
	edit1(&k, "rename", "{'id':'g1','from':'whatever','to':'Uno'}",
	      "{'protocol':'graph','command':'rename','payload':{'id':'g1','from':'One','to':"
	      "'Uno'}}");
	CHECK_STREQ(graphs_find(&gs, "g1")->name, "Uno");

	/* removenode: a compound effect, its parts in applied order (02 §1.1,
	 * §3.4): the initials and edges into it (port order), the edges out of
	 * it, its inports, its outports, then the node. */
	command(&k.a, "graph", "addinitial",
		"{'tgt':{'node':'n1','port':'in1'},'src':{'data':1},'graph':'g1'}", k.sa);
	command(&k.a, "graph", "addedge",
		"{'src':{'node':'n3','port':'out'},'tgt':{'node':'n1','port':'in2'},'graph':'g1'}",
		k.sa);
	command(&k.a, "graph", "addinport", "{'graph':'g1','node':'n1','port':'in2','public':'P'}",
		k.sa);
	command(&k.a, "graph", "addoutport", "{'graph':'g1','node':'n1','port':'out','public':'Q'}",
		k.sa);
	CHECK(k.a.count == 4 && k.b.count == 4);
	clear(&k.a);
	clear(&k.b);
	clear(&k.v);
	static const char *const removed[] = {
		"{'protocol':'graph','command':'removeinitial','payload':{'src':{'data':'1'},'tgt':"
		"{'node':'n1','port':'in1'},'graph':'g1'}}",
		"{'protocol':'graph','command':'removeedge','payload':{'graph':'g1','src':{'node':"
		"'n3','port':'out'},'tgt':{'node':'n1','port':'in2'}}}",
		"{'protocol':'graph','command':'removeedge','payload':{'graph':'g1','src':{'node':"
		"'n1','port':'out'},'tgt':{'node':'n3','port':'in1'}}}",
		"{'protocol':'graph','command':'removeinport','payload':{'graph':'g1','name':'P'}}",
		"{'protocol':'graph','command':'removeoutport','payload':{'graph':'g1','name':"
		"'Q'}}",
		"{'protocol':'graph','command':'removenode','payload':{'id':'n1','graph':'g1'}}",
	};
	edit(&k, "removenode", "{'id':'n1','graph':'g1'}", removed, 6);
	CHECK(graph_find_block(graphs_find(&gs, "g1"), "n1") == NULL);

	/* graph:list: a network:status per graph, to every authenticated
	 * connection. */
	CHECK(graphs_new_graph(&gs, "g2", "Two", NULL, NULL, NULL, NULL) == GRAPH_OK);
	clear(&k.a);
	clear(&k.b);
	clear(&k.v);
	command(&k.v, "graph", "list", "{}", k.sv);
	static const char *const list[] = {
		"{'protocol':'network','command':'status','payload':{'graph':'g1','name':'Uno',"
		"'description':'','uptime':0,'running':false,'started':false,'debug':false,'unsaved':true}}",
		"{'protocol':'network','command':'status','payload':{'graph':'g2','name':'Two',"
		"'description':'','uptime':0,'running':false,'started':false,'debug':false,'unsaved':true}}",
	};
	expect_all(all, 3, list, 2);

	/* A viewer may not edit. */
	command(&k.v, "graph", "addnode", "{'id':'x','graph':'g1','component':'math/Add2'}", k.sv);
	expect_error(&k.v, "Not permitted: graph:addnode");
	CHECK(k.a.count == 0 && k.b.count == 0);
	clients_close(&k);
	teardown();
}

/* A command from a that is refused with message (and "graph" if not NULL):
 * the error on a only, nothing on b (02 §1.1, §3.4.1). */
static void refused_cmd(Clients *k, const char *protocol, const char *cmd, const char *payload,
			const char *message, const char *graph)
{
	command(&k->a, protocol, cmd, payload, k->sa);
	CHECK(k->a.count == 1);
	if (!json_equal(msg(&k->a, 0), graph_error_json(message, graph)))
		fprintf(stderr, "  for %s:%s %s\n", protocol, cmd, jq(payload));
	clear(&k->a);
	CHECK(k->b.count == 0 && k->v.count == 0);
}

static void test_graph_refusals(void)
{
	setup();
	CHECK(graphs_new_graph(&gs, "g1", "One", NULL, NULL, NULL, NULL) == GRAPH_OK);
	Graph *g = graphs_find(&gs, "g1");
	CHECK(graphs_add_node(&gs, "g1", "n1", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "n2", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "n1", "out", "n2", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_INPORT, "P", "n1", "in1", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_OUTPORT, "Q", "n1", "out", NULL) == GRAPH_OK);
	Clients k;
	clients_open(&k, "g1");
	char long_id[80];
	memset(long_id, 'x', 65);
	long_id[65] = '\0';
	char payload[256];

	/* Every case of 02 §3.4.1 that a graph command can hit. */
	refused_cmd(&k, "graph", "addnode", "{'id':'x','graph':'zz','component':'math/Add2'}",
		    "Unknown graph: zz", "zz");
	refused_cmd(&k, "graph", "addnode", "{'id':'','graph':'g1','component':'math/Add2'}",
		    "Invalid id", "g1");
	snprintf(payload, sizeof(payload), "{'id':'x','graph':'%s','component':'math/Add2'}",
		 long_id);
	refused_cmd(&k, "graph", "addnode", payload, "Invalid id", NULL);
	refused_cmd(&k, "graph", "addnode", "{'id':'n1','graph':'g1','component':'math/Add2'}",
		    "Node exists: n1", "g1");
	refused_cmd(&k, "graph", "addnode", "{'id':'n9','graph':'g1','component':'nope/X'}",
		    "Unknown component: nope/X", "g1");
	refused_cmd(&k, "graph", "removenode", "{'id':'zz','graph':'g1'}", "Unknown node: zz",
		    "g1");
	refused_cmd(&k, "graph", "renamenode", "{'from':'n1','to':'n2','graph':'g1'}",
		    "Node exists: n2", "g1");
	refused_cmd(&k, "graph", "renamenode", "{'from':'n1','to':'n1','graph':'g1'}",
		    "Node exists: n1", "g1");
	refused_cmd(&k, "graph", "changenode", "{'id':'zz','metadata':{'x':1},'graph':'g1'}",
		    "Unknown node: zz", "g1");
	refused_cmd(&k, "graph", "changenode", "{'id':'n1','metadata':{'x':1},'graph':'zz'}",
		    "Unknown graph: zz", "zz");
	refused_cmd(&k, "graph", "addedge",
		    "{'src':{'node':'n1','port':'nope'},'tgt':{'node':'n2','port':'in2'},"
		    "'graph':'g1'}",
		    "Unknown port: n1.nope", "g1");
	refused_cmd(&k, "graph", "addedge",
		    "{'src':{'node':'zz','port':'out'},'tgt':{'node':'n2','port':'in2'},"
		    "'graph':'g1'}",
		    "Unknown node: zz", "g1");
	refused_cmd(&k, "graph", "addedge",
		    "{'src':{'node':'n1','port':'in1'},'tgt':{'node':'n2','port':'in2'},"
		    "'graph':'g1'}",
		    "Invalid edge direction", "g1");
	refused_cmd(&k, "graph", "addedge",
		    "{'src':{'node':'n2','port':'out'},'tgt':{'node':'n2','port':'in1'},"
		    "'graph':'g1'}",
		    "Input already connected: n2.in1", "g1");
	refused_cmd(&k, "graph", "addinitial",
		    "{'tgt':{'node':'n2','port':'in1'},'src':{'data':1},'graph':'g1'}",
		    "Input already connected: n2.in1", "g1");
	refused_cmd(&k, "graph", "addinitial",
		    "{'tgt':{'node':'n2','port':'out'},'src':{'data':1},'graph':'g1'}",
		    "Invalid edge direction", "g1");
	refused_cmd(&k, "graph", "addinitial",
		    "{'tgt':{'node':'n2','port':'nope'},'src':{'data':1},'graph':'g1'}",
		    "Unknown port: n2.nope", "g1");
	refused_cmd(&k, "graph", "addinitial",
		    "{'tgt':{'node':'zz','port':'in1'},'src':{'data':'x'},'graph':'g1'}",
		    "Unknown node: zz", "g1");
	refused_cmd(&k, "graph", "removeedge",
		    "{'src':{'node':'n2','port':'out'},'tgt':{'node':'n1','port':'in2'},"
		    "'graph':'g1'}",
		    "No such connection", "g1");
	refused_cmd(&k, "graph", "changeedge",
		    "{'src':{'node':'n2','port':'out'},'tgt':{'node':'n1','port':'in2'},"
		    "'metadata':{},'graph':'g1'}",
		    "No such connection", "g1");
	refused_cmd(&k, "graph", "removeinitial", "{'tgt':{'node':'n1','port':'in2'},'graph':'g1'}",
		    "No such connection", "g1");
	refused_cmd(&k, "graph", "addinport",
		    "{'graph':'g1','node':'n2','port':'in2','public':'P'}", "Port exists: P", "g1");
	refused_cmd(&k, "graph", "addinport",
		    "{'graph':'g1','node':'n2','port':'out','public':'P2'}", "Unknown port: n2.out",
		    "g1");
	refused_cmd(&k, "graph", "addoutport",
		    "{'graph':'g1','node':'n2','port':'out','public':'Q'}", "Port exists: Q", "g1");
	refused_cmd(&k, "graph", "renameinport", "{'graph':'g1','from':'P','to':'P'}",
		    "Port exists: P", "g1");
	refused_cmd(&k, "graph", "removeinport", "{'graph':'g1','public':'nope'}",
		    "Unknown port: nope", "g1");
	refused_cmd(&k, "graph", "removeoutport", "{'graph':'g1','public':'P'}",
		    "Unknown port: P", "g1");
	refused_cmd(&k, "graph", "renameoutport", "{'graph':'g1','from':'nope','to':'x'}",
		    "Unknown port: nope", "g1");
	refused_cmd(&k, "graph", "rename", "{'id':'zz','from':'a','to':'b'}", "Unknown graph: zz",
		    "zz");
	refused_cmd(&k, "graph", "connect", "{'id':'zz'}", "Unknown graph: zz", "zz");
	refused_cmd(&k, "graph", "connect", "{'id':''}", "Invalid id", NULL);
	refused_cmd(&k, "network", "start", "{'graph':'zz'}", "Unknown graph: zz", "zz");
	refused_cmd(&k, "network", "stop", "{'graph':''}", "Invalid id", NULL);
	refused_cmd(&k, "network", "getstatus", "{'graph':'zz'}", "Unknown graph: zz", "zz");
	refused_cmd(&k, "network", "persist", "{'graph':'zz'}", "Unknown graph: zz", "zz");
	refused_cmd(&k, "network", "persist", "{'graph':''}", "Invalid id", NULL);

	/* Missing or mistyped payload fields (02 §1.2), checked before the
	 * refusals above, in the order 02 §3.4 lists them. */
	refused_cmd(&k, "graph", "addnode", "{'id':'x','graph':'g1'}", "Invalid field: component",
		    "g1");
	refused_cmd(&k, "graph", "addnode", "{'graph':'zz'}", "Invalid field: id", "zz");
	refused_cmd(&k, "graph", "addnode", "{'id':'x','graph':5,'component':'c'}",
		    "Invalid field: graph", NULL);
	refused_cmd(&k, "graph", "addnode",
		    "{'id':'x','graph':'g1','component':'math/Add2','metadata':{'x':'1'}}",
		    "Invalid field: metadata", "g1");
	refused_cmd(&k, "graph", "addnode",
		    "{'id':'x','graph':'zz','component':'math/Add2','metadata':[]}",
		    "Invalid field: metadata", "zz");
	refused_cmd(&k, "graph", "removenode", "{'id':7,'graph':'g1'}", "Invalid field: id", "g1");
	refused_cmd(&k, "graph", "renamenode", "{'from':'n1','graph':'g1'}", "Invalid field: to",
		    "g1");
	refused_cmd(&k, "graph", "changenode", "{'id':'n1','graph':'g1'}",
		    "Invalid field: metadata", "g1");
	refused_cmd(&k, "graph", "changenode", "{'id':'zz','metadata':{'y':true},'graph':'g1'}",
		    "Invalid field: metadata", "g1");
	refused_cmd(&k, "graph", "addedge", "{'src':'n1.out','tgt':{'node':'n2','port':'in2'},"
		    "'graph':'g1'}", "Invalid field: src", "g1");
	refused_cmd(&k, "graph", "addedge", "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n2'},"
		    "'graph':'g1'}", "Invalid field: tgt", "g1");
	refused_cmd(&k, "graph", "addedge",
		    "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n2','port':'in2'},"
		    "'graph':'g1','metadata':[1]}",
		    "Invalid field: metadata", "g1");
	refused_cmd(&k, "graph", "changeedge",
		    "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n2','port':'in1'},"
		    "'graph':'g1'}",
		    "Invalid field: metadata", "g1");
	refused_cmd(&k, "graph", "addinitial",
		    "{'tgt':{'node':'n1','port':'in2'},'src':{'data':{}},'graph':'g1'}",
		    "Invalid field: src", "g1");
	refused_cmd(&k, "graph", "addinitial",
		    "{'tgt':{'node':'n1','port':'in2'},'src':{'data':null},'graph':'g1'}",
		    "Invalid field: src", "g1");
	refused_cmd(&k, "graph", "addinitial", "{'tgt':{'node':'n1','port':'in2'},'graph':'g1'}",
		    "Invalid field: src", "g1");
	refused_cmd(&k, "graph", "removeinitial", "{'graph':'g1'}", "Invalid field: tgt", "g1");
	refused_cmd(&k, "graph", "addinport", "{'graph':'g1','node':'n1','port':'in2'}",
		    "Invalid field: public", "g1");
	refused_cmd(&k, "graph", "removeoutport", "{'graph':'g1'}", "Invalid field: public", "g1");
	refused_cmd(&k, "graph", "renameinport", "{'graph':'g1','to':'x'}", "Invalid field: from",
		    "g1");
	refused_cmd(&k, "graph", "rename", "{'id':'g1','from':'a'}", "Invalid field: to", "g1");
	refused_cmd(&k, "graph", "connect", "{}", "Invalid field: id", NULL);
	refused_cmd(&k, "network", "start", "{}", "Invalid field: graph", NULL);
	refused_cmd(&k, "network", "persist", "{'graph':1}", "Invalid field: graph", NULL);
	refused_cmd(&k, "network", "edges", "{'graph':'g1'}", "Invalid field: edges", "g1");
	refused_cmd(&k, "network", "edges",
		    "{'graph':'g1','edges':[{'src':{'node':'n1','port':'out'}}]}",
		    "Invalid field: edges", "g1");

	/* Nothing changed. */
	CHECK(g->block_count == 2 && g->port_count[GRAPH_INPORT] == 1 &&
	      g->port_count[GRAPH_OUTPORT] == 1);
	CHECK(graph_find_block(g, "n2")->inputs[0].source == INPUT_SOURCE_EDGE);
	CHECK(graph_find_block(g, "n1")->inputs[1].source == INPUT_SOURCE_NONE);
	clients_close(&k);
	teardown();
}

/* Scoping (02 §4.1): graph-specific events go to connections scoped to
 * their graph or unscoped, system-wide ones to every authenticated one. */
static void test_scoping(void)
{
	setup();
	CHECK(graphs_new_graph(&gs, "g1", "One", NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_new_graph(&gs, "g2", "Two", NULL, NULL, NULL, NULL) == GRAPH_OK);
	FakeConn a, b, u;
	conn_open(&a);
	conn_open(&b);
	conn_open(&u);
	const char *sa = login_as(&a, USER_ROLE_EDITOR);
	const char *sb = login_as(&b, USER_ROLE_EDITOR);
	login_as(&u, USER_ROLE_VIEWER); /* unscoped: every graph */
	command(&a, "graph", "connect", "{'id':'g1'}", sa);
	command(&b, "graph", "connect", "{'id':'g2'}", sb);
	clear(&a);
	clear(&b);

	/* A g1 event: a and u, not b. */
	command(&b, "graph", "addnode", "{'id':'n','graph':'g1','component':'math/Add2'}", sb);
	CHECK(a.count == 1 && u.count == 1 && b.count == 0);
	CHECK(json_equal(msg(&a, 0), msg(&u, 0)));
	clear(&a);
	clear(&u);
	command(&b, "graph", "rename", "{'id':'g1','to':'Uno'}", sb);
	CHECK(a.count == 1 && u.count == 1 && b.count == 0);
	clear(&a);
	clear(&u);
	command(&b, "network", "start", "{'graph':'g2'}", sb);
	CHECK(a.count == 0 && u.count == 1 && b.count == 1);
	CHECK(strstr(msg(&b, 0), "\"command\":\"started\"") != NULL);
	clear(&b);
	clear(&u);
	/* System-wide: status, new_graph, errors reach everyone. */
	command(&a, "network", "getstatus", "{'graph':'g2'}", sa);
	CHECK(a.count == 1 && b.count == 1 && u.count == 1);
	CHECK(strstr(msg(&a, 0), "\"command\":\"status\"") != NULL);
	clear(&a);
	clear(&b);
	clear(&u);
	command(&a, "runtime", "new_graph", "{'id':'g3'}", sa);
	CHECK(a.count == 2 && b.count == 2 && u.count == 2); /* and its status */
	clear(&a);
	clear(&b);
	clear(&u);
	graphs_notify_error(&gs, "g2", "boom");
	CHECK(a.count == 1 && b.count == 1 && u.count == 1);
	clear(&a);
	clear(&b);
	clear(&u);

	/* Re-scoping: a moves to g2. */
	command(&a, "graph", "connect", "{'id':'g2'}", sa);
	CHECK(a.count > 0 && strstr(msg(&a, 0), "\"command\":\"clear\"") != NULL);
	clear(&a);
	command(&b, "graph", "addnode", "{'id':'m','graph':'g1','component':'math/Add2'}", sb);
	CHECK(a.count == 0 && b.count == 0 && u.count == 1);
	clear(&u);
	command(&b, "graph", "addnode", "{'id':'m','graph':'g2','component':'math/Add2'}", sb);
	CHECK(a.count == 1 && b.count == 1 && u.count == 1);
	clear(&a);
	clear(&b);
	clear(&u);
	/* A failed connect keeps the scope. */
	command(&a, "graph", "connect", "{'id':'nope'}", sa);
	clear(&a);
	command(&b, "graph", "addnode", "{'id':'k','graph':'g2','component':'math/Add2'}", sb);
	CHECK(a.count == 1);
	clear(&a);
	clear(&b);
	clear(&u);
	/* A logout and a new login start unscoped (02 §4.1). */
	command(&a, "auth", "logout", "{}", "");
	clear(&a);
	login_as(&a, USER_ROLE_EDITOR);
	command(&b, "graph", "addnode", "{'id':'k','graph':'g1','component':'math/Add2'}", sb);
	CHECK(a.count == 1);
	conn_close(&a);
	conn_close(&b);
	conn_close(&u);
	teardown();
}

/* A graph with every kind of snapshot entry, a placeholder and its retained
 * entries (05 §2.2). */
static const char snap_file[] =
	"{'id':'gs','name':'Snap','description':'desc','icon':'cog','started':false,"
	"'blocks':{'a':{'component':'math/Add2','metadata':{'x':1,'y':2,'change_threshold':0.5,"
	"'k':'v'}},'b':{'component':'math/Add2'},'p':{'component':'nope/Missing','metadata':"
	"{'x':9,'y':9}}},"
	"'connections':[{'src':{'node':'a','port':'out'},'tgt':{'node':'b','port':'in1'},"
	"'metadata':{'route':3}},{'src':{'data':7},'tgt':{'node':'a','port':'in2'}},"
	"{'src':{'data':'3.5'},'tgt':{'node':'b','port':'in2'}},"
	"{'src':{'node':'p','port':'x'},'tgt':{'node':'a','port':'in1'}},"
	"{'src':{'data':1},'tgt':{'node':'p','port':'y'}}],"
	"'inports':[{'name':'I','node':'a','port':'in1'},{'name':'R','node':'p','port':'y'}],"
	"'outports':[{'name':'O','node':'b','port':'out'}]}";

static const char *const snapshot[] = {
	"{'protocol':'graph','command':'clear','payload':{'id':'gs','name':'Snap','main':true,"
	"'description':'desc','icon':'cog'}}",
	"{'protocol':'graph','command':'addnode','payload':{'graph':'gs','component':"
	"'math/Add2','id':'a','metadata':{'x':1,'y':2,'change_threshold':0.5,'k':'v'}}}",
	"{'protocol':'graph','command':'addnode','payload':{'graph':'gs','component':"
	"'math/Add2','id':'b','metadata':{'x':50,'y':50}}}",
	"{'protocol':'graph','command':'addnode','payload':{'graph':'gs','component':"
	"'nope/Missing','id':'p','metadata':{'x':9,'y':9}}}",
	"{'protocol':'graph','command':'addinport','payload':{'graph':'gs','name':'I','node':"
	"'a','port':'in1'}}",
	"{'protocol':'graph','command':'addoutport','payload':{'graph':'gs','name':'O','node':"
	"'b','port':'out'}}",
	"{'protocol':'graph','command':'addinitial','payload':{'graph':'gs','src':{'data':'7'},"
	"'tgt':{'node':'a','port':'in2'}}}",
	"{'protocol':'graph','command':'addinitial','payload':{'graph':'gs','src':{'data':"
	"'3.5'},'tgt':{'node':'b','port':'in2'}}}",
	"{'protocol':'graph','command':'addedge','payload':{'graph':'gs','src':{'node':'a',"
	"'port':'out'},'tgt':{'node':'b','port':'in1'},'metadata':{'route':3}}}",
};
#define SNAPSHOT_LEN (sizeof(snapshot) / sizeof(snapshot[0]))

static size_t snapshot_bytes(void)
{
	size_t n = 0;
	for (size_t i = 0; i < SNAPSHOT_LEN; i++) {
		cJSON *m = cJSON_Parse(jq(snapshot[i]));
		char *t = graph_json_print(m);
		n += strlen(t) + PROTO_FRAME_OVERHEAD;
		free(t);
		cJSON_Delete(m);
	}
	return n;
}

static void load_snap(void)
{
	const char *text = jq(snap_file);
	CHECK(graph_files_load_text(&gf, text, strlen(text), "snap", NULL, NULL) == GRAPH_FILES_OK);
}

static void test_connect_snapshot(void)
{
	setup();
	load_snap();
	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	FakeConn f, o;
	conn_open(&f);
	conn_open(&o);
	const char *s = login_as(&f, USER_ROLE_VIEWER);
	login_as(&o, USER_ROLE_VIEWER);
	command(&f, "graph", "connect", "{'id':'gs'}", s);
	FakeConn *one[] = {&f};
	expect_all(one, 1, snapshot, SNAPSHOT_LEN);
	CHECK(o.count == 0); /* a reply to the requester only */
	/* The first message byte for byte. */
	command(&f, "graph", "connect", "{'id':'gs'}", s);
	CHECK_STREQ(msg(&f, 0), "{\"protocol\":\"graph\",\"command\":\"clear\",\"payload\":{"
				"\"id\":\"gs\",\"name\":\"Snap\",\"main\":true,\"description\":"
				"\"desc\",\"icon\":\"cog\"}}");
	clear(&f);
	/* Scoped to gs now. */
	CHECK(graphs_add_node(&gs, "g1", "x", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(f.count == 0 && o.count == 1);
	clear(&o);
	/* The placeholder can be moved and removed like any block; its
	 * retained entries go with it, without events. */
	command(&f, "auth", "logout", "{}", "");
	clear(&f);
	s = login_as(&f, USER_ROLE_EDITOR);
	/* gs was loaded, so saved: its first change is followed by its status
	 * (02 §3.3). */
	command(&f, "graph", "changenode", "{'id':'p','metadata':{'x':3},'graph':'gs'}", s);
	CHECK(f.count == 2 && strstr(msg(&f, 1), "\"unsaved\":true}") != NULL);
	CHECK(f.count == 2 && json_equal(msg(&f, 0),
					 "{'protocol':'graph','command':'changenode','payload':{"
					 "'id':'p','graph':'gs','metadata':{'x':3,'y':9}}}"));
	clear(&f);
	command(&f, "graph", "removenode", "{'id':'p','graph':'gs'}", s);
	CHECK(f.count == 1 && json_equal(msg(&f, 0),
					 "{'protocol':'graph','command':'removenode','payload':{"
					 "'id':'p','graph':'gs'}}"));
	clear(&f);
	clear(&o);
	conn_close(&f);
	conn_close(&o);
	teardown();
}

/* The snapshot and the outgoing buffer (02 §4.1.1; decided, plan task 18). */
static void test_connect_buffer(void)
{
	setup();
	load_snap();
	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	size_t need = snapshot_bytes();
	FakeConn f;
	conn_open(&f);
	const char *s = login_as(&f, USER_ROLE_VIEWER);

	/* Exactly fits. */
	f.space = need;
	command(&f, "graph", "connect", "{'id':'gs'}", s);
	CHECK(f.count == SNAPSHOT_LEN);
	clear(&f);
	command(&f, "auth", "logout", "{}", "");
	clear(&f);
	s = login_as(&f, USER_ROLE_VIEWER); /* unscoped again */

	/* Larger than an empty buffer: refused, the connection stays open and
	 * unscoped. */
	f.space = need - 1;
	f.pending = 0;
	command(&f, "graph", "connect", "{'id':'gs'}", s);
	expect_one(&f, graph_error_json("Graph too large to send: gs", "gs"));
	CHECK(!f.closed);
	f.space = 1u << 20;
	CHECK(graphs_add_node(&gs, "g1", "x", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(f.count == 1);
	clear(&f);

	/* Does not fit while the buffer holds data: deferred, the scope
	 * unchanged until the snapshot goes out; events meanwhile arrive
	 * under the old scope, before the snapshot's graph:clear. */
	f.space = 200;
	f.pending = 100;
	command(&f, "graph", "connect", "{'id':'gs'}", s);
	CHECK(f.count == 0 && !f.closed);
	f.space = 1u << 20;
	CHECK(graphs_add_node(&gs, "gs", "c", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "y", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(f.count == 3); /* gs's addnode and its first-change status, g1's addnode */
	clear(&f);
	f.pending = 0;
	protocol_conn_drained(f.pc);
	CHECK(f.count == SNAPSHOT_LEN + 1); /* made at the drain: with node c */
	CHECK(json_equal(msg(&f, 0), snapshot[0]));
	CHECK(strstr(msg(&f, 4), "\"id\":\"c\"") != NULL);
	clear(&f);
	CHECK(graphs_add_node(&gs, "g1", "z", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(f.count == 0); /* scoped to gs now */
	CHECK(graphs_remove_node(&gs, "gs", "c", NULL) == GRAPH_OK);
	CHECK(f.count == 1);
	clear(&f);

	/* Deferred, then too large for the drained buffer. */
	f.space = 200;
	f.pending = 100;
	command(&f, "graph", "connect", "{'id':'g1'}", s);
	CHECK(f.count == 0);
	f.pending = 0;
	protocol_conn_drained(f.pc);
	expect_one(&f, graph_error_json("Graph too large to send: g1", "g1"));
	CHECK(graphs_add_node(&gs, "g1", "w", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(f.count == 0); /* still scoped to gs */

	/* Deferred, and the graph is deleted meanwhile; a newer connect
	 * replaces a deferred one. */
	f.pending = 100;
	command(&f, "graph", "connect", "{'id':'gs'}", s);
	CHECK(f.count == 0);
	f.space = 1u << 20;
	CHECK(graphs_delete_graph(&gs, "gs", "Snap", NULL) == GRAPH_OK);
	clear(&f);
	f.pending = 0;
	protocol_conn_drained(f.pc);
	expect_one(&f, graph_error_json("Unknown graph: gs", "gs"));
	f.space = 10;
	f.pending = 100;
	command(&f, "graph", "connect", "{'id':'g1'}", s);
	command(&f, "graph", "connect", "{'id':'g1'}", s);
	f.space = 1u << 20;
	f.pending = 0;
	protocol_conn_drained(f.pc);
	CHECK(f.count == 1 + 4); /* clear and g1's 4 blocks, once */
	clear(&f);
	/* A logout drops a deferred snapshot. */
	f.space = 10;
	f.pending = 100;
	command(&f, "graph", "connect", "{'id':'g1'}", s);
	f.space = 1u << 20;
	command(&f, "auth", "logout", "{}", "");
	clear(&f);
	f.pending = 0;
	protocol_conn_drained(f.pc);
	CHECK(f.count == 0);
	conn_close(&f);
	teardown();
}

/* The data of t1's input port after an addinitial of data (JSON with '),
 * as the event carries it, and the stored value's type. */
static void check_initial(Clients *k, const char *port, const char *data, const char *want,
			  enum linkable_type type)
{
	char payload[256];
	snprintf(payload, sizeof(payload),
		 "{'tgt':{'node':'t1','port':'%s'},'src':{'data':%s},'graph':'g1'}", port, data);
	command(&k->a, "graph", "addinitial", payload, k->sa);
	CHECK(k->a.count == 1 && k->b.count == 1);
	char expected[512];
	cJSON *e = cJSON_CreateObject();
	cJSON_AddStringToObject(e, "protocol", "graph");
	cJSON_AddStringToObject(e, "command", "addinitial");
	cJSON *pl = cJSON_AddObjectToObject(e, "payload");
	cJSON_AddStringToObject(pl, "graph", "g1");
	cJSON_AddStringToObject(cJSON_AddObjectToObject(pl, "src"), "data", want);
	cJSON *tgt = cJSON_AddObjectToObject(pl, "tgt");
	cJSON_AddStringToObject(tgt, "node", "t1");
	cJSON_AddStringToObject(tgt, "port", port);
	char *t = cJSON_PrintUnformatted(e);
	snprintf(expected, sizeof(expected), "%s", t);
	free(t);
	cJSON_Delete(e);
	if (!json_equal(msg(&k->a, 0), expected) || !json_equal(msg(&k->b, 0), expected))
		fprintf(stderr, "  for %s = %s\n", port, jq(data));
	clear(&k->a);
	clear(&k->b);
	clear(&k->v);
	const Block *b = graph_find_block(graphs_find(&gs, "g1"), "t1");
	int32_t i = block_input_index(b, port);
	CHECK(b->inputs[i].initial.type == type && b->in_values[i].type == type);
}

static void test_initial_encoding(void)
{
	setup();
	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "t1", "test/Types", NULL, NULL) == GRAPH_OK);
	Clients k;
	clients_open(&k, "g1");
	/* "number": a number, a numeric string; other text -> 0, a former
	 * unit form ("4.2 V", 00-README open question 9) included. */
	check_initial(&k, "n", "5", "5", LINKABLE_F64);
	check_initial(&k, "n", "-0.125", "-0.125", LINKABLE_F64);
	check_initial(&k, "n", "0.1", "0.1", LINKABLE_F64);
	check_initial(&k, "n", "'2.5'", "2.5", LINKABLE_F64);
	check_initial(&k, "n", "'4.2 V'", "0", LINKABLE_F64);
	check_initial(&k, "n", "'abc'", "0", LINKABLE_F64);
	check_initial(&k, "n", "'Infinity'", "Infinity", LINKABLE_F64);
	check_initial(&k, "n", "1e999", "Infinity", LINKABLE_F64);
	check_initial(&k, "n", "true", "true", LINKABLE_BOOL);
	/* "bool": to_bool of a string. */
	check_initial(&k, "b", "'true'", "true", LINKABLE_BOOL);
	check_initial(&k, "b", "'yes'", "false", LINKABLE_BOOL);
	check_initial(&k, "b", "false", "false", LINKABLE_BOOL);
	check_initial(&k, "b", "1", "1", LINKABLE_F64);
	/* "string": as-is. */
	check_initial(&k, "s", "'hello world'", "hello world", LINKABLE_STRING);
	check_initial(&k, "s", "''", "", LINKABLE_STRING);
	check_initial(&k, "s", "'5'", "5", LINKABLE_STRING);
	check_initial(&k, "s", "7", "7", LINKABLE_F64);
	/* removeinitial carries the removed value, as a string. */
	command(&k.a, "graph", "removeinitial", "{'tgt':{'node':'t1','port':'s'},'graph':'g1'}",
		k.sa);
	FakeConn *two[] = {&k.a, &k.b};
	static const char *const removed[] = {
		"{'protocol':'graph','command':'removeinitial','payload':{'src':{'data':'7'},"
		"'tgt':{'node':'t1','port':'s'},'graph':'g1'}}",
	};
	expect_all(two, 2, removed, 1);
	clear(&k.v);
	clients_close(&k);
	teardown();
}

/* network:start, stop, getstatus, persist (02 §3.3). */
static void test_network_commands(void)
{
	setup();
	CHECK(graphs_new_graph(&gs, "g1", "One", "first", NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_new_graph(&gs, "g2", "Two", NULL, NULL, NULL, NULL) == GRAPH_OK);
	Clients k;
	clients_open(&k, "g1");
	FakeConn op, other;
	conn_open(&op);
	conn_open(&other);
	const char *so = login_as(&op, USER_ROLE_OPERATOR);
	const char *sx = login_as(&other, USER_ROLE_VIEWER);
	command(&other, "graph", "connect", "{'id':'g2'}", sx);
	clear(&other);
	FakeConn *subs[] = {&k.a, &k.b, &k.v, &op};

	/* start: network:started to the graph's subscribers (op is
	 * unscoped), the sender included; not to a connection on g2. */
	runtime_set_now(&rt, 2000);
	command(&op, "network", "start", "{'graph':'g1'}", so);
	static const char *const started[] = {
		"{'protocol':'network','command':'started','payload':{'graph':'g1','time':"
		"'1970-01-01T00:00:02Z','started':true,'running':true,'debug':false}}",
	};
	expect_all(subs, 4, started, 1);
	CHECK(other.count == 0);
	/* Again: re-sent, the run's start time kept. */
	runtime_set_now(&rt, 5000);
	command(&op, "network", "start", "{'graph':'g1'}", so);
	expect_all(subs, 4, started, 1);
	/* getstatus: to every authenticated connection, the uptime in whole
	 * seconds. */
	runtime_set_now(&rt, 5999);
	command(&op, "network", "getstatus", "{'graph':'g1'}", so);
	static const char *const status[] = {
		"{'protocol':'network','command':'status','payload':{'graph':'g1','name':'One',"
		"'description':'first','uptime':3,'running':true,'started':true,'debug':false,'unsaved':true}}",
	};
	expect_all(subs, 4, status, 1);
	FakeConn *others[] = {&other};
	expect_all(others, 1, status, 1);
	/* stop: the start and duration of the run; again: the same. */
	runtime_set_now(&rt, 9500);
	command(&op, "network", "stop", "{'graph':'g1'}", so);
	static const char *const stopped[] = {
		"{'protocol':'network','command':'stopped','payload':{'graph':'g1','time':"
		"'1970-01-01T00:00:02Z','uptime':7,'started':false,'running':false,'debug':false}}",
	};
	expect_all(subs, 4, stopped, 1);
	runtime_set_now(&rt, 20000);
	command(&op, "network", "stop", "{'graph':'g1'}", so);
	expect_all(subs, 4, stopped, 1);
	/* A graph that never ran: the epoch and 0. */
	command(&op, "network", "stop", "{'graph':'g2'}", so);
	CHECK(op.count == 1 && other.count == 1 && k.a.count == 0);
	CHECK(json_equal(msg(&other, 0),
			 "{'protocol':'network','command':'stopped','payload':{'graph':'g2','time':"
			 "'1970-01-01T00:00:00Z','uptime':0,'started':false,'running':false,"
			 "'debug':false}}"));
	clear(&op);
	clear(&other);
	/* An operator may not persist; a viewer may not start. */
	command(&op, "network", "persist", "{'graph':'g1'}", so);
	expect_error(&op, "Not permitted: network:persist");
	command(&k.v, "network", "start", "{'graph':'g1'}", k.sv);
	expect_error(&k.v, "Not permitted: network:start");

	/* persist: a failed save is broadcast to everyone (graphs/ is a file
	 * here, so it cannot be created). */
	char path[PAL_PATH_MAX + 16];
	snprintf(path, sizeof(path), "%s/%s", gf.datadir, GRAPH_FILES_GRAPHS_DIR);
	CHECK(pal_storage_write(path, "x", 1) == PAL_OK);
	command(&k.a, "network", "persist", "{'graph':'g1'}", k.sa);
	static const char *const failed[] = {
		"{'protocol':'network','command':'error','payload':{'stack':'','message':"
		"'Unable to save graph: One (g1)','graph':'g1'}}",
	};
	expect_all(subs, 4, failed, 1);
	expect_all(others, 1, failed, 1);
	/* Success: no reply, but the graph's status to everyone, saved (02 §3.3);
	 * the file written. */
	CHECK(pal_storage_remove(path) == PAL_OK);
	command(&k.a, "network", "persist", "{'graph':'g1'}", k.sa);
	CHECK(k.a.count == 1 && k.b.count == 1 && op.count == 1);
	CHECK(strstr(msg(&k.a, 0), "\"command\":\"status\"") != NULL &&
	      strstr(msg(&k.a, 0), "\"unsaved\":false}") != NULL);
	clear(&k.a);
	clear(&k.b);
	clear(&op);
	CHECK(file_exists("g1"));
	conn_close(&op);
	conn_close(&other);
	clients_close(&k);
	teardown();
}

/* The network:data message expected for tgt node.port with data. */
static const char *data_json(const char *graph, const char *sn, const char *sp, const char *tn,
			     const char *tp, const char *data)
{
	static char buf[4][512];
	static int next;
	char *p = buf[next++ % 4];
	snprintf(p, 512,
		 "{'protocol':'network','command':'data','payload':{'id':'unknown','graph':'%s',"
		 "'src':{'node':'%s','port':'%s'},'tgt':{'node':'%s','port':'%s'},'data':'%s'}}",
		 graph, sn, sp, tn, tp, data);
	return p;
}

static void set_initial(const char *node, const char *port, double v)
{
	Linkable l = linkable_from_f64(v);
	CHECK(graphs_add_initial(&gs, "g1", node, port, &l, NULL) == GRAPH_OK);
}

/* network:edges and network:data (02 §4.2). */
static void test_network_edges(void)
{
	setup();
	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "n1", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "n2", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "n1", "out", "n2", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_start(&gs, "g1", NULL) == GRAPH_OK);
	runtime_drain(&rt, SIZE_MAX);
	FakeConn f, e;
	conn_open(&f);
	conn_open(&e);
	const char *s = login_as(&f, USER_ROLE_VIEWER);
	login_as(&e, USER_ROLE_VIEWER);
	/* Graph events go elsewhere, so only the link values reach f. */
	CHECK(protocol_conn_scope(f.pc, "elsewhere"));

	/* The echo (requester only), then the current value of each existing
	 * tgt; a nonexistent tgt is ignored. */
	static const char req[] =
		"{'graph':'g1','edges':[{'src':{'node':'n1','port':'out'},'tgt':{'node':'n2',"
		"'port':'in1'}},{'src':{'node':'x','port':'y'},'tgt':{'node':'nope','port':'in1'}},"
		"{'src':{'node':'x','port':'y'},'tgt':{'node':'n2','port':'out'}}],'extra':1.5}";
	command(&f, "network", "edges", req, s);
	CHECK(f.count == 2);
	char echo[1024];
	snprintf(echo, sizeof(echo), "{'protocol':'network','command':'edges','payload':%s}", req);
	CHECK(json_equal(msg(&f, 0), echo));
	CHECK(json_equal(msg(&f, 1), data_json("g1", "n1", "out", "n2", "in1", "0")));
	/* The payload byte for byte. */
	CHECK(strstr(msg(&f, 1), "\"payload\":{\"id\":\"unknown\",\"graph\":\"g1\",\"src\":{"
				 "\"node\":\"n1\",\"port\":\"out\"},\"tgt\":{\"node\":\"n2\","
				 "\"port\":\"in1\"},\"data\":\"0\"}") != NULL);
	clear(&f);
	CHECK(e.count == 0);

	/* A change: one network:data when the value arrives (after the
	 * drain), none for a value that did not change. */
	set_initial("n1", "in1", 5);
	CHECK(f.count == 0);
	runtime_drain(&rt, SIZE_MAX);
	expect_one(&f, data_json("g1", "n1", "out", "n2", "in1", "5"));
	set_initial("n1", "in2", 0);
	runtime_drain(&rt, SIZE_MAX);
	CHECK(f.count == 0);
	set_initial("n1", "in2", 0.5);
	runtime_drain(&rt, SIZE_MAX);
	expect_one(&f, data_json("g1", "n1", "out", "n2", "in1", "5.5"));

	/* Back-pressure: values that do not fit are coalesced; the latest one
	 * goes out at the drain. */
	f.space = 50;
	f.pending = 100;
	set_initial("n1", "in2", 1);
	runtime_drain(&rt, SIZE_MAX);
	set_initial("n1", "in2", 2);
	runtime_drain(&rt, SIZE_MAX);
	CHECK(f.count == 0 && !f.closed);
	f.space = 1u << 20;
	f.pending = 0;
	protocol_conn_drained(f.pc);
	expect_one(&f, data_json("g1", "n1", "out", "n2", "in1", "7"));
	protocol_conn_drained(f.pc);
	CHECK(f.count == 0);

	/* Replacement: the new set only. A structural change on the watched
	 * input (an initial) is reported at once. */
	command(&f, "network", "edges",
		"{'graph':'g1','edges':[{'src':{'node':'a','port':'b'},'tgt':{'node':'n1','port':"
		"'in1'}}]}",
		s);
	CHECK(f.count == 2);
	CHECK(json_equal(msg(&f, 1), data_json("g1", "a", "b", "n1", "in1", "5")));
	clear(&f);
	set_initial("n1", "in1", 6);
	expect_one(&f, data_json("g1", "a", "b", "n1", "in1", "6"));
	runtime_drain(&rt, SIZE_MAX); /* n2.in1 changes: no longer watched */
	CHECK(f.count == 0);
	/* An invalid request leaves the set as it was. */
	command(&f, "network", "edges", "{'graph':'g1','edges':[1]}", s);
	expect_one(&f, graph_error_json("Invalid field: edges", "g1"));
	set_initial("n1", "in1", 7);
	CHECK(f.count == 1);
	clear(&f);
	/* An empty array clears it. */
	command(&f, "network", "edges", "{'graph':'g1','edges':[]}", s);
	expect_one(&f, "{'protocol':'network','command':'edges','payload':{'graph':'g1',"
		       "'edges':[]}}");
	set_initial("n1", "in1", 8);
	runtime_drain(&rt, SIZE_MAX);
	CHECK(f.count == 0);
	/* An unknown graph: every pair ignored, still echoed. */
	command(&f, "network", "edges",
		"{'graph':'zz','edges':[{'src':{'node':'a','port':'b'},'tgt':{'node':'n1','port':"
		"'in1'}}]}",
		s);
	CHECK(f.count == 1 && strstr(msg(&f, 0), "\"command\":\"edges\"") != NULL);
	clear(&f);

	static const char watch_n1[] =
		"{'graph':'g1','edges':[{'src':{'node':'a','port':'b'},'tgt':{'node':'n1','port':"
		"'in1'}},{'src':{'node':'n1','port':'out'},'tgt':{'node':'n2','port':'in1'}}]}";
	/* Torn down at logout. */
	command(&f, "network", "edges", watch_n1, s);
	CHECK(f.count == 3);
	clear(&f);
	command(&f, "auth", "logout", "{}", "");
	clear(&f);
	set_initial("n1", "in1", 9);
	runtime_drain(&rt, SIZE_MAX);
	CHECK(f.count == 0);
	s = login_as(&f, USER_ROLE_VIEWER);
	CHECK(protocol_conn_scope(f.pc, "elsewhere"));
	set_initial("n1", "in1", 10);
	runtime_drain(&rt, SIZE_MAX);
	CHECK(f.count == 0);
	/* At expiry: nothing after its deadline, torn down at the tick. */
	command(&f, "network", "edges", watch_n1, s);
	clear(&f);
	auth_of(&f)->expired = true;
	set_initial("n1", "in1", 11);
	CHECK(f.count == 0);
	AuthConn *ac = auth_of(&f);
	ac->authenticated = false;
	ac->expired = false;
	ac->events->on_expired(ac->user);
	expect_one(&f, "{'protocol':'auth','command':'logout'}");
	s = login_as(&f, USER_ROLE_VIEWER);
	CHECK(protocol_conn_scope(f.pc, "elsewhere"));
	set_initial("n1", "in1", 12);
	runtime_drain(&rt, SIZE_MAX);
	CHECK(f.count == 0);
	/* The watched block removed: its watch ends; the others go on. */
	command(&f, "network", "edges", watch_n1, s);
	clear(&f);
	CHECK(graphs_remove_node(&gs, "g1", "n2", NULL) == GRAPH_OK);
	set_initial("n1", "in1", 13);
	expect_one(&f, data_json("g1", "a", "b", "n1", "in1", "13"));
	runtime_drain(&rt, SIZE_MAX);
	CHECK(f.count == 0);
	command(&f, "network", "edges", "{'graph':'g1','edges':[]}", s); /* replaced */
	clear(&f);
	/* Closed with live watches, and a graph deleted under them. */
	command(&f, "network", "edges", watch_n1, s);
	clear(&f);
	FakeConn h;
	conn_open(&h);
	const char *sh = login_as(&h, USER_ROLE_VIEWER);
	CHECK(protocol_conn_scope(h.pc, "elsewhere"));
	command(&h, "network", "edges", watch_n1, sh);
	clear(&h);
	conn_close(&f);
	set_initial("n1", "in1", 14);
	CHECK(h.count == 1);
	clear(&h);
	CHECK(graphs_delete_graph(&gs, "g1", graphs_find(&gs, "g1")->name, NULL) == GRAPH_OK);
	clear(&h);
	command(&h, "network", "edges", "{'graph':'g1','edges':[]}", sh);
	clear(&h);
	conn_close(&h);
	conn_close(&e);
	teardown();
}

/* The reserve of the outgoing buffer (02 §0, §4.2; plan task 20 fix): a
 * network:data goes out only if it leaves the reserve free, replies and
 * events may use the reserve, and a deferred burst goes before any value. */
static void test_data_reserve(void)
{
	setup();
	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "n1", "math/Add2", NULL, NULL) == GRAPH_OK);
	FakeConn f;
	conn_open(&f);
	const char *s = login_as(&f, USER_ROLE_VIEWER);
	CHECK(protocol_conn_scope(f.pc, "elsewhere"));
	command(&f, "network", "edges",
		"{'graph':'g1','edges':[{'src':{'node':'a','port':'b'},'tgt':{'node':'n1','port':"
		"'in1'}}]}",
		s);
	CHECK(f.count == 2);
	size_t len = strlen(msg(&f, 1)); /* every value below is one digit */
	clear(&f);
	f.capacity = 4000;
	size_t reserve = 4000 / PROTO_DATA_RESERVE_DIV;
	f.pending = 100;

	/* Exactly the reserve left after it: sent. */
	f.space = len + reserve;
	set_initial("n1", "in1", 1);
	expect_one(&f, data_json("g1", "a", "b", "n1", "in1", "1"));
	/* One byte into the reserve: coalesced, not sent, not closed. */
	f.space = len + reserve - 1;
	set_initial("n1", "in1", 2);
	set_initial("n1", "in1", 3);
	CHECK(f.count == 0 && !f.closed);
	/* A reply may use the reserve. */
	f.space = reserve;
	command(&f, "trace", "stop", "{'graph':'g'}", s);
	CHECK(f.count == 1 && !f.closed && strstr(msg(&f, 0), "\"trace\"") != NULL);
	clear(&f);
	/* So may an event (system-wide, so it reaches the scoped f). */
	CHECK(graphs_new_graph(&gs, "g2", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(f.count == 2 && !f.closed && strstr(msg(&f, 0), "new_graph") != NULL);
	clear(&f);
	/* At the drain, the latest value. */
	f.space = 4000;
	f.pending = 0;
	protocol_conn_drained(f.pc);
	expect_one(&f, data_json("g1", "a", "b", "n1", "in1", "3"));
	/* A value too large to leave the reserve free even in an empty buffer
	 * is dropped. */
	f.capacity = f.space = len;
	set_initial("n1", "in1", 4);
	CHECK(f.count == 0 && !f.closed);
	protocol_conn_drained(f.pc);
	CHECK(f.count == 0);

	/* A deferred burst goes first: values wait for it even when they would
	 * fit, then follow it at the drain. */
	size_t types = block_types_count();
	f.capacity = f.space = 2000;
	f.pending = 100;
	command(&f, "component", "list", "{}", s);
	CHECK(f.count == 0);
	f.capacity = f.space = 1u << 20;
	set_initial("n1", "in1", 5);
	set_initial("n1", "in1", 6);
	CHECK(f.count == 0 && !f.closed);
	f.pending = 0;
	protocol_conn_drained(f.pc);
	CHECK(f.count == types + 2);
	CHECK(strstr(msg(&f, types), "componentsready") != NULL);
	CHECK(json_equal(msg(&f, types + 1), data_json("g1", "a", "b", "n1", "in1", "6")));
	clear(&f);
	/* Afterwards values go out at once again. */
	set_initial("n1", "in1", 7);
	expect_one(&f, data_json("g1", "a", "b", "n1", "in1", "7"));
	conn_close(&f);
	teardown();
}

/* environment:values with entries (JSON with ' for "). */
static const char *values_json(const char *entries)
{
	static char buf[4][1024];
	static int next;
	char *p = buf[next++ % 4];
	snprintf(p, 1024, "{'protocol':'environment','command':'values','payload':{'values':[%s]}}",
		 entries);
	return p;
}

/* environment:watch and unwatch (02 §3.7, plan task 76): the echo, every
 * channel with a value, then each change; under back-pressure a catch-up
 * pass with the current values, in messages that fit; a further pass for a
 * channel that changed while the pass waited, before its cursor. */
static void test_env_watch(void)
{
	/* Without an environment. */
	setup();
	FakeConn f;
	conn_open(&f);
	const char *s = login_as(&f, USER_ROLE_VIEWER);
	command(&f, "environment", "watch", "{}", s);
	expect_error(&f, "No environment");
	command(&f, "environment", "unwatch", "{}", s);
	expect_error(&f, "No environment");
	conn_close(&f);
	teardown();

	with_env = &env;
	setup();
	conn_open(&f);
	s = login_as(&f, USER_ROLE_VIEWER); /* live values: a viewer may watch */
	/* Nothing has a value yet: an empty list. */
	command(&f, "environment", "watch", "{}", s);
	CHECK(f.count == 2);
	CHECK(json_equal(msg(&f, 0), "{'protocol':'environment','command':'watch','payload':{}}"));
	CHECK(json_equal(msg(&f, 1), values_json("")));
	clear(&f);
	/* Each change, one message; the index only for an addressable port. */
	sense(0, 2, linkable_from_f64(1.5));
	expect_one(&f, values_json("{'env':'ai','index':2,'value':1.5}"));
	sense(1, 0, linkable_from_bool(true));
	expect_one(&f, values_json("{'env':'di','value':true}"));
	/* Watching again starts over: everything with a value. */
	command(&f, "environment", "watch", "{}", s);
	CHECK(f.count == 2);
	CHECK(json_equal(msg(&f, 1), values_json("{'env':'ai','index':2,'value':1.5},"
						 "{'env':'di','value':true}")));
	clear(&f);
	/* The longest message of one entry below, and the echo. */
	size_t one = strlen(jq(values_json("{'env':'ai','index':1,'value':6}")));
	size_t echo = strlen(jq("{'protocol':'environment','command':'watch','payload':{}}"));

	/* Back-pressure: nothing while the room above the reserve is too small;
	 * at the drain the current values, together. */
	f.capacity = 4000;
	size_t reserve = 4000 / PROTO_DATA_RESERVE_DIV;
	f.pending = 100;
	f.space = reserve + 10;
	sense(0, 1, linkable_from_f64(7));
	sense(0, 2, linkable_from_f64(8));
	sense(0, 2, linkable_from_f64(9));
	CHECK(f.count == 0 && !f.closed);
	f.space = 4000;
	f.pending = 0;
	protocol_conn_drained(f.pc);
	expect_one(&f, values_json("{'env':'ai','index':1,'value':7},"
				   "{'env':'ai','index':2,'value':9}"));
	sense(0, 1, linkable_from_f64(6)); /* at once again */
	expect_one(&f, values_json("{'env':'ai','index':1,'value':6}"));

	/* A pass fills each message up to the room: here one entry each. */
	f.space = reserve + one;
	command(&f, "environment", "watch", "{}", s);
	CHECK(f.count == 4);
	CHECK(json_equal(msg(&f, 1), values_json("{'env':'ai','index':1,'value':6}")));
	CHECK(json_equal(msg(&f, 2), values_json("{'env':'ai','index':2,'value':9}")));
	CHECK(json_equal(msg(&f, 3), values_json("{'env':'di','value':true}")));
	clear(&f);

	/* The room runs out within a pass: it goes on at the drain, and a
	 * channel that changed meanwhile before its cursor follows. */
	f.space = 4000;
	sense(1, 0, linkable_from_bool(false));
	clear(&f);
	f.consume = true;
	f.pending = 100;
	f.space = reserve + echo + one; /* the echo and one message of one entry */
	command(&f, "environment", "watch", "{}", s);
	CHECK(f.count == 2); /* the echo and ai[1] */
	CHECK(json_equal(msg(&f, 1), values_json("{'env':'ai','index':1,'value':6}")));
	clear(&f);
	sense(0, 1, linkable_from_f64(5)); /* before the cursor */
	CHECK(f.count == 0);
	f.consume = false;
	f.space = 4000;
	f.pending = 0;
	protocol_conn_drained(f.pc);
	CHECK(f.count == 2);
	CHECK(json_equal(msg(&f, 0), values_json("{'env':'ai','index':2,'value':9},"
						 "{'env':'di','value':false}")));
	CHECK(json_equal(msg(&f, 1), values_json("{'env':'ai','index':1,'value':5}")));
	clear(&f);

	/* Two watchers each get a change; unwatch and logout end it. */
	FakeConn g;
	conn_open(&g);
	const char *t = login_as(&g, USER_ROLE_EDITOR);
	command(&g, "environment", "watch", "{}", t);
	clear(&g);
	sense(0, 1, linkable_from_f64(4));
	expect_one(&f, values_json("{'env':'ai','index':1,'value':4}"));
	expect_one(&g, values_json("{'env':'ai','index':1,'value':4}"));
	command(&f, "environment", "unwatch", "{}", s);
	expect_one(&f, "{'protocol':'environment','command':'unwatch','payload':{}}");
	command(&g, "auth", "logout", "{}", t);
	clear(&g);
	sense(0, 1, linkable_from_f64(3));
	CHECK(f.count == 0 && g.count == 0);
	conn_close(&g);
	conn_close(&f);
	teardown();
}

/* Composite types over the dispatcher (plan task 21): component:list with
 * subgraph true, getsource with the authoring JSON as a string (02 §3.5),
 * the snapshot showing an instance as one node, network:edges on an exposed
 * input. */
static void test_composite(void)
{
	setup();
	static const char sum3[] =
		"{'name':'t/Sum3','icon':'add','description':'a+b+c','blocks':[{'name':'A',"
		"'type':'math/Add2'},{'name':'B','type':'math/Add2'}],'connections':[{'src':"
		"'A.out','tgt':'B.in1'}],'initials':[{'src':0.5,'tgt':'B.in2'}],'inports':[{"
		"'name':'a','tgt':'A.in1'},{'name':'b','tgt':'A.in2','description':'Second'}],"
		"'outports':[{'name':'sum','src':'B.out'}]}";
	char text[1024];
	snprintf(text, sizeof(text), "%s", jq(sum3));
	CompositeMessage cm;
	CHECK(composite_types_register_text(text, strlen(text), &cm, NULL) == COMPOSITE_OK);
	FakeConn f;
	conn_open(&f);
	const char *s = login_as(&f, USER_ROLE_EDITOR);

	command(&f, "component", "list", "{}", s);
	size_t types = block_types_count();
	CHECK(f.count == types + 1);
	CHECK(json_equal(msg(&f, types - 1),
			 "{'protocol':'component','command':'component','payload':{'name':"
			 "'t/Sum3','description':'a+b+c','subgraph':true,'icon':'add','inPorts':["
			 "{'id':'a','description':'Operand 1','type':'number','addressable':false},"
			 "{'id':'b','description':'Second','type':'number','addressable':false}],"
			 "'outPorts':[{'id':'sum','description':'Result','type':'number',"
			 "'addressable':false}]}}"));
	clear(&f);

	/* getsource: the canonical authoring JSON, as a string. */
	command(&f, "component", "getsource", "{'name':'t/Sum3'}", s);
	CHECK(f.count == 1);
	cJSON *m = cJSON_Parse(msg(&f, 0));
	const cJSON *code =
		cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "payload"),
						 "code");
	CHECK(cJSON_IsString(code));
	if (cJSON_IsString(code))
		CHECK_STREQ(code->valuestring, text);
	cJSON_Delete(m);
	clear(&f);

	/* A graph with an instance: one node in the snapshot. */
	CHECK(graphs_new_graph(&gs, "g1", "G", NULL, NULL, NULL, NULL) == GRAPH_OK);
	clear(&f); /* runtime:new_graph */
	command(&f, "graph", "addnode", "{'id':'n','graph':'g1','component':'math/Add2'}", s);
	command(&f, "graph", "addnode", "{'id':'c','graph':'g1','component':'t/Sum3'}", s);
	command(&f, "graph", "addedge",
		"{'src':{'node':'n','port':'out'},'tgt':{'node':'c','port':'a'},'graph':'g1'}", s);
	command(&f, "graph", "addinitial",
		"{'src':{'data':'2'},'tgt':{'node':'c','port':'b'},'graph':'g1'}", s);
	command(&f, "graph", "addinport", "{'graph':'g1','node':'c','port':'b','public':'x'}", s);
	CHECK(f.count == 5);
	CHECK(json_equal(msg(&f, 1), "{'protocol':'graph','command':'addnode','payload':{"
				     "'graph':'g1','component':'t/Sum3','id':'c','metadata':{"
				     "'x':50,'y':50}}}"));
	clear(&f);
	command(&f, "graph", "connect", "{'id':'g1'}", s);
	FakeConn *one[] = {&f};
	const char *snap[] = {
		"{'protocol':'graph','command':'clear','payload':{'id':'g1','name':'G','main':"
		"true,'description':'','icon':''}}",
		"{'protocol':'graph','command':'addnode','payload':{'graph':'g1','component':"
		"'math/Add2','id':'n','metadata':{'x':50,'y':50}}}",
		"{'protocol':'graph','command':'addnode','payload':{'graph':'g1','component':"
		"'t/Sum3','id':'c','metadata':{'x':50,'y':50}}}",
		"{'protocol':'graph','command':'addinport','payload':{'graph':'g1','name':'x',"
		"'node':'c','port':'b'}}",
		"{'protocol':'graph','command':'addinitial','payload':{'graph':'g1','src':{"
		"'data':'2'},'tgt':{'node':'c','port':'b'}}}",
		"{'protocol':'graph','command':'addedge','payload':{'graph':'g1','src':{'node':"
		"'n','port':'out'},'tgt':{'node':'c','port':'a'}}}",
	};
	expect_all(one, 1, snap, sizeof(snap) / sizeof(snap[0]));

	/* network:edges on an exposed input: its value is the inner input's. */
	command(&f, "network", "start", "{'graph':'g1'}", s);
	runtime_drain(&rt, SIZE_MAX);
	clear(&f);
	command(&f, "network", "edges",
		"{'graph':'g1','edges':[{'src':{'node':'n','port':'out'},'tgt':{'node':'c',"
		"'port':'a'}}]}",
		s);
	CHECK(f.count == 2);
	CHECK(json_equal(msg(&f, 1), data_json("g1", "n", "out", "c", "a", "0")));
	clear(&f);
	set_initial("n", "in1", 4);
	runtime_drain(&rt, SIZE_MAX);
	CHECK(f.count == 2); /* graph:addinitial, then the value */
	CHECK(json_equal(msg(&f, 1), data_json("g1", "n", "out", "c", "a", "4")));
	clear(&f);
	/* Removing the instance ends the subscription silently. */
	command(&f, "graph", "removenode", "{'id':'c','graph':'g1'}", s);
	clear(&f);
	set_initial("n", "in1", 5);
	runtime_drain(&rt, SIZE_MAX);
	CHECK(f.count == 1); /* graph:addinitial only */
	clear(&f);

	conn_close(&f);
	teardown();
	composite_types_reset();
}

/* ---- Groups (02 §3.4.2; plan task 21b) --------------------------------------------- */

/* The path of the type file of name in the fixture's datadir. */
static const char *type_path(const char *name)
{
	static char buf[2][PAL_PATH_MAX];
	static int next;
	char *p = buf[next++ % 2];
	CHECK(graph_files_path(&gf, GRAPH_FILES_TYPES_DIR, name, p, PAL_PATH_MAX) ==
	      GRAPH_FILES_OK);
	return p;
}

/* The type file of name holds the registered type's canonical source. */
static void expect_type_file(const char *name)
{
	const BlockTypeDef *def = block_types_get(name);
	CHECK(def != NULL);
	char *want = def != NULL ? composite_types_source(def, NULL) : NULL;
	char *text = NULL;
	size_t len = 0;
	CHECK(graph_files_read(type_path(name), 1u << 16, &text, &len) == GRAPH_FILES_OK);
	CHECK(text != NULL && want != NULL && strcmp(text, want) == 0);
	free(text);
	free(want);
}

/* g1: n1 -> n2 -> n3 (Add2), an initial on n2.in2; g2 empty. The clients a,
 * b, v connected to g1, o logged in and scoped to g2. */
static void groups_setup(Clients *k, FakeConn *o, const char **so)
{
	setup();
	CHECK(graphs_new_graph(&gs, "g1", "One", NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_new_graph(&gs, "g2", "Two", NULL, NULL, NULL, NULL) == GRAPH_OK);
	for (int i = 1; i <= 3; i++) {
		char n[4];
		snprintf(n, sizeof(n), "n%d", i);
		CHECK(graphs_add_node(&gs, "g1", n, "math/Add2", NULL, NULL) == GRAPH_OK);
	}
	CHECK(graphs_add_edge(&gs, "g1", "n1", "out", "n2", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "n2", "out", "n3", "in1", NULL, NULL) == GRAPH_OK);
	Linkable three = linkable_from_f64(3);
	CHECK(graphs_add_initial(&gs, "g1", "n2", "in2", &three, NULL) == GRAPH_OK);
	clients_open(k, "g1");
	conn_open(o);
	*so = login_as(o, USER_ROLE_EDITOR);
	CHECK(protocol_conn_scope(o->pc, "g2"));
}

static void groups_teardown(Clients *k, FakeConn *o)
{
	conn_close(o);
	clients_close(k);
	teardown();
	composite_types_reset();
}

/* The selected nodes are as they were: three Add2 blocks, n1 -> n2 -> n3,
 * the initial on n2. */
static void expect_g1_unchanged(void)
{
	Graph *g = graphs_find(&gs, "g1");
	CHECK(g->block_count >= 3);
	for (uint32_t i = 0; i < 3; i++)
		CHECK(g->blocks[i]->type == block_types_get("math/Add2"));
	Block *n2 = graph_find_block(g, "n2"), *n3 = graph_find_block(g, "n3");
	CHECK(n2->inputs[0].source == INPUT_SOURCE_EDGE &&
	      n2->inputs[0].src_block == graph_find_block(g, "n1"));
	CHECK(n2->inputs[1].source == INPUT_SOURCE_INITIAL);
	CHECK(n3->inputs[0].source == INPUT_SOURCE_EDGE && n3->inputs[0].src_block == n2);
}

static const char pair_component[] =
	"{'protocol':'component','command':'component','payload':{'name':'t/Pair',"
	"'description':'Two','subgraph':true,'icon':'add','inPorts':[],'outPorts':[{'id':"
	"'out','description':'Result','type':'number','addressable':false}]}}";

static void test_groups(void)
{
	Clients k;
	FakeConn o;
	const char *so;
	groups_setup(&k, &o, &so);
	FakeConn *g1[] = {&k.a, &k.b, &k.v};
	FakeConn *other[] = {&o};

	/* addgroup: graph:addgroup to g1's subscribers (the applied nodes in
	 * creation order, a node named twice once), the new type's
	 * component:component to every authenticated connection. */
	size_t before = block_types_count();
	command(&k.a, "graph", "addgroup",
		"{'graph':'g1','name':'t/Pair','nodes':['n2','n1','n2'],'metadata':{"
		"'description':'Two','icon':'add'}}",
		k.sa);
	const char *added[] = {
		"{'protocol':'graph','command':'addgroup','payload':{'graph':'g1','name':"
		"'t/Pair','nodes':['n1','n2'],'metadata':{'description':'Two','icon':'add'}}}",
		pair_component,
	};
	expect_all(g1, 3, added, 2);
	expect_all(other, 1, &added[1], 1);
	CHECK(block_types_count() == before + 1);
	expect_g1_unchanged();
	expect_type_file("t/Pair");
	/* The authoring JSON (03 §5.2). */
	command(&o, "component", "getsource", "{'name':'t/Pair'}", so);
	CHECK(o.count == 1);
	cJSON *m = cJSON_Parse(msg(&o, 0));
	const cJSON *code =
		cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "payload"),
						 "code");
	CHECK(cJSON_IsString(code) &&
	      json_same(code->valuestring,
			"{'name':'t/Pair','icon':'add','description':'Two','blocks':[{'name':"
			"'n1','type':'math/Add2'},{'name':'n2','type':'math/Add2'}],"
			"'connections':[{'src':'n1.out','tgt':'n2.in1'}],'initials':[{'src':3,"
			"'tgt':'n2.in2'}],'inports':[],'outports':[{'name':'out','src':"
			"'n2.out'}]}"));
	cJSON_Delete(m);
	clear(&o);

	/* An icon that is null is absent: the event carries the applied "". */
	edit(&k, "addgroup",
	     "{'graph':'g1','name':'t/One','nodes':['n3'],'metadata':{'description':'',"
	     "'icon':null}}",
	     (const char *const[]){
		     "{'protocol':'graph','command':'addgroup','payload':{'graph':'g1','name':"
		     "'t/One','nodes':['n3'],'metadata':{'description':'','icon':''}}}",
		     "{'protocol':'component','command':'component','payload':{'name':'t/One',"
		     "'description':'','subgraph':true,'icon':'','inPorts':[{'id':'in1',"
		     "'description':'Operand 1','type':'number','addressable':false}],"
		     "'outPorts':[]}}"},
	     2);
	clear(&o);

	/* An instance: the type is in use. */
	edit1(&k, "addnode", "{'id':'c','graph':'g1','component':'t/Pair'}",
	      "{'protocol':'graph','command':'addnode','payload':{'graph':'g1','component':"
	      "'t/Pair','id':'c','metadata':{'x':50,'y':50}}}");
	refused_cmd(&k, "graph", "removegroup", "{'graph':'g1','name':'t/Pair'}",
		    "Component in use: t/Pair", "g1");
	refused_cmd(&k, "graph", "renamegroup", "{'graph':'g1','from':'t/Pair','to':'t/Duo'}",
		    "Component in use: t/Pair", "g1");
	expect_type_file("t/Pair");

	/* changegroup is allowed while in use; the icon is kept when absent. */
	command(&k.a, "graph", "changegroup",
		"{'graph':'g1','name':'t/Pair','metadata':{'description':'A pair'}}", k.sa);
	const char *changed[] = {
		"{'protocol':'graph','command':'changegroup','payload':{'graph':'g1','name':"
		"'t/Pair','metadata':{'description':'A pair','icon':'add'}}}",
		"{'protocol':'component','command':'component','payload':{'name':'t/Pair',"
		"'description':'A pair','subgraph':true,'icon':'add','inPorts':[],'outPorts':[{"
		"'id':'out','description':'Result','type':'number','addressable':false}]}}",
	};
	expect_all(g1, 3, changed, 2);
	expect_all(other, 1, &changed[1], 1);
	expect_type_file("t/Pair");
	command(&k.a, "graph", "changegroup",
		"{'graph':'g1','name':'t/Pair','metadata':{'description':'Two','icon':'sum'}}",
		k.sa);
	CHECK(k.a.count == 2 && json_equal(msg(&k.a, 0),
					   "{'protocol':'graph','command':'changegroup','payload':{"
					   "'graph':'g1','name':'t/Pair','metadata':{'description':"
					   "'Two','icon':'sum'}}}"));
	clear(&k.a);
	clear(&k.b);
	clear(&k.v);
	clear(&o);
	expect_type_file("t/Pair");

	/* renamegroup once unused: the new file, the old one gone. */
	edit1(&k, "removenode", "{'id':'c','graph':'g1'}",
	      "{'protocol':'graph','command':'removenode','payload':{'id':'c','graph':'g1'}}");
	command(&k.a, "graph", "renamegroup", "{'graph':'g1','from':'t/Pair','to':'t/Duo'}",
		k.sa);
	const char *renamed[] = {
		"{'protocol':'graph','command':'renamegroup','payload':{'graph':'g1','from':"
		"'t/Pair','to':'t/Duo'}}",
		"{'protocol':'component','command':'component','payload':{'name':'t/Duo',"
		"'description':'Two','subgraph':true,'icon':'sum','inPorts':[],'outPorts':[{"
		"'id':'out','description':'Result','type':'number','addressable':false}]}}",
	};
	expect_all(g1, 3, renamed, 2);
	expect_all(other, 1, &renamed[1], 1);
	CHECK(block_types_get("t/Pair") == NULL);
	CHECK(block_types_at(block_types_count() - 1) == block_types_get("t/Duo"));
	expect_type_file("t/Duo");
	CHECK(!pal_storage_exists(type_path("t/Pair")));

	/* removegroup: graph:removegroup only (no component:* message). */
	edit1(&k, "removegroup", "{'graph':'g1','name':'t/Duo'}",
	      "{'protocol':'graph','command':'removegroup','payload':{'graph':'g1','name':"
	      "'t/Duo'}}");
	CHECK(o.count == 0);
	CHECK(block_types_get("t/Duo") == NULL);
	CHECK(!pal_storage_exists(type_path("t/Duo")));
	CHECK(block_types_count() == before + 1); /* t/One */
	expect_g1_unchanged();

	/* A viewer may not (02 §2.4). */
	command(&k.v, "graph", "removegroup", "{'graph':'g1','name':'t/One'}", k.sv);
	expect_error(&k.v, "Not permitted: graph:removegroup");
	CHECK(k.a.count == 0 && k.b.count == 0);
	groups_teardown(&k, &o);
}

/* Every refusal goes to the sender only, with "graph", and changes nothing
 * (02 §1.1, §1.2, §3.4.2). */
static void test_group_refusals(void)
{
	Clients k;
	FakeConn o;
	const char *so;
	groups_setup(&k, &o, &so);
	Graph *g = graphs_find(&gs, "g1");
	CHECK(graph_create_placeholder(g, "ph", "x/Gone", NULL, NULL) == GRAPH_OK);
	clear(&k.a);
	clear(&k.b);
	clear(&k.v);
	size_t before = block_types_count();
	char long_id[80], payload[256];
	memset(long_id, 'x', 65);
	long_id[65] = '\0';

	/* addgroup: the fields in order, then the graph, the name, the nodes,
	 * the registration. */
	refused_cmd(&k, "graph", "addgroup", "{}", "Invalid field: graph", NULL);
	refused_cmd(&k, "graph", "addgroup", "{'graph':'g1'}", "Invalid field: name", "g1");
	refused_cmd(&k, "graph", "addgroup", "{'graph':'g1','name':'t/X'}", "Invalid field: nodes",
		    "g1");
	refused_cmd(&k, "graph", "addgroup", "{'graph':'g1','name':'t/X','nodes':[]}",
		    "Invalid field: nodes", "g1");
	refused_cmd(&k, "graph", "addgroup", "{'graph':'g1','name':'t/X','nodes':['n1',2]}",
		    "Invalid field: nodes", "g1");
	refused_cmd(&k, "graph", "addgroup", "{'graph':'g1','name':'t/X','nodes':'n1'}",
		    "Invalid field: nodes", "g1");
	refused_cmd(&k, "graph", "addgroup", "{'graph':'g1','name':'t/X','nodes':['n1']}",
		    "Invalid field: metadata", "g1");
	refused_cmd(&k, "graph", "addgroup",
		    "{'graph':'g1','name':'t/X','nodes':['n1'],'metadata':{}}",
		    "Invalid field: metadata", "g1");
	refused_cmd(&k, "graph", "addgroup",
		    "{'graph':'g1','name':'t/X','nodes':['n1'],'metadata':{'description':'',"
		    "'icon':5}}",
		    "Invalid field: metadata", "g1");
	refused_cmd(&k, "graph", "addgroup",
		    "{'graph':'zz','name':'t/X','nodes':['nope'],'metadata':{'description':''}}",
		    "Unknown graph: zz", "zz");
	snprintf(payload, sizeof(payload),
		 "{'graph':'%s','name':'t/X','nodes':['n1'],'metadata':{'description':''}}",
		 long_id);
	refused_cmd(&k, "graph", "addgroup", payload, "Invalid id", NULL);
	snprintf(payload, sizeof(payload),
		 "{'graph':'g1','name':'%s','nodes':['nope'],'metadata':{'description':''}}",
		 long_id);
	refused_cmd(&k, "graph", "addgroup", payload, "Invalid field: name", "g1");
	refused_cmd(&k, "graph", "addgroup",
		    "{'graph':'g1','name':'','nodes':['n1'],'metadata':{'description':''}}",
		    "Invalid field: name", "g1");
	refused_cmd(&k, "graph", "addgroup",
		    "{'graph':'g1','name':'math/Add2','nodes':['nope'],'metadata':{"
		    "'description':''}}",
		    "Component exists: math/Add2", "g1");
	refused_cmd(&k, "graph", "addgroup",
		    "{'graph':'g1','name':'t/X','nodes':['n1','nope'],'metadata':{"
		    "'description':''}}",
		    "Unknown node: nope", "g1");
	/* A selected placeholder: refused when the result is registered. */
	refused_cmd(&k, "graph", "addgroup",
		    "{'graph':'g1','name':'t/X','nodes':['n1','ph'],'metadata':{"
		    "'description':''}}",
		    "Unknown component: x/Gone", "g1");
	CHECK(block_types_count() == before);
	CHECK(!pal_storage_exists(type_path("t/X")));

	/* A type to refuse the other commands on. */
	CHECK(type_files_add_group(&gf, "g1",
				   &(CompositeSelection){.name = "t/Y",
							 .nodes = (const char *[]){"n1"},
							 .node_count = 1},
				   NULL) == GRAPH_FILES_OK);
	clear(&k.a);
	clear(&k.b);
	clear(&k.v);
	clear(&o);

	refused_cmd(&k, "graph", "removegroup", "{'name':'t/Y'}", "Invalid field: graph", NULL);
	refused_cmd(&k, "graph", "removegroup", "{'graph':'g1'}", "Invalid field: name", "g1");
	refused_cmd(&k, "graph", "removegroup", "{'graph':'zz','name':'t/Y'}",
		    "Unknown graph: zz", "zz");
	refused_cmd(&k, "graph", "removegroup", "{'graph':'g1','name':'t/Nope'}",
		    "Unknown component: t/Nope", "g1");
	refused_cmd(&k, "graph", "removegroup", "{'graph':'g1','name':'math/Add2'}",
		    "Unknown component: math/Add2", "g1");

	refused_cmd(&k, "graph", "renamegroup", "{'graph':'g1','from':'t/Y'}",
		    "Invalid field: to", "g1");
	refused_cmd(&k, "graph", "renamegroup", "{'graph':'g1','to':'t/Z'}",
		    "Invalid field: from", "g1");
	refused_cmd(&k, "graph", "renamegroup", "{'graph':'zz','from':'t/Y','to':'t/Z'}",
		    "Unknown graph: zz", "zz");
	refused_cmd(&k, "graph", "renamegroup", "{'graph':'g1','from':'math/Add2','to':'t/Z'}",
		    "Unknown component: math/Add2", "g1");
	refused_cmd(&k, "graph", "renamegroup", "{'graph':'g1','from':'t/Y','to':''}",
		    "Invalid field: to", "g1");
	refused_cmd(&k, "graph", "renamegroup", "{'graph':'g1','from':'t/Y','to':'math/Add2'}",
		    "Component exists: math/Add2", "g1");
	refused_cmd(&k, "graph", "renamegroup", "{'graph':'g1','from':'t/Y','to':'t/Y'}",
		    "Component exists: t/Y", "g1");

	refused_cmd(&k, "graph", "changegroup", "{'graph':'g1','name':'t/Y'}",
		    "Invalid field: metadata", "g1");
	refused_cmd(&k, "graph", "changegroup",
		    "{'graph':'g1','name':'t/Y','metadata':{'icon':'x'}}",
		    "Invalid field: metadata", "g1");
	refused_cmd(&k, "graph", "changegroup",
		    "{'graph':'zz','name':'t/Y','metadata':{'description':''}}",
		    "Unknown graph: zz", "zz");
	refused_cmd(&k, "graph", "changegroup",
		    "{'graph':'g1','name':'math/Add2','metadata':{'description':''}}",
		    "Unknown component: math/Add2", "g1");
	refused_cmd(&k, "graph", "changegroup",
		    "{'graph':'g1','name':'t/Nope','metadata':{'description':''}}",
		    "Unknown component: t/Nope", "g1");

	/* Nothing changed, nothing reached the others. */
	CHECK(o.count == 0);
	const BlockTypeDef *y = block_types_get("t/Y");
	CHECK(y != NULL && strcmp(y->description, "") == 0);
	CHECK(block_types_count() == before + 1);
	expect_type_file("t/Y");
	expect_g1_unchanged();
	groups_teardown(&k, &o);
}

/* A type file that cannot be written refuses the command as a whole: the
 * registration, rename or change is not applied (plan task 21b). */
static void test_group_storage(void)
{
	Clients k;
	FakeConn o;
	const char *so;
	groups_setup(&k, &o, &so);
	char types[PAL_PATH_MAX];
	CHECK(graph_files_dir(&gf, GRAPH_FILES_TYPES_DIR, types, sizeof(types)) ==
	      GRAPH_FILES_OK);
	size_t before = block_types_count();

	/* types/ is a file: addgroup is rolled back. */
	CHECK(pal_storage_write(types, "x", 1) == PAL_OK);
	refused_cmd(&k, "graph", "addgroup",
		    "{'graph':'g1','name':'t/Pair','nodes':['n1','n2'],'metadata':{"
		    "'description':'Two'}}",
		    "Unable to save component: t/Pair", "g1");
	CHECK(block_types_get("t/Pair") == NULL && block_types_count() == before);
	CHECK(composite_types_count() == 0);
	CHECK(pal_storage_remove(types) == PAL_OK);
	command(&k.a, "graph", "addgroup",
		"{'graph':'g1','name':'t/Pair','nodes':['n1','n2'],'metadata':{"
		"'description':'Two'}}",
		k.sa);
	CHECK(k.a.count == 2);
	clear(&k.a);
	clear(&k.b);
	clear(&k.v);
	clear(&o);
	const BlockTypeDef *pair = block_types_get("t/Pair");
	CHECK(pair != NULL);

	/* The new name's file cannot be written (a directory is in the way):
	 * the type keeps its name, its place and its file. */
	CHECK(mkdir(type_path("t/Duo"), 0700) == 0);
	refused_cmd(&k, "graph", "renamegroup", "{'graph':'g1','from':'t/Pair','to':'t/Duo'}",
		    "Unable to save component: t/Duo", "g1");
	CHECK(block_types_get("t/Pair") == pair && block_types_get("t/Duo") == NULL);
	CHECK(block_types_at(block_types_count() - 1) == pair);
	expect_type_file("t/Pair");
	CHECK(rmdir(type_path("t/Duo")) == 0);

	/* The file cannot be rewritten: the description stays. */
	CHECK(pal_storage_remove(type_path("t/Pair")) == PAL_OK);
	CHECK(mkdir(type_path("t/Pair"), 0700) == 0);
	refused_cmd(&k, "graph", "changegroup",
		    "{'graph':'g1','name':'t/Pair','metadata':{'description':'New','icon':'i'}}",
		    "Unable to save component: t/Pair", "g1");
	CHECK(strcmp(pair->description, "Two") == 0 && strcmp(pair->icon, "") == 0);
	CHECK(rmdir(type_path("t/Pair")) == 0);
	CHECK(o.count == 0);

	/* With the obstacles gone the commands are applied. */
	command(&k.a, "graph", "changegroup",
		"{'graph':'g1','name':'t/Pair','metadata':{'description':'New','icon':'i'}}",
		k.sa);
	CHECK(k.a.count == 2);
	expect_type_file("t/Pair");
	clear(&k.a);
	command(&k.a, "graph", "renamegroup", "{'graph':'g1','from':'t/Pair','to':'t/Duo'}",
		k.sa);
	CHECK(k.a.count == 2);
	expect_type_file("t/Duo");
	CHECK(!pal_storage_exists(type_path("t/Pair")));
	clear(&k.a);
	clear(&k.b);
	clear(&k.v);
	clear(&o);
	groups_teardown(&k, &o);
}

/* component:source (02 §3.5; plan task 58): a type file registered as at
 * startup, its port descriptions kept, its file and component graph made,
 * component:component to every authenticated connection. */
static void test_component_source(void)
{
	Clients k;
	FakeConn o;
	const char *so;
	groups_setup(&k, &o, &so);
	static const char src[] =
		"{'name':'t/Src','language':'json','library':'t','code':'{\\'name\\':\\'t/Src\\',"
		"\\'description\\':\\'Doubled\\',\\'blocks\\':[{\\'name\\':\\'A\\',\\'type\\':"
		"\\'math/Add2\\'}],\\'inports\\':[{\\'name\\':\\'x\\',\\'tgt\\':\\'A.in1\\',"
		"\\'description\\':\\'Mine\\'}],\\'outports\\':[{\\'name\\':\\'y\\',\\'src\\':"
		"\\'A.out\\'}]}'}";
	command(&k.a, "component", "source", src, k.sa);
	static const char comp[] =
		"{'protocol':'component','command':'component','payload':{'name':'t/Src',"
		"'description':'Doubled','subgraph':true,'icon':'','inPorts':[{'id':'x',"
		"'description':'Mine','type':'number','addressable':false}],'outPorts':[{'id':"
		"'y','description':'Result','type':'number','addressable':false}]}}";
	FakeConn *all[] = {&k.a, &k.b, &k.v, &o};
	for (size_t i = 0; i < 4; i++) {
		CHECK(all[i]->count == 1);
		CHECK(json_equal(msg(all[i], 0), comp));
		clear(all[i]);
	}
	CHECK(k.anon.count == 0);
	expect_type_file("t/Src");
	const Graph *cg = graphs_find(&gs, "t/Src");
	CHECK(cg != NULL && cg->component && cg->block_count == 1);

	/* The name without its library is the upstream form. */
	command(&k.a, "component", "source",
		"{'name':'Two','language':'json','library':'t','code':'{\\'name\\':\\'t/Two\\',"
		"\\'blocks\\':[{\\'name\\':\\'S\\',\\'type\\':\\'t/Src\\'}]}'}",
		k.sa);
	CHECK(k.a.count == 1 && block_types_get("t/Two") != NULL);
	clear(&k.a);
	clear(&k.b);
	clear(&k.v);
	clear(&o);

	/* Refusals, to the sender only. */
	refused_cmd(&k, "component", "source", "{'language':'json','code':'{}'}",
		    "Invalid field: name", NULL);
	refused_cmd(&k, "component", "source", "{'name':'t/X','language':'c','code':'{}'}",
		    "Invalid field: language", NULL);
	refused_cmd(&k, "component", "source", "{'name':'t/X','language':'json'}",
		    "Invalid field: code", NULL);
	refused_cmd(&k, "component", "source",
		    "{'name':'t/X','language':'json','code':'{}','library':5}",
		    "Invalid field: library", NULL);
	refused_cmd(&k, "component", "source", "{'name':'t/X','language':'json','code':'[1]'}",
		    "Invalid JSON", NULL);
	refused_cmd(&k, "component", "source", "{'name':'t/X','language':'json','code':'{} x'}",
		    "Invalid JSON", NULL);
	refused_cmd(&k, "component", "source",
		    "{'name':'t/X','language':'json','code':'{\\'name\\':\\'t/Y\\',"
		    "\\'blocks\\':[]}'}",
		    "Invalid field: name", NULL);
	refused_cmd(&k, "component", "source", src, "Component exists: t/Src", NULL);
	refused_cmd(&k, "component", "source",
		    "{'name':'g1','language':'json','code':'{\\'name\\':\\'g1\\',\\'blocks\\':[]}'}",
		    "Graph exists: g1", NULL);
	refused_cmd(&k, "component", "source",
		    "{'name':'t/U','language':'json','code':'{\\'name\\':\\'t/U\\',\\'blocks\\':[{"
		    "\\'name\\':\\'A\\',\\'type\\':\\'no/Such\\'}]}'}",
		    "Unknown component: no/Such", NULL);
	CHECK(o.count == 0);
	command(&k.v, "component", "source", src, k.sv);
	expect_error(&k.v, "Not permitted: component:source");
	clear(&k.v);

	/* The file cannot be written: the type is unregistered again. */
	char types[PAL_PATH_MAX];
	CHECK(graph_files_dir(&gf, GRAPH_FILES_TYPES_DIR, types, sizeof(types)) ==
	      GRAPH_FILES_OK);
	CHECK(pal_storage_remove(type_path("t/Src")) == PAL_OK);
	CHECK(pal_storage_remove(type_path("t/Two")) == PAL_OK);
	CHECK(rmdir(types) == 0);
	CHECK(pal_storage_write(types, "x", 1) == PAL_OK);
	refused_cmd(&k, "component", "source",
		    "{'name':'t/V','language':'json','code':'{\\'name\\':\\'t/V\\',\\'blocks\\':[{"
		    "\\'name\\':\\'A\\',\\'type\\':\\'math/Add2\\'}]}'}",
		    "Unable to save component: t/V", NULL);
	CHECK(block_types_get("t/V") == NULL && graphs_find(&gs, "t/V") == NULL);
	CHECK(pal_storage_remove(types) == PAL_OK);
	CHECK(o.count == 0);
	groups_teardown(&k, &o);
}

/* ---- The users file re-read at every challenge (02 §2.2; plan task 18) ------------- */

static struct {
	enum users_status status;
	const UserStore *src;
	int calls;
} loader;

static enum users_status fake_loader(void *ctx, UserStore *out)
{
	(void)ctx;
	loader.calls++;
	users_init(out);
	if (loader.status == USERS_OK)
		*out = *loader.src;
	return loader.status;
}

static void test_users_reload(void)
{
	char path[PAL_PATH_MAX + 32];
	snprintf(path, sizeof(path), "%s/users-reload.json", root);
	static UserStore users;
	users_init(&users);
	CHECK(users_add(&users, "alice", USER_ROLE_EDITOR, "pencil", 6, 64, test_random) ==
	      USERS_OK);
	CHECK(users_save(&users, path) == USERS_OK);
	static UserStore start;
	CHECK(users_load(&start, path) == USERS_OK);
	AuthConfig ac = {
		.users = &start,
		.load_users = users_loader,
		.load_users_ctx = path,
		.server_secret = "dGVzdCBzZXJ2ZXIgc2VjcmV0IG9ubHkhIQ==",
		.idle_timeout_s = 1000,
		.max_lifetime_s = 10000,
		.clock = test_clock,
		.random_bytes = test_random,
	};
	Auth *auth = NULL;
	CHECK(auth_create(&ac, &auth));
	setup_with(auth_authorizer(auth), false);

	FakeConn f;
	conn_open(&f);
	CHECK(client_login(&f, "alice", "pencil")[0] != '\0');
	conn_close(&f);
	/* A user added while the engine runs can log in at once. */
	CHECK(users_add(&users, "bob", USER_ROLE_EDITOR, "secret", 6, 64, test_random) ==
	      USERS_OK);
	CHECK(users_save(&users, path) == USERS_OK);
	FakeConn b;
	conn_open(&b);
	char sb[64];
	snprintf(sb, sizeof(sb), "%s", client_login(&b, "bob", "secret"));
	CHECK(sb[0] != '\0');
	/* A removed user is refused; a session already open stays valid. */
	CHECK(users_remove(&users, "bob") == USERS_OK);
	CHECK(users_save(&users, path) == USERS_OK);
	conn_open(&f);
	CHECK(client_login(&f, "bob", "secret")[0] == '\0');
	conn_close(&f);
	command(&b, "trace", "stop", "{'graph':'g'}", sb);
	CHECK(b.count == 1 && strstr(msg(&b, 0), "\"trace\"") != NULL);
	clear(&b);
	/* A changed password takes effect at the next login. */
	CHECK(users_passwd(&users, "alice", "eraser", 6, 64, test_random) == USERS_OK);
	CHECK(users_save(&users, path) == USERS_OK);
	conn_open(&f);
	CHECK(client_login(&f, "alice", "eraser")[0] != '\0');
	conn_close(&f);
	/* A malformed file means no users (as at startup). */
	CHECK(pal_storage_write(path, "[{", 2) == PAL_OK);
	conn_open(&f);
	CHECK(client_login(&f, "alice", "eraser")[0] == '\0');
	conn_close(&f);
	/* Repaired: users again; deleted: no users. */
	CHECK(users_save(&users, path) == USERS_OK);
	conn_open(&f);
	CHECK(client_login(&f, "alice", "eraser")[0] != '\0');
	conn_close(&f);
	CHECK(pal_storage_remove(path) == PAL_OK);
	conn_open(&f);
	CHECK(client_login(&f, "alice", "eraser")[0] == '\0');
	conn_close(&f);
	conn_close(&b);
	teardown();
	auth_destroy(auth);

	/* A file that cannot be read keeps the previous users; NULL never
	 * re-reads. */
	static UserStore none;
	users_init(&none);
	loader.status = USERS_OK;
	loader.src = &users;
	loader.calls = 0;
	ac.users = &none;
	ac.load_users = fake_loader;
	ac.load_users_ctx = NULL;
	CHECK(auth_create(&ac, &auth));
	setup_with(auth_authorizer(auth), false);
	conn_open(&f);
	CHECK(client_login(&f, "alice", "eraser")[0] != '\0');
	conn_close(&f);
	CHECK(loader.calls == 1);
	loader.status = USERS_ERR_STORAGE;
	conn_open(&f);
	CHECK(client_login(&f, "alice", "eraser")[0] != '\0');
	conn_close(&f);
	CHECK(loader.calls == 2);
	loader.status = USERS_ERR_NO_MEMORY;
	conn_open(&f);
	CHECK(client_login(&f, "alice", "eraser")[0] != '\0');
	conn_close(&f);
	loader.status = USERS_ERR_NOT_FOUND;
	conn_open(&f);
	CHECK(client_login(&f, "alice", "eraser")[0] == '\0');
	conn_close(&f);
	teardown();
	auth_destroy(auth);
}

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb;
	(void)flag;
	(void)ftw;
	return remove(path);
}

/* The links between processes on the wire (02 §3.8, plan task 79): list with
 * every process's ports, add and remove as system-wide events, refusals,
 * the capability, links ended by a port rename, and links.json written after
 * each command that changed them. */
static bool links_file(const char *expected)
{
	char path[PAL_PATH_MAX];
	CHECK(graph_files_top_path(&gf, LINKS_FILE_NAME, path, sizeof(path)) == GRAPH_FILES_OK);
	if (expected == NULL)
		return !pal_storage_exists(path);
	char *text = NULL;
	size_t len = 0;
	bool same = graph_files_read(path, 4096, &text, &len) == GRAPH_FILES_OK &&
		    strcmp(text, expected) == 0;
	if (!same)
		fprintf(stderr, "links.json: %s\n", text != NULL ? text : "(none)");
	free(text);
	return same;
}

static void test_links(void)
{
	setup();
	/* p: s, outport o = s.out; q: t, inports i = t.in1, i2 = t.in2 (with a
	 * value), outport qo; a component graph user/C. */
	CHECK(graphs_new_graph(&gs, "p", "P", NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_new_graph(&gs, "q", "Q", NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "p", "s", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "q", "t", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "p", GRAPH_OUTPORT, "o", "s", "out", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "q", GRAPH_INPORT, "i", "t", "in1", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "q", GRAPH_INPORT, "i2", "t", "in2", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "q", GRAPH_OUTPORT, "qo", "t", "out", NULL) == GRAPH_OK);
	Linkable one = linkable_from_f64(1);
	CHECK(graphs_add_initial(&gs, "q", "t", "in2", &one, NULL) == GRAPH_OK);
	CHECK(graphs_new_component(&gs, "user/C", "user/C", NULL, NULL, NULL, NULL) == GRAPH_OK);
	Clients k;
	clients_open(&k, "p"); /* a, b and v scoped to p: the events are system-wide */

	command(&k.v, "link", "list", "{}", k.sv);
	expect_one(&k.v, "{'protocol':'link','command':'links','payload':{'links':[],'processes':["
			 "{'graph':'p','inports':[],'outports':['o']},"
			 "{'graph':'q','inports':['i','i2'],'outports':['qo']}]}}");

	static const char link[] =
		"{'src':{'graph':'p','port':'o'},'tgt':{'graph':'q','port':'i'}}";
	char added[256], removed[256];
	snprintf(added, sizeof(added), "{'protocol':'link','command':'add','payload':%s}", link);
	snprintf(removed, sizeof(removed), "{'protocol':'link','command':'remove','payload':%s}",
		 link);
	command(&k.a, "link", "add", link, k.sa);
	FakeConn *all[] = {&k.a, &k.b, &k.v};
	const char *ev[] = {added};
	expect_all(all, 3, ev, 1);
	CHECK(links_file("{\"links\":[{\"from\":\"p.o\",\"to\":\"q.i\"}]}"));
	command(&k.v, "link", "list", "{}", k.sv);
	CHECK(k.v.count == 1 && strstr(msg(&k.v, 0), "\"links\":[{\"src\":{\"graph\":\"p\","
						      "\"port\":\"o\"},\"tgt\":{\"graph\":\"q\","
						      "\"port\":\"i\"}}]") != NULL);
	clear(&k.v);

	/* A viewer may not; refusals to the sender only, in 02 §3.8's order. */
	command(&k.v, "link", "add", link, k.sv);
	expect_error(&k.v, "Not permitted: link:add");
	struct {
		const char *payload, *message, *graph; /* graph: the error's, the source */
	} refusals[] = {
		{"{'tgt':{'graph':'q','port':'i'}}", "Invalid field: src", NULL},
		{"{'src':{'graph':'p','port':'o'},'tgt':{'graph':'q'}}", "Invalid field: tgt", "p"},
		{"{'src':{'graph':'p','port':''},'tgt':{'graph':'q','port':'i'}}", "Invalid id",
		 "p"},
		{"{'src':{'graph':'zz','port':'o'},'tgt':{'graph':'q','port':'i'}}",
		 "Unknown graph: zz", "zz"},
		{"{'src':{'graph':'p','port':'o'},'tgt':{'graph':'user/C','port':'i'}}",
		 "Not a process: user/C", "p"},
		{"{'src':{'graph':'p','port':'nope'},'tgt':{'graph':'q','port':'i'}}",
		 "Unknown port: p.nope", "p"},
		{"{'src':{'graph':'q','port':'i'},'tgt':{'graph':'q','port':'i'}}",
		 "Invalid link direction", "q"},
		{link, "Link exists", "p"},
		{"{'src':{'graph':'p','port':'o'},'tgt':{'graph':'q','port':'i2'}}",
		 "Input already connected: q.i2", "p"},
	};
	for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
		command(&k.a, "link", "add", refusals[i].payload, k.sa);
		const char *graph = refusals[i].graph;
		expect_one(&k.a, graph != NULL ? graph_error_json(refusals[i].message, graph)
					       : error_json(refusals[i].message));
	}
	command(&k.a, "link", "remove", "{'src':{'graph':'p','port':'o'},'tgt':{'graph':'q','port':"
					"'i2'}}", k.sa);
	expect_one(&k.a, graph_error_json("No such link", "p"));
	CHECK(k.b.count == 0 && k.v.count == 0);

	/* Ended by renaming its inport: link:remove before the rename, and
	 * links.json written without it. */
	command(&k.a, "graph", "renameinport", "{'graph':'q','from':'i','to':'in'}", k.sa);
	CHECK(k.b.count == 2 && json_equal(msg(&k.b, 0), removed) &&
	      strstr(msg(&k.b, 1), "\"renameinport\"") != NULL);
	clear(&k.a);
	clear(&k.b);
	clear(&k.v);
	CHECK(links_file(NULL));

	/* Made and removed again. */
	static const char again[] =
		"{'src':{'graph':'p','port':'o'},'tgt':{'graph':'q','port':'in'}}";
	command(&k.a, "link", "add", again, k.sa);
	CHECK(links_file("{\"links\":[{\"from\":\"p.o\",\"to\":\"q.in\"}]}"));
	command(&k.a, "link", "remove", again, k.sa);
	CHECK(k.b.count == 2 && strstr(msg(&k.b, 1), "\"command\":\"remove\"") != NULL);
	CHECK(links_file(NULL));
	clients_close(&k);
	teardown();
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	CHECK(block_types_register(&types_def) == BLOCK_TYPES_OK);
	const char *base = getenv("TMPDIR");
	snprintf(root, sizeof(root), "%s/hakkas-dispatch-XXXXXX", base && *base ? base : "/tmp");
	if (mkdtemp(root) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	RUN(test_messages);
	RUN(test_envelope);
	RUN(test_secret_gating);
	RUN(test_capabilities);
	RUN(test_login_flow);
	RUN(test_expiry);
	RUN(test_getruntime);
	RUN(test_new_delete_graph);
	RUN(test_component);
	RUN(test_trace);
	RUN(test_bursts);
	RUN(test_mirror);
	RUN(test_real_authorizer);
	RUN(test_graph_commands);
	RUN(test_graph_refusals);
	RUN(test_scoping);
	RUN(test_connect_snapshot);
	RUN(test_connect_buffer);
	RUN(test_initial_encoding);
	RUN(test_network_commands);
	RUN(test_network_edges);
	RUN(test_data_reserve);
	RUN(test_env_watch);
	RUN(test_users_reload);
	RUN(test_composite);
	RUN(test_groups);
	RUN(test_group_refusals);
	RUN(test_group_storage);
	RUN(test_component_source);
	RUN(test_links);

	block_types_reset();
	if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
	return test_result();
}
