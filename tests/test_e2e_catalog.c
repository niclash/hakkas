/*
 * End-to-end catalog tests (plan task 20, 05 §5): the 02 message catalog as
 * the assertion basis, over real WebSocket connections to the whole engine
 * started in-process (e2e.h). For each request/response pair of 02 §3 —
 * runtime, network, graph (the group commands of §3.4.2 included), component,
 * trace, environment — at least one case
 * asserting the documented reply or broadcast; each refusal case of 02
 * §3.4.1 as its own case (the error to the sender only, with "graph", and no
 * event anywhere); the envelope errors of §1 and the authentication errors
 * of §2 ("Not authenticated", "Not permitted: …", "Login failed", the close
 * with 1008 after three failed logins, logout).
 *
 * Clients: a and b (alice and bob, editors) and v (vera, viewer), all
 * logged in and unscoped unless a test says otherwise, so every broadcast
 * reaches all three; each broadcast is checked to be the same frame, byte
 * for byte, on every connection that gets it (02 §4.1).
 */
#include "e2e.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "blocktypes/block_types.h"
#include "cJSON.h"
#include "engine/version.h"
#include "platform/pal.h"
#include "protocol/messages.h"

#define EDITOR_CAPS                                                                                \
	"['protocol:runtime','protocol:graph','protocol:component','graph:readonly',"             \
	"'network:status','network:persist','network:data','network:control']"
#define OPERATOR_CAPS                                                                              \
	"['protocol:component','graph:readonly','network:status','network:data','network:control']"
#define VIEWER_CAPS "['protocol:component','graph:readonly','network:status','network:data']"

static E2eClient a, b, v;

/* ---- Helpers ------------------------------------------------------------------ */

static void open_clients(void)
{
	CHECK(e2e_connect(&a) && e2e_login(&a, "alice"));
	CHECK(e2e_connect(&b) && e2e_login(&b, "bob"));
	CHECK(e2e_connect(&v) && e2e_login(&v, "vera"));
}

static void close_clients(void)
{
	e2e_close(&a);
	e2e_close(&b);
	e2e_close(&v);
	e2e_iterate(2); /* the engine sees the closes */
}

/* The next message on a is protocol:command with payload, and b and v (if
 * not NULL) got the very same frame. */
static void expect_all(E2eClient *b_, E2eClient *v_, const char *protocol, const char *command,
		       const char *payload)
{
	EXPECT(&a, protocol, command, payload);
	E2eClient *others[] = {b_, v_};
	for (int i = 0; i < 2; i++) {
		if (others[i] == NULL)
			continue;
		CHECK(e2e_recv(others[i]) != NULL);
		CHECK(others[i]->frame_len == a.frame_len && a.frame != NULL &&
		      memcmp(others[i]->frame, a.frame, a.frame_len) == 0);
	}
}

/* A command from a whose one event, eprotocol:ecommand with payload event
 * (NULL: any), reaches a, b and v alike. */
static void event1x(const char *protocol, const char *command, const char *payload,
		    const char *eprotocol, const char *ecommand, const char *event)
{
	e2e_send(&a, protocol, command, payload);
	expect_all(&b, &v, eprotocol, ecommand, event);
}

/* The same, the event named like the command. */
static void event1(const char *protocol, const char *command, const char *payload,
		   const char *event)
{
	event1x(protocol, command, payload, protocol, command, event);
}

/* runtime:new_graph from a: its event (payload event, NULL: any), then the
 * new graph's status, unsaved (02 §3.3), reach a, b and v alike. */
static void new_graph1(const char *payload, const char *event)
{
	event1("runtime", "new_graph", payload, event);
	const char *status = e2e_recv(&a);
	CHECK(e2e_match(status, "network", "status", NULL) &&
	      strstr(status, "\"unsaved\":true}") != NULL);
	E2eClient *others[] = {&b, &v};
	for (int i = 0; i < 2; i++) {
		CHECK(e2e_recv(others[i]) != NULL);
		CHECK(others[i]->frame_len == a.frame_len && a.frame != NULL &&
		      memcmp(others[i]->frame, a.frame, a.frame_len) == 0);
	}
}

/* Nothing more is waiting on a, b or v. */
static void all_quiet(void)
{
	QUIET(&a);
	QUIET(&b);
	QUIET(&v);
}

/* Whether s is a wire time stamp of the wall clock now (02 §3.3: without an
 * environment the platform's clock, plan task 22): the ISO 8601 UTC form of
 * pal_wall_ms within a minute. */
static bool is_time(const char *s)
{
	int64_t now = pal_wall_ms();
	char lo[PROTO_TIME_SIZE], hi[PROTO_TIME_SIZE];
	proto_msg_time(now - 60000, lo);
	proto_msg_time(now + 60000, hi);
	unsigned y, mo, d, h, m, sec;
	char z = 0;
	return strlen(s) == 20 &&
	       sscanf(s, "%4u-%2u-%2uT%2u:%2u:%2u%c", &y, &mo, &d, &h, &m, &sec, &z) == 7 &&
	       z == 'Z' && strcmp(s, lo) >= 0 && strcmp(s, hi) <= 0;
}

/* The graph:connect snapshot of id on c (every message up to a barrier). */
static int snapshot(E2eClient *c, const char *id, char ***msgs)
{
	char payload[128];
	snprintf(payload, sizeof(payload), "{'id':'%s'}", id);
	e2e_send(c, "graph", "connect", payload);
	return e2e_collect(c, msgs);
}

static bool same_msgs(char **x, int nx, char **y, int ny)
{
	if (nx != ny || nx < 0)
		return false;
	for (int i = 0; i < nx; i++)
		if (strcmp(x[i], y[i]) != 0)
			return false;
	return true;
}

/* ---- environment (02 §2, §3.1) -------------------------------------------------- */

static void test_login(void)
{
	E2eClient c;
	CHECK(e2e_connect(&c));
	/* The challenge: salt (16 bytes), iterations, the nonce extended. */
	e2e_send(&c, "environment", "challenge",
		 "{'user':'alice','cnonce':'MDEyMzQ1Njc4OWFiY2RlZg=='}");
	const char *m = e2e_recv(&c);
	CHECK(e2e_match(m, "environment", "challenge", "{'salt':'*','iterations':64,'nonce':'*'}"));
	CHECK(strlen(e2e_payload_str(m, "salt")) == 24);
	/* An unknown user gets a normal-looking reply (the default count). */
	e2e_send(&c, "environment", "challenge",
		 "{'user':'mallory','cnonce':'MDEyMzQ1Njc4OWFiY2RlZg=='}");
	m = e2e_recv(&c);
	CHECK(e2e_match(m, "environment", "challenge",
			"{'salt':'*','iterations':100000,'nonce':'*'}"));
	CHECK(strlen(e2e_payload_str(m, "salt")) == 24);
	CHECK(strlen(e2e_payload_str(m, "nonce")) == 48);
	/* The login reply: secret, role, capabilities, server signature. */
	CHECK(e2e_login(&c, "alice"));
	CHECK(e2e_match(c.text, "environment", "login",
			"{'secret':'*','role':'editor','capabilities':" EDITOR_CAPS
			",'server_signature':'*','resume':'*'}"));
	/* A challenge on an authenticated connection: "Login failed". */
	e2e_send_secret(&c, "environment", "challenge",
			"{'user':'alice','cnonce':'MDEyMzQ1Njc4OWFiY2RlZg=='}", "");
	EXPECT_ERROR(&c, "Login failed", NULL);
	e2e_close(&c);

	/* The roles' capabilities (02 §2.4). */
	CHECK(e2e_connect(&c) && e2e_login(&c, "oscar"));
	CHECK(e2e_match(c.text, "environment", "login",
			"{'secret':'*','role':'operator','capabilities':" OPERATOR_CAPS
			",'server_signature':'*','resume':'*'}"));
	e2e_close(&c);
	CHECK(e2e_connect(&c) && e2e_login(&c, "vera"));
	CHECK(e2e_match(c.text, "environment", "login",
			"{'secret':'*','role':'viewer','capabilities':" VIEWER_CAPS
			",'server_signature':'*','resume':'*'}"));
	/* environment's unknown commands: the generic error (not gated). */
	e2e_send_secret(&c, "environment", "whoami", "{}", "");
	EXPECT_ERROR(&c, "Unknown command: environment:whoami", NULL);
	e2e_close(&c);
}

/* Resuming a session (02 §2.3.1): a reload logs in without the password. */
static void test_resume(void)
{
	E2eClient first, resumed, c;
	CHECK(e2e_connect(&first) && e2e_login(&first, "oscar"));
	E2eClient kept = first;
	e2e_close(&first);
	/* A new connection: the operator's session again, with a new id. */
	CHECK(e2e_connect(&resumed) && e2e_resume(&resumed, &kept));
	CHECK(e2e_match(resumed.text, "environment", "login",
			"{'secret':'*','role':'operator','capabilities':" OPERATOR_CAPS
			",'server_signature':'*','resume':'*'}"));
	CHECK(strstr(e2e_logs(), "login: user 'oscar' from 127.0.0.1 as operator (resumed)") !=
	      NULL);
	CHECK(strstr(e2e_logs(), kept.resume) == NULL);
	CHECK(strstr(e2e_logs(), resumed.resume) == NULL);
	/* Authenticated, as an operator. */
	e2e_send_secret(&resumed, "graph", "addnode", "{}", resumed.secret);
	EXPECT_ERROR(&resumed, "Not permitted: graph:addnode", NULL);
	/* The old id is used up; a wrong key fails too ("Login failed"). */
	CHECK(e2e_connect(&c));
	CHECK(!e2e_resume(&c, &kept));
	CHECK(e2e_match(c.text, "network", "error", "{'stack':'','message':'Login failed'}"));
	E2eClient wrong = resumed;
	wrong.resume_key[0] ^= 1;
	CHECK(!e2e_resume(&c, &wrong));
	CHECK(e2e_match(c.text, "network", "error", "{'stack':'','message':'Login failed'}"));
	e2e_close(&c);
	/* A logout ends the chain. */
	kept = resumed;
	e2e_send_secret(&resumed, "environment", "logout", "{}", resumed.secret);
	CHECK(e2e_expect(&resumed, "environment", "logout", NULL));
	e2e_close(&resumed);
	CHECK(e2e_connect(&c));
	CHECK(!e2e_resume(&c, &kept));
	e2e_close(&c);
	/* Succeed once, so the failures above delay no other test's login. */
	CHECK(e2e_connect(&c) && e2e_login(&c, "oscar"));
	e2e_close(&c);
}

static void test_login_failed(void)
{
	E2eClient c, d;
	CHECK(e2e_connect(&c));
	/* A wrong password, a login without a challenge: "Login failed". */
	CHECK(!e2e_login_password(&c, "alice", "wrong"));
	CHECK(e2e_match(c.text, "network", "error", "{'stack':'','message':'Login failed'}"));
	e2e_send(&c, "environment", "login",
		 "{'user':'alice','nonce':'MDEyMzQ1Njc4OWFiY2RlZg==','proof':'eA=='}");
	EXPECT_ERROR(&c, "Login failed", NULL);
	/* A malformed challenge too (not counted as a failed login). */
	e2e_send(&c, "environment", "challenge", "{'user':'alice','cnonce':'short'}");
	EXPECT_ERROR(&c, "Login failed", NULL);
	/* The third failed login: "Login failed", then the close with 1008. */
	CHECK(!e2e_login_password(&c, "alice", "pencil2"));
	CHECK(e2e_match(c.text, "network", "error", "{'stack':'','message':'Login failed'}"));
	CHECK(e2e_expect_close(&c, WS_CLOSE_POLICY));
	e2e_close(&c);
	/* A success from the address ends its run of failures (02 §2.5 item 7),
	 * so the other tests' logins are not delayed. */
	CHECK(e2e_connect(&d) && e2e_login(&d, "alice"));
	e2e_close(&d);
}

static void test_not_authenticated(void)
{
	open_clients();
	E2eClient u;
	CHECK(e2e_connect(&u));
	/* No secret, a made-up one, another connection's (02 §2.5 items 2, 5):
	 * never with "graph", even for a command naming one. */
	e2e_send(&u, "graph", "list", "{}");
	EXPECT_ERROR(&u, "Not authenticated", NULL);
	e2e_send_secret(&u, "network", "start", "{'graph':'g1'}", "AAAAAAAAAAAAAAAAAAAAAA");
	EXPECT_ERROR(&u, "Not authenticated", NULL);
	e2e_send_secret(&u, "graph", "addnode", "{'id':'x','graph':'g1','component':'math/Add2'}",
			a.secret);
	EXPECT_ERROR(&u, "Not authenticated", NULL);
	/* An unknown command before login: gated first. */
	e2e_send(&u, "graph", "merge", "{}");
	EXPECT_ERROR(&u, "Not authenticated", NULL);
	/* A secret that is not the connection's own does not work on a
	 * logged-in connection either. */
	e2e_send_secret(&a, "graph", "list", "{}", b.secret);
	EXPECT_ERROR(&a, "Not authenticated", NULL);
	QUIET(&u);
	all_quiet();
	e2e_close(&u);
	close_clients();
}

static void test_not_permitted(void)
{
	open_clients();
	E2eClient o;
	CHECK(e2e_connect(&o) && e2e_login(&o, "oscar"));
	new_graph1("{'id':'perm','name':'Perm'}", NULL);
	CHECK(e2e_expect(&o, "runtime", "new_graph", NULL));
	CHECK(e2e_expect(&o, "network", "status", NULL));
	/* The viewer (02 §2.4). */
	static const char *const viewer[][3] = {
		{"graph", "addnode", "{'id':'x','graph':'perm','component':'math/Add2'}"},
		{"graph", "rename", "{'id':'perm','to':'x'}"},
		{"runtime", "new_graph", "{'id':'x'}"},
		{"runtime", "delete_graph", "{'id':'perm','name':'Perm'}"},
		{"network", "start", "{'graph':'perm'}"},
		{"network", "stop", "{'graph':'perm'}"},
		{"network", "persist", "{'graph':'perm'}"},
	};
	for (size_t i = 0; i < sizeof(viewer) / sizeof(viewer[0]); i++) {
		char message[128];
		snprintf(message, sizeof(message), "Not permitted: %s:%s", viewer[i][0],
			 viewer[i][1]);
		e2e_send(&v, viewer[i][0], viewer[i][1], viewer[i][2]);
		EXPECT_ERROR(&v, message, NULL);
	}
	/* The operator may start and stop, not edit or persist. */
	e2e_send(&o, "graph", "addnode", "{'id':'x','graph':'perm','component':'math/Add2'}");
	EXPECT_ERROR(&o, "Not permitted: graph:addnode", NULL);
	e2e_send(&o, "network", "persist", "{'graph':'perm'}");
	EXPECT_ERROR(&o, "Not permitted: network:persist", NULL);
	e2e_send(&o, "network", "start", "{'graph':'perm'}");
	CHECK(e2e_expect(&o, "network", "started", NULL));
	expect_all(&b, &v, "network", "started", NULL);
	e2e_send(&o, "network", "stop", "{'graph':'perm'}");
	CHECK(e2e_expect(&o, "network", "stopped", NULL));
	expect_all(&b, &v, "network", "stopped", NULL);
	/* What the viewer may do: list, connect, getstatus, edges, component,
	 * trace. */
	e2e_send(&v, "graph", "connect", "{'id':'perm'}");
	CHECK(e2e_expect(&v, "graph", "clear", NULL));
	e2e_send(&v, "network", "edges", "{'graph':'perm','edges':[]}");
	CHECK(e2e_expect(&v, "network", "edges", "{'graph':'perm','edges':[]}"));
	e2e_send(&v, "component", "getsource", "{'name':'math/Add2'}");
	CHECK(e2e_expect(&v, "component", "source", NULL));
	event1("runtime", "delete_graph", "{'id':'perm','name':'Perm'}", NULL);
	CHECK(e2e_expect(&o, "runtime", "delete_graph", NULL));
	QUIET(&o);
	all_quiet();
	e2e_close(&o);
	close_clients();
}

static void test_logout(void)
{
	open_clients();
	e2e_send(&b, "environment", "logout", "{}");
	CHECK(e2e_recv(&b) != NULL);
	CHECK_STREQ(b.text, "{\"protocol\":\"environment\",\"command\":\"logout\"}");
	/* The old secret is dead, and nothing is delivered any more. */
	e2e_send(&b, "graph", "list", "{}");
	EXPECT_ERROR(&b, "Not authenticated", NULL);
	char secret[64];
	snprintf(secret, sizeof(secret), "%s", b.secret);
	b.secret[0] = '\0';
	e2e_send(&a, "runtime", "new_graph", "{'id':'lo','name':'Lo'}");
	EXPECT(&a, "runtime", "new_graph", "{'graph':'lo','name':'Lo','description':'','icon':''}");
	CHECK(e2e_expect(&a, "network", "status", NULL));
	CHECK(e2e_expect(&v, "runtime", "new_graph", NULL));
	CHECK(e2e_expect(&v, "network", "status", NULL));
	QUIET(&b);
	/* Logging in again starts a new session with a new secret. */
	CHECK(e2e_login(&b, "bob"));
	CHECK(strcmp(secret, b.secret) != 0);
	event1("runtime", "delete_graph", "{'id':'lo','name':'Lo'}",
	       "{'graph':'lo','name':'Lo'}");
	all_quiet();
	close_clients();
}

/* ---- Envelope (02 §1, §1.2) ------------------------------------------------------- */

static void test_envelope(void)
{
	E2eClient c;
	CHECK(e2e_connect(&c) && e2e_login(&c, "alice"));
	static const char *const bad[] = {
		"hello",
		"[1,2]",
		"{\"protocol\":\"graph\",\"command\":\"list\",\"payload\":{}}",
		"{\"protocol\":\"graph\",\"command\":\"list\",\"secret\":\"\"}",
		"{\"protocol\":\"graph\",\"payload\":{},\"secret\":\"\"}",
		"{\"command\":\"list\",\"payload\":{},\"secret\":\"\"}",
		"{\"protocol\":\"graph\",\"command\":\"list\",\"payload\":[],\"secret\":\"\"}",
		"{\"protocol\":\"graph\",\"command\":\"list\",\"payload\":{},\"secret\":null}",
		"{\"protocol\":\"graph\",\"command\":7,\"payload\":{},\"secret\":\"\"}",
		"{\"protocol\":\"Graph\",\"command\":\"list\",\"payload\":{},\"secret\":\"\"}",
		"{\"protocol\":\"foo\",\"command\":\"list\",\"payload\":{},\"secret\":\"\"}",
		"{\"protocol\":\"graph\",\"command\":\"list\",\"payload\":{},\"secret\":\"\"} x",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		char message[512];
		snprintf(message, sizeof(message), "Badly formatted request: %s", bad[i]);
		e2e_send_text(&c, bad[i]);
		EXPECT_ERROR(&c, message, NULL);
	}
	/* The raw text is cut to its first 256 bytes, "..." appended. */
	char big[600], message[600];
	memset(big, 'x', 500);
	big[0] = '{';
	big[500] = '\0';
	snprintf(message, sizeof(message), "Badly formatted request: %.256s...", big);
	e2e_send_text(&c, big);
	EXPECT_ERROR(&c, message, NULL);
	/* An unknown command in each sub-protocol: the generic error. */
	static const char *const protocols[] = {"runtime", "network", "graph", "component",
						"trace"};
	for (size_t i = 0; i < sizeof(protocols) / sizeof(protocols[0]); i++) {
		snprintf(message, sizeof(message), "Unknown command: %s:nope", protocols[i]);
		e2e_send(&c, protocols[i], "nope", "{}");
		EXPECT_ERROR(&c, message, NULL);
	}
	/* Members a command does not use are ignored, an upstream requestId
	 * among them. */
	e2e_send_text(&c, "{\"protocol\":\"trace\",\"command\":\"stop\",\"payload\":{\"graph\":"
			  "\"t\",\"extra\":1},\"secret\":\"x\",\"requestId\":\"r1\"}");
	EXPECT_ERROR(&c, "Not authenticated", NULL);
	char *msg = e2e_envelope("trace", "stop", "{'graph':'t','extra':1}", c.secret);
	size_t len = strlen(msg);
	snprintf(msg + len - 1, 64, ",\"requestId\":\"r1\"}");
	e2e_send_text(&c, msg);
	free(msg);
	EXPECT(&c, "trace", "stop", "{'graph':'t'}");
	QUIET(&c);
	e2e_close(&c);
}

/* ---- runtime (02 §3.2) ------------------------------------------------------------ */

static void test_runtime(void)
{
	const Config *cfg = hakkas_config(e2e_hakkas);
	char want[1200];
	const char *rest = ",'type':'hakkas','namespace':'hakkas','repository':'',"
			   "'repositoryVersion':''}";

	/* Before login: runtime:runtime only, no capabilities. */
	E2eClient c;
	CHECK(e2e_connect(&c));
	e2e_send(&c, "runtime", "getruntime", "{}");
	snprintf(want, sizeof(want),
		 "{'id':'%s','label':'Hakkas','version':'%s','allCapabilities':" EDITOR_CAPS
		 ",'capabilities':[]%s",
		 cfg->engine_id, hakkas_version(), rest);
	EXPECT(&c, "runtime", "runtime", want);
	QUIET(&c);

	/* After login: runtime:runtime, one component:component per type, the
	 * count, and a network:status broadcast per graph. */
	open_clients();
	new_graph1("{'id':'rt1','name':'One','description':'d','icon':'i'}",
		   "{'graph':'rt1','name':'One','description':'d','icon':'i'}");
	CHECK(e2e_login(&c, "alice"));
	e2e_send(&c, "runtime", "getruntime", "{}");
	snprintf(want, sizeof(want),
		 "{'id':'%s','label':'Hakkas','version':'%s','allCapabilities':" EDITOR_CAPS
		 ",'capabilities':" EDITOR_CAPS "%s",
		 cfg->engine_id, hakkas_version(), rest);
	EXPECT(&c, "runtime", "runtime", want);
	size_t types = block_types_count();
	CHECK(types >= 20);
	for (size_t i = 0; i < types; i++) {
		const char *m = e2e_recv(&c);
		CHECK(e2e_match(m, "component", "component", NULL));
		CHECK_STREQ(e2e_payload_str(m, "name"), block_types_at(i)->name);
	}
	char count[16];
	snprintf(count, sizeof(count), "%zu", types);
	EXPECT(&c, "component", "componentsready", count);
	static const char status[] = "{'graph':'rt1','name':'One','description':'d','uptime':0,"
				     "'running':false,'started':false,'debug':false,'unsaved':true}";
	EXPECT(&c, "network", "status", status);
	expect_all(&b, &v, "network", "status", status);
	CHECK(c.frame_len == a.frame_len && memcmp(c.frame, a.frame, a.frame_len) == 0);
	QUIET(&c);

	/* new_graph without an id: a fresh lowercase UUID v4, the defaults. */
	e2e_send(&a, "runtime", "new_graph", "{'name':null}");
	const char *m = e2e_recv(&a);
	CHECK(e2e_match(m, "runtime", "new_graph",
			"{'graph':'*','name':'<<new graph>>','description':'','icon':''}"));
	char id[64];
	snprintf(id, sizeof(id), "%s", e2e_payload_str(m, "graph"));
	CHECK(strlen(id) == 36 && id[14] == '4' && strspn(id, "0123456789abcdef-") == 36);
	CHECK(e2e_expect(&a, "network", "status", NULL)); /* new: unsaved (02 §3.3) */
	E2eClient *others[] = {&b, &v, &c};
	for (int i = 0; i < 3; i++) {
		CHECK(e2e_expect(others[i], "runtime", "new_graph", NULL));
		CHECK(e2e_expect(others[i], "network", "status", NULL));
	}
	char payload[256];
	snprintf(payload, sizeof(payload), "{'id':'%s','name':'<<new graph>>'}", id);
	e2e_send(&b, "runtime", "delete_graph", payload);
	snprintf(payload, sizeof(payload), "{'graph':'%s','name':'<<new graph>>'}", id);
	EXPECT(&b, "runtime", "delete_graph", payload);
	EXPECT(&a, "runtime", "delete_graph", payload);
	EXPECT(&v, "runtime", "delete_graph", payload);
	EXPECT(&c, "runtime", "delete_graph", payload);

	/* delete_graph of a started, persisted graph: network:stopped first,
	 * then runtime:delete_graph; the file is gone. */
	event1x("network", "start", "{'graph':'rt1'}", "network", "started", NULL);
	CHECK(e2e_expect(&c, "network", "started", NULL));
	/* A save broadcasts the status, now saved (02 §3.3). */
	e2e_send(&a, "network", "persist", "{'graph':'rt1'}");
	static const char saved[] = "{'graph':'rt1','name':'One','description':'d','uptime':'*',"
				    "'running':true,'started':true,'debug':false,'unsaved':false}";
	expect_all(&b, &v, "network", "status", saved);
	CHECK(e2e_expect(&c, "network", "status", saved));
	all_quiet();
	QUIET(&c);
	const char *file = e2e_path(e2e_path(e2e_basedir, "graphs"), "rt1.json");
	CHECK(pal_storage_exists(file));
	e2e_send(&a, "runtime", "delete_graph", "{'id':'rt1','name':'One'}");
	expect_all(&b, &v, "network", "stopped",
		   "{'graph':'rt1','time':'*','uptime':'*','started':false,'running':false,"
		   "'debug':false}");
	expect_all(&b, &v, "runtime", "delete_graph", "{'graph':'rt1','name':'One'}");
	CHECK(e2e_expect(&c, "network", "stopped", NULL));
	CHECK(e2e_expect(&c, "runtime", "delete_graph", NULL));
	CHECK(!pal_storage_exists(file));
	/* Missing fields. */
	e2e_send(&a, "runtime", "delete_graph", "{'id':'rt1'}");
	EXPECT_ERROR(&a, "Invalid field: name", "rt1");
	e2e_send(&a, "runtime", "new_graph", "{'id':5}");
	EXPECT_ERROR(&a, "Invalid field: id", NULL);
	all_quiet();
	QUIET(&c);
	e2e_close(&c);
	close_clients();
}

/* ---- component, trace (02 §3.5, §3.6) ----------------------------------------------- */

static const char add2_component[] =
	"{'name':'math/Add2','description':'out = in1+in2','subgraph':false,'icon':'add',"
	"'inPorts':[{'id':'in1','description':'Operand 1','type':'number','addressable':false},"
	"{'id':'in2','description':'Operand 2','type':'number','addressable':false}],"
	"'outPorts':[{'id':'out','description':'Result','type':'number','addressable':false}]}";
static const char interval_component[] =
	"{'name':'timing/Interval','description':'*','subgraph':false,'icon':'timer',"
	"'inPorts':[{'id':'milliseconds','description':'*','type':'number','addressable':false},"
	"{'id':'duty-cycle','description':'*','type':'number','addressable':false}],"
	"'outPorts':[{'id':'out','description':'*','type':'bool','addressable':false}]}";

static void test_component(void)
{
	open_clients();
	e2e_send(&v, "component", "list", "{}");
	size_t types = block_types_count();
	bool add2 = false, interval = false;
	for (size_t i = 0; i < types; i++) {
		const char *m = e2e_recv(&v);
		CHECK(e2e_match(m, "component", "component", NULL));
		const char *name = e2e_payload_str(m, "name");
		if (strcmp(name, "math/Add2") == 0) {
			add2 = true;
			CHECK(e2e_match(m, "component", "component", add2_component));
		} else if (strcmp(name, "timing/Interval") == 0) {
			interval = true;
			CHECK(e2e_match(m, "component", "component", interval_component));
		}
	}
	CHECK(add2 && interval);
	char count[16];
	snprintf(count, sizeof(count), "%zu", types);
	EXPECT(&v, "component", "componentsready", count);

	e2e_send(&v, "component", "getsource", "{'name':'math/Add2'}");
	EXPECT(&v, "component", "source",
	       "{'name':'math/Add2','language':'json','library':'hakkas','code':'','tests':''}");
	e2e_send(&v, "component", "getsource", "{'name':'nope/X'}");
	EXPECT_ERROR(&v, "Unknown component: nope/X", NULL);
	e2e_send(&v, "component", "getsource", "{}");
	EXPECT_ERROR(&v, "Invalid field: name", NULL);
	all_quiet();
	close_clients();
}

static void test_trace(void)
{
	open_clients();
	e2e_send(&v, "trace", "start", "{'graph':'tr','buffersize':10}");
	EXPECT(&v, "trace", "start", "{'graph':'tr'}");
	e2e_send(&v, "trace", "stop", "{'graph':'tr'}");
	EXPECT(&v, "trace", "stop", "{'graph':'tr'}");
	e2e_send(&v, "trace", "clear", "{'graph':'tr'}");
	EXPECT(&v, "trace", "clear", "{'graph':'tr'}");
	e2e_send(&v, "trace", "dump", "{'graph':'tr'}");
	EXPECT(&v, "trace", "dump", "{'graph':'tr','type':'flowtrace.json','flowtrace':''}");
	e2e_send(&v, "trace", "dump", "{'graph':1}");
	EXPECT_ERROR(&v, "Invalid field: graph", NULL);
	all_quiet();
	close_clients();
}

/* ---- network (02 §3.3) -------------------------------------------------------------- */

static void test_network(void)
{
	open_clients();
	new_graph1("{'id':'net','name':'Net'}", NULL);
	event1("graph", "addnode", "{'id':'n','graph':'net','component':'math/Add2'}", NULL);

	/* stop of a graph that never ran: the epoch and 0. */
	event1x("network", "stop", "{'graph':'net'}", "network", "stopped",
	       "{'graph':'net','time':'1970-01-01T00:00:00Z','uptime':0,'started':false,"
	       "'running':false,'debug':false}");
	/* start, and start again: the same time. */
	event1x("network", "start", "{'graph':'net'}", "network", "started",
	       "{'graph':'net','time':'*','started':true,'running':true,'debug':false}");
	char time[32];
	snprintf(time, sizeof(time), "%s", e2e_payload_str(a.text, "time"));
	CHECK(is_time(time));
	char want[256];
	snprintf(want, sizeof(want),
		 "{'graph':'net','time':'%s','started':true,'running':true,'debug':false}", time);
	event1x("network", "start", "{'graph':'net'}", "network", "started", want);
	/* getstatus: a broadcast to every authenticated connection. */
	event1x("network", "getstatus", "{'graph':'net'}", "network", "status",
	       "{'graph':'net','name':'Net','description':'','uptime':'*','running':true,"
	       "'started':true,'debug':false,'unsaved':true}");
	/* stop, and stop again: the run's time and uptime. */
	snprintf(want, sizeof(want),
		 "{'graph':'net','time':'%s','uptime':'*','started':false,'running':false,"
		 "'debug':false}",
		 time);
	event1x("network", "stop", "{'graph':'net'}", "network", "stopped", want);
	event1x("network", "stop", "{'graph':'net'}", "network", "stopped", want);
	/* persist: no reply, the status to everyone, now saved (02 §3.3); the file
	 * written. */
	e2e_send(&a, "network", "persist", "{'graph':'net'}");
	expect_all(&b, &v, "network", "status",
		   "{'graph':'net','name':'Net','description':'','uptime':0,'running':false,"
		   "'started':false,'debug':false,'unsaved':false}");
	all_quiet();
	const char *graphs = e2e_path(e2e_basedir, "graphs");
	char file[PAL_PATH_MAX];
	snprintf(file, sizeof(file), "%s", e2e_path(graphs, "net.json"));
	CHECK(pal_storage_exists(file));
	/* A save that fails: the error broadcast to every authenticated
	 * connection, with "graph" (needs a directory the user cannot write). */
	if (geteuid() != 0) {
		CHECK(chmod(graphs, 0500) == 0);
		e2e_send(&a, "network", "persist", "{'graph':'net'}");
		expect_all(&b, &v, "network", "error",
			   "{'stack':'','message':'Unable to save graph: Net (net)',"
			   "'graph':'net'}");
		CHECK(chmod(graphs, 0700) == 0);
	}
	/* edges: the echo to the requester only, then the input's value. */
	e2e_send(&v, "network", "edges",
		 "{'graph':'net','edges':[{'src':{'node':'x','port':'out'},'tgt':{'node':'n',"
		 "'port':'in1'}},{'src':{'node':'x','port':'out'},'tgt':{'node':'zz','port':"
		 "'in1'}}]}");
	EXPECT(&v, "network", "edges",
	       "{'graph':'net','edges':[{'src':{'node':'x','port':'out'},'tgt':{'node':'n',"
	       "'port':'in1'}},{'src':{'node':'x','port':'out'},'tgt':{'node':'zz','port':"
	       "'in1'}}]}");
	EXPECT(&v, "network", "data",
	       "{'id':'unknown','graph':'net','src':{'node':'x','port':'out'},'tgt':{'node':'n',"
	       "'port':'in1'},'data':'0'}");
	/* An initial on the watched input: graph:addinitial to everyone, the
	 * new value to the watcher only. */
	e2e_send(&a, "graph", "addinitial",
		 "{'tgt':{'node':'n','port':'in1'},'src':{'data':4.5},'graph':'net'}");
	static const char addinitial[] =
		"{'graph':'net','src':{'data':'4.5'},'tgt':{'node':'n','port':'in1'}}";
	/* The first change since the save: the status follows the event, now
	 * unsaved (02 §3.3). */
	static const char unsaved[] = "{'graph':'net','name':'Net','description':'','uptime':0,"
				      "'running':false,'started':false,'debug':false,"
				      "'unsaved':true}";
	EXPECT(&a, "graph", "addinitial", addinitial);
	EXPECT(&a, "network", "status", unsaved);
	EXPECT(&b, "graph", "addinitial", addinitial);
	EXPECT(&b, "network", "status", unsaved);
	/* v also watches the input: the value, the event and the status, the
	 * value in no fixed order with the other two. */
	bool data = false, event = false, status = false;
	for (int i = 0; i < 3; i++) {
		const char *m = e2e_recv(&v);
		if (m != NULL && strstr(m, "\"command\":\"data\"") != NULL) {
			CHECK(!data);
			data = true;
			CHECK(e2e_match(m, "network", "data",
					"{'id':'unknown','graph':'net','src':{'node':'x','port':"
					"'out'},'tgt':{'node':'n','port':'in1'},'data':'4.5'}"));
		} else if (!event) {
			event = true;
			CHECK(e2e_match(m, "graph", "addinitial", addinitial));
		} else {
			CHECK(!status);
			status = true;
			CHECK(e2e_match(m, "network", "status", unsaved));
		}
	}
	CHECK(data && event && status);
	/* An empty set ends the watch. */
	e2e_send(&v, "network", "edges", "{'graph':'net','edges':[]}");
	EXPECT(&v, "network", "edges", "{'graph':'net','edges':[]}");
	event1("graph", "removeinitial", "{'tgt':{'node':'n','port':'in1'},'graph':'net'}",
	       "{'src':{'data':'4.5'},'tgt':{'node':'n','port':'in1'},'graph':'net'}");
	/* Missing fields. */
	e2e_send(&a, "network", "start", "{}");
	EXPECT_ERROR(&a, "Invalid field: graph", NULL);
	e2e_send(&a, "network", "edges", "{'graph':'net'}");
	EXPECT_ERROR(&a, "Invalid field: edges", "net");
	all_quiet();
	event1("runtime", "delete_graph", "{'id':'net','name':'Net'}", NULL);
	CHECK(!pal_storage_exists(file));
	close_clients();
}

/* ---- graph (02 §3.4, §4.1.1) --------------------------------------------------------- */

static void test_graph(void)
{
	open_clients();
	new_graph1("{'id':'g','name':'G'}", NULL);
	/* b is connected to g: graph-specific events still reach it. */
	e2e_send(&b, "graph", "connect", "{'id':'g'}");
	EXPECT(&b, "graph", "clear",
	       "{'id':'g','name':'G','main':true,'description':'','icon':''}");
	QUIET(&b);

	event1("graph", "addnode", "{'id':'n1','graph':'g','component':'math/Add2'}",
	       "{'graph':'g','component':'math/Add2','id':'n1','metadata':{'x':50,'y':50}}");
	event1("graph", "addnode",
	       "{'id':'n2','graph':'g','component':'math/Add2','metadata':{'x':10.5,'y':-3,"
	       "'change_threshold':0.25,'label':'L'}}",
	       "{'graph':'g','component':'math/Add2','id':'n2','metadata':{'x':10.5,'y':-3,"
	       "'change_threshold':0.25,'label':'L'}}");
	event1("graph", "renamenode", "{'from':'n2','to':'n3','graph':'g'}",
	       "{'graph':'g','from':'n2','to':'n3'}");
	event1("graph", "changenode",
	       "{'id':'n3','metadata':{'x':1,'change_threshold':null,'label':null,'extra':[1,2]},"
	       "'graph':'g'}",
	       "{'id':'n3','graph':'g','metadata':{'x':1,'y':-3,'extra':[1,2]}}");
	event1("graph", "addedge",
	       "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n3','port':'in1'},'graph':'g',"
	       "'metadata':{'route':3}}",
	       "{'graph':'g','src':{'node':'n1','port':'out'},'tgt':{'node':'n3','port':'in1'},"
	       "'metadata':{'route':3}}");
	event1("graph", "changeedge",
	       "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n3','port':'in1'},'metadata':"
	       "{'route':5,'secure':true},'graph':'g'}",
	       "{'graph':'g','src':{'node':'n1','port':'out'},'tgt':{'node':'n3','port':'in1'},"
	       "'metadata':{'route':5,'secure':true}}");
	event1("graph", "addedge",
	       "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n3','port':'in2'},'graph':'g'}",
	       "{'graph':'g','src':{'node':'n1','port':'out'},'tgt':{'node':'n3','port':'in2'}}");
	event1("graph", "removeedge",
	       "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n3','port':'in2'},'graph':'g'}",
	       "{'graph':'g','src':{'node':'n1','port':'out'},'tgt':{'node':'n3','port':'in2'}}");
	event1("graph", "addinitial", "{'tgt':{'node':'n1','port':'in1'},'src':{'data':5},"
	       "'graph':'g'}",
	       "{'graph':'g','src':{'data':'5'},'tgt':{'node':'n1','port':'in1'}}");
	/* Text that is not wholly a number, a former unit form included, is 0
	 * on a number port (02 §3.4); the event carries the applied value. */
	event1("graph", "addinitial",
	       "{'tgt':{'node':'n1','port':'in2'},'src':{'data':'2 m'},'graph':'g'}",
	       "{'graph':'g','src':{'data':'0'},'tgt':{'node':'n1','port':'in2'}}");
	event1("graph", "removeinitial", "{'tgt':{'node':'n1','port':'in2'},'graph':'g'}",
	       "{'src':{'data':'0'},'tgt':{'node':'n1','port':'in2'},'graph':'g'}");
	event1("graph", "addinport", "{'graph':'g','node':'n1','port':'in2','public':'IN'}",
	       "{'graph':'g','name':'IN','node':'n1','port':'in2'}");
	event1("graph", "addoutport", "{'graph':'g','node':'n3','port':'out','public':'OUT'}",
	       "{'graph':'g','name':'OUT','node':'n3','port':'out'}");
	event1("graph", "renameinport", "{'graph':'g','from':'IN','to':'IN2'}",
	       "{'graph':'g','from':'IN','to':'IN2'}");
	event1("graph", "renameoutport", "{'graph':'g','from':'OUT','to':'OUT2'}",
	       "{'graph':'g','from':'OUT','to':'OUT2'}");
	event1("graph", "addinport", "{'graph':'g','node':'n3','port':'in2','public':'X'}",
	       "{'graph':'g','name':'X','node':'n3','port':'in2'}");
	event1("graph", "removeinport", "{'graph':'g','public':'X'}", "{'graph':'g','name':'X'}");
	event1("graph", "addoutport", "{'graph':'g','node':'n1','port':'out','public':'Y'}",
	       "{'graph':'g','name':'Y','node':'n1','port':'out'}");
	event1("graph", "removeoutport", "{'graph':'g','public':'Y'}", "{'graph':'g','name':'Y'}");
	event1("graph", "rename", "{'id':'g','from':'whatever','to':'G2'}",
	       "{'id':'g','from':'G','to':'G2'}");

	/* The snapshot (02 §4.1.1): clear, blocks, ports, initials, edges. */
	char **snap;
	int n = snapshot(&v, "g", &snap);
	static const char *const want[] = {
		"{'protocol':'graph','command':'clear','payload':{'id':'g','name':'G2','main':true,"
		"'description':'','icon':''}}",
		"{'protocol':'graph','command':'addnode','payload':{'graph':'g','component':"
		"'math/Add2','id':'n1','metadata':{'x':50,'y':50}}}",
		"{'protocol':'graph','command':'addnode','payload':{'graph':'g','component':"
		"'math/Add2','id':'n3','metadata':{'x':1,'y':-3,'extra':[1,2]}}}",
		"{'protocol':'graph','command':'addinport','payload':{'graph':'g','name':'IN2',"
		"'node':'n1','port':'in2'}}",
		"{'protocol':'graph','command':'addoutport','payload':{'graph':'g','name':'OUT2',"
		"'node':'n3','port':'out'}}",
		"{'protocol':'graph','command':'addinitial','payload':{'graph':'g','src':{'data':"
		"'5'},'tgt':{'node':'n1','port':'in1'}}}",
		"{'protocol':'graph','command':'addedge','payload':{'graph':'g','src':{'node':'n1',"
		"'port':'out'},'tgt':{'node':'n3','port':'in1'},'metadata':{'route':5,"
		"'secure':true}}}",
	};
	int nwant = (int)(sizeof(want) / sizeof(want[0]));
	CHECK(n == nwant);
	for (int i = 0; i < n && i < nwant; i++) {
		if (!e2e_json_eq(snap[i], want[i]))
			fprintf(stderr, "  snapshot %d: %s\n", i, snap[i]);
		CHECK(e2e_json_eq(snap[i], want[i]));
	}
	e2e_free_msgs(snap, n);

	/* removenode: its connections, its ports, then the node (02 §1.1). */
	e2e_send(&a, "graph", "removenode", "{'id':'n1','graph':'g'}");
	expect_all(&b, &v, "graph", "removeinitial",
		   "{'src':{'data':'5'},'tgt':{'node':'n1','port':'in1'},'graph':'g'}");
	expect_all(&b, &v, "graph", "removeedge",
		   "{'graph':'g','src':{'node':'n1','port':'out'},'tgt':{'node':'n3','port':"
		   "'in1'}}");
	expect_all(&b, &v, "graph", "removeinport", "{'graph':'g','name':'IN2'}");
	expect_all(&b, &v, "graph", "removenode", "{'id':'n1','graph':'g'}");

	/* graph:list: a network:status per graph, to every authenticated
	 * connection. */
	e2e_send(&v, "graph", "list", "{}");
	EXPECT(&v, "network", "status",
	       "{'graph':'g','name':'G2','description':'','uptime':0,'running':false,"
	       "'started':false,'debug':false,'unsaved':true}");
	EXPECT(&a, "network", "status", NULL);
	EXPECT(&b, "network", "status", NULL);
	/* Missing fields (02 §1.2). */
	e2e_send(&a, "graph", "addnode", "{'id':'x','graph':'g'}");
	EXPECT_ERROR(&a, "Invalid field: component", "g");
	e2e_send(&a, "graph", "connect", "{}");
	EXPECT_ERROR(&a, "Invalid field: id", NULL);
	all_quiet();
	event1("runtime", "delete_graph", "{'id':'g','name':'G2'}", NULL);
	close_clients();
}

/* ---- Groups (02 §3.4.2) ----------------------------------------------------------- */

static const char chain_out[] =
	"[{'id':'out','description':'Result','type':'number','addressable':false}]";

static void test_groups(void)
{
	open_clients();
	new_graph1("{'id':'gg','name':'GG'}", NULL);
	static const char *const nodes[] = {"p", "q", "r"};
	for (int i = 0; i < 3; i++) {
		char payload[128];
		snprintf(payload, sizeof(payload),
			 "{'id':'%s','graph':'gg','component':'math/Add2'}", nodes[i]);
		event1("graph", "addnode", payload, NULL);
	}
	event1("graph", "addedge",
	       "{'src':{'node':'p','port':'out'},'tgt':{'node':'q','port':'in1'},'graph':'gg'}",
	       NULL);
	event1("graph", "addedge",
	       "{'src':{'node':'q','port':'out'},'tgt':{'node':'r','port':'in1'},'graph':'gg'}",
	       NULL);

	/* addgroup: graph:addgroup, then the new type's component:component,
	 * to every connection alike; the type file is written. */
	e2e_send(&a, "graph", "addgroup",
		 "{'graph':'gg','name':'e2e/Chain','nodes':['q','p'],'metadata':{"
		 "'description':'Chain','icon':'link'}}");
	expect_all(&b, &v, "graph", "addgroup",
		   "{'graph':'gg','name':'e2e/Chain','nodes':['p','q'],'metadata':{"
		   "'description':'Chain','icon':'link'}}");
	char component[512];
	snprintf(component, sizeof(component),
		 "{'name':'e2e/Chain','description':'Chain','subgraph':true,'icon':'link',"
		 "'inPorts':[],'outPorts':%s}",
		 chain_out);
	expect_all(&b, &v, "component", "component", component);
	CHECK(pal_storage_exists(e2e_path(e2e_path(e2e_basedir, "types"), "e2e%2FChain.json")));

	/* component:list has it, last (the registration order). */
	e2e_send(&v, "component", "list", "{}");
	size_t types = block_types_count();
	for (size_t i = 0; i + 1 < types; i++)
		CHECK(e2e_match(e2e_recv(&v), "component", "component", NULL));
	EXPECT(&v, "component", "component", component);
	char count[16];
	snprintf(count, sizeof(count), "%zu", types);
	EXPECT(&v, "component", "componentsready", count);

	/* getsource: the canonical authoring JSON. */
	e2e_send(&v, "component", "getsource", "{'name':'e2e/Chain'}");
	const char *m = e2e_recv(&v);
	CHECK(e2e_match(m, "component", "source",
			"{'name':'e2e/Chain','language':'json','library':'hakkas','code':'*',"
			"'tests':''}"));
	CHECK(e2e_json_eq(e2e_payload_str(m, "code"),
			  "{'name':'e2e/Chain','icon':'link','description':'Chain','blocks':[{"
			  "'name':'p','type':'math/Add2'},{'name':'q','type':'math/Add2'}],"
			  "'connections':[{'src':'p.out','tgt':'q.in1'}],'initials':[],"
			  "'inports':[],'outports':[{'name':'out','src':'q.out'}]}"));

	/* An instance; while it exists the type cannot be removed. */
	event1("graph", "addnode", "{'id':'c','graph':'gg','component':'e2e/Chain'}",
	       "{'graph':'gg','component':'e2e/Chain','id':'c','metadata':{'x':50,'y':50}}");
	e2e_send(&a, "graph", "removegroup", "{'graph':'gg','name':'e2e/Chain'}");
	EXPECT_ERROR(&a, "Component in use: e2e/Chain", "gg");
	all_quiet();

	event1("graph", "removenode", "{'id':'c','graph':'gg'}", NULL);
	event1("graph", "removegroup", "{'graph':'gg','name':'e2e/Chain'}",
	       "{'graph':'gg','name':'e2e/Chain'}");
	CHECK(!pal_storage_exists(e2e_path(e2e_path(e2e_basedir, "types"), "e2e%2FChain.json")));
	all_quiet();
	event1("runtime", "delete_graph", "{'id':'gg','name':'GG'}", NULL);
	close_clients();
}

/* ---- Component graphs (02 §3.4.3) ------------------------------------------------------ */

#define PUMP_TYPE_FILE e2e_path(e2e_path(e2e_basedir, "types"), "e2e%2FPump.json")

static void test_component_graphs(void)
{
	open_clients();

	/* graph:clear, main false: a new, empty component graph; the reply goes
	 * to the requester only, and no process appears (no new_graph, no
	 * status). */
	e2e_send(&a, "graph", "clear",
		 "{'id':'e2e/Pump','name':'Pump','library':'e2e','main':false,"
		 "'description':'A pump','icon':'cog'}");
	EXPECT(&a, "graph", "clear",
	       "{'id':'e2e/Pump','name':'Pump','library':'e2e','main':false,"
	       "'description':'A pump','icon':'cog'}");
	all_quiet();

	/* Built with the graph commands; their events as for any graph, but no
	 * status (it is no process). */
	event1("graph", "addnode", "{'id':'p','graph':'e2e/Pump','component':'math/Add2'}", NULL);
	event1("graph", "addnode", "{'id':'q','graph':'e2e/Pump','component':'math/Add2'}", NULL);
	event1("graph", "addedge",
	       "{'src':{'node':'p','port':'out'},'tgt':{'node':'q','port':'in1'},"
	       "'graph':'e2e/Pump'}",
	       NULL);
	event1("graph", "addinitial",
	       "{'tgt':{'node':'q','port':'in2'},'src':{'data':2},'graph':'e2e/Pump'}", NULL);
	event1x("graph", "addinport",
		"{'graph':'e2e/Pump','node':'p','port':'in1','public':'flow'}", "graph",
		"addinport", "{'graph':'e2e/Pump','name':'flow','node':'p','port':'in1'}");
	event1x("graph", "addoutport",
		"{'graph':'e2e/Pump','node':'q','port':'out','public':'total'}", "graph",
		"addoutport", "{'graph':'e2e/Pump','name':'total','node':'q','port':'out'}");
	all_quiet();

	/* It is no process. */
	e2e_send(&a, "network", "start", "{'graph':'e2e/Pump'}");
	EXPECT_ERROR(&a, "Not a process: e2e/Pump", "e2e/Pump");
	e2e_send(&a, "network", "getstatus", "{'graph':'e2e/Pump'}");
	EXPECT_ERROR(&a, "Not a process: e2e/Pump", "e2e/Pump");
	all_quiet();

	/* persist makes it the type: exactly the graph's ports, mapped to its
	 * blocks; component:component to everyone; the type file written. */
	e2e_send(&a, "network", "persist", "{'graph':'e2e/Pump'}");
	expect_all(&b, &v, "component", "component",
		   "{'name':'e2e/Pump','description':'A pump','subgraph':true,'icon':'cog',"
		   "'inPorts':[{'id':'flow','type':'number','description':'*',"
		   "'addressable':false}],'outPorts':[{'id':'total','type':'number',"
		   "'description':'*','addressable':false}]}");
	CHECK(pal_storage_exists(PUMP_TYPE_FILE));
	e2e_send(&v, "component", "getsource", "{'name':'e2e/Pump'}");
	const char *m = e2e_recv(&v);
	/* (e2e_payload_str's buffer holds the start of the code.) */
	static const char *const parts[] = {
		"\\\"initials\\\":[{\\\"src\\\":2,\\\"tgt\\\":\\\"q.in2\\\"}]",
		"\\\"inports\\\":[{\\\"name\\\":\\\"flow\\\",\\\"tgt\\\":\\\"p.in1\\\"}]",
		"\\\"outports\\\":[{\\\"name\\\":\\\"total\\\",\\\"src\\\":\\\"q.out\\\"}]",
	};
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
		CHECK(strstr(m, parts[i]) != NULL);

	/* graph:connect shows it, main false. */
	e2e_send(&v, "graph", "connect", "{'id':'e2e/Pump'}");
	EXPECT(&v, "graph", "clear",
	       "{'id':'e2e/Pump','name':'Pump','library':'e2e','main':false,"
	       "'description':'A pump','icon':'cog'}");
	for (int i = 0; i < 6; i++) /* 2 blocks, 2 ports, the initial, the edge */
		CHECK(e2e_recv(&v) != NULL);
	all_quiet();

	/* An instance in a process; while it exists the type is neither
	 * replaced nor deleted. */
	new_graph1("{'id':'pg','name':'PG'}", NULL);
	/* (v is scoped to e2e/Pump now: pg's block events do not reach it.) */
	e2e_send(&a, "graph", "addnode", "{'id':'c','graph':'pg','component':'e2e/Pump'}");
	expect_all(&b, NULL, "graph", "addnode", NULL);
	e2e_send(&a, "network", "persist", "{'graph':'e2e/Pump'}");
	EXPECT_ERROR(&a, "Component in use: e2e/Pump", "e2e/Pump");
	e2e_send(&a, "runtime", "delete_graph", "{'id':'e2e/Pump','name':'Pump'}");
	EXPECT_ERROR(&a, "Component in use: e2e/Pump", "e2e/Pump");
	all_quiet();
	e2e_send(&a, "graph", "removenode", "{'id':'c','graph':'pg'}");
	expect_all(&b, NULL, "graph", "removenode", NULL);
	all_quiet();

	/* Refusals of graph:clear. */
	e2e_send(&a, "graph", "clear", "{'id':'e2e/X','name':'X'}");
	EXPECT_ERROR(&a, "Invalid field: main", "e2e/X");
	e2e_send(&a, "graph", "clear", "{'id':'pg','main':false}");
	EXPECT_ERROR(&a, "Graph exists: pg", "pg");
	e2e_send(&a, "graph", "clear", "{'id':'math/Add2','main':false}");
	EXPECT_ERROR(&a, "Component exists: math/Add2", "math/Add2");
	e2e_send(&a, "graph", "clear", "{'id':'','main':false}");
	EXPECT_ERROR(&a, "Invalid id", NULL);
	/* An empty component graph is no type. */
	e2e_send(&a, "graph", "clear", "{'id':'e2e/Empty','main':false}");
	CHECK(e2e_match(e2e_recv(&a), "graph", "clear", NULL));
	e2e_send(&a, "network", "persist", "{'graph':'e2e/Empty'}");
	EXPECT_ERROR(&a, "Empty component: e2e/Empty", "e2e/Empty");
	/* graph_files: a process persists into graphs/, not a component. */
	e2e_send(&a, "runtime", "delete_graph", "{'id':'e2e/Empty','name':'<<new graph>>'}");
	expect_all(&b, &v, "runtime", "delete_graph",
		   "{'graph':'e2e/Empty','name':'<<new graph>>'}");
	all_quiet();

	/* runtime:delete_graph takes the type and its file with it. */
	e2e_send(&a, "runtime", "delete_graph", "{'id':'e2e/Pump','name':'Pump'}");
	expect_all(&b, &v, "runtime", "delete_graph", "{'graph':'e2e/Pump','name':'Pump'}");
	CHECK(!pal_storage_exists(PUMP_TYPE_FILE));
	e2e_send(&a, "graph", "addnode", "{'id':'d','graph':'pg','component':'e2e/Pump'}");
	EXPECT_ERROR(&a, "Unknown component: e2e/Pump", "pg");
	all_quiet();
	event1("runtime", "delete_graph", "{'id':'pg','name':'PG'}", NULL);
	close_clients();
}

/* ---- Refusals (02 §3.4.1) ------------------------------------------------------------- */

typedef struct {
	const char *protocol, *command, *payload, *message, *graph;
} Refusal;

static void test_refusals(void)
{
	open_clients();
	new_graph1("{'id':'r','name':'R'}", NULL);
	event1("graph", "addnode", "{'id':'n1','graph':'r','component':'math/Add2'}", NULL);
	event1("graph", "addnode", "{'id':'n2','graph':'r','component':'math/Add2'}", NULL);
	event1("graph", "addedge",
	       "{'src':{'node':'n1','port':'out'},'tgt':{'node':'n2','port':'in1'},'graph':'r'}",
	       NULL);
	event1("graph", "addinport", "{'graph':'r','node':'n1','port':'in1','public':'P'}", NULL);
	event1("graph", "addinport", "{'graph':'r','node':'n1','port':'in2','public':'R'}", NULL);
	event1("graph", "addoutport", "{'graph':'r','node':'n1','port':'out','public':'Q'}", NULL);
	char **before;
	int nb = snapshot(&b, "r", &before);
	CHECK(nb > 0);
	e2e_free_msgs(before, nb);
	int nbefore = snapshot(&v, "r", &before);

	char long_add[256];
	snprintf(long_add, sizeof(long_add), "{'id':'x','graph':'%065d','component':'math/Add2'}",
		 0);
	const Refusal cases[] = {
		{"graph", "addnode", "{'id':'x','graph':'zz','component':'math/Add2'}",
		 "Unknown graph: zz", "zz"},
		{"network", "start", "{'graph':'zz'}", "Unknown graph: zz", "zz"},
		{"graph", "removenode", "{'id':'zz','graph':'r'}", "Unknown node: zz", "r"},
		{"graph", "addedge",
		 "{'src':{'node':'n1','port':'nope'},'tgt':{'node':'n2','port':'in2'},'graph':'r'}",
		 "Unknown port: n1.nope", "r"},
		{"graph", "addnode", "{'id':'n1','graph':'r','component':'math/Add2'}",
		 "Node exists: n1", "r"},
		{"graph", "renamenode", "{'from':'n1','to':'n2','graph':'r'}", "Node exists: n2",
		 "r"},
		{"graph", "addnode", "{'id':'n9','graph':'r','component':'nope/X'}",
		 "Unknown component: nope/X", "r"},
		{"graph", "addedge",
		 "{'src':{'node':'n1','port':'in1'},'tgt':{'node':'n2','port':'in2'},'graph':'r'}",
		 "Invalid edge direction", "r"},
		{"graph", "addedge",
		 "{'src':{'node':'n2','port':'out'},'tgt':{'node':'n2','port':'in1'},'graph':'r'}",
		 "Input already connected: n2.in1", "r"},
		{"graph", "addinitial",
		 "{'tgt':{'node':'n2','port':'in1'},'src':{'data':1},'graph':'r'}",
		 "Input already connected: n2.in1", "r"},
		{"graph", "removeedge",
		 "{'src':{'node':'n2','port':'out'},'tgt':{'node':'n1','port':'in2'},'graph':'r'}",
		 "No such connection", "r"},
		{"graph", "changeedge",
		 "{'src':{'node':'n2','port':'out'},'tgt':{'node':'n1','port':'in2'},"
		 "'metadata':{},'graph':'r'}",
		 "No such connection", "r"},
		{"graph", "removeinitial", "{'tgt':{'node':'n1','port':'in2'},'graph':'r'}",
		 "No such connection", "r"},
		{"graph", "addinport", "{'graph':'r','node':'n2','port':'in2','public':'P'}",
		 "Port exists: P", "r"},
		{"graph", "addoutport", "{'graph':'r','node':'n2','port':'out','public':'Q'}",
		 "Port exists: Q", "r"},
		{"graph", "renameinport", "{'graph':'r','from':'P','to':'R'}", "Port exists: R",
		 "r"},
		{"graph", "renameoutport", "{'graph':'r','from':'Q','to':'Q'}", "Port exists: Q",
		 "r"},
		{"runtime", "delete_graph", "{'id':'r','name':'Wrong'}", "Graph name mismatch: r",
		 "r"},
		{"runtime", "new_graph", "{'id':'r'}", "Graph exists: r", "r"},
		{"graph", "addnode", "{'id':'','graph':'r','component':'math/Add2'}", "Invalid id",
		 "r"},
		{"graph", "addnode", long_add, "Invalid id", NULL},
		{"graph", "removeinport", "{'graph':'r','public':'nope'}", "Unknown port: nope",
		 "r"},
		{"graph", "removeoutport", "{'graph':'r','public':'nope'}", "Unknown port: nope",
		 "r"},
		{"graph", "renameinport", "{'graph':'r','from':'nope','to':'x'}",
		 "Unknown port: nope", "r"},
		{"graph", "renameoutport", "{'graph':'r','from':'nope','to':'x'}",
		 "Unknown port: nope", "r"},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const Refusal *r = &cases[i];
		int failures = test_failures;
		e2e_send(&a, r->protocol, r->command, r->payload);
		EXPECT_ERROR(&a, r->message, r->graph);
		all_quiet();
		fprintf(stderr, "%s refusal %s:%s -> %s\n",
			test_failures == failures ? "PASS" : "FAIL", r->protocol, r->command,
			r->message);
	}
	/* The graph is exactly as it was. */
	char **after;
	int nafter = snapshot(&v, "r", &after);
	CHECK(nbefore > 0 && same_msgs(before, nbefore, after, nafter));
	e2e_free_msgs(before, nbefore);
	e2e_free_msgs(after, nafter);
	all_quiet();
	event1("runtime", "delete_graph", "{'id':'r','name':'R'}", NULL);
	close_clients();
}

int main(void)
{
	e2e_setup("catalog");
	static Config cfg;
	e2e_config(&cfg);
	CHECK(config_set(&cfg, "max_connections", "8", "test") == CONFIG_OK);
	if (!e2e_start_with(&cfg, NULL)) {
		e2e_teardown();
		return 1;
	}
	RUN(test_login);
	RUN(test_login_failed);
	RUN(test_resume);
	RUN(test_not_authenticated);
	RUN(test_not_permitted);
	RUN(test_logout);
	RUN(test_envelope);
	RUN(test_runtime);
	RUN(test_component);
	RUN(test_trace);
	RUN(test_network);
	RUN(test_graph);
	RUN(test_groups);
	RUN(test_component_graphs);
	RUN(test_refusals);
	e2e_stop();
	e2e_teardown();
	return test_result();
}
