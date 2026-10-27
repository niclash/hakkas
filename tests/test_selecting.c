/*
 * The selecting block types of plan Phase 13: math/Min and math/Max
 * (03 §3.10), which ignore an input fed by nothing (BlockCtx.sourced,
 * 01 §2.3). Driven in a graph, as tests/test_process_more.c does: inputs set
 * as initials or connections, then the queue drained.
 */
#include "test.h"

#include <math.h>
#include <string.h>

#include "blocktypes/block_types.h"
#include "blocktypes/composite_types.h"
#include "graphs/graph.h"

static Runtime rt;
static Graph *gr;

static void setup(void)
{
	runtime_init(&rt);
	gr = graph_create(&rt);
	CHECK(gr != NULL);
}

static void teardown(void)
{
	graph_destroy(gr);
	gr = NULL;
	runtime_deinit(&rt);
}

static void drain(void)
{
	runtime_drain(&rt, SIZE_MAX);
}

static Block *add(const char *name, const char *type)
{
	Block *b = NULL;
	CHECK(graph_create_block(gr, name, block_types_get(type), NULL, &b) == GRAPH_OK);
	return b;
}

static void set(const char *node, const char *port, double v)
{
	Linkable l = linkable_from_f64(v);
	CHECK(graph_add_initial(gr, node, port, &l) == GRAPH_OK);
	drain();
}

static void unset(const char *node, const char *port)
{
	CHECK(graph_remove_initial(gr, node, port) == GRAPH_OK);
	drain();
}

static double out(const Block *b)
{
	const Linkable *v = block_output_value(b, 0);
	CHECK(v->type == LINKABLE_F64);
	return linkable_to_f64(v);
}

/* ---- math/Min and math/Max (03 §3.10) ------------------------------------ */

static void test_min_max_connected_only(void)
{
	setup();
	Block *mn = add("mn", "math/Min"), *mx = add("mx", "math/Max");
	graph_start(gr);
	drain();
	/* Nothing fed: 0. */
	CHECK(out(mn) == 0 && out(mx) == 0);
	/* Negative values only: the resting 0 of the other inputs does not win. */
	set("mx", "in2", -5);
	set("mx", "in4", -3);
	CHECK(out(mx) == -3);
	set("mn", "in3", 4);
	set("mn", "in1", 7);
	CHECK(out(mn) == 4);
	/* An input fed 0 counts. */
	set("mx", "in1", 0);
	CHECK(out(mx) == 0);
	set("mn", "in2", 0);
	CHECK(out(mn) == 0);
	/* Removing a source takes the input out again. */
	unset("mx", "in1");
	CHECK(out(mx) == -3);
	unset("mn", "in2");
	unset("mn", "in3");
	CHECK(out(mn) == 7);
	unset("mn", "in1");
	CHECK(out(mn) == 0);
	teardown();
}

static void test_min_max_values(void)
{
	setup();
	Block *mn = add("mn", "math/Min"), *mx = add("mx", "math/Max");
	graph_start(gr);
	drain();
	set("mx", "in1", -INFINITY);
	CHECK(out(mx) == -INFINITY);
	set("mx", "in2", INFINITY);
	CHECK(out(mx) == INFINITY);
	/* A NaN among the fed inputs gives NaN: a failed sensor shows. */
	set("mx", "in3", NAN);
	CHECK(isnan(out(mx)));
	unset("mx", "in3");
	CHECK(out(mx) == INFINITY);
	set("mn", "in1", 2.5);
	set("mn", "in4", NAN);
	CHECK(isnan(out(mn)));
	teardown();
}

static void test_min_max_connections(void)
{
	setup();
	Block *a = add("a", "math/Add2");
	Block *mx = add("mx", "math/Max");
	(void)a;
	graph_start(gr);
	drain();
	set("a", "in1", -8);
	set("mx", "in1", -10);
	CHECK(out(mx) == -10);
	/* A connection is a source, whatever its value. */
	CHECK(graph_connect(gr, "a", "out", "mx", "in3", NULL) == GRAPH_OK);
	drain();
	CHECK(out(mx) == -8);
	set("a", "in1", -20);
	CHECK(out(mx) == -10);
	CHECK(graph_disconnect(gr, "a", "out", "mx", "in3") == GRAPH_OK);
	drain();
	CHECK(out(mx) == -10);
	teardown();
}

/* An inner input fed through an exposed input of its composite instance
 * (03 §5) has a source when that exposed input has one. */
static void test_min_max_in_composite(void)
{
	static const char doc[] =
		"{\"name\":\"t/Highest\",\"blocks\":[{\"name\":\"M\",\"type\":\"math/Max\"}],"
		"\"inports\":[{\"name\":\"a\",\"tgt\":\"M.in1\"},"
		"{\"name\":\"b\",\"tgt\":\"M.in2\"}],"
		"\"outports\":[{\"name\":\"out\",\"src\":\"M.out\"}]}";
	CompositeMessage cm;
	CHECK(composite_types_register_text(doc, strlen(doc), &cm, NULL) == COMPOSITE_OK);
	static const char outer[] =
		"{\"name\":\"t/Outer\",\"blocks\":[{\"name\":\"H\",\"type\":\"t/Highest\"}],"
		"\"inports\":[{\"name\":\"x\",\"tgt\":\"H.a\"}],"
		"\"outports\":[{\"name\":\"out\",\"src\":\"H.out\"}]}";
	CHECK(composite_types_register_text(outer, strlen(outer), &cm, NULL) == COMPOSITE_OK);
	setup();
	Block *h = add("h", "t/Highest"), *o = add("o", "t/Outer");
	graph_start(gr);
	drain();
	set("h", "b", -4);
	CHECK(out(h) == -4); /* a is fed by nothing */
	set("h", "a", -6);
	CHECK(out(h) == -4);
	unset("h", "b");
	CHECK(out(h) == -6);
	/* Through two levels; H.b is exposed by nothing. */
	set("o", "x", -2);
	CHECK(out(o) == -2);
	unset("o", "x");
	CHECK(out(o) == 0);
	teardown();
	composite_types_reset();
}

int main(void)
{
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	RUN(test_min_max_connected_only);
	RUN(test_min_max_values);
	RUN(test_min_max_connections);
	RUN(test_min_max_in_composite);
	return test_result();
}
