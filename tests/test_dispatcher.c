/*
 * The protocol dispatcher (protocol/dispatcher.h, protocol/messages.h; 02 §1-
 * §3.6, §4.1, 05 §4), driven in-process: fake connections capture every
 * message sent, and a fake Authorizer (02 §2.5 item 10) controls each
 * session. Covers every malformed envelope, secret gating, the capability
 * table per role, challenge/login/logout including the delayed challenge
 * and the close after failures, session expiry, getruntime before and after
 * login, new_graph/delete_graph broadcasts and refusals, component:list and
 * getsource, the trace no-ops, deferred bursts, log mirroring, and a full
 * SCRAM login through the real authorizer with a test user made here.
 */
#define _XOPEN_SOURCE 700

#include "test.h"

#include <ftw.h>
#include <stdarg.h>
#include <stdlib.h>

#include "app/graph_files.h"
#include "blocktypes/block_types.h"
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
	bool closed;
	uint16_t close_code;
	bool log_on_send; /* log a line from inside send_text (recursion test) */
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
	return true;
}

static size_t f_send_space(void *conn)
{
	return ((FakeConn *)conn)->space;
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

static const ProtoTransport fake_transport = {f_send_text, f_send_space, f_send_pending, f_close};

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
				     AuthChallenge *out)
{
	(void)self;
	(void)c;
	fake.saw_null_user = user == NULL;
	fake.saw_null_cnonce = cnonce == NULL;
	if (fake.challenge_result == AUTH_OK) {
		snprintf(out->salt, sizeof(out->salt), "c2FsdHNhbHRzYWx0c2FsdA==");
		out->iterations = 4096;
		snprintf(out->nonce, sizeof(out->nonce), "%sSERVER", cnonce ? cnonce : "");
	}
	return fake.challenge_result;
}

static enum auth_result fa_verify(void *self, AuthConn *c, const char *user, const char *nonce,
				  const char *proof, AuthProof *out)
{
	(void)self;
	(void)c;
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

static bool fa_issue(void *self, AuthConn *c, char out[AUTH_SECRET_SIZE])
{
	(void)self;
	if (!fake.issue_ok)
		return false;
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
	ProtocolConfig pc = {&gs, &gf, &cfg, az};
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
	graphs_deinit(&gs);
	runtime_deinit(&rt);
}

static void conn_open(FakeConn *f)
{
	memset(f, 0, sizeof(*f));
	f->space = 1u << 20;
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

static void expect_error(FakeConn *f, const char *message)
{
	expect_one(f, error_json(message));
}

/* Log f in with the fake authorizer as role; returns its secret. */
static const char *login_as(FakeConn *f, enum user_role role)
{
	fake.role = role;
	command(f, "environment", "challenge", "{'user':'u','cnonce':'Y25vbmNl'}", "");
	command(f, "environment", "login", "{'user':'u','nonce':'n','proof':'p'}", "");
	CHECK(f->count == 2);
	clear(f);
	static char secret[4][AUTH_SECRET_SIZE];
	static int next;
	char *s = secret[next++ % 4];
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
	CHECK_STREQ(m, "{\"protocol\":\"environment\",\"command\":\"logout\"}");
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
	command(&f, "graph", "clear", "{}", "");
	expect_error(&f, "Not authenticated");
	command(&f, "runtime", "foo", "{}", "x");
	expect_error(&f, "Not authenticated");
	/* environment is never gated: an unknown command is the generic error. */
	command(&f, "environment", "foo", "{}", "");
	expect_error(&f, "Unknown command: environment:foo");
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
	command(&f, "graph", "clear", "{}", secret);
	expect_error(&f, "Unknown command: graph:clear");
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
		{"network", "persist", 0xC},
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

/* ---- environment (02 §2.3, §3.1) -------------------------------------------------- */

static void test_login_flow(void)
{
	setup();
	FakeConn f;
	conn_open(&f);
	command(&f, "environment", "challenge", "{'user':'alice','cnonce':'Y25vbmNl'}", "");
	expect_one(&f, "{'protocol':'environment','command':'challenge','payload':"
		       "{'salt':'c2FsdHNhbHRzYWx0c2FsdA==','iterations':4096,"
		       "'nonce':'Y25vbmNlSERVER'}}");
	/* Key order, byte for byte. */
	command(&f, "environment", "challenge", "{'user':'alice','cnonce':'Y25vbmNl'}", "");
	CHECK_STREQ(msg(&f, 0), "{\"protocol\":\"environment\",\"command\":\"challenge\","
				"\"payload\":{\"salt\":\"c2FsdHNhbHRzYWx0c2FsdA==\",\"iterations\":"
				"4096,\"nonce\":\"Y25vbmNlSERVER\"}}");
	clear(&f);
	/* Mistyped or missing fields reach the authorizer as NULL. */
	fake.challenge_result = AUTH_FAILED;
	command(&f, "environment", "challenge", "{'user':5}", "");
	CHECK(fake.saw_null_user && fake.saw_null_cnonce);
	expect_error(&f, "Login failed");

	/* Delayed: nothing now; the callback replies later. */
	fake.challenge_result = AUTH_DELAYED;
	command(&f, "environment", "challenge", "{'user':'alice','cnonce':'Y25vbmNl'}", "");
	CHECK(f.count == 0);
	AuthConn *ac = auth_of(&f);
	AuthChallenge ch = {"c2FsdA==", 7, "Y25vbmNlX"};
	ac->events->on_challenge(ac->user, &ch);
	expect_one(&f, "{'protocol':'environment','command':'challenge','payload':"
		       "{'salt':'c2FsdA==','iterations':7,'nonce':'Y25vbmNlX'}}");
	ac->events->on_challenge(ac->user, NULL);
	expect_error(&f, "Login failed");

	/* Login. */
	fake.role = USER_ROLE_OPERATOR;
	command(&f, "environment", "login", "{'user':'alice','nonce':'n','proof':'p'}", "");
	CHECK(!fake.saw_null_user && !fake.saw_null_nonce && !fake.saw_null_proof);
	CHECK(f.count == 1);
	CHECK_STREQ(msg(&f, 0),
		    "{\"protocol\":\"environment\",\"command\":\"login\",\"payload\":{\"secret\":"
		    "\"secret0\",\"role\":\"operator\",\"capabilities\":[\"protocol:component\","
		    "\"graph:readonly\",\"network:status\",\"network:data\",\"network:control\"],"
		    "\"server_signature\":\"c2lnbmF0dXJl\"}}");
	clear(&f);
	CHECK(fa_caps(NULL, ac) != 0);

	/* Logout: no payload at all, back to unauthenticated. */
	command(&f, "environment", "logout", "{}", "");
	CHECK(f.count == 1);
	CHECK_STREQ(msg(&f, 0), "{\"protocol\":\"environment\",\"command\":\"logout\"}");
	clear(&f);
	command(&f, "trace", "clear", "{'graph':'g'}", "secret0");
	expect_error(&f, "Not authenticated");

	/* Failures: the secret not issued, a wrong proof, missing fields. */
	fake.issue_ok = false;
	command(&f, "environment", "login", "{'user':'alice','nonce':'n','proof':'p'}", "");
	expect_error(&f, "Login failed");
	fake.issue_ok = true;
	fake.verify_result = AUTH_FAILED;
	command(&f, "environment", "login", "{'nonce':7}", "");
	CHECK(fake.saw_null_user && fake.saw_null_nonce && fake.saw_null_proof);
	expect_error(&f, "Login failed");
	CHECK(!f.closed);
	/* The third failure closes with 1008 after the reply. */
	fake.verify_result = AUTH_FAILED_CLOSE;
	command(&f, "environment", "login", "{'user':'alice','nonce':'n','proof':'p'}", "");
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
	command(&b, "runtime", "new_graph", "{'id':'g1'}", sb);
	CHECK(a.count == 1 && b.count == 1);
	clear(&a);
	clear(&b);
	/* The deadline passed: no delivery even before the tick. */
	auth_of(&a)->expired = true;
	command(&b, "runtime", "new_graph", "{'id':'g2'}", sb);
	CHECK(a.count == 0 && b.count == 1);
	clear(&b);
	/* The tick ends it: logout, and no more events. */
	AuthConn *ac = auth_of(&a);
	ac->authenticated = false;
	ac->expired = false;
	ac->events->on_expired(ac->user);
	CHECK(a.count == 1);
	CHECK_STREQ(msg(&a, 0), "{\"protocol\":\"environment\",\"command\":\"logout\"}");
	clear(&a);
	command(&b, "runtime", "new_graph", "{'id':'g3'}", sb);
	CHECK(a.count == 0 && b.count == 1);
	clear(&b);
	/* A new login gets events again. */
	login_as(&a, USER_ROLE_VIEWER);
	command(&b, "runtime", "new_graph", "{'id':'g4'}", sb);
	CHECK(a.count == 1 && b.count == 1);
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
		 "\"network:control\"],\"capabilities\":[%s],\"type\":\"hakkas\",\"namespace\":"
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
		"'description':'first','uptime':0,'running':false,'started':false,'debug':false}}",
		"{'protocol':'network','command':'status','payload':{'graph':'g2','name':"
		"'<<new graph>>','description':'','uptime':0,'running':false,'started':false,"
		"'debug':false}}",
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
			 "'debug':false}}"));
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
	expect_one(&a, created);
	expect_one(&b, created);
	expect_one(&viewer, created);
	CHECK(anon.count == 0);

	/* Defaults; null is the same as absent. */
	command(&a, "runtime", "new_graph", "{'name':null}", sa);
	CHECK(a.count == 1 && b.count == 1);
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
	CHECK(viewer.count == 1);
	clear(&viewer);
	CHECK(graphs_start(&gs, "g5", NULL) == GRAPH_OK);
	CHECK(viewer.count == 0 && a.count == 2);
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
	f.space = 1u << 20;
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
	command(f, "environment", "challenge", ch, "");
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
	command(f, "environment", "login", payload, "");
	CHECK(f->count == 1);
	static char secret[64];
	snprintf(secret, sizeof(secret), "%s", payload_str(msg(f, 0), "secret"));
	if (secret[0] != '\0') {
		CHECK_STREQ(payload_str(msg(f, 0), "server_signature"), ssig64);
		CHECK_STREQ(payload_str(msg(f, 0), "role"), "editor");
		CHECK(strstr(msg(f, 0), "\"capabilities\":[\"protocol:runtime\",\"protocol:graph\","
					"\"protocol:component\",\"graph:readonly\","
					"\"network:status\",\"network:persist\",\"network:data\","
					"\"network:control\"]") != NULL);
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
	CHECK_STREQ(msg(&f, 0), "{\"protocol\":\"environment\",\"command\":\"logout\"}");
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

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb;
	(void)flag;
	(void)ftw;
	return remove(path);
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
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

	block_types_reset();
	if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
	return test_result();
}
