/*
 * The process/ types of 03 §3.2-§3.9 and §7.1 (blocktypes/process_more.c):
 * their ports, and their behavior in a graph driven with explicit engine
 * time like tests/test_pid.c — inputs are set as initials, the tests play the
 * engine loop (runtime_fire_timers, then drain).
 */
#include "test.h"

#include <math.h>
#include <stdint.h>
#include <strings.h>

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

static void set_str(const char *port, const char *text)
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
static size_t fire(uint64_t now)
{
	size_t n = runtime_fire_timers(&rt, now);
	drain();
	return n;
}

static uint64_t next_due(void)
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

/* ---- process/Alarm (03 §7.1) ------------------------------------------------ */

static const char *const alarm_states[] = {
	"normal", "activated", "deactivated", "reactivated", "acknowledged", "disabled", "blocked",
};

/* The alarm is in state name (e.g. "Activated"): its boolean output alone is
 * true, and the state string names it. */
static bool alarm_is(const char *name)
{
	bool ok = true;
	for (size_t i = 0; i < sizeof(alarm_states) / sizeof(alarm_states[0]); i++) {
		bool want = strcasecmp(alarm_states[i], name) == 0;
		if (flag(alarm_states[i]) != want)
			ok = false;
	}
	const Linkable *v = output("state");
	if (v->type != LINKABLE_STRING || strcmp(v->as.s, name) != 0)
		ok = false;
	if (!ok)
		fprintf(stderr, "  not in state %s (state output: %s)\n", name,
			v->type == LINKABLE_STRING ? v->as.s : "?");
	return ok;
}

static void ack(void)
{
	set_bool("acknowledge", true);
	set_bool("acknowledge", false);
}

static void test_alarm_ports(void)
{
	static const char *const ins[] = {"input:bool",	  "acknowledge:bool", "block:bool",
					  "disable:bool", "category:string",  "class:string"};
	static const char *const outs[] = {
		"normal:bool",	     "activated:bool", "deactivated:bool", "reactivated:bool",
		"acknowledged:bool", "disabled:bool",  "blocked:bool",	   "state:string",
	};
	CHECK_PORTS("process/Alarm", ins, outs);
	const BlockTypeDef *t = block_types_get("process/Alarm");
	CHECK(t->compute_on_input && t->timer == TIMER_NONE && t->state_size > 0);
}

/* The transition table, every row. */
static void test_alarm_transitions(void)
{
	setup("process/Alarm");
	start_at(0);
	CHECK(alarm_is("Normal")); /* at rest: enabled, not blocked */
	ack();
	CHECK(alarm_is("Normal")); /* Ack in Normal: nothing */
	set_bool("input", true);
	CHECK(alarm_is("Activated"));
	set_bool("input", false);
	CHECK(alarm_is("Deactivated"));
	set_bool("input", true);
	CHECK(alarm_is("Reactivated"));
	set_bool("input", false);
	CHECK(alarm_is("Deactivated"));
	set_bool("input", true);
	ack();
	CHECK(alarm_is("Acknowledged")); /* from Reactivated */
	ack();
	CHECK(alarm_is("Acknowledged"));
	set_bool("input", false);
	CHECK(alarm_is("Normal"));

	set_bool("input", true);
	ack();
	CHECK(alarm_is("Acknowledged")); /* from Activated */
	set_bool("input", false);
	set_bool("input", true);
	CHECK(alarm_is("Activated"));
	set_bool("input", false);
	ack();
	CHECK(alarm_is("Normal")); /* from Deactivated */

	/* Holding acknowledge does nothing more: only its rising edge counts. */
	set_bool("acknowledge", true);
	set_bool("input", true);
	CHECK(alarm_is("Activated"));
	set("input", 1); /* the same condition again: no change */
	CHECK(alarm_is("Activated"));
	set_bool("acknowledge", false);
	CHECK(alarm_is("Activated"));
	ack();
	CHECK(alarm_is("Acknowledged"));
	teardown();
}

static void test_alarm_disable_block(void)
{
	setup("process/Alarm");
	set_bool("input", true);
	start_at(0);
	CHECK(alarm_is("Activated")); /* START: Normal, then the condition */

	set_bool("block", true);
	CHECK(alarm_is("Blocked"));
	set_bool("input", false);
	ack();
	CHECK(alarm_is("Blocked")); /* no state changes */
	set_bool("disable", true);
	CHECK(alarm_is("Disabled")); /* disable wins */
	set_bool("block", false);
	CHECK(alarm_is("Disabled"));
	set_bool("block", true);
	set_bool("disable", false);
	CHECK(alarm_is("Blocked"));
	set_bool("block", false);
	CHECK(alarm_is("Normal")); /* Normal, then input (false) */

	set_bool("input", true);
	ack();
	CHECK(alarm_is("Acknowledged"));
	set_bool("disable", true);
	CHECK(alarm_is("Disabled"));
	set_bool("disable", false);
	CHECK(alarm_is("Activated")); /* a new alarm: Normal, then the condition */
	teardown();

	/* Started disabled or blocked. */
	setup("process/Alarm");
	set_bool("disable", true);
	set_bool("block", true);
	set_bool("input", true);
	start_at(0);
	CHECK(alarm_is("Disabled"));
	teardown();
	setup("process/Alarm");
	set_bool("block", true);
	start_at(0);
	CHECK(alarm_is("Blocked"));
	teardown();
}

/* category and class change nothing; nothing survives a stop. */
static void test_alarm_presentation_and_restart(void)
{
	setup("process/Alarm");
	start_at(0);
	set_bool("input", true);
	set_str("category", "Elevators");
	set_str("class", "A");
	CHECK(alarm_is("Activated"));
	set_str("class", "X");
	CHECK(alarm_is("Activated"));
	ack();
	CHECK(alarm_is("Acknowledged"));
	stop_at(10);
	CHECK(alarm_is("Acknowledged")); /* STOP changes nothing */
	start_at(20);
	CHECK(alarm_is("Activated")); /* the acknowledgement did not survive */
	teardown();
}

/* ---- process/Delay (03 §3.4) ------------------------------------------------ */

static void set_bool_at(uint64_t now, const char *port, bool v)
{
	set_at(now, port, linkable_from_bool(v));
}

static void test_delay(void)
{
	static const char *const ins[] = {"input:bool", "milliseconds:number"};
	static const char *const outs[] = {"output:bool"};
	CHECK_PORTS("process/Delay", ins, outs);
	const BlockTypeDef *t = block_types_get("process/Delay");
	CHECK(t->compute_on_input && t->timer == TIMER_SCHEDULED && t->state_size > 0);

	/* START: output = input at once. */
	setup("process/Delay");
	set("milliseconds", 1000);
	set_bool("input", true);
	start_at(0);
	CHECK(flag("output"));
	CHECK(next_due() == UINT64_MAX);

	/* Both edges are delayed; every change starts the wait over. */
	set_bool_at(100, "input", false);
	CHECK(flag("output") && next_due() == 1100);
	set_bool_at(600, "input", true); /* back to output's value: nothing pending */
	CHECK(flag("output") && next_due() == UINT64_MAX);
	set_bool_at(700, "input", false);
	CHECK(next_due() == 1700);
	CHECK(fire(1699) == 0 && flag("output"));
	CHECK(fire(1700) == 1 && !flag("output"));
	CHECK(next_due() == UINT64_MAX);

	/* A pulse shorter than the delay never gets through. */
	set_bool_at(2000, "input", true);
	set_bool_at(2500, "input", false);
	CHECK(!flag("output") && next_due() == UINT64_MAX);
	/* The same value again is no change. */
	set_bool_at(3000, "input", true);
	set_at(3400, "input", linkable_from_f64(5));
	CHECK(next_due() == 4000);
	/* A late timer applies the change then. */
	CHECK(fire(4250) == 1 && flag("output"));

	/* milliseconds changed during a wait counts from the change of input. */
	set_bool_at(5000, "input", false);
	set_at(5100, "milliseconds", linkable_from_f64(300));
	CHECK(next_due() == 5300);
	set_at(5200, "milliseconds", linkable_from_f64(150)); /* already lasted that long */
	CHECK(!flag("output") && next_due() == UINT64_MAX);
	teardown();
}

/* milliseconds <= 0, NaN or infinite: no delay (its resting value 0). */
static void test_delay_none(void)
{
	setup("process/Delay");
	start_at(0);
	CHECK(!flag("output"));
	set_bool_at(10, "input", true);
	CHECK(flag("output") && next_due() == UINT64_MAX);
	set_at(20, "milliseconds", linkable_from_f64(-5));
	set_bool_at(30, "input", false);
	CHECK(!flag("output"));
	set_at(40, "milliseconds", linkable_from_f64(NAN));
	set_bool_at(50, "input", true);
	CHECK(flag("output"));
	/* Rounded to whole milliseconds. */
	set_at(60, "milliseconds", linkable_from_f64(99.6));
	set_bool_at(70, "input", false);
	CHECK(next_due() == 170);
	/* A stop cancels the wait; the start takes input at once. */
	stop_at(80);
	CHECK(next_due() == UINT64_MAX);
	start_at(90);
	CHECK(!flag("output") && next_due() == UINT64_MAX);
	teardown();
}

/* ---- process/Trigger (03 §3.5) ---------------------------------------------- */

static void test_trigger(void)
{
	static const char *const ins[] = {"arm:bool", "fire:bool"};
	static const char *const outs[] = {"output:bool"};
	CHECK_PORTS("process/Trigger", ins, outs);
	const BlockTypeDef *t = block_types_get("process/Trigger");
	CHECK(t->compute_on_input && t->timer == TIMER_NONE && t->state_size > 0);

	setup("process/Trigger");
	start_at(0);
	CHECK(!flag("output"));
	set_bool("fire", true); /* fire rises, also while not armed */
	CHECK(flag("output"));
	set_bool("arm", true); /* arm rises while fire is true: false */
	CHECK(!flag("output"));
	set_bool("fire", false);
	CHECK(!flag("output"));
	set_bool("fire", true);
	CHECK(flag("output"));
	set_bool("fire", false); /* falling edges change nothing */
	set_bool("arm", false);
	CHECK(flag("output"));
	set_bool("arm", true);
	CHECK(!flag("output"));
	set("arm", 2); /* still true: no edge */
	set_bool("fire", true);
	CHECK(flag("output"));
	set("fire", 7);
	CHECK(flag("output"));
	teardown();

	/* Inputs true at the start are no edges. */
	setup("process/Trigger");
	set_bool("fire", true);
	start_at(0);
	CHECK(!flag("output"));
	set_bool("arm", true);
	set_bool("arm", false);
	CHECK(!flag("output"));
	set_bool("fire", false);
	set_bool("fire", true);
	CHECK(flag("output"));
	stop_at(10);
	CHECK(flag("output")); /* STOP changes nothing */
	start_at(20);
	CHECK(!flag("output")); /* fire still true: no edge at the restart */
	teardown();
}

/* ---- process/Limit (03 §3.6) ------------------------------------------------ */

static void test_limit(void)
{
	static const char *const ins[] = {"input:number", "min:number", "max:number"};
	static const char *const outs[] = {"output:number"};
	CHECK_PORTS("process/Limit", ins, outs);
	const BlockTypeDef *t = block_types_get("process/Limit");
	CHECK(t->compute_on_input && t->timer == TIMER_NONE && t->state_size == 0);

	setup("process/Limit");
	set("input", -1234.5);
	start_at(0);
	CHECK(near(num("output"), -1234.5)); /* at rest: equal limits, none */
	set("min", 10);
	set("max", 20);
	CHECK(near(num("output"), 10));
	set("input", 15);
	CHECK(near(num("output"), 15));
	set("input", 25);
	CHECK(near(num("output"), 20));
	set("min", 30); /* the wrong way round: swapped, [20, 30] */
	CHECK(near(num("output"), 25));
	set("input", 35);
	CHECK(near(num("output"), 30));
	set("max", 30); /* equal: no limits */
	CHECK(near(num("output"), 35));
	set("max", NAN); /* no upper limit; min 30 */
	CHECK(near(num("output"), 35));
	set("input", 5);
	CHECK(near(num("output"), 30));
	set("input", NAN);
	CHECK(isnan(num("output")));
	teardown();
}

/* ---- process/Hysteresis (03 §3.7) ------------------------------------------- */

static void test_hysteresis(void)
{
	static const char *const ins[] = {"input:number", "threshold:number", "hysteresis:number",
					  "inverted:bool"};
	static const char *const outs[] = {"output:bool"};
	CHECK_PORTS("process/Hysteresis", ins, outs);
	const BlockTypeDef *t = block_types_get("process/Hysteresis");
	CHECK(t->compute_on_input && t->timer == TIMER_NONE && t->state_size > 0);

	setup("process/Hysteresis");
	set("threshold", 20);
	set("hysteresis", 2);
	set("input", 21); /* inside the band at the start: off */
	start_at(0);
	CHECK(!flag("output"));
	set("input", 22); /* on only above 22 */
	CHECK(!flag("output"));
	set("input", 22.5);
	CHECK(flag("output"));
	set("input", 18.5); /* inside the band: stays on */
	CHECK(flag("output"));
	set("input", 18);
	CHECK(flag("output"));
	set("input", 17.9);
	CHECK(!flag("output"));
	set("input", 21);
	CHECK(!flag("output"));

	/* inverted negates at once, the state stays. */
	set_bool("inverted", true);
	CHECK(flag("output"));
	set("input", 30);
	CHECK(!flag("output"));
	set_bool("inverted", false);
	CHECK(flag("output"));

	/* A negative hysteresis counts as positive; NaN input or threshold holds. */
	set("hysteresis", -2);
	set("input", 19);
	CHECK(flag("output"));
	set("input", NAN);
	CHECK(flag("output"));
	set("input", 10);
	CHECK(!flag("output"));
	set("threshold", NAN);
	set("input", 100);
	CHECK(!flag("output"));
	teardown();

	/* At rest: a plain comparator around 0 that keeps its state at 0. */
	setup("process/Hysteresis");
	set("input", 1);
	start_at(0);
	CHECK(flag("output"));
	set("input", 0);
	CHECK(flag("output"));
	set("input", -0.5);
	CHECK(!flag("output"));
	set("input", 0);
	CHECK(!flag("output"));
	set("hysteresis", NAN); /* counts as 0 */
	set("input", 0.1);
	CHECK(flag("output"));
	stop_at(10);
	set("input", -5);
	CHECK(flag("output")); /* stopped: nothing computes */
	start_at(20);
	CHECK(!flag("output"));
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
	RUN(test_alarm_ports);
	RUN(test_alarm_transitions);
	RUN(test_alarm_disable_block);
	RUN(test_alarm_presentation_and_restart);
	RUN(test_delay);
	RUN(test_delay_none);
	RUN(test_trigger);
	RUN(test_limit);
	RUN(test_hysteresis);
	return test_result();
}
