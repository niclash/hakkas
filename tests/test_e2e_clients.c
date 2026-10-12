/*
 * End-to-end multi-client and robustness tests (plan task 20, 05 §5, 02
 * §1.1, §0, §4), over real WebSocket connections to the whole engine started
 * in-process (e2e.h):
 *  - the command → event rule: two connections on the same graph get the
 *    identical event sequence (byte-identical frames) whoever sends, a
 *    conflict is won by the first command and the loser alone gets the
 *    error, a refused command produces no event on either;
 *  - scoping: a connection scoped to another graph gets the system-wide
 *    events, not the graph-specific ones; an unauthenticated connection
 *    gets nothing;
 *  - what only a real socket shows: the graph:connect snapshot against the
 *    real outgoing buffer (a snapshot exactly as large as an empty buffer
 *    takes, and one byte more: "Graph too large to send"), the snapshot
 *    deferred behind a reply in the buffer, network:data coalescing while
 *    the client does not read, and its replies and events still arriving
 *    meanwhile (the reserve), a users file edited while the engine runs;
 *  - robustness: an oversized message (1009), a message split across TCP
 *    writes and WebSocket fragments (with a ping between them), many
 *    messages pipelined in one TCP write, clients disconnecting mid-burst
 *    in both directions; the engine keeps serving the others;
 *  - log mirroring (log.mirror, 05 §4): network:output to the logged-in
 *    connections only.
 */
#include "e2e.h"

#include <math.h>
#include <stdlib.h>
#include <sys/socket.h>

#include "blocktypes/block_types.h"
#include "engine/ws_server.h"
#include "platform/pal.h"
#include "protocol/dispatcher.h"
#include "protocol/users.h"

/* ---- Helpers ------------------------------------------------------------------ */

/* The next message on x and on y, the very same frame on both: x's text. */
static const char *same_next(E2eClient *x, E2eClient *y)
{
	const char *m = e2e_recv(x);
	CHECK(m != NULL && e2e_recv(y) != NULL);
	bool same = x->frame != NULL && y->frame != NULL && x->frame_len == y->frame_len &&
		    memcmp(x->frame, y->frame, x->frame_len) == 0;
	if (!same)
		fprintf(stderr, "  frames differ: %s / %s\n", m ? m : "-", y->text ? y->text : "-");
	CHECK(same);
	return m;
}

/* Whether msg is protocol:command (quietly, unlike e2e_match). */
static bool is(const char *msg, const char *protocol, const char *command)
{
	char expected[128];
	snprintf(expected, sizeof(expected), "{'protocol':'%s','command':'%s','payload':'*'}",
		 protocol, command);
	return e2e_json_eq(msg, expected);
}

static bool connect_login(E2eClient *c, const char *user)
{
	bool ok = e2e_connect(c) && e2e_login(c, user);
	CHECK(ok);
	return ok;
}

/* c scoped to graph id (its snapshot read). */
static void scope(E2eClient *c, const char *id)
{
	char payload[128];
	snprintf(payload, sizeof(payload), "{'id':'%s'}", id);
	e2e_send(c, "graph", "connect", payload);
	char **msgs;
	int n = e2e_collect(c, &msgs);
	CHECK(n >= 1 && e2e_match(msgs[0], "graph", "clear", NULL));
	e2e_free_msgs(msgs, n);
}

/* The bytes the dispatcher counts for these messages against the free
 * outgoing space (02 §1.2 "Bursts": PROTO_FRAME_OVERHEAD per message). */
static size_t burst_need(char **msgs, int n)
{
	size_t need = 0;
	for (int i = 0; i < n; i++)
		need += strlen(msgs[i]) + PROTO_FRAME_OVERHEAD;
	return need;
}

/* The free space of an empty upgraded connection's outgoing buffer as
 * ws_send_space reports it: WS_OUT_CAP less the largest frame header a
 * message of that size needs (02 §0, engine/ws_server.c). */
#define EMPTY_SPACE (WS_OUT_CAP - 4)

/* ---- Command → event (02 §1.1) ---------------------------------------------------- */

static void test_same_events(void)
{
	E2eClient a, b;
	connect_login(&a, "alice");
	connect_login(&b, "bob");
	e2e_send(&a, "runtime", "new_graph", "{'id':'m','name':'M'}");
	same_next(&a, &b);
	scope(&a, "m");
	scope(&b, "m");

	/* Commands from a, then from b: the same sequence on both. */
	e2e_send(&a, "graph", "addnode", "{'id':'x','graph':'m','component':'math/Add2'}");
	CHECK(e2e_match(same_next(&a, &b), "graph", "addnode", NULL));
	e2e_send(&b, "graph", "addnode", "{'id':'y','graph':'m','component':'math/Add2'}");
	CHECK(e2e_match(same_next(&a, &b), "graph", "addnode", NULL));
	e2e_send(&a, "graph", "addedge",
		 "{'src':{'node':'x','port':'out'},'tgt':{'node':'y','port':'in1'},'graph':'m'}");
	CHECK(e2e_match(same_next(&a, &b), "graph", "addedge", NULL));
	e2e_send(&b, "graph", "addinitial",
		 "{'tgt':{'node':'x','port':'in1'},'src':{'data':3},'graph':'m'}");
	CHECK(e2e_match(same_next(&a, &b), "graph", "addinitial", NULL));
	e2e_send(&b, "network", "start", "{'graph':'m'}");
	CHECK(e2e_match(same_next(&a, &b), "network", "started", NULL));
	/* A compound effect: its parts in the same order on both. */
	e2e_send(&a, "graph", "removenode", "{'id':'x','graph':'m'}");
	CHECK(e2e_match(same_next(&a, &b), "graph", "removeinitial", NULL));
	CHECK(e2e_match(same_next(&a, &b), "graph", "removeedge", NULL));
	CHECK(e2e_match(same_next(&a, &b), "graph", "removenode", NULL));
	QUIET(&a);
	QUIET(&b);

	/* A conflict (02 §1.1): both add node z in the same instant. The
	 * first wins and is broadcast to both; the other's sender alone gets
	 * the error. */
	char *ma = e2e_envelope("graph", "addnode",
				"{'id':'z','graph':'m','component':'math/Add2'}", a.secret);
	char *mb = e2e_envelope("graph", "addnode",
				"{'id':'z','graph':'m','component':'math/Subtract'}", b.secret);
	e2e_send_text(&a, ma);
	e2e_send_text(&b, mb);
	free(ma);
	free(mb);
	char **ra, **rb;
	int na = e2e_collect(&a, &ra), nb = e2e_collect(&b, &rb);
	static const char add[] = "{'protocol':'graph','command':'addnode','payload':'*'}";
	static const char refused[] = "{'protocol':'network','command':'error','payload':"
				      "{'stack':'','message':'Node exists: z','graph':'m'}}";
	int adds = 0, errors_a = 0, errors_b = 0;
	for (int i = 0; i < na; i++) {
		adds += e2e_json_eq(ra[i], add);
		errors_a += e2e_json_eq(ra[i], refused);
	}
	for (int i = 0; i < nb; i++)
		errors_b += e2e_json_eq(rb[i], refused);
	CHECK(adds == 1 && errors_a + errors_b == 1 && na + nb == 3);
	/* The event is the same on both, and names the winner's component. */
	const char *event_a = NULL, *event_b = NULL;
	for (int i = 0; i < na; i++)
		if (strstr(ra[i], "\"command\":\"addnode\"") != NULL)
			event_a = ra[i];
	for (int i = 0; i < nb; i++)
		if (strstr(rb[i], "\"command\":\"addnode\"") != NULL)
			event_b = rb[i];
	CHECK(event_a != NULL && event_b != NULL && strcmp(event_a, event_b) == 0);
	if (event_a != NULL)
		CHECK(strstr(event_a, errors_b ? "math/Add2" : "math/Subtract") != NULL);
	e2e_free_msgs(ra, na);
	e2e_free_msgs(rb, nb);

	/* A refused command: no event on either, the error on the sender. */
	e2e_send(&b, "graph", "addedge",
		 "{'src':{'node':'y','port':'in1'},'tgt':{'node':'z','port':'in1'},'graph':'m'}");
	EXPECT_ERROR(&b, "Invalid edge direction", "m");
	QUIET(&a);
	QUIET(&b);
	e2e_send(&a, "runtime", "delete_graph", "{'id':'m','name':'M'}");
	CHECK(e2e_match(same_next(&a, &b), "network", "stopped", NULL));
	CHECK(e2e_match(same_next(&a, &b), "runtime", "delete_graph", NULL));
	e2e_close(&a);
	e2e_close(&b);
}

/* ---- Scoping (02 §4.1, §2.5 item 6) ----------------------------------------------- */

static void test_scoping(void)
{
	E2eClient a, b, c, u;
	connect_login(&a, "alice");
	connect_login(&b, "bob");
	connect_login(&c, "vera");
	CHECK(e2e_connect(&u)); /* never logs in */
	e2e_send(&a, "runtime", "new_graph", "{'id':'s1','name':'S1'}");
	same_next(&a, &b);
	CHECK(e2e_expect(&c, "runtime", "new_graph", NULL));
	e2e_send(&a, "runtime", "new_graph", "{'id':'s2','name':'S2'}");
	same_next(&a, &b);
	CHECK(e2e_expect(&c, "runtime", "new_graph", NULL));
	scope(&a, "s1");
	scope(&b, "s1");
	scope(&c, "s2");

	/* Graph-specific events of s1: a and b, not c. */
	e2e_send(&a, "graph", "addnode", "{'id':'n','graph':'s1','component':'math/Add2'}");
	CHECK(e2e_match(same_next(&a, &b), "graph", "addnode", NULL));
	e2e_send(&a, "graph", "changenode", "{'id':'n','metadata':{'x':5},'graph':'s1'}");
	CHECK(e2e_match(same_next(&a, &b), "graph", "changenode", NULL));
	e2e_send(&a, "graph", "rename", "{'id':'s1','to':'S1b'}");
	CHECK(e2e_match(same_next(&a, &b), "graph", "rename", NULL));
	e2e_send(&a, "network", "start", "{'graph':'s1'}");
	CHECK(e2e_match(same_next(&a, &b), "network", "started", NULL));
	QUIET(&c);
	/* System-wide events reach c too: status, new_graph, delete_graph. */
	e2e_send(&a, "network", "getstatus", "{'graph':'s1'}");
	const char *m = same_next(&a, &b);
	CHECK(e2e_match(m, "network", "status", NULL));
	CHECK(e2e_recv(&c) != NULL && c.frame_len == a.frame_len &&
	      memcmp(c.frame, a.frame, a.frame_len) == 0);
	e2e_send(&b, "runtime", "new_graph", "{'id':'s3','name':'S3'}");
	CHECK(e2e_match(same_next(&a, &b), "runtime", "new_graph", NULL));
	EXPECT(&c, "runtime", "new_graph", "{'graph':'s3','name':'S3','description':'','icon':''}");
	e2e_send(&b, "runtime", "delete_graph", "{'id':'s3','name':'S3'}");
	CHECK(e2e_match(same_next(&a, &b), "runtime", "delete_graph", NULL));
	EXPECT(&c, "runtime", "delete_graph", "{'graph':'s3','name':'S3'}");
	/* c's own graph: c alone. */
	e2e_send(&c, "network", "getstatus", "{'graph':'s2'}");
	CHECK(e2e_match(same_next(&a, &b), "network", "status", NULL));
	EXPECT(&c, "network", "status", NULL);
	/* Re-scoped to s1, c gets s1's events from its snapshot on. */
	scope(&c, "s1");
	e2e_send(&a, "network", "stop", "{'graph':'s1'}");
	CHECK(e2e_match(same_next(&a, &b), "network", "stopped", NULL));
	EXPECT(&c, "network", "stopped", NULL);
	CHECK(c.frame_len == a.frame_len && memcmp(c.frame, a.frame, a.frame_len) == 0);
	QUIET(&a);
	QUIET(&b);
	QUIET(&c);
	/* The unauthenticated connection got none of it. */
	QUIET(&u);
	e2e_send(&a, "runtime", "delete_graph", "{'id':'s1','name':'S1b'}");
	same_next(&a, &b);
	EXPECT(&c, "runtime", "delete_graph", NULL);
	e2e_send(&a, "runtime", "delete_graph", "{'id':'s2','name':'S2'}");
	same_next(&a, &b);
	EXPECT(&c, "runtime", "delete_graph", NULL);
	QUIET(&u);
	e2e_close(&a);
	e2e_close(&b);
	e2e_close(&c);
	e2e_close(&u);
}

/* ---- The outgoing buffer (02 §0, §1.2, §4.1.1, §4.2) ------------------------------- */

/* The snapshot of id taken by c: its message count, the burst bytes; then
 * c is scoped back to "quiet". */
static int measure(E2eClient *c, const char *id, size_t *need)
{
	char payload[128];
	snprintf(payload, sizeof(payload), "{'id':'%s'}", id);
	e2e_send(c, "graph", "connect", payload);
	char **msgs;
	int n = e2e_collect(c, &msgs);
	*need = n > 0 ? burst_need(msgs, n) : 0;
	CHECK(n > 0 && e2e_match(msgs[0], "graph", "clear", NULL));
	e2e_free_msgs(msgs, n);
	scope(c, "quiet");
	return n;
}

/* Sets node n0's opaque metadata key "p" of graph big to len characters. */
static void pad(E2eClient *w, size_t len)
{
	char *payload = malloc(len + 128);
	CHECK(payload != NULL);
	int k = snprintf(payload, len + 128, "{'id':'n0','graph':'big','metadata':{'p':'");
	memset(payload + k, 'x', len);
	snprintf(payload + k + len, 64, "'}}");
	e2e_send(w, "graph", "changenode", payload);
	free(payload);
}

/* count more nodes n<nodes>... in graph big from w, pipelined. */
static void add_nodes(E2eClient *w, int *nodes, int count)
{
	uint8_t *wire = malloc((size_t)count * 256);
	CHECK(wire != NULL);
	size_t sent = 0;
	for (int i = 0; i < count; i++, (*nodes)++) {
		char payload[128];
		snprintf(payload, sizeof(payload),
			 "{'id':'n%d','graph':'big','component':'math/Add2'}", *nodes);
		char *msg = e2e_envelope("graph", "addnode", payload, w->secret);
		sent += e2e_frame(wire + sent, WS_OP_TEXT, true, msg, strlen(msg));
		free(msg);
	}
	e2e_send_bytes(w, wire, sent);
	free(wire);
	QUIET(w);
}

static void test_snapshot_buffer(void)
{
	/* w builds the graph, m measures; both scoped to the empty graph
	 * "quiet", so neither gets big's events. */
	E2eClient w, m;
	connect_login(&w, "alice");
	connect_login(&m, "bob");
	e2e_send(&w, "runtime", "new_graph", "{'id':'quiet','name':'Q'}");
	same_next(&w, &m);
	e2e_send(&w, "runtime", "new_graph", "{'id':'big','name':'Big'}");
	same_next(&w, &m);
	scope(&w, "quiet");
	scope(&m, "quiet");

	/* Nodes until the snapshot is less than 1 KiB under the limit. */
	size_t need = 0;
	int nodes = 0;
	add_nodes(&w, &nodes, 100);
	measure(&m, "big", &need);
	for (int i = 0; i < 4 && need + 1000 < EMPTY_SPACE; i++) {
		size_t per_node = need / (size_t)nodes + 1;
		add_nodes(&w, &nodes, (int)((EMPTY_SPACE - need - 500) / per_node));
		measure(&m, "big", &need);
	}
	CHECK(need < EMPTY_SPACE - 7);

	/* Exactly the space of an empty buffer: the snapshot arrives whole. */
	pad(&w, EMPTY_SPACE - need - 7);
	QUIET(&w);
	size_t full = 0;
	int n = measure(&m, "big", &full);
	CHECK(full == EMPTY_SPACE && n == nodes + 1);

	/* The snapshot deferred behind a reply in the buffer (02 §4.1.1): the
	 * component list and the connect in one write, so the list is still in
	 * the buffer when the connect is handled; the snapshot follows when the
	 * buffer has drained, whole. */
	char *list = e2e_envelope("component", "list", "{}", m.secret);
	char *conn = e2e_envelope("graph", "connect", "{'id':'big'}", m.secret);
	uint8_t frames[512];
	size_t len = e2e_frame(frames, WS_OP_TEXT, true, list, strlen(list));
	len += e2e_frame(frames + len, WS_OP_TEXT, true, conn, strlen(conn));
	free(list);
	free(conn);
	e2e_send_bytes(&m, frames, len);
	char **msgs;
	int got = e2e_collect(&m, &msgs);
	size_t types = block_types_count();
	CHECK(got == (int)types + 1 + nodes + 1);
	if (got == (int)types + 1 + nodes + 1) {
		CHECK(e2e_match(msgs[types], "component", "componentsready", NULL));
		CHECK(e2e_match(msgs[types + 1], "graph", "clear", NULL));
		CHECK(burst_need(msgs + types + 1, nodes + 1) == EMPTY_SPACE);
	}
	e2e_free_msgs(msgs, got);
	scope(&m, "quiet");

	/* One byte more: "Graph too large to send", the connection open and
	 * still scoped to quiet. */
	pad(&w, EMPTY_SPACE - need - 7 + 1);
	QUIET(&w);
	e2e_send(&m, "graph", "connect", "{'id':'big'}");
	EXPECT_ERROR(&m, "Graph too large to send: big", "big");
	QUIET(&m);
	e2e_send(&w, "graph", "changenode", "{'id':'n1','metadata':{'x':1},'graph':'big'}");
	QUIET(&w);
	QUIET(&m);
	e2e_send(&w, "graph", "addnode", "{'id':'q','graph':'quiet','component':'math/Add2'}");
	CHECK(e2e_match(same_next(&w, &m), "graph", "addnode", NULL));

	e2e_send(&w, "runtime", "delete_graph", "{'id':'big','name':'Big'}");
	CHECK(e2e_match(same_next(&w, &m), "runtime", "delete_graph", NULL));
	e2e_send(&w, "runtime", "delete_graph", "{'id':'quiet','name':'Q'}");
	CHECK(e2e_match(same_next(&w, &m), "runtime", "delete_graph", NULL));
	e2e_close(&w);
	e2e_close(&m);
}

/* A test type with a string input, so a watched value can be large: the
 * value is a string port's initial, taken as-is (02 §3.4 "Initial value
 * encoding"). */
static int64_t text_compute(BlockCtx *ctx)
{
	block_ctx_set_output(ctx, 0, linkable_copy(&ctx->inputs[0]));
	return 0;
}

static const PortDescriptor text_inputs[] = {
	{.name = "in", .type = "string", .default_value = LINKABLE_EMPTY_STRING_INIT},
};

static const PortDescriptor text_outputs[] = {
	{.name = "out", .type = "string", .default_value = LINKABLE_EMPTY_STRING_INIT},
};

static const BlockTypeDef text_def = {
	.name = "test/Text",
	.inputs = text_inputs,
	.input_count = 1,
	.outputs = text_outputs,
	.output_count = 1,
	.compute = text_compute,
	.change_threshold = NAN,
};

/* Changes from to to of co's n.in from e while the watcher does not read:
 * batches of initials, each the string "<k> " and pad_len characters. With
 * PAD, every network:data takes about 1 KiB and the socket's and the
 * engine's buffers fill after a few thousand. */
enum { VALUES = 4000, BATCH = 200, PAD = 900 };

static void stream_values(E2eClient *e, int from, int to, int pad_len)
{
	char pad[PAD + 1];
	memset(pad, 'u', (size_t)pad_len);
	pad[pad_len] = '\0';
	for (int i = from; i <= to; i += BATCH) {
		size_t sent = 0;
		uint8_t *batch = malloc((size_t)BATCH * (PAD + 256));
		CHECK(batch != NULL);
		for (int k = i; k < i + BATCH && k <= to; k++) {
			char payload[PAD + 128];
			snprintf(payload, sizeof(payload),
				 "{'tgt':{'node':'n','port':'in'},'src':{'data':'%d %s'},"
				 "'graph':'co'}",
				 k, pad);
			char *msg = e2e_envelope("graph", "addinitial", payload, e->secret);
			sent += e2e_frame(batch + sent, WS_OP_TEXT, true, msg, strlen(msg));
			free(msg);
		}
		e2e_send_bytes(e, batch, sent);
		free(batch);
		QUIET(e);
	}
}

/* Graphs co (node n, a test/Text) and co-quiet (node q), e and w scoped to
 * co-quiet (so neither gets co's events), w watching n.in (its first value
 * read). */
static void stream_setup(E2eClient *e, E2eClient *w)
{
	if (block_types_get(text_def.name) == NULL)
		CHECK(block_types_register(&text_def) == BLOCK_TYPES_OK);
	e2e_send(e, "runtime", "new_graph", "{'id':'co','name':'Co'}");
	same_next(e, w);
	e2e_send(e, "runtime", "new_graph", "{'id':'co-quiet','name':'Q'}");
	same_next(e, w);
	scope(e, "co-quiet");
	scope(w, "co-quiet");
	e2e_send(e, "graph", "addnode", "{'id':'n','graph':'co','component':'test/Text'}");
	QUIET(e);
	e2e_send(e, "graph", "addnode", "{'id':'q','graph':'co-quiet','component':'math/Add2'}");
	CHECK(e2e_match(same_next(e, w), "graph", "addnode", NULL));
	e2e_send(w, "network", "edges",
		 "{'graph':'co','edges':[{'src':{'node':'s','port':'out'},'tgt':{'node':'n',"
		 "'port':'in'}}]}");
	CHECK(e2e_expect(w, "network", "edges", NULL));
	EXPECT(w, "network", "data",
	       "{'id':'unknown','graph':'co','src':{'node':'s','port':'out'},'tgt':{'node':'n',"
	       "'port':'in'},'data':''}");
}

static void stream_teardown(E2eClient *e, E2eClient *w)
{
	QUIET(w);
	e2e_send(e, "runtime", "delete_graph", "{'id':'co','name':'Co'}");
	CHECK(e2e_match(same_next(e, w), "runtime", "delete_graph", NULL));
	e2e_send(e, "runtime", "delete_graph", "{'id':'co-quiet','name':'Q'}");
	CHECK(e2e_match(same_next(e, w), "runtime", "delete_graph", NULL));
	e2e_close(e);
	e2e_close(w);
}

/* network:data under back-pressure (02 §4.2): a watcher that does not read
 * misses intermediate values but ends with the current one, and stays
 * connected. */
static void test_data_coalescing(void)
{
	E2eClient e, w;
	connect_login(&e, "alice");
	CHECK(e2e_connect_rcvbuf(&w, 4096) && e2e_login(&w, "vera"));
	stream_setup(&e, &w);
	stream_values(&e, 1, VALUES, PAD);
	/* Now w reads: increasing values, fewer than the changes, until the
	 * current one, sent when the buffer has drained. */
	int n = 0;
	long last = 0;
	bool increasing = true;
	while (last != VALUES && n <= VALUES) {
		const char *m = e2e_recv(&w);
		if (!e2e_match(m, "network", "data", NULL))
			break;
		long value = strtol(e2e_payload_str(m, "data"), NULL, 10);
		increasing = increasing && value > last;
		last = value;
		n++;
	}
	CHECK(increasing);
	CHECK(last == VALUES);
	CHECK(n < VALUES);
	stream_teardown(&e, &w);
}

/* Replies and events while values stream into a watcher that does not read
 * (02 §0, §4.2 "Back-pressure", plan task 20 fix): the values leave the
 * outgoing buffer's reserve free, so w's own commands and an unrelated
 * event still fit; w stays connected, gets every reply and event, then the
 * current value. */
static void test_data_stream_replies(void)
{
	E2eClient e, w;
	connect_login(&e, "alice");
	CHECK(e2e_connect_rcvbuf(&w, 4096) && e2e_login(&w, "bob"));
	stream_setup(&e, &w);
	/* The buffers filled with large values, then the rest of the engine's
	 * buffer with small ones (about 150 bytes each): without the reserve,
	 * less free space would be left than the replies and the event below
	 * take together, and the first that did not fit would close w. */
	stream_values(&e, 1, VALUES - 1000, PAD);
	stream_values(&e, VALUES - 1000 + 1, VALUES, 1);
	/* w, still not reading, sends commands; e makes an unrelated event. */
	e2e_send(&w, "network", "getstatus", "{'graph':'co'}");
	e2e_send(&w, "graph", "changenode", "{'id':'q','metadata':{'x':5},'graph':'co-quiet'}");
	EXPECT(&e, "network", "status", NULL);	 /* a broadcast */
	EXPECT(&e, "graph", "changenode", NULL); /* e is scoped to co-quiet too */
	e2e_send(&e, "runtime", "new_graph", "{'id':'co-other','name':'O'}");
	EXPECT(&e, "runtime", "new_graph", NULL);
	QUIET(&e);
	/* Now w reads: values, the replies and events, the current value. */
	int n = 0, status = 0, changed = 0, created = 0, others = 0;
	long last = 0;
	bool increasing = true;
	while (!(last == VALUES && status && changed && created) && n <= VALUES + 3) {
		const char *m = e2e_recv(&w);
		if (m == NULL)
			break;
		n++;
		if (is(m, "network", "data")) {
			long value = strtol(e2e_payload_str(m, "data"), NULL, 10);
			increasing = increasing && value > last;
			last = value;
		} else if (is(m, "network", "status") &&
			   strcmp(e2e_payload_str(m, "graph"), "co") == 0) {
			status++;
		} else if (is(m, "graph", "changenode") &&
			   strcmp(e2e_payload_str(m, "id"), "q") == 0) {
			changed++;
		} else if (is(m, "runtime", "new_graph")) {
			created++;
		} else {
			fprintf(stderr, "  unexpected: %s\n", m);
			others++;
		}
	}
	CHECK(!w.closed && !w.eof);
	CHECK(status == 1 && changed == 1 && created == 1 && others == 0);
	CHECK(increasing);
	CHECK(last == VALUES);
	CHECK(n < VALUES); /* the values were coalesced: the buffer was full */
	e2e_send(&e, "runtime", "delete_graph", "{'id':'co-other','name':'O'}");
	CHECK(e2e_match(same_next(&e, &w), "runtime", "delete_graph", NULL));
	stream_teardown(&e, &w);
}

/* ---- Users file (02 §2.2 "When it is read") ---------------------------------------- */

static void test_users_edit(void)
{
	E2eClient bob, c;
	connect_login(&bob, "bob");
	/* carol added and bob removed while the engine runs. */
	const char *file = e2e_path(e2e_confdir, "users.json");
	char saved[16384];
	size_t len = 0;
	CHECK(pal_storage_read(file, saved, sizeof(saved) - 1, &len) == PAL_OK);
	saved[len] = '\0';
	static UserStore users;
	CHECK(users_load(&users, file) == USERS_OK);
	CHECK(users_remove(&users, "bob") == USERS_OK);
	CHECK(users_add(&users, "carol", USER_ROLE_EDITOR, E2E_PASSWORD, strlen(E2E_PASSWORD),
			E2E_ITERATIONS, pal_random_bytes) == USERS_OK);
	CHECK(users_save(&users, file) == USERS_OK);

	/* The next challenge sees it: bob is unknown now (the default
	 * iteration count, not the test users' 64), carol logs in; bob's open
	 * session goes on. */
	CHECK(e2e_connect(&c));
	e2e_send(&c, "environment", "challenge",
		 "{'user':'bob','cnonce':'MDEyMzQ1Njc4OWFiY2RlZg=='}");
	EXPECT(&c, "environment", "challenge", "{'salt':'*','iterations':100000,'nonce':'*'}");
	CHECK(e2e_login(&c, "carol"));
	e2e_send(&bob, "trace", "start", "{'graph':'x'}");
	EXPECT(&bob, "trace", "start", "{'graph':'x'}");
	e2e_close(&c);
	e2e_close(&bob);
	e2e_put(file, saved);
}

/* ---- Robustness (02 §0) ------------------------------------------------------------ */

static void test_oversized(void)
{
	E2eClient a, x;
	connect_login(&a, "alice");
	CHECK(e2e_connect(&x));
	/* One byte over max_message_bytes (16 KiB): close 1009. */
	size_t max = hakkas_config(e2e_hakkas)->max_message_bytes;
	char *big = malloc(max + 2);
	CHECK(big != NULL);
	memset(big, ' ', max + 1);
	big[0] = '{';
	big[max] = '}';
	big[max + 1] = '\0';
	e2e_send_text(&x, big);
	CHECK(e2e_expect_close(&x, WS_CLOSE_TOO_BIG));
	e2e_close(&x);
	/* The same, fragmented: the total counts. */
	CHECK(e2e_connect(&x));
	uint8_t *f = malloc(max + 64);
	CHECK(f != NULL);
	size_t half = max / 2 + 1;
	size_t len = e2e_frame(f, WS_OP_TEXT, false, big, half);
	e2e_send_bytes(&x, f, len);
	len = e2e_frame(f, WS_OP_CONTINUATION, true, big + half, half);
	e2e_send_bytes(&x, f, len);
	CHECK(e2e_expect_close(&x, WS_CLOSE_TOO_BIG));
	e2e_close(&x);
	/* Exactly max_message_bytes passes (a badly formatted request). */
	CHECK(e2e_connect(&x));
	big[max] = '\0';
	e2e_send_text(&x, big);
	const char *m = e2e_recv(&x);
	CHECK(e2e_match(m, "network", "error", NULL));
	CHECK(m != NULL && strstr(m, "Badly formatted request: {") != NULL);
	e2e_close(&x);
	free(f);
	free(big);
	/* a is served as before. */
	e2e_send(&a, "trace", "start", "{'graph':'ok'}");
	EXPECT(&a, "trace", "start", "{'graph':'ok'}");
	QUIET(&a);
	e2e_close(&a);
}

static void test_fragmented(void)
{
	E2eClient a;
	connect_login(&a, "alice");
	/* One command in three WebSocket fragments with a ping between them,
	 * each fragment sent in pieces of 3 bytes, the loop running between
	 * the pieces. */
	char *msg = e2e_envelope("trace", "dump", "{'graph':'fragmented'}", a.secret);
	size_t len = strlen(msg), third = len / 3;
	uint8_t wire[1024];
	size_t n = e2e_frame(wire, WS_OP_TEXT, false, msg, third);
	n += e2e_frame(wire + n, WS_OP_PING, true, "hi", 2);
	n += e2e_frame(wire + n, WS_OP_CONTINUATION, false, msg + third, third);
	n += e2e_frame(wire + n, WS_OP_CONTINUATION, true, msg + 2 * third, len - 2 * third);
	free(msg);
	for (size_t i = 0; i < n; i += 3) {
		e2e_send_bytes(&a, wire + i, n - i < 3 ? n - i : 3);
		e2e_iterate(1);
	}
	EXPECT(&a, "trace", "dump",
	       "{'graph':'fragmented','type':'flowtrace.json','flowtrace':''}");
	CHECK(a.pongs == 1);
	QUIET(&a);
	e2e_close(&a);
}

static void test_pipelined(void)
{
	E2eClient a, b;
	connect_login(&a, "alice");
	connect_login(&b, "bob");
	e2e_send(&a, "runtime", "new_graph", "{'id':'pipe','name':'P'}");
	same_next(&a, &b);
	scope(&a, "pipe");
	scope(&b, "pipe");
	/* 200 commands in one write: replies in order, events in order and
	 * identical on both. */
	enum { COUNT = 200 };
	uint8_t *wire = malloc(COUNT * 256);
	CHECK(wire != NULL);
	size_t n = 0;
	for (int i = 0; i < COUNT; i++) {
		char payload[128];
		if (i % 2 == 0)
			snprintf(payload, sizeof(payload), "{'graph':'t%d'}", i);
		else
			snprintf(payload, sizeof(payload),
				 "{'id':'n%d','graph':'pipe','component':'math/Add2'}", i);
		char *msg = e2e_envelope(i % 2 == 0 ? "trace" : "graph",
					 i % 2 == 0 ? "start" : "addnode", payload, a.secret);
		n += e2e_frame(wire + n, WS_OP_TEXT, true, msg, strlen(msg));
		free(msg);
	}
	e2e_send_bytes(&a, wire, n);
	free(wire);
	for (int i = 0; i < COUNT; i++) {
		char want[128];
		if (i % 2 == 0) {
			snprintf(want, sizeof(want), "{'graph':'t%d'}", i);
			EXPECT(&a, "trace", "start", want);
		} else {
			const char *m = same_next(&a, &b);
			snprintf(want, sizeof(want),
				 "{'graph':'pipe','component':'math/Add2','id':'n%d','metadata':"
				 "{'x':50,'y':50}}",
				 i);
			CHECK(e2e_match(m, "graph", "addnode", want));
		}
	}
	QUIET(&a);
	QUIET(&b);
	e2e_send(&a, "runtime", "delete_graph", "{'id':'pipe','name':'P'}");
	CHECK(e2e_match(same_next(&a, &b), "runtime", "delete_graph", NULL));
	e2e_close(&a);
	e2e_close(&b);
}

static void test_disconnect_mid_burst(void)
{
	E2eClient a, b, x;
	connect_login(&a, "alice");
	connect_login(&b, "bob");
	e2e_send(&a, "runtime", "new_graph", "{'id':'burst','name':'B'}");
	same_next(&a, &b);
	scope(&a, "burst");
	scope(&b, "burst");

	/* x sends a burst of commands and is gone before the engine reads
	 * them all (a reset: SO_LINGER 0). */
	connect_login(&x, "alice");
	enum { COUNT = 400 };
	uint8_t *wire = malloc(COUNT * 256);
	CHECK(wire != NULL);
	size_t n = 0;
	for (int i = 0; i < COUNT; i++) {
		char payload[128];
		snprintf(payload, sizeof(payload),
			 "{'id':'x%d','graph':'burst','component':'math/Add2'}", i);
		char *msg = e2e_envelope("graph", "addnode", payload, x.secret);
		n += e2e_frame(wire + n, WS_OP_TEXT, true, msg, strlen(msg));
		free(msg);
	}
	e2e_send_bytes(&x, wire, n);
	free(wire);
	struct linger lg = {1, 0};
	CHECK(setsockopt(x.fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0);
	e2e_close(&x);
	e2e_iterate(10); /* the engine handles what it got, and the reset */
	/* Whatever of it was applied reached a and b alike, in order. */
	e2e_send(&a, "graph", "addnode", "{'id':'after','graph':'burst','component':'math/Add2'}");
	int applied = 0;
	for (;;) {
		const char *m = same_next(&a, &b);
		if (m == NULL || strstr(m, "\"id\":\"after\"") != NULL)
			break;
		char want[160];
		snprintf(want, sizeof(want),
			 "{'graph':'burst','component':'math/Add2','id':'x%d','metadata':'*'}",
			 applied++);
		CHECK(e2e_match(m, "graph", "addnode", want));
	}
	CHECK(applied <= COUNT);

	/* y asks for replies and stops reading, then resets: the engine's
	 * outgoing buffer for it is cut off mid-burst. */
	E2eClient y;
	CHECK(e2e_connect_rcvbuf(&y, 2048) && e2e_login(&y, "vera"));
	char *list = e2e_envelope("component", "list", "{}", y.secret);
	for (int i = 0; i < 30; i++) {
		e2e_send_text(&y, list);
		e2e_iterate(1);
	}
	free(list);
	CHECK(setsockopt(y.fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0);
	e2e_close(&y);
	e2e_iterate(3);

	/* The engine keeps serving a and b. */
	e2e_send(&b, "graph", "removenode", "{'id':'after','graph':'burst'}");
	CHECK(e2e_match(same_next(&a, &b), "graph", "removenode", NULL));
	QUIET(&a);
	QUIET(&b);
	e2e_send(&a, "runtime", "delete_graph", "{'id':'burst','name':'B'}");
	CHECK(e2e_match(same_next(&a, &b), "runtime", "delete_graph", NULL));
	e2e_close(&a);
	e2e_close(&b);
}

/* ---- Log mirroring (05 §4, 02 §3.3 network:output) -------------------------------- */

static void test_log_mirror(void)
{
	/* The engine again, with log.mirror on. */
	e2e_stop();
	static Config cfg;
	e2e_config(&cfg);
	CHECK(config_set(&cfg, "log.mirror", "true", "test") == CONFIG_OK);
	CHECK(e2e_start_with(&cfg, NULL));
	if (e2e_hakkas == NULL)
		return;
	E2eClient a, b, u;
	connect_login(&a, "alice");
	CHECK(e2e_connect(&u));
	char **msgs;
	int n = e2e_collect(&a, &msgs); /* lines logged meanwhile */
	e2e_free_msgs(msgs, n);
	/* bob's login is logged, and mirrored to a as network:output: the
	 * line as formatted, never a secret; u (not logged in) gets nothing. */
	connect_login(&b, "bob");
	n = e2e_collect(&a, &msgs);
	int logins = 0;
	for (int i = 0; i < n; i++) {
		CHECK(e2e_match(msgs[i], "network", "output", "{'type':'message','message':'*'}"));
		const char *line = e2e_payload_str(msgs[i], "message");
		CHECK(strncmp(line, "info ", 5) == 0 || strncmp(line, "warn ", 5) == 0);
		CHECK(strstr(msgs[i], b.secret) == NULL);
		logins += strstr(line, "login: user 'bob' from 127.0.0.1 as editor") != NULL;
	}
	CHECK(logins == 1);
	e2e_free_msgs(msgs, n);
	QUIET(&u);
	e2e_close(&a);
	e2e_close(&b);
	e2e_close(&u);
}

int main(void)
{
	e2e_setup("clients");
	static Config cfg;
	e2e_config(&cfg);
	CHECK(config_set(&cfg, "max_connections", "8", "test") == CONFIG_OK);
	if (!e2e_start_with(&cfg, NULL)) {
		e2e_teardown();
		return 1;
	}
	RUN(test_same_events);
	RUN(test_scoping);
	RUN(test_snapshot_buffer);
	RUN(test_data_coalescing);
	RUN(test_data_stream_replies);
	RUN(test_users_edit);
	RUN(test_oversized);
	RUN(test_fragmented);
	RUN(test_pipelined);
	RUN(test_disconnect_mid_burst);
	RUN(test_log_mirror);
	e2e_stop();
	e2e_teardown();
	return test_result();
}
