/*
 * The process/ types of 03 §3.2-§3.9 and §7.1 (blocktypes/process_more.c):
 * their ports, and their behavior in a graph driven with explicit engine
 * time like tests/test_pid.c — inputs are set as initials, the tests play the
 * engine loop (runtime_fire_timers, then drain).
 */
#include "test.h"

#include <math.h>
#include <stdint.h>

#include "blocktypes/block_types.h"
#include "graphs/deadlines.h"
#include "graphs/graph.h"

static Runtime rt;
static Graph *gr;
static Block *blk;

/* ---- Helpers ---------------------------------------------------------------- */

/* A graph with one block "B" of type, every input at its resting value. */
static void setup(const char *type)
{
	runtime_init(&rt);
	gr = graph_create(&rt);
	CHECK(gr != NULL);
	blk = NULL;
	CHECK(graph_create_block(gr, "B", block_types_get(type), NULL, &blk) == GRAPH_OK);
}

static void teardown(void)
{
	graph_destroy(gr);
	gr = NULL;
	CHECK(deadline_heap_len(&rt.timers) == 0);
	runtime_deinit(&rt);
}

static void drain(void)
{
	runtime_drain(&rt, SIZE_MAX);
}

/* Set an input at engine time now (an initial: delivered at once). */
static void set_at(uint64_t now, const char *port, Linkable v)
{
	runtime_set_now(&rt, now);
	CHECK(graph_add_initial(gr, "B", port, &v) == GRAPH_OK);
	linkable_clear(&v);
	drain();
}

static void set(const char *port, double v)
{
	set_at(rt.now_ms, port, linkable_from_f64(v));
}

static inline void set_bool(const char *port, bool v)
{
	set_at(rt.now_ms, port, linkable_from_bool(v));
}

static inline void set_str(const char *port, const char *text)
{
	Linkable v = {0};
	CHECK(linkable_from_string(&v, text, strlen(text)));
	set_at(rt.now_ms, port, v);
}

static void start_at(uint64_t now)
{
	runtime_set_now(&rt, now);
	graph_start(gr);
	drain();
}

static void stop_at(uint64_t now)
{
	runtime_set_now(&rt, now);
	graph_stop(gr);
	drain();
}

/* One engine loop iteration at time now. Returns the expiries fired. */
static inline size_t fire(uint64_t now)
{
	size_t n = runtime_fire_timers(&rt, now);
	drain();
	return n;
}

static inline uint64_t next_due(void)
{
	uint64_t due;
	return runtime_next_deadline(&rt, &due) ? due : UINT64_MAX;
}

static const Linkable *output(const char *port)
{
	int32_t i = block_output_index(blk, port);
	CHECK(i >= 0);
	return block_output_value(blk, (uint32_t)(i < 0 ? 0 : i));
}

static double num(const char *port)
{
	const Linkable *v = output(port);
	CHECK(v->type == LINKABLE_F64);
	return linkable_to_f64(v);
}

static bool flag(const char *port)
{
	const Linkable *v = output(port);
	CHECK(v->type == LINKABLE_BOOL);
	return linkable_to_bool(v);
}

static bool near(double a, double b)
{
	if (a == b || fabs(a - b) <= 1e-9 * fmax(1.0, fabs(b)))
		return true;
	fprintf(stderr, "  got %.17g, expected %.17g\n", a, b);
	return false;
}

/* The type's ports: names "in1:number,in2:bool" etc., in order. */
static void check_ports(const char *type, const char *const *ins, size_t n_in,
			const char *const *outs, size_t n_out)
{
	const BlockTypeDef *t = block_types_get(type);
	CHECK(t != NULL);
	if (t == NULL)
		return;
	CHECK(t->input_count == n_in && t->output_count == n_out);
	for (size_t k = 0; k < 2; k++) {
		const PortDescriptor *ports = k == 0 ? t->inputs : t->outputs;
		const char *const *want = k == 0 ? ins : outs;
		size_t n = k == 0 ? n_in : n_out;
		if (n != (k == 0 ? t->input_count : t->output_count))
			continue;
		for (size_t j = 0; j < n; j++) {
			char name[64];
			const char *colon = strchr(want[j], ':');
			snprintf(name, sizeof(name), "%.*s", (int)(colon - want[j]), want[j]);
			CHECK_STREQ(ports[j].name, name);
			CHECK_STREQ(ports[j].type, colon + 1);
			CHECK(ports[j].description != NULL && ports[j].description[0] != '\0');
			CHECK(!ports[j].addressable && ports[j].inner == NULL);
		}
	}
}

#define CHECK_PORTS(type, ins, outs) \
	check_ports(type, ins, sizeof(ins) / sizeof(ins[0]), outs, sizeof(outs) / sizeof(outs[0]))

/* ---- process/Curve (03 §3.2) ------------------------------------------------ */

static void curve_points(const double *x, const double *y)
{
	char port[4];
	for (int i = 0; i < 7; i++) {
		snprintf(port, sizeof(port), "x%d", i + 1);
		set(port, x[i]);
		snprintf(port, sizeof(port), "y%d", i + 1);
		set(port, y[i]);
	}
}

static void test_curve_ports(void)
{
	static const char *const ins[] = {
		"input:number", "x1:number", "x2:number", "x3:number", "x4:number",
		"x5:number",	"x6:number", "x7:number", "y1:number", "y2:number",
		"y3:number",	"y4:number", "y5:number", "y6:number", "y7:number",
	};
	static const char *const outs[] = {"output:number", "min:bool", "max:bool"};
	CHECK_PORTS("process/Curve", ins, outs);
	const BlockTypeDef *t = block_types_get("process/Curve");
	CHECK(t->compute_on_input && t->timer == TIMER_NONE && t->state_size == 0);
	CHECK(isnan(t->change_threshold));
}

/* The spec's example: unused points at (0, 0) after positive X drop out. */
static void test_curve_example(void)
{
	static const double x[7] = {0, 10, 20, 30, 0, 0, 0};
	static const double y[7] = {5, 15, 15, 40, 0, 0, 0};
	setup("process/Curve");
	start_at(0);
	curve_points(x, y);
	static const struct {
		double in, out;
		bool min, max;
	} cases[] = {
		{-3, 5, true, false},  {0, 5, false, false},   {5, 10, false, false},
		{10, 15, false, false}, {15, 15, false, false}, {25, 27.5, false, false},
		{30, 40, false, false}, {99, 40, false, true},
	};
	for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
		set("input", cases[k].in);
		CHECK(near(num("output"), cases[k].out));
		CHECK(flag("min") == cases[k].min);
		CHECK(flag("max") == cases[k].max);
	}
	teardown();
}

static void test_curve_points_used(void)
{
	setup("process/Curve");
	start_at(0);
	/* At rest every point is (0, 0): one point, output 0. */
	set("input", 4);
	CHECK(near(num("output"), 0) && flag("max") && !flag("min"));
	set("input", -4);
	CHECK(near(num("output"), 0) && flag("min") && !flag("max"));

	/* Falling X: point 3 (X 5, below point 2's 20) is skipped. */
	static const double x[7] = {0, 20, 5, 40, 0, 0, 0};
	static const double y[7] = {0, 20, 99, 0, 0, 0, 0};
	curve_points(x, y);
	set("input", 10);
	CHECK(near(num("output"), 10));
	set("input", 30);
	CHECK(near(num("output"), 10)); /* between (20,20) and (40,0) */

	/* A curve below 0: the unused points copy the last one. */
	static const double xn[7] = {-30, -20, -10, -10, -10, -10, -10};
	static const double yn[7] = {1, 2, 4, 4, 4, 4, 4};
	curve_points(xn, yn);
	set("input", -15);
	CHECK(near(num("output"), 3));
	set("input", 0);
	CHECK(near(num("output"), 4) && flag("max"));

	/* A non-finite X is skipped, also as point 1. */
	set("x1", NAN);
	set("input", -40);
	CHECK(near(num("output"), 2) && flag("min")); /* the curve starts at (-20, 2) */
	set("x1", INFINITY);
	CHECK(near(num("output"), 2));
	teardown();
}

static void test_curve_edge_cases(void)
{
	setup("process/Curve");
	set("x2", 10);
	set("y2", 100);
	set("input", 2.5);
	start_at(0);
	CHECK(near(num("output"), 25)); /* START computes */
	set("input", NAN);
	CHECK(isnan(num("output")) && !flag("min") && !flag("max"));
	/* No point used: output 0. */
	for (int i = 1; i <= 7; i++) {
		char port[4];
		snprintf(port, sizeof(port), "x%d", i);
		set(port, NAN);
	}
	set("input", 3);
	CHECK(near(num("output"), 0) && !flag("min") && !flag("max"));
	/* STOP changes nothing. */
	stop_at(10);
	CHECK(near(num("output"), 0));
	teardown();
}

/* ---- process/AutoMan (03 §3.3) ---------------------------------------------- */

static void test_automan(void)
{
	static const char *const ins[] = {"input:number", "override:bool", "manual:number"};
	static const char *const outs[] = {"output:number"};
	CHECK_PORTS("process/AutoMan", ins, outs);
	const BlockTypeDef *t = block_types_get("process/AutoMan");
	CHECK(t->compute_on_input && t->timer == TIMER_NONE && t->state_size == 0);

	setup("process/AutoMan");
	set("input", 12);
	set("manual", 40);
	start_at(0);
	CHECK(near(num("output"), 12)); /* at rest: not overridden */
	set("input", 13);
	CHECK(near(num("output"), 13));
	set_bool("override", true);
	CHECK(near(num("output"), 40)); /* at once, no bumpless transfer */
	set("input", 14);
	CHECK(near(num("output"), 40));
	set("manual", 41);
	CHECK(near(num("output"), 41));
	set_bool("override", false);
	CHECK(near(num("output"), 14));
	stop_at(10);
	set("input", 15); /* a stopped graph computes nothing */
	CHECK(near(num("output"), 14));
	start_at(20);
	CHECK(near(num("output"), 15));
	teardown();
}

int main(void)
{
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	RUN(test_curve_ports);
	RUN(test_curve_example);
	RUN(test_curve_points_used);
	RUN(test_curve_edge_cases);
	RUN(test_automan);
	return test_result();
}
