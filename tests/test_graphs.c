/*
 * The Graphs registry and event notification (01 §2.2, §4; 02 §3.2–§3.4,
 * §3.4.1, §4.1): graph create/delete/rename, UUIDs from the real PAL random
 * source, graph-level ports, start/stop and uptime with controlled time,
 * subscriber fan-out and scoping, and the exact refusal messages.
 */
#include "test.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>

#include "blocktypes/block_types.h"
#include "graphs/graphs.h"
#include "platform/pal.h"
#include "system/log.h"

static Runtime rt;
static Graphs gs;

/* ---- Event logs, one per subscriber ----------------------------------------- */

typedef struct {
	GraphsSubscriber sub;
	char log[4096];
} Sink;

static void append(char *buf, size_t size, const char *fmt, ...) LOG_PRINTF_FORMAT(3, 4);

static void append(char *buf, size_t size, const char *fmt, ...)
{
	size_t len = strlen(buf);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + len, size - len, fmt, ap);
	va_end(ap);
}

static const char *text(const Linkable *v)
{
	static char buf[4][64];
	static int next;
	char *b = buf[next++ % 4];
	linkable_stringify(v, b, 64);
	return b;
}

static const char *in_name(const Block *b, uint32_t i)
{
	return block_input_port(b, i)->name;
}

/* One line per event, prefixed with the graph id. */
static void describe(char *log, size_t size, const GraphEvent *ev)
{
	const Graph *g = ev->graph;
	const char *id = g != NULL ? g->id : "-";
	const Block *b = ev->block;
	const GraphPort *p = ev->port;
	switch (ev->type) {
	case GRAPH_EVENT_BLOCK_ADDED:
	case GRAPH_EVENT_BLOCK_CHANGED:
		append(log, size, "%s:%s %s(%g,%g,%s);", id,
		       ev->type == GRAPH_EVENT_BLOCK_ADDED ? "+node" : "changenode", b->name,
		       b->metadata.x, b->metadata.y,
		       b->metadata.extra != NULL ? b->metadata.extra : "-");
		break;
	case GRAPH_EVENT_BLOCK_REMOVED:
		append(log, size, "%s:-node %s;", id, b->name);
		break;
	case GRAPH_EVENT_BLOCK_RENAMED:
		append(log, size, "%s:renamenode %s>%s;", id, ev->old_name, b->name);
		break;
	case GRAPH_EVENT_EDGE_ADDED:
	case GRAPH_EVENT_EDGE_REMOVED:
		append(log, size, "%s:%cedge %s.%s>%s.%s;", id,
		       ev->type == GRAPH_EVENT_EDGE_ADDED ? '+' : '-', ev->src_block->name,
		       block_output_port(ev->src_block, ev->src_output)->name, b->name,
		       in_name(b, ev->input));
		break;
	case GRAPH_EVENT_EDGE_CHANGED: {
		const char *md = b->inputs[ev->input].edge_metadata;
		append(log, size, "%s:changeedge %s.%s>%s.%s %s;", id, ev->src_block->name,
		       block_output_port(ev->src_block, ev->src_output)->name, b->name,
		       in_name(b, ev->input), md != NULL ? md : "-");
		break;
	}
	case GRAPH_EVENT_INITIAL_ADDED:
	case GRAPH_EVENT_INITIAL_REMOVED:
		append(log, size, "%s:%cinit %s>%s.%s;", id,
		       ev->type == GRAPH_EVENT_INITIAL_ADDED ? '+' : '-', text(ev->value), b->name,
		       in_name(b, ev->input));
		break;
	case GRAPH_EVENT_INPORT_ADDED:
	case GRAPH_EVENT_OUTPORT_ADDED: {
		enum graph_port_dir dir =
			ev->type == GRAPH_EVENT_INPORT_ADDED ? GRAPH_INPORT : GRAPH_OUTPORT;
		append(log, size, "%s:+%s %s=%s.%s;", id,
		       dir == GRAPH_INPORT ? "inport" : "outport", p->name, p->block->name,
		       graph_port_inner_name(dir, p));
		break;
	}
	case GRAPH_EVENT_INPORT_REMOVED:
	case GRAPH_EVENT_OUTPORT_REMOVED:
		append(log, size, "%s:-%s %s;", id,
		       ev->type == GRAPH_EVENT_INPORT_REMOVED ? "inport" : "outport", p->name);
		break;
	case GRAPH_EVENT_INPORT_RENAMED:
	case GRAPH_EVENT_OUTPORT_RENAMED:
		append(log, size, "%s:rename%s %s>%s;", id,
		       ev->type == GRAPH_EVENT_INPORT_RENAMED ? "inport" : "outport", ev->old_name,
		       p->name);
		break;
	case GRAPH_EVENT_GRAPH_CREATED:
		append(log, size, "%s:+graph(%s,%s,%s);", id, g->name, g->description, g->icon);
		break;
	case GRAPH_EVENT_GRAPH_DELETED:
		append(log, size, "%s:-graph(%s);", id, g->name);
		break;
	case GRAPH_EVENT_GRAPH_RENAMED:
		append(log, size, "%s:rename %s>%s;", id, ev->old_name, g->name);
		break;
	case GRAPH_EVENT_GRAPH_STARTED:
	case GRAPH_EVENT_GRAPH_STOPPED:
		append(log, size, "%s:%s@%llu+%llu;", id,
		       ev->type == GRAPH_EVENT_GRAPH_STARTED ? "started" : "stopped",
		       (unsigned long long)ev->time_ms, (unsigned long long)ev->uptime_ms);
		break;
	case GRAPH_EVENT_GRAPH_STATUS:
		append(log, size, "%s:status(%s,%s,%llu);", id, g->name,
		       g->started ? "started" : "stopped", (unsigned long long)ev->uptime_ms);
		break;
	case GRAPH_EVENT_ERROR:
		append(log, size, "%s:error(%s,%s);", id, ev->graph_id ? ev->graph_id : "-",
		       ev->message);
		break;
	}
}

static void on_event(void *ctx, const GraphEvent *ev)
{
	Sink *k = ctx;
	describe(k->log, sizeof(k->log), ev);
}

static Sink all; /* the default, unscoped subscriber */

static void sink_init(Sink *k)
{
	graphs_subscriber_init(&k->sub, on_event, k);
	k->log[0] = '\0';
}

static void setup(void)
{
	runtime_init(&rt);
	graphs_init(&gs, &rt, block_types_get, pal_random_bytes);
	sink_init(&all);
	graphs_subscribe(&gs, &all.sub);
}

static void teardown(void)
{
	graphs_deinit(&gs);
	runtime_deinit(&rt);
}

static void clear(void)
{
	all.log[0] = '\0';
}

static Graph *new_graph(const char *id)
{
	Graph *g = NULL;
	CHECK(graphs_new_graph(&gs, id, id, NULL, NULL, &g, NULL) == GRAPH_OK);
	CHECK(g != NULL);
	return g;
}

static void add(const char *graph, const char *node)
{
	CHECK(graphs_add_node(&gs, graph, node, "math/Add2", NULL, NULL) == GRAPH_OK);
}

/* Expect a refusal with status st and message text, and no event. */
#define REFUSED(call, st, want)                                                                    \
	do {                                                                                       \
		GraphsMessage m_;                                                                  \
		all.log[0] = '\0';                                                                 \
		CHECK((call) == (st));                                                             \
		CHECK_STREQ(m_.text, (want));                                                      \
		CHECK_STREQ(all.log, "");                                                          \
	} while (0)

static enum pal_err failing_random(void *buf, size_t len)
{
	(void)buf;
	(void)len;
	return PAL_ERR_UNAVAILABLE;
}

/* Always the same bytes, so every UUID collides. */
static enum pal_err constant_random(void *buf, size_t len)
{
	memset(buf, 0xab, len);
	return PAL_OK;
}

/* ---- Graphs: create, delete, rename (02 §3.2, §3.4) ------------------------ */

static void test_new_graph(void)
{
	setup();
	Graph *g = NULL;
	GraphsMessage m;
	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, &g, &m) == GRAPH_OK);
	CHECK_STREQ(m.text, "");
	CHECK_STREQ(all.log, "g1:+graph(<<new graph>>,,);");
	CHECK(g != NULL && !g->started && g->block_count == 0);
	CHECK(graphs_find(&gs, "g1") == g);
	CHECK(graphs_find(&gs, "g2") == NULL && graphs_find(&gs, NULL) == NULL);
	clear();
	CHECK(graphs_new_graph(&gs, "g2", "Two", "the second", "cog", NULL, NULL) == GRAPH_OK);
	CHECK_STREQ(all.log, "g2:+graph(Two,the second,cog);");
	new_graph("g3");
	/* Creation order. */
	CHECK(graphs_count(&gs) == 3);
	CHECK_STREQ(graphs_at(&gs, 0)->id, "g1");
	CHECK_STREQ(graphs_at(&gs, 1)->id, "g2");
	CHECK_STREQ(graphs_at(&gs, 2)->id, "g3");
	teardown();
}

static void test_new_graph_refusals(void)
{
	setup();
	new_graph("g1");
	char long_id[GRAPH_ID_MAX + 2];
	memset(long_id, 'x', sizeof(long_id) - 1);
	long_id[GRAPH_ID_MAX + 1] = '\0';
	Graph *g = (Graph *)&gs; /* overwritten with NULL on refusal */
	REFUSED(graphs_new_graph(&gs, "g1", "x", NULL, NULL, &g, &m_), GRAPH_ERR_GRAPH_EXISTS,
		"Graph exists: g1");
	CHECK(g == NULL);
	REFUSED(graphs_new_graph(&gs, "", NULL, NULL, NULL, NULL, &m_), GRAPH_ERR_INVALID_ID,
		"Invalid id");
	REFUSED(graphs_new_graph(&gs, long_id, NULL, NULL, NULL, NULL, &m_), GRAPH_ERR_INVALID_ID,
		"Invalid id");
	long_id[GRAPH_ID_MAX] = '\0'; /* 64 bytes is valid */
	CHECK(graphs_new_graph(&gs, long_id, NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	gs.random_bytes = failing_random;
	REFUSED(graphs_new_graph(&gs, NULL, NULL, NULL, NULL, NULL, &m_), GRAPH_ERR_UNAVAILABLE,
		"Random source unavailable");
	/* An explicit id needs no random bytes. */
	CHECK(graphs_new_graph(&gs, "g3", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	/* A UUID that is already taken (after every retry) is refused. */
	gs.random_bytes = constant_random;
	CHECK(graphs_new_graph(&gs, NULL, NULL, NULL, NULL, &g, NULL) == GRAPH_OK);
	char id[GRAPHS_UUID_SIZE];
	memcpy(id, g->id, sizeof(id));
	CHECK_STREQ(id, "abababab-abab-4bab-abab-abababababab");
	char expected[64];
	snprintf(expected, sizeof(expected), "Graph exists: %s", id);
	REFUSED(graphs_new_graph(&gs, NULL, NULL, NULL, NULL, NULL, &m_), GRAPH_ERR_GRAPH_EXISTS,
		expected);
	CHECK(graphs_count(&gs) == 4);
	teardown();
}

static bool is_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static void test_uuid(void)
{
	setup();
	enum { N = 64 };
	for (int i = 0; i < N; i++) {
		Graph *g = NULL;
		CHECK(graphs_new_graph(&gs, NULL, NULL, NULL, NULL, &g, NULL) == GRAPH_OK);
		const char *id = g->id;
		CHECK(strlen(id) == 36);
		for (int k = 0; k < 36; k++) {
			if (k == 8 || k == 13 || k == 18 || k == 23)
				CHECK(id[k] == '-');
			else
				CHECK(is_hex(id[k]));
		}
		CHECK(id[14] == '4'); /* version 4 */
		CHECK(strchr("89ab", id[19]) != NULL); /* RFC 9562 variant */
	}
	for (int i = 0; i < N; i++) {
		for (int k = i + 1; k < N; k++)
			CHECK(strcmp(graphs_at(&gs, i)->id, graphs_at(&gs, k)->id) != 0);
	}
	teardown();
}

static void test_delete_graph(void)
{
	setup();
	new_graph("g1");
	new_graph("g2");
	new_graph("g3");
	add("g2", "A");
	add("g2", "B");
	CHECK(graphs_add_edge(&gs, "g2", "A", "out", "B", "in1", NULL, NULL) == GRAPH_OK);
	REFUSED(graphs_delete_graph(&gs, "", "g1", &m_), GRAPH_ERR_INVALID_ID, "Invalid id");
	REFUSED(graphs_delete_graph(&gs, "nope", "g1", &m_), GRAPH_ERR_UNKNOWN_GRAPH,
		"Unknown graph: nope");
	REFUSED(graphs_delete_graph(&gs, "g1", "G1", &m_), GRAPH_ERR_NAME_MISMATCH,
		"Graph name mismatch: g1");
	REFUSED(graphs_delete_graph(&gs, "g1", NULL, &m_), GRAPH_ERR_NAME_MISMATCH,
		"Graph name mismatch: g1");

	/* A stopped graph: just the deletion; its blocks go without events. */
	clear();
	CHECK(graphs_delete_graph(&gs, "g2", "g2", NULL) == GRAPH_OK);
	CHECK_STREQ(all.log, "g2:-graph(g2);");
	CHECK(graphs_find(&gs, "g2") == NULL && graphs_count(&gs) == 2);
	CHECK_STREQ(graphs_at(&gs, 0)->id, "g1");
	CHECK_STREQ(graphs_at(&gs, 1)->id, "g3");

	/* A started graph: stopped first, then deleted (02 §1.1 compound). */
	runtime_set_now(&rt, 1000);
	CHECK(graphs_start(&gs, "g3", NULL) == GRAPH_OK);
	runtime_set_now(&rt, 3500);
	clear();
	CHECK(graphs_delete_graph(&gs, "g3", "g3", NULL) == GRAPH_OK);
	CHECK_STREQ(all.log, "g3:stopped@1000+2500;g3:-graph(g3);");
	/* The id is free again. */
	CHECK(graphs_new_graph(&gs, "g3", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	teardown();
}

static void test_rename_graph(void)
{
	setup();
	Graph *g = new_graph("g1");
	clear();
	CHECK(graphs_rename_graph(&gs, "g1", "First", NULL) == GRAPH_OK);
	CHECK(graphs_rename_graph(&gs, "g1", "Again", NULL) == GRAPH_OK);
	CHECK_STREQ(all.log, "g1:rename g1>First;g1:rename First>Again;");
	CHECK_STREQ(g->id, "g1");
	CHECK(graphs_find(&gs, "g1") == g);
	REFUSED(graphs_rename_graph(&gs, "nope", "x", &m_), GRAPH_ERR_UNKNOWN_GRAPH,
		"Unknown graph: nope");
	REFUSED(graphs_rename_graph(&gs, "", "x", &m_), GRAPH_ERR_INVALID_ID, "Invalid id");
	REFUSED(graphs_rename_graph(&gs, "g1", NULL, &m_), GRAPH_ERR_INVALID_ARGUMENT,
		"Invalid argument");
	/* delete_graph checks the current name. */
	REFUSED(graphs_delete_graph(&gs, "g1", "g1", &m_), GRAPH_ERR_NAME_MISMATCH,
		"Graph name mismatch: g1");
	CHECK(graphs_delete_graph(&gs, "g1", "Again", NULL) == GRAPH_OK);
	teardown();
}

/* ---- Start, stop, uptime (02 §3.3) ----------------------------------------- */

static void test_start_stop(void)
{
	setup();
	Graph *g = new_graph("g1");
	new_graph("g2");
	add("g1", "A");
	Linkable one = linkable_from_f64(1);
	CHECK(graphs_add_initial(&gs, "g1", "A", "in1", &one, NULL) == GRAPH_OK);
	Block *a = graph_find_block(g, "A");
	clear();

	/* Never started: stop reports the epoch and 0. */
	CHECK(graphs_stop(&gs, "g2", NULL) == GRAPH_OK);
	CHECK_STREQ(all.log, "g2:stopped@0+0;");
	clear();

	runtime_set_now(&rt, 1000);
	CHECK(graphs_start(&gs, "g1", NULL) == GRAPH_OK);
	CHECK(g->started && a->started);
	CHECK(linkable_to_f64(&a->out_values[0]) == 1);
	/* Starting again re-reports without restarting. */
	CHECK(graphs_add_initial(&gs, "g1", "A", "in2", &one, NULL) == GRAPH_OK);
	runtime_set_now(&rt, 2500);
	size_t queued = work_queue_len(&rt.queue);
	CHECK(graphs_start(&gs, "g1", NULL) == GRAPH_OK);
	CHECK(work_queue_len(&rt.queue) == queued); /* start would push every output */
	CHECK_STREQ(all.log, "g1:started@1000+0;g1:+init 1>A.in2;g1:started@1000+1500;");
	clear();

	runtime_set_now(&rt, 4799);
	CHECK(graphs_uptime_ms(g) == 3799);
	CHECK(graphs_status(&gs, "g1", NULL) == GRAPH_OK);
	CHECK(graphs_stop(&gs, "g1", NULL) == GRAPH_OK);
	CHECK(!g->started && !a->started);
	CHECK(graphs_uptime_ms(g) == 0);
	runtime_set_now(&rt, 9000);
	CHECK(graphs_stop(&gs, "g1", NULL) == GRAPH_OK); /* the same run again */
	CHECK(graphs_status(&gs, "g1", NULL) == GRAPH_OK);
	CHECK_STREQ(all.log, "g1:status(g1,started,3799);g1:stopped@1000+3799;"
			     "g1:stopped@1000+3799;g1:status(g1,stopped,0);");
	clear();

	/* A new run has its own start. */
	CHECK(graphs_start(&gs, "g1", NULL) == GRAPH_OK);
	runtime_set_now(&rt, 9100);
	graphs_status_all(&gs);
	CHECK_STREQ(all.log, "g1:started@9000+0;g1:status(g1,started,100);"
			     "g2:status(g2,stopped,0);");
	/* A clock behind the start (only a test can do that) gives 0. */
	runtime_set_now(&rt, 10);
	CHECK(graphs_uptime_ms(g) == 0);

	REFUSED(graphs_start(&gs, "nope", &m_), GRAPH_ERR_UNKNOWN_GRAPH, "Unknown graph: nope");
	REFUSED(graphs_stop(&gs, "nope", &m_), GRAPH_ERR_UNKNOWN_GRAPH, "Unknown graph: nope");
	REFUSED(graphs_status(&gs, "nope", &m_), GRAPH_ERR_UNKNOWN_GRAPH, "Unknown graph: nope");
	REFUSED(graphs_start(&gs, "", &m_), GRAPH_ERR_INVALID_ID, "Invalid id");
	REFUSED(graphs_stop(&gs, NULL, &m_), GRAPH_ERR_INVALID_ID, "Invalid id");
	REFUSED(graphs_status(&gs, "", &m_), GRAPH_ERR_INVALID_ID, "Invalid id");
	teardown();
}

/* ---- Graph-level ports (01 §2.2, 02 §3.4) ---------------------------------- */

static void test_ports(void)
{
	setup();
	Graph *g = new_graph("g1");
	add("g1", "A");
	add("g1", "B");
	clear();
	CHECK(graphs_add_port(&gs, "g1", GRAPH_INPORT, "x", "A", "in1", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_INPORT, "y", "A", "in1", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_OUTPORT, "x", "B", "out", NULL) == GRAPH_OK);
	CHECK_STREQ(all.log, "g1:+inport x=A.in1;g1:+inport y=A.in1;g1:+outport x=B.out;");
	CHECK(g->port_count[GRAPH_INPORT] == 2 && g->port_count[GRAPH_OUTPORT] == 1);
	const GraphPort *p = graph_find_port(g, GRAPH_OUTPORT, "x");
	CHECK(p != NULL && p->block == graph_find_block(g, "B") && p->port == 0);
	CHECK(graph_find_port(g, GRAPH_OUTPORT, "y") == NULL);
	clear();

	CHECK(graphs_rename_port(&gs, "g1", GRAPH_INPORT, "x", "z", NULL) == GRAPH_OK);
	CHECK(graphs_remove_port(&gs, "g1", GRAPH_INPORT, "y", NULL) == GRAPH_OK);
	CHECK(graphs_rename_port(&gs, "g1", GRAPH_OUTPORT, "x", "result", NULL) == GRAPH_OK);
	CHECK_STREQ(all.log, "g1:renameinport x>z;g1:-inport y;g1:renameoutport x>result;");
	CHECK(graph_find_port(g, GRAPH_INPORT, "x") == NULL);
	CHECK(graph_find_port(g, GRAPH_INPORT, "z") != NULL);

	REFUSED(graphs_add_port(&gs, "g1", GRAPH_INPORT, "z", "B", "in2", &m_),
		GRAPH_ERR_PORT_EXISTS, "Port exists: z");
	REFUSED(graphs_add_port(&gs, "g1", GRAPH_INPORT, "w", "X", "in1", &m_),
		GRAPH_ERR_UNKNOWN_NODE, "Unknown node: X");
	REFUSED(graphs_add_port(&gs, "g1", GRAPH_INPORT, "w", "A", "nope", &m_),
		GRAPH_ERR_UNKNOWN_PORT, "Unknown port: A.nope");
	/* An inport maps to an input, an outport to an output. */
	REFUSED(graphs_add_port(&gs, "g1", GRAPH_INPORT, "w", "A", "out", &m_),
		GRAPH_ERR_UNKNOWN_PORT, "Unknown port: A.out");
	REFUSED(graphs_add_port(&gs, "g1", GRAPH_OUTPORT, "w", "A", "in1", &m_),
		GRAPH_ERR_UNKNOWN_PORT, "Unknown port: A.in1");
	REFUSED(graphs_add_port(&gs, "g1", GRAPH_INPORT, "", "A", "in1", &m_),
		GRAPH_ERR_INVALID_ID, "Invalid id");
	REFUSED(graphs_add_port(&gs, "g1", GRAPH_INPORT, "w", "", "in1", &m_),
		GRAPH_ERR_INVALID_ID, "Invalid id");
	REFUSED(graphs_add_port(&gs, "nope", GRAPH_INPORT, "w", "A", "in1", &m_),
		GRAPH_ERR_UNKNOWN_GRAPH, "Unknown graph: nope");
	REFUSED(graphs_remove_port(&gs, "g1", GRAPH_INPORT, "nope", &m_), GRAPH_ERR_UNKNOWN_PORT,
		"Unknown port: nope");
	REFUSED(graphs_remove_port(&gs, "g1", GRAPH_OUTPORT, "z", &m_), GRAPH_ERR_UNKNOWN_PORT,
		"Unknown port: z");
	REFUSED(graphs_remove_port(&gs, "g1", GRAPH_INPORT, "", &m_), GRAPH_ERR_INVALID_ID,
		"Invalid id");
	REFUSED(graphs_rename_port(&gs, "g1", GRAPH_INPORT, "nope", "q", &m_),
		GRAPH_ERR_UNKNOWN_PORT, "Unknown port: nope");
	REFUSED(graphs_rename_port(&gs, "g1", GRAPH_INPORT, "z", "z", &m_), GRAPH_ERR_PORT_EXISTS,
		"Port exists: z");
	REFUSED(graphs_rename_port(&gs, "g1", GRAPH_INPORT, "z", "", &m_), GRAPH_ERR_INVALID_ID,
		"Invalid id");
	CHECK(graphs_add_port(&gs, "g1", GRAPH_INPORT, "w", "B", "in2", NULL) == GRAPH_OK);
	REFUSED(graphs_rename_port(&gs, "g1", GRAPH_INPORT, "z", "w", &m_), GRAPH_ERR_PORT_EXISTS,
		"Port exists: w");
	teardown();
}

static void test_ports_follow_block(void)
{
	setup();
	Graph *g = new_graph("g1");
	add("g1", "A");
	add("g1", "B");
	add("g1", "C");
	CHECK(graphs_add_edge(&gs, "g1", "A", "out", "B", "in1", NULL, NULL) == GRAPH_OK);
	Linkable seven = linkable_from_f64(7);
	CHECK(graphs_add_initial(&gs, "g1", "B", "in2", &seven, NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_INPORT, "i1", "B", "in1", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_INPORT, "i2", "C", "in1", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_INPORT, "i3", "B", "in2", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_OUTPORT, "o1", "B", "out", NULL) == GRAPH_OK);

	/* A rename carries the ports along, with no port event. */
	clear();
	CHECK(graphs_rename_node(&gs, "g1", "B", "Bee", NULL) == GRAPH_OK);
	CHECK_STREQ(all.log, "g1:renamenode B>Bee;");
	CHECK_STREQ(graph_find_port(g, GRAPH_INPORT, "i1")->block->name, "Bee");

	/* A removal removes them: edges and initials, then ports, then the node. */
	clear();
	CHECK(graphs_remove_node(&gs, "g1", "Bee", NULL) == GRAPH_OK);
	CHECK_STREQ(all.log, "g1:-edge A.out>Bee.in1;g1:-init 7>Bee.in2;g1:-inport i1;"
			     "g1:-inport i3;g1:-outport o1;g1:-node Bee;");
	CHECK(g->port_count[GRAPH_INPORT] == 1 && g->port_count[GRAPH_OUTPORT] == 0);
	CHECK_STREQ(g->ports[GRAPH_INPORT][0].name, "i2");
	teardown();
}

/* ---- Event scoping (02 §4.1) ------------------------------------------------- */

static void test_scoping(void)
{
	setup();
	Sink s1, s2;
	sink_init(&s1);
	sink_init(&s2);
	graphs_subscribe(&gs, &s1.sub);
	graphs_subscribe(&gs, &s2.sub);
	new_graph("g1");
	new_graph("g2");
	CHECK(graphs_subscriber_scope(&s1.sub, "g1"));
	CHECK(graphs_subscriber_scope(&s2.sub, "g2"));
	CHECK(!graphs_subscriber_scope(&s2.sub, ""));
	CHECK_STREQ(s2.sub.scope, "g2"); /* unchanged */
	clear();
	s1.log[0] = s2.log[0] = '\0';

	add("g1", "A");
	CHECK(graphs_start(&gs, "g2", NULL) == GRAPH_OK);
	CHECK(graphs_rename_graph(&gs, "g1", "One", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_INPORT, "p", "A", "in1", NULL) == GRAPH_OK);
	new_graph("g3"); /* system-wide */
	graphs_status_all(&gs);
	graphs_notify_error(&gs, "g2", "Unable to save graph: g2 (g2)");
	graphs_notify_error(&gs, NULL, "boom");
	CHECK(graphs_stop(&gs, "g2", NULL) == GRAPH_OK);
	CHECK(graphs_delete_graph(&gs, "g3", "g3", NULL) == GRAPH_OK);

	const char *wide = "g3:+graph(g3,,);g1:status(One,stopped,0);"
			   "g2:status(g2,started,0);g3:status(g3,stopped,0);"
			   "g2:error(g2,Unable to save graph: g2 (g2));-:error(-,boom);";
	char expect[1024];
	snprintf(expect, sizeof(expect),
		 "g1:+node A(50,50,-);g2:started@0+0;g1:rename g1>One;g1:+inport p=A.in1;%s"
		 "g2:stopped@0+0;g3:-graph(g3);",
		 wide);
	CHECK_STREQ(all.log, expect);
	snprintf(expect, sizeof(expect),
		 "g1:+node A(50,50,-);g1:rename g1>One;g1:+inport p=A.in1;%sg3:-graph(g3);",
		 wide);
	CHECK_STREQ(s1.log, expect);
	snprintf(expect, sizeof(expect), "g2:started@0+0;%sg2:stopped@0+0;g3:-graph(g3);", wide);
	CHECK_STREQ(s2.log, expect);

	/* Re-scoping, and back to every graph. */
	s1.log[0] = s2.log[0] = '\0';
	CHECK(graphs_subscriber_scope(&s2.sub, "g1"));
	CHECK(graphs_subscriber_scope(&s1.sub, NULL));
	add("g1", "B");
	add("g2", "C");
	CHECK_STREQ(s1.log, "g1:+node B(50,50,-);g2:+node C(50,50,-);");
	CHECK_STREQ(s2.log, "g1:+node B(50,50,-);");

	/* Unsubscribed: nothing more. */
	graphs_unsubscribe(&gs, &s1.sub);
	graphs_unsubscribe(&gs, &s1.sub); /* not subscribed: no-op */
	s1.log[0] = '\0';
	add("g1", "D");
	CHECK_STREQ(s1.log, "");
	CHECK_STREQ(s2.log, "g1:+node B(50,50,-);g1:+node D(50,50,-);");
	teardown();
}

/* changeedge (02 §3.4): replaces the edge's metadata and is reported to every
 * subscriber of the graph; an edge that does not exist is refused. */
static void test_change_edge(void)
{
	setup();
	Sink s1, s2;
	sink_init(&s1);
	sink_init(&s2);
	graphs_subscribe(&gs, &s1.sub);
	graphs_subscribe(&gs, &s2.sub);
	new_graph("g1");
	new_graph("g2");
	CHECK(graphs_subscriber_scope(&s1.sub, "g1"));
	CHECK(graphs_subscriber_scope(&s2.sub, "g2"));
	add("g1", "A");
	add("g1", "B");
	CHECK(graphs_add_edge(&gs, "g1", "A", "out", "B", "in1", "{\"route\":3}", NULL) ==
	      GRAPH_OK);
	Block *b = graph_find_block(graphs_find(&gs, "g1"), "B");
	CHECK_STREQ(b->inputs[0].edge_metadata, "{\"route\":3}");
	clear();
	s1.log[0] = s2.log[0] = '\0';

	GraphsMessage m;
	CHECK(graphs_change_edge(&gs, "g1", "A", "out", "B", "in1", "{\"route\":5,\"x\":true}",
				 &m) == GRAPH_OK);
	CHECK_STREQ(m.text, "");
	const char *ev = "g1:changeedge A.out>B.in1 {\"route\":5,\"x\":true};";
	CHECK_STREQ(all.log, ev);
	CHECK_STREQ(s1.log, ev);
	CHECK_STREQ(s2.log, ""); /* scoped to another graph */
	/* NULL removes the metadata; the edge stays. */
	clear();
	CHECK(graphs_change_edge(&gs, "g1", "A", "out", "B", "in1", NULL, NULL) == GRAPH_OK);
	CHECK_STREQ(all.log, "g1:changeedge A.out>B.in1 -;");
	CHECK(b->inputs[0].source == INPUT_SOURCE_EDGE && b->inputs[0].edge_metadata == NULL);

	/* Refusals: the same checks as removeedge. */
	REFUSED(graphs_change_edge(&gs, "g1", "B", "out", "A", "in1", "{}", &m_),
		GRAPH_ERR_NO_SUCH_CONNECTION, "No such connection");
	REFUSED(graphs_change_edge(&gs, "g1", "A", "out", "B", "in2", "{}", &m_),
		GRAPH_ERR_NO_SUCH_CONNECTION, "No such connection");
	REFUSED(graphs_change_edge(&gs, "g1", "A", "in1", "B", "in1", "{}", &m_),
		GRAPH_ERR_NO_SUCH_CONNECTION, "No such connection");
	REFUSED(graphs_change_edge(&gs, "g1", "X", "out", "B", "in1", "{}", &m_),
		GRAPH_ERR_UNKNOWN_NODE, "Unknown node: X");
	REFUSED(graphs_change_edge(&gs, "g1", "A", "out", "B", "nope", "{}", &m_),
		GRAPH_ERR_UNKNOWN_PORT, "Unknown port: B.nope");
	REFUSED(graphs_change_edge(&gs, "nope", "A", "out", "B", "in1", "{}", &m_),
		GRAPH_ERR_UNKNOWN_GRAPH, "Unknown graph: nope");
	REFUSED(graphs_change_edge(&gs, "g1", "", "out", "B", "in1", "{}", &m_),
		GRAPH_ERR_INVALID_ID, "Invalid id");

	/* Removing the edge drops its metadata; a new edge starts with none. */
	CHECK(graphs_change_edge(&gs, "g1", "A", "out", "B", "in1", "{\"a\":1}", NULL) ==
	      GRAPH_OK);
	CHECK(graphs_remove_edge(&gs, "g1", "A", "out", "B", "in1", NULL) == GRAPH_OK);
	CHECK(b->inputs[0].edge_metadata == NULL);
	CHECK(graphs_add_edge(&gs, "g1", "A", "out", "B", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(b->inputs[0].edge_metadata == NULL);
	teardown();
}

/* Events carry the applied state, not the request (02 §1.1). */
static void test_applied_state(void)
{
	setup();
	new_graph("g1");
	clear();
	BlockMetadata md = block_metadata_default();
	md.x = 10;
	md.extra = (char *)"{\"change_threshold\":0.5}"; /* opaque (plan task 21c) */
	CHECK(graphs_add_node(&gs, "g1", "A", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "B", "math/Add2", &md, NULL) == GRAPH_OK);
	BlockChange chg = {.set_y = true, .y = 7};
	CHECK(graphs_change_node(&gs, "g1", "B", &chg, NULL) == GRAPH_OK);
	chg = (BlockChange){.set_extra = true, .extra = NULL};
	CHECK(graphs_change_node(&gs, "g1", "B", &chg, NULL) == GRAPH_OK);
	Linkable s = {0};
	CHECK(linkable_from_string(&s, "12.5", 4));
	CHECK(graphs_add_initial(&gs, "g1", "A", "in1", &s, NULL) == GRAPH_OK);
	linkable_clear(&s);
	CHECK(graphs_remove_initial(&gs, "g1", "A", "in1", NULL) == GRAPH_OK);
	CHECK_STREQ(all.log, "g1:+node A(50,50,-);g1:+node B(10,50,{\"change_threshold\":0.5});"
			     "g1:changenode B(10,7,{\"change_threshold\":0.5});"
			     "g1:changenode B(10,7,-);"
			     "g1:+init 12.5>A.in1;g1:-init 12.5>A.in1;");
	teardown();
}

/* ---- Subscribers changing during a notification (01 §4) -------------------- */

static Sink *victim;   /* unsubscribed by meddler, if not NULL */
static Sink *newcomer; /* subscribed by meddler, if not NULL */
static int calls;

/* On its first event: unsubscribes itself and victim, subscribes newcomer. */
static void meddler(void *ctx, const GraphEvent *ev)
{
	GraphsSubscriber *self = ctx;
	(void)ev;
	if (calls++ > 0)
		return;
	graphs_unsubscribe(&gs, self);
	if (victim != NULL)
		graphs_unsubscribe(&gs, &victim->sub);
	if (newcomer != NULL) {
		sink_init(newcomer);
		graphs_subscribe(&gs, &newcomer->sub);
	}
}

static void test_subscribers_during_notification(void)
{
	setup();
	GraphsSubscriber m;
	Sink next, late;
	graphs_subscriber_init(&m, meddler, &m);
	sink_init(&next);
	graphs_subscribe(&gs, &m); /* order: all, m, next */
	graphs_subscribe(&gs, &next.sub);

	/* m removes itself and the subscriber next in line. */
	victim = &next;
	newcomer = NULL;
	calls = 0;
	new_graph("g1");
	CHECK(calls == 1);
	CHECK_STREQ(all.log, "g1:+graph(g1,,);");
	CHECK_STREQ(next.log, "");
	add("g1", "A");
	CHECK(calls == 1);
	CHECK_STREQ(next.log, "");

	/* m adds a subscriber, which the delivery reaches but skips. */
	victim = NULL;
	newcomer = &late;
	calls = 0;
	graphs_subscribe(&gs, &m); /* order: all, m; then late */
	add("g1", "B");
	CHECK(calls == 1);
	CHECK_STREQ(late.log, "");
	add("g1", "C");
	CHECK_STREQ(late.log, "g1:+node C(50,50,-);");
	CHECK_STREQ(all.log, "g1:+graph(g1,,);g1:+node A(50,50,-);g1:+node B(50,50,-);"
			     "g1:+node C(50,50,-);");
	teardown();
}

/* ---- Refusal messages (02 §3.4.1) ----------------------------------------- */

static void test_refusal_message_render(void)
{
	char buf[GRAPHS_MESSAGE_MAX];
	GraphRefusal c = {.graph = "g", .node = "N", .port = "p", .name = "nm"};
	static const struct {
		enum graph_status st;
		const char *text;
	} cases[] = {
		{GRAPH_OK, ""},
		{GRAPH_ERR_UNKNOWN_GRAPH, "Unknown graph: g"},
		{GRAPH_ERR_UNKNOWN_NODE, "Unknown node: N"},
		{GRAPH_ERR_UNKNOWN_PORT, "Unknown port: N.p"},
		{GRAPH_ERR_NODE_EXISTS, "Node exists: N"},
		{GRAPH_ERR_UNKNOWN_COMPONENT, "Unknown component: nm"},
		{GRAPH_ERR_INVALID_EDGE_DIRECTION, "Invalid edge direction"},
		{GRAPH_ERR_INPUT_CONNECTED, "Input already connected: N.p"},
		{GRAPH_ERR_NO_SUCH_CONNECTION, "No such connection"},
		{GRAPH_ERR_PORT_EXISTS, "Port exists: nm"},
		{GRAPH_ERR_NAME_MISMATCH, "Graph name mismatch: g"},
		{GRAPH_ERR_GRAPH_EXISTS, "Graph exists: g"},
		{GRAPH_ERR_INVALID_ID, "Invalid id"},
		{GRAPH_ERR_INVALID_ARGUMENT, "Invalid argument"},
		{GRAPH_ERR_NO_MEMORY, "Out of memory"},
		{GRAPH_ERR_UNAVAILABLE, "Random source unavailable"},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		size_t n = graph_refusal_message(cases[i].st, &c, buf, sizeof(buf));
		CHECK_STREQ(buf, cases[i].text);
		CHECK(n == strlen(cases[i].text));
	}
	/* A graph port: no node. */
	c.node = NULL;
	graph_refusal_message(GRAPH_ERR_UNKNOWN_PORT, &c, buf, sizeof(buf));
	CHECK_STREQ(buf, "Unknown port: p");
	/* No context, and a short buffer: cut, NUL-terminated, full length. */
	graph_refusal_message(GRAPH_ERR_UNKNOWN_NODE, NULL, buf, sizeof(buf));
	CHECK_STREQ(buf, "Unknown node: ");
	c.graph = "abcdef";
	CHECK(graph_refusal_message(GRAPH_ERR_GRAPH_EXISTS, &c, buf, 8) == 20);
	CHECK_STREQ(buf, "Graph e");
}

static void test_command_refusals(void)
{
	setup();
	new_graph("g1");
	add("g1", "A");
	add("g1", "B");
	add("g1", "C");
	CHECK(graphs_add_edge(&gs, "g1", "A", "out", "B", "in1", NULL, NULL) == GRAPH_OK);
	Linkable v = linkable_from_f64(3);
	CHECK(graphs_add_initial(&gs, "g1", "B", "in2", &v, NULL) == GRAPH_OK);
	BlockChange chg = {.set_x = true, .x = 1};

	/* Ids before the graph, the graph before nodes. */
	REFUSED(graphs_add_node(&gs, "nope", "", "math/Add2", NULL, &m_), GRAPH_ERR_INVALID_ID,
		"Invalid id");
	REFUSED(graphs_add_edge(&gs, "nope", "A", "out", "", "in1", NULL, &m_),
		GRAPH_ERR_INVALID_ID, "Invalid id");
	REFUSED(graphs_rename_node(&gs, "nope", "A", NULL, &m_), GRAPH_ERR_INVALID_ID,
		"Invalid id");
	REFUSED(graphs_add_node(&gs, "nope", "X", "math/Add2", NULL, &m_),
		GRAPH_ERR_UNKNOWN_GRAPH, "Unknown graph: nope");
	REFUSED(graphs_remove_node(&gs, "nope", "A", &m_), GRAPH_ERR_UNKNOWN_GRAPH,
		"Unknown graph: nope");
	REFUSED(graphs_rename_node(&gs, "nope", "A", "Z", &m_), GRAPH_ERR_UNKNOWN_GRAPH,
		"Unknown graph: nope");
	REFUSED(graphs_change_node(&gs, "nope", "A", &chg, &m_), GRAPH_ERR_UNKNOWN_GRAPH,
		"Unknown graph: nope");
	REFUSED(graphs_add_edge(&gs, "nope", "A", "out", "C", "in1", NULL, &m_),
		GRAPH_ERR_UNKNOWN_GRAPH, "Unknown graph: nope");
	REFUSED(graphs_remove_edge(&gs, "nope", "A", "out", "B", "in1", &m_),
		GRAPH_ERR_UNKNOWN_GRAPH, "Unknown graph: nope");
	REFUSED(graphs_add_initial(&gs, "nope", "C", "in1", &v, &m_), GRAPH_ERR_UNKNOWN_GRAPH,
		"Unknown graph: nope");
	REFUSED(graphs_remove_initial(&gs, "nope", "B", "in2", &m_), GRAPH_ERR_UNKNOWN_GRAPH,
		"Unknown graph: nope");

	/* Nodes. */
	REFUSED(graphs_add_node(&gs, "g1", "A", "math/Nope", NULL, &m_), GRAPH_ERR_NODE_EXISTS,
		"Node exists: A");
	REFUSED(graphs_add_node(&gs, "g1", "X", "math/Nope", NULL, &m_),
		GRAPH_ERR_UNKNOWN_COMPONENT, "Unknown component: math/Nope");
	REFUSED(graphs_add_node(&gs, "g1", "X", NULL, NULL, &m_), GRAPH_ERR_UNKNOWN_COMPONENT,
		"Unknown component: ");
	REFUSED(graphs_remove_node(&gs, "g1", "X", &m_), GRAPH_ERR_UNKNOWN_NODE,
		"Unknown node: X");
	REFUSED(graphs_rename_node(&gs, "g1", "X", "Y", &m_), GRAPH_ERR_UNKNOWN_NODE,
		"Unknown node: X");
	REFUSED(graphs_rename_node(&gs, "g1", "A", "B", &m_), GRAPH_ERR_NODE_EXISTS,
		"Node exists: B");
	REFUSED(graphs_rename_node(&gs, "g1", "A", "A", &m_), GRAPH_ERR_NODE_EXISTS,
		"Node exists: A");
	REFUSED(graphs_change_node(&gs, "g1", "X", &chg, &m_), GRAPH_ERR_UNKNOWN_NODE,
		"Unknown node: X");
	REFUSED(graphs_change_node(&gs, "g1", "A", NULL, &m_), GRAPH_ERR_INVALID_ARGUMENT,
		"Invalid argument");

	/* Edges: the failing endpoint is named. */
	REFUSED(graphs_add_edge(&gs, "g1", "X", "out", "Y", "in1", NULL, &m_),
		GRAPH_ERR_UNKNOWN_NODE, "Unknown node: X");
	REFUSED(graphs_add_edge(&gs, "g1", "A", "out", "Y", "in1", NULL, &m_),
		GRAPH_ERR_UNKNOWN_NODE, "Unknown node: Y");
	REFUSED(graphs_add_edge(&gs, "g1", "A", "nope", "Y", "in1", NULL, &m_),
		GRAPH_ERR_UNKNOWN_PORT, "Unknown port: A.nope");
	REFUSED(graphs_add_edge(&gs, "g1", "A", "out", "C", "nope", NULL, &m_),
		GRAPH_ERR_UNKNOWN_PORT, "Unknown port: C.nope");
	/* Source and target the same node: the source port is the unknown one. */
	REFUSED(graphs_add_edge(&gs, "g1", "C", "nope", "C", "nope2", NULL, &m_),
		GRAPH_ERR_UNKNOWN_PORT, "Unknown port: C.nope");
	REFUSED(graphs_add_edge(&gs, "g1", "C", "out", "C", "nope2", NULL, &m_),
		GRAPH_ERR_UNKNOWN_PORT, "Unknown port: C.nope2");
	REFUSED(graphs_add_edge(&gs, "g1", "A", "in1", "C", "in1", NULL, &m_),
		GRAPH_ERR_INVALID_EDGE_DIRECTION, "Invalid edge direction");
	REFUSED(graphs_add_edge(&gs, "g1", "A", "out", "C", "out", NULL, &m_),
		GRAPH_ERR_INVALID_EDGE_DIRECTION, "Invalid edge direction");
	REFUSED(graphs_add_edge(&gs, "g1", "C", "out", "B", "in1", NULL, &m_),
		GRAPH_ERR_INPUT_CONNECTED, "Input already connected: B.in1");
	REFUSED(graphs_add_edge(&gs, "g1", "C", "out", "B", "in2", NULL, &m_),
		GRAPH_ERR_INPUT_CONNECTED, "Input already connected: B.in2");
	REFUSED(graphs_remove_edge(&gs, "g1", "C", "out", "B", "in1", &m_),
		GRAPH_ERR_NO_SUCH_CONNECTION, "No such connection");
	REFUSED(graphs_remove_edge(&gs, "g1", "A", "in1", "B", "in1", &m_),
		GRAPH_ERR_NO_SUCH_CONNECTION, "No such connection");
	REFUSED(graphs_remove_edge(&gs, "g1", "X", "out", "B", "in1", &m_),
		GRAPH_ERR_UNKNOWN_NODE, "Unknown node: X");
	REFUSED(graphs_remove_edge(&gs, "g1", "A", "out", "B", "nope", &m_),
		GRAPH_ERR_UNKNOWN_PORT, "Unknown port: B.nope");

	/* Initials. */
	REFUSED(graphs_add_initial(&gs, "g1", "B", "in1", &v, &m_), GRAPH_ERR_INPUT_CONNECTED,
		"Input already connected: B.in1");
	REFUSED(graphs_add_initial(&gs, "g1", "B", "out", &v, &m_),
		GRAPH_ERR_INVALID_EDGE_DIRECTION, "Invalid edge direction");
	REFUSED(graphs_add_initial(&gs, "g1", "X", "in1", &v, &m_), GRAPH_ERR_UNKNOWN_NODE,
		"Unknown node: X");
	REFUSED(graphs_add_initial(&gs, "g1", "C", "nope", &v, &m_), GRAPH_ERR_UNKNOWN_PORT,
		"Unknown port: C.nope");
	REFUSED(graphs_add_initial(&gs, "g1", "", "in1", &v, &m_), GRAPH_ERR_INVALID_ID,
		"Invalid id");
	REFUSED(graphs_remove_initial(&gs, "g1", "C", "in1", &m_), GRAPH_ERR_NO_SUCH_CONNECTION,
		"No such connection");
	REFUSED(graphs_remove_initial(&gs, "g1", "C", "nope", &m_), GRAPH_ERR_UNKNOWN_PORT,
		"Unknown port: C.nope");

	/* Successes leave an empty message. */
	GraphsMessage m;
	strcpy(m.text, "stale");
	CHECK(graphs_remove_edge(&gs, "g1", "A", "out", "B", "in1", &m) == GRAPH_OK);
	CHECK_STREQ(m.text, "");
	teardown();
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	RUN(test_new_graph);
	RUN(test_new_graph_refusals);
	RUN(test_uuid);
	RUN(test_delete_graph);
	RUN(test_rename_graph);
	RUN(test_start_stop);
	RUN(test_ports);
	RUN(test_ports_follow_block);
	RUN(test_scoping);
	RUN(test_change_edge);
	RUN(test_applied_state);
	RUN(test_subscribers_during_notification);
	RUN(test_refusal_message_render);
	RUN(test_command_refusals);
	block_types_reset();
	return test_result();
}
