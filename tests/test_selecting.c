/*
 * The selecting block types of plan Phase 13: math/Min and math/Max
 * (03 §3.10), which ignore an input fed by nothing (BlockCtx.sourced,
 * 01 §2.3), and the selectors (03 §3.11). Driven in a graph, as
 * tests/test_process_more.c does: inputs set as initials or connections,
 * then the queue drained.
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

static void set_bool(const char *node, const char *port, bool v)
{
	Linkable l = linkable_from_bool(v);
	CHECK(graph_add_initial(gr, node, port, &l) == GRAPH_OK);
	drain();
}

static double out_at(const Block *b, uint32_t i)
{
	const Linkable *v = block_output_value(b, i);
	CHECK(v->type == LINKABLE_F64);
	return linkable_to_f64(v);
}

static double out(const Block *b)
{
	return out_at(b, 0);
}

/* The outputs of b, out1... as 1, 2, ...: which one holds want (0 if none
 * does), every other being 0. */
static int holding(const Block *b, double want)
{
	int found = 0;
	for (uint32_t i = 0; i < b->type->output_count; i++) {
		double v = out_at(b, i);
		if (v == want && found == 0 && want != 0)
			found = (int)i + 1;
		else
			CHECK(v == 0);
	}
	return found;
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

/* ---- The selectors (03 §3.11) --------------------------------------------- */

static void test_select_in_out(void)
{
	setup();
	Block *si = add("si", "process/SelectIn"), *so = add("so", "process/SelectOut");
	set("si", "in_false", 3);
	set("si", "in_true", 9);
	set("so", "input", 5);
	graph_start(gr);
	drain();
	/* An unfed select is false: the false side. */
	CHECK(out(si) == 3);
	CHECK(holding(so, 5) == 1);
	set_bool("si", "select", true);
	set_bool("so", "select", true);
	CHECK(out(si) == 9);
	CHECK(holding(so, 5) == 2);
	/* The chosen side follows its value; the other does not matter. */
	set("si", "in_true", -1);
	set("si", "in_false", 100);
	CHECK(out(si) == -1);
	set("so", "input", 7.5);
	CHECK(holding(so, 7.5) == 2);
	/* A number on select converts (01 §2.1): non-zero is true. */
	set("si", "select", 0);
	CHECK(out(si) == 100);
	set("so", "select", 2);
	CHECK(holding(so, 7.5) == 2);
	unset("so", "select");
	CHECK(holding(so, 7.5) == 1);
	teardown();
}

static void test_multiplex(void)
{
	setup();
	Block *mi = add("mi", "process/MultiplexIn"), *mo = add("mo", "process/MultiplexOut");
	for (int i = 1; i <= 8; i++) {
		char port[16];
		snprintf(port, sizeof(port), "in%d", i);
		set("mi", port, 10 * i);
	}
	set("mo", "input", 4);
	graph_start(gr);
	drain();
	/* An unfed select (0) chooses none. */
	CHECK(out(mi) == 0);
	CHECK(holding(mo, 4) == 0);
	for (int i = 1; i <= 8; i++) {
		set("mi", "select", i);
		set("mo", "select", i);
		CHECK(out(mi) == 10 * i);
		CHECK(holding(mo, 4) == i);
	}
	/* Rounded to the nearest whole number, halves away from zero. */
	static const struct {
		double select;
		int chosen;
	} cases[] = {{2.4, 2}, {2.5, 3}, {0.5, 1}, {0.49, 0}, {8.49, 8}, {8.5, 0},
		     {-1, 0},  {9, 0},   {NAN, 0},  {INFINITY, 0}, {-INFINITY, 0}};
	for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
		set("mi", "select", cases[k].select);
		set("mo", "select", cases[k].select);
		CHECK(out(mi) == 10 * cases[k].chosen);
		CHECK(holding(mo, 4) == cases[k].chosen);
	}
	/* The chosen output follows input; a stop changes nothing. */
	set("mo", "select", 6);
	set("mo", "input", -3);
	CHECK(holding(mo, -3) == 6);
	graph_stop(gr);
	drain();
	CHECK(holding(mo, -3) == 6);
	teardown();
}

int main(void)
{
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	RUN(test_min_max_connected_only);
	RUN(test_min_max_values);
	RUN(test_min_max_connections);
	RUN(test_min_max_in_composite);
	RUN(test_select_in_out);
	RUN(test_multiplex);
	return test_result();
}
