/*
 * Links between processes (01 §2.2 Port, graphs/graph.h; plan task 21f) on
 * the Graphs registry with controlled draining: values cross a link through
 * the work queue with the source block's change threshold; no fan-in in
 * either direction; a stopped source keeps the target's last value, its
 * start pushes; removing the link, deleting either process, removing or
 * renaming a linked port, removing a linked port's block — each reported
 * (LINK_REMOVED, in its place in the compound's events) and the target
 * input reverting to its default, which propagates; values queued from a
 * removed link dropped; a link within one process; renames that keep the
 * link; the exact refusal messages; links left at shutdown (asan).
 */
#include "test.h"

#include <stdarg.h>
#include <stdint.h>

#include "blocktypes/block_types.h"
#include "graphs/graphs.h"
#include "platform/pal.h"
#include "system/log.h"

static Runtime rt;
static Graphs gs;
static GraphsSubscriber sub;
static char events[4096];

static void append(const char *fmt, ...) LOG_PRINTF_FORMAT(1, 2);

static void append(const char *fmt, ...)
{
	size_t len = strlen(events);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(events + len, sizeof(events) - len, fmt, ap);
	va_end(ap);
}

static const char *in_name(const Block *b, uint32_t i)
{
	return block_input_port(b, i)->name;
}

/* One entry per event, prefixed with the graph id (a link's: its source). */
static void on_event(void *ctx, const GraphEvent *ev)
{
	(void)ctx;
	const char *id = ev->graph != NULL ? ev->graph->id : "-";
	const Block *b = ev->block;
	switch (ev->type) {
	case GRAPH_EVENT_LINK_ADDED:
	case GRAPH_EVENT_LINK_REMOVED:
		append("%s:%clink %s>%s.%s;", id, ev->type == GRAPH_EVENT_LINK_ADDED ? '+' : '-',
		       ev->link->src_port, ev->link->tgt_graph->id, ev->link->tgt_port);
		break;
	case GRAPH_EVENT_EDGE_REMOVED:
		append("%s:-edge %s.%s>%s.%s;", id, ev->src_block->name,
		       block_output_port(ev->src_block, ev->src_output)->name, b->name,
		       in_name(b, ev->input));
		break;
	case GRAPH_EVENT_INITIAL_REMOVED:
		append("%s:-init %s.%s;", id, b->name, in_name(b, ev->input));
		break;
	case GRAPH_EVENT_INPORT_REMOVED:
	case GRAPH_EVENT_OUTPORT_REMOVED:
		append("%s:-%s %s;", id,
		       ev->type == GRAPH_EVENT_INPORT_REMOVED ? "inport" : "outport",
		       ev->port->name);
		break;
	case GRAPH_EVENT_INPORT_RENAMED:
	case GRAPH_EVENT_OUTPORT_RENAMED:
		append("%s:rename%s %s>%s;", id,
		       ev->type == GRAPH_EVENT_INPORT_RENAMED ? "inport" : "outport", ev->old_name,
		       ev->port->name);
		break;
	case GRAPH_EVENT_BLOCK_REMOVED:
		append("%s:-node %s;", id, b->name);
		break;
	case GRAPH_EVENT_GRAPH_STARTED:
		append("%s:started;", id);
		break;
	case GRAPH_EVENT_GRAPH_STOPPED:
		append("%s:stopped;", id);
		break;
	case GRAPH_EVENT_GRAPH_DELETED:
		append("%s:-graph;", id);
		break;
	default: /* the others: tests/test_graphs.c */
		break;
	}
}

static void clear(void)
{
	events[0] = '\0';
}

static void drain(void)
{
	runtime_drain(&rt, SIZE_MAX);
}

static Block *block(const char *graph, const char *node)
{
	Graph *g = graphs_find(&gs, graph);
	return g != NULL ? graph_find_block(g, node) : NULL;
}

/* The current value of graph's node.port (an input) as a number. */
static double in(const char *graph, const char *node, const char *port)
{
	Block *b = block(graph, node);
	int32_t i = b != NULL ? block_input_index(b, port) : -1;
	CHECK(i >= 0);
	return i >= 0 ? linkable_to_f64(block_input_value(b, (uint32_t)i)) : -1;
}

static double out(const char *graph, const char *node)
{
	Block *b = block(graph, node);
	CHECK(b != NULL);
	return b != NULL ? linkable_to_f64(block_output_value(b, 0)) : -1;
}

static void init(const char *graph, const char *node, const char *port, double v)
{
	Linkable value = linkable_from_f64(v);
	CHECK(graphs_add_initial(&gs, graph, node, port, &value, NULL) == GRAPH_OK);
}

static void add(const char *graph, const char *node)
{
	CHECK(graphs_add_node(&gs, graph, node, "math/Add2", NULL, NULL) == GRAPH_OK);
}

static void port(const char *graph, enum graph_port_dir dir, const char *name, const char *node,
		 const char *p)
{
	CHECK(graphs_add_port(&gs, graph, dir, name, node, p, NULL) == GRAPH_OK);
}

static enum graph_status link(const char *from, const char *outport, const char *to,
			      const char *inport)
{
	return graphs_add_link(&gs, from, outport, to, inport, NULL);
}

static void start(const char *graph)
{
	CHECK(graphs_start(&gs, graph, NULL) == GRAPH_OK);
}

static void stop(const char *graph)
{
	CHECK(graphs_stop(&gs, graph, NULL) == GRAPH_OK);
}

/* Expect a refusal with status st and message text, and no event. */
#define REFUSED(call, st, want)                                                                    \
	do {                                                                                       \
		GraphsMessage m_;                                                                  \
		clear();                                                                           \
		CHECK((call) == (st));                                                             \
		CHECK_STREQ(m_.text, (want));                                                      \
		CHECK_STREQ(events, "");                                                           \
	} while (0)

/*
 * Two processes, every block math/Add2 (out = in1 + in2):
 *   A: s (s.in1 = 1); outport o = s.out, inport ai = s.in2;
 *   B: t -> u (t.out to u.in1); inports i = t.in1, i2 = t.in2; outport bo =
 *      u.out.
 * Both stopped, no link yet, the event log cleared.
 */
static void build(void)
{
	runtime_init(&rt);
	graphs_init(&gs, &rt, block_types_get, pal_random_bytes);
	graphs_subscriber_init(&sub, on_event, NULL);
	graphs_subscribe(&gs, &sub);
	CHECK(graphs_new_graph(&gs, "A", "A", NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_new_graph(&gs, "B", "B", NULL, NULL, NULL, NULL) == GRAPH_OK);
	add("A", "s");
	port("A", GRAPH_OUTPORT, "o", "s", "out");
	port("A", GRAPH_INPORT, "ai", "s", "in2");
	init("A", "s", "in1", 1);
	add("B", "t");
	add("B", "u");
	CHECK(graphs_add_edge(&gs, "B", "t", "out", "u", "in1", NULL, NULL) == GRAPH_OK);
	port("B", GRAPH_INPORT, "i", "t", "in1");
	port("B", GRAPH_INPORT, "i2", "t", "in2");
	port("B", GRAPH_OUTPORT, "bo", "u", "out");
	clear();
}

/* Links still there are dropped silently (asan: nothing dangles). */
static void teardown(void)
{
	graphs_deinit(&gs);
	runtime_deinit(&rt);
}

/* ---- Values ------------------------------------------------------------------------ */

static void test_values(void)
{
	build();
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	CHECK_STREQ(events, "A:+link o>B.i;");
	CHECK(runtime_link_count(&rt) == 1);
	const ProcessLink *l = runtime_link_at(&rt, 0);
	CHECK(l->src_graph == graphs_find(&gs, "A") && strcmp(l->src_port, "o") == 0);
	CHECK(l->tgt_graph == graphs_find(&gs, "B") && strcmp(l->tgt_port, "i") == 0);
	CHECK(in("B", "t", "in1") == 0); /* A is stopped: nothing to deliver */

	/* A's start pushes s.out through the queue, like an edge. */
	start("B");
	start("A");
	CHECK(in("B", "t", "in1") == 0);
	drain();
	CHECK(in("B", "t", "in1") == 1 && in("B", "u", "in1") == 1);
	init("A", "s", "in1", 2);
	CHECK(in("B", "t", "in1") == 1); /* queued */
	drain();
	CHECK(in("B", "t", "in1") == 2 && out("B", "u") == 2);

	/* The source block's change threshold (engine-wide 0.05, 01 §3.3). */
	init("A", "s", "in1", 2.01);
	drain();
	CHECK(in("B", "t", "in1") == 2);
	init("A", "s", "in1", 2.06);
	drain();
	CHECK(in("B", "t", "in1") == 2.06);
	teardown();
}

/* A link made while the source runs delivers its output at once. */
static void test_new_link_delivers(void)
{
	build();
	start("A");
	start("B");
	drain();
	CHECK(in("B", "t", "in1") == 0);
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	CHECK(in("B", "t", "in1") == 1 && out("B", "t") == 1);
	drain();
	CHECK(in("B", "u", "in1") == 1);
	teardown();
}

/* A stopped source keeps the target's last value; its start pushes. */
static void test_stopped_source(void)
{
	build();
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	start("A");
	start("B");
	drain();
	CHECK(in("B", "t", "in1") == 1);
	stop("A");
	init("A", "s", "in1", 5);
	drain();
	CHECK(in("B", "t", "in1") == 1 && in("B", "u", "in1") == 1);
	start("A");
	drain();
	CHECK(in("B", "t", "in1") == 5 && in("B", "u", "in1") == 5);
	/* A stopped target only stores (01 §2.2), like any stopped graph. */
	stop("B");
	init("A", "s", "in1", 6);
	drain();
	CHECK(in("B", "t", "in1") == 6 && in("B", "u", "in1") == 5);
	teardown();
}

/* ---- Ending a link --------------------------------------------------------------- */

/* Removed: the target reverts at once and propagates; values queued from
 * the link are dropped. */
static void test_remove(void)
{
	build();
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	start("A");
	start("B");
	drain();
	CHECK(in("B", "u", "in1") == 1);
	init("A", "s", "in1", 7); /* s.out = 7, queued for t.in1 */
	clear();
	GraphsMessage m;
	CHECK(graphs_remove_link(&gs, "A", "o", "B", "i", &m) == GRAPH_OK);
	CHECK_STREQ(m.text, "");
	CHECK_STREQ(events, "A:-link o>B.i;");
	CHECK(runtime_link_count(&rt) == 0);
	CHECK(block("A", "s")->outputs[0].link_count == 0);
	CHECK(in("B", "t", "in1") == 0 && out("B", "t") == 0);
	drain();
	CHECK(in("B", "t", "in1") == 0); /* the queued 7 was dropped */
	CHECK(in("B", "u", "in1") == 0); /* the revert propagated */
	/* The input is free again: an edge, an initial, a new link. */
	init("B", "t", "in1", 3);
	CHECK(graphs_remove_initial(&gs, "B", "t", "in1", NULL) == GRAPH_OK);
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	CHECK(in("B", "t", "in1") == 7);
	REFUSED(graphs_remove_link(&gs, "A", "o", "B", "i2", &m_), GRAPH_ERR_NO_SUCH_LINK,
		"No such link");
	REFUSED(graphs_remove_link(&gs, "B", "i", "A", "o", &m_), GRAPH_ERR_NO_SUCH_LINK,
		"No such link");
	REFUSED(graphs_remove_link(&gs, "Z", "o", "B", "i", &m_), GRAPH_ERR_UNKNOWN_GRAPH,
		"Unknown graph: Z");
	REFUSED(graphs_remove_link(&gs, "A", "o", "B", "", &m_), GRAPH_ERR_INVALID_ID,
		"Invalid id");
	teardown();
}

/* Deleting the source: stopped, the link removed (the target reverts and
 * propagates), deleted (02 §1.1). Deleting the target likewise. */
static void test_delete_process(void)
{
	build();
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	start("A");
	start("B");
	drain();
	CHECK(in("B", "u", "in1") == 1);
	clear();
	CHECK(graphs_delete_graph(&gs, "A", "A", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "A:stopped;A:-link o>B.i;A:-graph;");
	CHECK(runtime_link_count(&rt) == 0);
	CHECK(in("B", "t", "in1") == 0);
	drain();
	CHECK(in("B", "u", "in1") == 0);
	teardown();

	build();
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	start("A");
	clear();
	CHECK(graphs_delete_graph(&gs, "B", "B", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "A:-link o>B.i;B:-graph;"); /* B never ran: no stop */
	CHECK(block("A", "s")->outputs[0].link_count == 0);
	init("A", "s", "in1", 3); /* nothing left to push to */
	drain();
	CHECK(runtime_link_count(&rt) == 0);
	teardown();
}

/* A linked port removed or renamed (also with its block): the link goes
 * first, then the port (decided, 01 §2.2). */
static void test_ports(void)
{
	build();
	start("A");
	start("B");
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	CHECK(in("B", "t", "in1") == 1);
	clear();
	CHECK(graphs_remove_port(&gs, "A", GRAPH_OUTPORT, "o", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "A:-link o>B.i;A:-outport o;");
	CHECK(in("B", "t", "in1") == 0);

	port("A", GRAPH_OUTPORT, "o", "s", "out");
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	clear();
	CHECK(graphs_rename_port(&gs, "B", GRAPH_INPORT, "i", "j", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "A:-link o>B.i;B:renameinport i>j;");
	CHECK(in("B", "t", "in1") == 0 && runtime_link_count(&rt) == 0);
	CHECK(link("A", "o", "B", "j") == GRAPH_OK);
	clear();
	CHECK(graphs_rename_port(&gs, "A", GRAPH_OUTPORT, "o", "o2", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "A:-link o>B.j;A:renameoutport o>o2;");
	/* A refused rename or removal changes nothing. */
	CHECK(link("A", "o2", "B", "j") == GRAPH_OK);
	REFUSED(graphs_rename_port(&gs, "A", GRAPH_OUTPORT, "o2", "o2", &m_),
		GRAPH_ERR_PORT_EXISTS, "Port exists: o2");
	CHECK(runtime_link_count(&rt) == 1);

	/* The source's block removed: its initial, its ports (inports first),
	 * each with its links, then the block. */
	clear();
	CHECK(graphs_remove_node(&gs, "A", "s", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "A:-init s.in1;A:-inport ai;A:-link o2>B.j;A:-outport o2;A:-node s;");
	CHECK(in("B", "t", "in1") == 0 && runtime_link_count(&rt) == 0);
	teardown();

	/* The target's block removed: its edges, then its inports' links. */
	build();
	start("A");
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	clear();
	CHECK(graphs_remove_node(&gs, "B", "t", NULL) == GRAPH_OK);
	CHECK_STREQ(events,
		    "B:-edge t.out>u.in1;A:-link o>B.i;B:-inport i;B:-inport i2;B:-node t;");
	CHECK(block("A", "s")->outputs[0].link_count == 0 && runtime_link_count(&rt) == 0);
	init("A", "s", "in1", 4);
	drain();
	teardown();
}

/* Renaming a block or a process keeps the link: a port follows its block
 * and a process's id never changes. */
static void test_renames_keep_link(void)
{
	build();
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	start("A");
	start("B");
	CHECK(graphs_rename_node(&gs, "A", "s", "s2", NULL) == GRAPH_OK);
	CHECK(graphs_rename_node(&gs, "B", "t", "t2", NULL) == GRAPH_OK);
	CHECK(graphs_rename_graph(&gs, "A", "Renamed", NULL) == GRAPH_OK);
	CHECK(runtime_link_count(&rt) == 1);
	init("A", "s2", "in1", 8);
	drain();
	CHECK(in("B", "t2", "in1") == 8);
	teardown();
}

/* A process's outport linked to its own inport. */
static void test_self_link(void)
{
	build();
	add("A", "s2");
	port("A", GRAPH_INPORT, "i2", "s2", "in1");
	CHECK(link("A", "o", "A", "i2") == GRAPH_OK);
	start("A");
	drain();
	CHECK(in("A", "s2", "in1") == 1);
	clear();
	CHECK(graphs_delete_graph(&gs, "A", "A", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "A:stopped;A:-link o>A.i2;A:-graph;");
	teardown();
}

/* ---- No fan-in, and the refusals --------------------------------------------- */

static void test_fan_in(void)
{
	build();
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	/* Onto a linked input: no edge, no initial. */
	REFUSED(graphs_add_edge(&gs, "B", "u", "out", "t", "in1", NULL, &m_),
		GRAPH_ERR_INPUT_CONNECTED, "Input already connected: t.in1");
	REFUSED(graphs_add_initial(&gs, "B", "t", "in1", &(Linkable){0}, &m_),
		GRAPH_ERR_INPUT_CONNECTED, "Input already connected: t.in1");
	/* A link onto an input with an initial, an edge, a retained
	 * connection or another link. */
	init("B", "t", "in2", 5);
	REFUSED(graphs_add_link(&gs, "A", "o", "B", "i2", &m_), GRAPH_ERR_INPUT_CONNECTED,
		"Input already connected: B.i2");
	port("B", GRAPH_INPORT, "ui", "u", "in1");
	REFUSED(graphs_add_link(&gs, "A", "o", "B", "ui", &m_), GRAPH_ERR_INPUT_CONNECTED,
		"Input already connected: B.ui");
	Graph *b = graphs_find(&gs, "B");
	Block *ph = NULL;
	CHECK(graph_create_placeholder(b, "ph", "no/Such", NULL, &ph) == GRAPH_OK);
	add("B", "v");
	Block *v = block("B", "v");
	RetainedConnection rc = {.src = ph, .src_port = "x", .tgt = v, .tgt_port = "in1"};
	CHECK(graph_retain_connection(b, &rc) == GRAPH_OK);
	port("B", GRAPH_INPORT, "vi", "v", "in1");
	REFUSED(graphs_add_link(&gs, "A", "o", "B", "vi", &m_), GRAPH_ERR_INPUT_CONNECTED,
		"Input already connected: B.vi");
	CHECK(graphs_new_graph(&gs, "C", "C", NULL, NULL, NULL, NULL) == GRAPH_OK);
	add("C", "c");
	port("C", GRAPH_OUTPORT, "co", "c", "out");
	REFUSED(graphs_add_link(&gs, "C", "co", "B", "i", &m_), GRAPH_ERR_INPUT_CONNECTED,
		"Input already connected: B.i");
	REFUSED(graphs_add_link(&gs, "A", "o", "B", "i", &m_), GRAPH_ERR_LINK_EXISTS,
		"Link exists");
	/* An outport may feed several inports (fan-out). */
	port("B", GRAPH_INPORT, "free", "v", "in2");
	CHECK(link("A", "o", "B", "free") == GRAPH_OK);
	CHECK(runtime_link_count(&rt) == 2);
	teardown();
}

static void test_refusals(void)
{
	build();
	REFUSED(graphs_add_link(&gs, "", "o", "B", "i", &m_), GRAPH_ERR_INVALID_ID, "Invalid id");
	REFUSED(graphs_add_link(&gs, "A", NULL, "B", "i", &m_), GRAPH_ERR_INVALID_ID,
		"Invalid id");
	REFUSED(graphs_add_link(&gs, "Z", "o", "Y", "", &m_), GRAPH_ERR_INVALID_ID, "Invalid id");
	REFUSED(graphs_add_link(&gs, "Z", "o", "Y", "i", &m_), GRAPH_ERR_UNKNOWN_GRAPH,
		"Unknown graph: Z");
	REFUSED(graphs_add_link(&gs, "A", "o", "Y", "i", &m_), GRAPH_ERR_UNKNOWN_GRAPH,
		"Unknown graph: Y");
	REFUSED(graphs_add_link(&gs, "A", "nope", "B", "nope", &m_), GRAPH_ERR_UNKNOWN_PORT,
		"Unknown port: A.nope");
	REFUSED(graphs_add_link(&gs, "A", "o", "B", "nope", &m_), GRAPH_ERR_UNKNOWN_PORT,
		"Unknown port: B.nope");
	/* ai is an inport of A, bo an outport of B. */
	REFUSED(graphs_add_link(&gs, "A", "ai", "B", "i", &m_), GRAPH_ERR_INVALID_LINK_DIRECTION,
		"Invalid link direction");
	REFUSED(graphs_add_link(&gs, "A", "o", "B", "bo", &m_), GRAPH_ERR_INVALID_LINK_DIRECTION,
		"Invalid link direction");
	REFUSED(graphs_add_link(&gs, "A", "ai", "B", "nope", &m_),
		GRAPH_ERR_INVALID_LINK_DIRECTION, "Invalid link direction");
	CHECK(runtime_link_count(&rt) == 0);
	teardown();
}

/* Links left at shutdown: graph_destroy drops them silently, in whichever
 * order the processes go. */
static void test_shutdown(void)
{
	build();
	add("A", "s2");
	port("A", GRAPH_INPORT, "i2", "s2", "in1");
	CHECK(link("A", "o", "B", "i") == GRAPH_OK);
	CHECK(link("B", "bo", "A", "i2") == GRAPH_OK);
	CHECK(link("A", "o", "A", "ai") == GRAPH_OK);
	start("A");
	start("B");
	init("A", "s", "in1", 2); /* values queued across both links */
	clear();
	teardown();
	CHECK_STREQ(events, "");
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	RUN(test_values);
	RUN(test_new_link_delivers);
	RUN(test_stopped_source);
	RUN(test_remove);
	RUN(test_delete_process);
	RUN(test_ports);
	RUN(test_renames_keep_link);
	RUN(test_self_link);
	RUN(test_fan_in);
	RUN(test_refusals);
	RUN(test_shutdown);
	block_types_reset();
	return test_result();
}
