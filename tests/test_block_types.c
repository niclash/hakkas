/*
 * Block type registry and built-in stateless types (03 §3); the descriptors of
 * process/PID (its behavior: tests/test_pid.c) and timing/Interval (its
 * behavior: tests/test_timers.c). Every compute
 * function is called directly through its registered BlockTypeDef with a
 * hand-built BlockCtx, the way the generic runtime (plan task 6) will call it.
 */
#include "test.h"

#include <math.h>
#include <stdint.h>

#include "blocktypes/block_types.h"

/* ---- Registry ------------------------------------------------------------ */

static int64_t noop(BlockCtx *ctx)
{
	(void)ctx;
	return 0;
}

static const BlockTypeDef def_a = {.name = "test/A", .compute = noop, .change_threshold = NAN};
static const BlockTypeDef def_b = {.name = "test/B", .compute = noop, .change_threshold = NAN};
static const BlockTypeDef def_c = {.name = "test/C", .compute = noop, .change_threshold = NAN};
static const BlockTypeDef def_a_again = {.name = "test/A", .compute = noop};

static void test_register_and_lookup(void)
{
	block_types_reset();
	CHECK(block_types_count() == 0);
	CHECK(block_types_get("test/A") == NULL);
	CHECK(block_types_at(0) == NULL);

	CHECK(block_types_register(&def_a) == BLOCK_TYPES_OK);
	CHECK(block_types_register(&def_b) == BLOCK_TYPES_OK);
	CHECK(block_types_register(&def_c) == BLOCK_TYPES_OK);
	CHECK(block_types_count() == 3);

	CHECK(block_types_get("test/A") == &def_a);
	CHECK(block_types_get("test/B") == &def_b);
	CHECK(block_types_get("test/C") == &def_c);
	CHECK(block_types_get("test/a") == NULL); /* case-sensitive */
	CHECK(block_types_get("test/") == NULL);
	CHECK(block_types_get("") == NULL);
	CHECK(block_types_get(NULL) == NULL);

	/* Registration order. */
	CHECK(block_types_at(0) == &def_a);
	CHECK(block_types_at(1) == &def_b);
	CHECK(block_types_at(2) == &def_c);
	CHECK(block_types_at(3) == NULL);
	block_types_reset();
}

static void test_duplicate_refused(void)
{
	block_types_reset();
	CHECK(block_types_register(&def_a) == BLOCK_TYPES_OK);
	CHECK(block_types_register(&def_a) == BLOCK_TYPES_ERR_EXISTS);
	CHECK(block_types_register(&def_a_again) == BLOCK_TYPES_ERR_EXISTS);
	CHECK(block_types_count() == 1);
	CHECK(block_types_get("test/A") == &def_a);
	block_types_reset();
}

static void test_invalid_refused(void)
{
	static const BlockTypeDef no_name = {.compute = noop};
	static const BlockTypeDef empty_name = {.name = "", .compute = noop};
	block_types_reset();
	CHECK(block_types_register(NULL) == BLOCK_TYPES_ERR_INVALID);
	CHECK(block_types_register(&no_name) == BLOCK_TYPES_ERR_INVALID);
	CHECK(block_types_register(&empty_name) == BLOCK_TYPES_ERR_INVALID);
	CHECK(block_types_count() == 0);
}

static void test_unregister_keeps_order(void)
{
	block_types_reset();
	CHECK(block_types_register(&def_a) == BLOCK_TYPES_OK);
	CHECK(block_types_register(&def_b) == BLOCK_TYPES_OK);
	CHECK(block_types_register(&def_c) == BLOCK_TYPES_OK);

	CHECK(block_types_unregister("test/B"));
	CHECK(!block_types_unregister("test/B"));
	CHECK(!block_types_unregister("test/X"));
	CHECK(!block_types_unregister(NULL));
	CHECK(block_types_count() == 2);
	CHECK(block_types_get("test/B") == NULL);
	CHECK(block_types_at(0) == &def_a);
	CHECK(block_types_at(1) == &def_c);
	CHECK(block_types_at(2) == NULL);

	/* Re-registering (as a rename would) appends. */
	CHECK(block_types_register(&def_b) == BLOCK_TYPES_OK);
	CHECK(block_types_at(2) == &def_b);

	CHECK(block_types_unregister("test/A"));
	CHECK(block_types_unregister("test/B"));
	CHECK(block_types_unregister("test/C"));
	CHECK(block_types_count() == 0);
}

static void test_full(void)
{
	static char names[HAKKAS_BLOCK_TYPES_MAX + 1][16];
	static BlockTypeDef defs[HAKKAS_BLOCK_TYPES_MAX + 1];
	block_types_reset();
	for (int k = 0; k <= HAKKAS_BLOCK_TYPES_MAX; k++) {
		snprintf(names[k], sizeof(names[k]), "test/T%d", k);
		defs[k] = (BlockTypeDef){.name = names[k], .compute = noop};
	}
	for (int k = 0; k < HAKKAS_BLOCK_TYPES_MAX; k++)
		CHECK(block_types_register(&defs[k]) == BLOCK_TYPES_OK);
	CHECK(block_types_count() == HAKKAS_BLOCK_TYPES_MAX);
	CHECK(block_types_register(&defs[HAKKAS_BLOCK_TYPES_MAX]) == BLOCK_TYPES_ERR_FULL);
	/* A duplicate is reported as such even when full. */
	CHECK(block_types_register(&defs[0]) == BLOCK_TYPES_ERR_EXISTS);
	CHECK(block_types_get("test/T0") == &defs[0]);
	CHECK(block_types_at(HAKKAS_BLOCK_TYPES_MAX - 1) == &defs[HAKKAS_BLOCK_TYPES_MAX - 1]);
	/* Unregistering makes room again. */
	CHECK(block_types_unregister("test/T0"));
	CHECK(block_types_register(&defs[HAKKAS_BLOCK_TYPES_MAX]) == BLOCK_TYPES_OK);
	block_types_reset();
	CHECK(block_types_count() == 0);
}

/* ---- Built-ins: the 03 §3 table ------------------------------------------ */

struct expected_type {
	const char *name, *icon, *description;
	/* in1..inN; 0 for process/Linear's in, k, m; -1 for Deadband's in,
	 * threshold; -2 for logic/Not's in */
	int inputs;
	const char *out_type;
	const char *in_type; /* NULL: "number" */
};

static const struct expected_type table[] = {
	{"math/Add4", "add", "out = in1+in2+in3+in4", 4, "number", NULL},
	{"math/Add2", "add", "out = in1+in2", 2, "number", NULL},
	{"math/Mult2", "close", "out = in1*in2", 2, "number", NULL},
	{"math/Divide", "divide", "out = in1/in2", 2, "number", NULL},
	{"math/Subtract", "remove", "out = in1-in2", 2, "number", NULL},
	{"math/Modulo", "percent", "out = in1 MOD in2", 2, "number", NULL},
	{"math/Remainder", "percent", "out = in1 REMAINDER in2", 2, "number", NULL},
	{"compare/Greater", "compare", "out = in1 > in2", 2, "bool", NULL},
	{"compare/Less", "compare", "out = in1 < in2", 2, "bool", NULL},
	{"compare/Equal", "compare", "out = in1 == in2", 2, "bool", NULL},
	{"compare/NotEqual", "compare", "out = in1 != in2", 2, "bool", NULL},
	{"compare/LessEqual", "compare", "out = in1 <= in2", 2, "bool", NULL},
	{"compare/GreaterEqual", "compare", "out = in1 >= in2", 2, "bool", NULL},
	{"logic/And", "logic", "out = in1 AND in2 (boolean)", 2, "bool", "bool"},
	{"logic/Or", "logic", "out = in1 OR in2 (boolean)", 2, "bool", "bool"},
	{"logic/Xor", "logic", "out = in1 XOR in2 (boolean)", 2, "bool", "bool"},
	{"logic/Not", "logic", "out = NOT in (boolean)", -2, "bool", "bool"},
	{"process/Linear", "function", "out = k*in+m", 0, "number", NULL},
	{"process/Deadband", "filter", "out follows in only when it moves by at least threshold",
	 -1, "number", NULL},
};

#define EXPECTED_COUNT (sizeof(table) / sizeof(table[0]))

/* The calendar primitives (03 §6.2), after timing/Interval: their one
 * setting input (DayOfWeek also has sunday_first), a string defaulting to "". */
static const struct {
	const char *name, *icon, *description, *setting;
} calendar[] = {
	{"timing/MinuteRange", "schedule", "true within a time-of-day range", "range"},
	{"timing/DayOfWeek", "date_range", "true on the selected weekdays", "days"},
	{"timing/DayOfMonth", "date_range", "true on the selected days of the month", "days"},
	{"timing/MonthOfYear", "date_range", "true during the selected months", "months"},
	{"timing/DayOfYear", "date_range", "true on the selected days of the year", "days"},
	{"timing/WeekOfYear", "date_range", "true during the selected ISO weeks", "weeks"},
};

#define CALENDAR_COUNT (sizeof(calendar) / sizeof(calendar[0]))

/* The process/ types after process/PID (their ports and behavior:
 * tests/test_process_more.c), with their icons and descriptions. */
static const struct {
	const char *name, *icon, *description;
} process_more[] = {
	{"process/Curve", "show_chart", "output follows a curve of up to 7 points"},
	{"process/AutoMan", "pan_tool", "output = override ? manual : input"},
	{"process/Alarm", "notifications", "alarm states of a condition"},
};

#define PROCESS_MORE_COUNT (sizeof(process_more) / sizeof(process_more[0]))

static void check_port(const PortDescriptor *p, const char *name, const char *type)
{
	CHECK_STREQ(p->name, name);
	CHECK_STREQ(p->type, type);
	CHECK(p->description != NULL);
	CHECK(!p->addressable);
	CHECK(p->inner == NULL);
	if (strcmp(type, "bool") == 0) {
		CHECK(p->default_value.type == LINKABLE_BOOL && !p->default_value.as.b);
	} else {
		CHECK(p->default_value.type == LINKABLE_F64 && p->default_value.as.f64 == 0.0);
	}
}

/* Every built-in input rests at its type's resting value when nothing feeds it:
 * 0, false, "" (01 §2.1 default_value; no other per-port defaults, 03 §1). */
static void test_resting_defaults(void)
{
	block_types_reset();
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	for (size_t k = 0; k < block_types_count(); k++) {
		const BlockTypeDef *t = block_types_at(k);
		for (uint32_t j = 0; j < t->input_count; j++) {
			const PortDescriptor *p = &t->inputs[j];
			Linkable rest = linkable_default_value(p->type);
			char want[64], got[64];
			linkable_stringify(&rest, want, sizeof(want));
			linkable_stringify(&p->default_value, got, sizeof(got));
			if (p->default_value.type != rest.type || strcmp(want, got) != 0)
				fprintf(stderr, "  %s.%s rests at %s, not %s\n", t->name, p->name, got,
					want);
			CHECK(p->default_value.type == rest.type && strcmp(want, got) == 0);
			linkable_clear(&rest);
		}
	}
	block_types_reset();
}

static void test_builtins(void)
{
	block_types_reset();
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	CHECK(block_types_count() == EXPECTED_COUNT + 2 + PROCESS_MORE_COUNT + CALENDAR_COUNT);
	/* Registering them twice is refused. */
	CHECK(block_types_register_builtins() == BLOCK_TYPES_ERR_EXISTS);
	CHECK(block_types_count() == EXPECTED_COUNT + 2 + PROCESS_MORE_COUNT + CALENDAR_COUNT);

	for (size_t k = 0; k < EXPECTED_COUNT; k++) {
		const struct expected_type *e = &table[k];
		const BlockTypeDef *t = block_types_get(e->name);
		CHECK(t != NULL);
		if (t == NULL)
			continue;
		CHECK(block_types_at(k) == t); /* the table's order */
		CHECK_STREQ(t->name, e->name);
		CHECK_STREQ(t->icon, e->icon);
		CHECK_STREQ(t->description, e->description);
		CHECK(t->compute_on_input);
		CHECK(t->timer == TIMER_NONE);
		CHECK(t->period_ms == 0);
		CHECK(t->compute != NULL);
		CHECK(t->state_size == 0);
		if (e->inputs == -1)
			CHECK(t->change_threshold == 0); /* the engine adds no filtering */
		else
			CHECK(isnan(t->change_threshold));
		CHECK(!t->subgraph);
		CHECK(t->composite == NULL);
		CHECK(!t->wall_clock);
		const char *in_type = e->in_type != NULL ? e->in_type : "number";

		if (e->inputs == -2) {
			CHECK(t->input_count == 1);
			if (t->input_count == 1)
				check_port(&t->inputs[0], "in", in_type);
		} else if (e->inputs < 0) {
			static const char *const deadband_ins[] = {"in", "threshold"};
			CHECK(t->input_count == 2);
			for (uint32_t j = 0; j < 2 && j < t->input_count; j++)
				check_port(&t->inputs[j], deadband_ins[j], "number");
		} else if (e->inputs == 0) {
			static const char *const linear_ins[] = {"in", "k", "m"};
			CHECK(t->input_count == 3);
			for (uint32_t j = 0; j < 3 && j < t->input_count; j++)
				check_port(&t->inputs[j], linear_ins[j], "number");
		} else {
			CHECK(t->input_count == (uint32_t)e->inputs);
			for (uint32_t j = 0; j < t->input_count; j++) {
				char name[16];
				snprintf(name, sizeof(name), "in%u", (unsigned)(j + 1));
				check_port(&t->inputs[j], name, in_type);
			}
		}
		CHECK(t->output_count == 1);
		if (t->output_count >= 1)
			check_port(&t->outputs[0], "out", e->out_type);
	}

	/* process/PID (03 §6.1), after the stateless types. */
	const BlockTypeDef *t = block_types_get("process/PID");
	CHECK(t != NULL && block_types_at(EXPECTED_COUNT) == t);
	if (t != NULL) {
		static const struct {
			const char *name, *type;
			double def; /* a bool's default: 0 false, 1 true */
		} pid_ins[] = {
			{"setpoint", "number", 0},
			{"measured_value", "number", 0},
			{"kp", "number", 0},
			{"ki", "number", 0},
			{"kd", "number", 0},
			{"max_dgain", "number", 0},
			{"output_min", "number", 0},
			{"output_max", "number", 0},
			{"reverse", "bool", 0},
			{"auto", "bool", 0},
			{"manual_value", "number", 0},
			{"sample_ms", "number", 0},
		};
		CHECK_STREQ(t->icon, "tune");
		CHECK_STREQ(t->description, "PID regulator");
		CHECK(t->compute_on_input);
		CHECK(t->timer == TIMER_PERIODIC);
		CHECK(t->period_ms == 0); /* sample_ms's resting 0: no sampling */
		CHECK(t->compute != NULL);
		CHECK(t->state_size > 0);
		CHECK(isnan(t->change_threshold));
		CHECK(!t->subgraph && t->composite == NULL && !t->wall_clock);
		CHECK(t->input_count == 12 && t->output_count == 2);
		for (uint32_t j = 0; j < 12 && j < t->input_count; j++) {
			const PortDescriptor *p = &t->inputs[j];
			CHECK_STREQ(p->name, pid_ins[j].name);
			CHECK_STREQ(p->type, pid_ins[j].type);
			CHECK(p->description != NULL && p->description[0] != '\0');
			CHECK(!p->addressable && p->inner == NULL);
			if (strcmp(pid_ins[j].type, "bool") == 0)
				CHECK(p->default_value.type == LINKABLE_BOOL &&
				      p->default_value.as.b == (pid_ins[j].def != 0));
			else
				CHECK(p->default_value.type == LINKABLE_F64 &&
				      p->default_value.as.f64 == pid_ins[j].def);
		}
		if (t->output_count == 2) {
			check_port(&t->outputs[0], "output", "number");
			check_port(&t->outputs[1], "saturated", "bool");
		}
	}

	/* The process/ types after process/PID. */
	for (size_t k = 0; k < PROCESS_MORE_COUNT; k++) {
		t = block_types_get(process_more[k].name);
		CHECK(t != NULL && block_types_at(EXPECTED_COUNT + 1 + k) == t);
		if (t == NULL)
			continue;
		CHECK_STREQ(t->icon, process_more[k].icon);
		CHECK_STREQ(t->description, process_more[k].description);
		CHECK(t->compute != NULL && !t->subgraph && t->composite == NULL);
		CHECK(!t->wall_clock);
	}

	/* timing/Interval (03 §3.1), after the process/ family. */
	t = block_types_get("timing/Interval");
	CHECK(t != NULL && block_types_at(EXPECTED_COUNT + 1 + PROCESS_MORE_COUNT) == t);
	if (t != NULL) {
		CHECK_STREQ(t->icon, "timer");
		CHECK_STREQ(t->description, "periodic pulse generator: PWM or edge generator");
		CHECK(t->compute_on_input);
		CHECK(t->timer == TIMER_SCHEDULED);
		CHECK(t->compute != NULL);
		CHECK(t->state_size > 0);
		CHECK(isnan(t->change_threshold));
		CHECK(!t->subgraph && t->composite == NULL);
		CHECK(t->input_count == 2 && t->output_count == 1);
		if (t->input_count == 2 && t->output_count == 1) {
			CHECK_STREQ(t->inputs[0].name, "milliseconds");
			CHECK_STREQ(t->inputs[0].type, "number");
			CHECK(t->inputs[0].default_value.type == LINKABLE_F64 &&
			      t->inputs[0].default_value.as.f64 == 0.0); /* idle */
			CHECK_STREQ(t->inputs[1].name, "duty-cycle");
			CHECK_STREQ(t->inputs[1].type, "number");
			CHECK(t->inputs[1].default_value.type == LINKABLE_F64 &&
			      t->inputs[1].default_value.as.f64 == 0.0);
			check_port(&t->outputs[0], "out", "bool");
			CHECK(!t->inputs[0].addressable && !t->inputs[1].addressable);
		}
		CHECK(!t->wall_clock);
	}

	/* The calendar primitives, after timing/Interval. */
	for (size_t k = 0; k < CALENDAR_COUNT; k++) {
		t = block_types_get(calendar[k].name);
		size_t at = EXPECTED_COUNT + 2 + PROCESS_MORE_COUNT + k;
		CHECK(t != NULL && block_types_at(at) == t);
		if (t == NULL)
			continue;
		CHECK_STREQ(t->icon, calendar[k].icon);
		CHECK_STREQ(t->description, calendar[k].description);
		CHECK(t->compute_on_input);
		CHECK(t->timer == TIMER_SCHEDULED);
		CHECK(t->wall_clock);
		CHECK(t->compute != NULL);
		CHECK(t->state_size == 0);
		CHECK(isnan(t->change_threshold));
		CHECK(!t->subgraph && t->composite == NULL);
		bool dow = strcmp(calendar[k].name, "timing/DayOfWeek") == 0;
		CHECK(t->input_count == (dow ? 2u : 1u));
		CHECK(t->output_count == 1);
		if (t->input_count < 1 || t->output_count != 1)
			continue;
		const PortDescriptor *p = &t->inputs[0];
		CHECK_STREQ(p->name, calendar[k].setting);
		CHECK_STREQ(p->type, "string");
		CHECK(p->default_value.type == LINKABLE_STRING && p->default_value.as.s[0] == '\0');
		CHECK(p->description != NULL && p->description[0] != '\0');
		CHECK(!p->addressable && p->inner == NULL);
		if (dow && t->input_count == 2)
			check_port(&t->inputs[1], "sunday_first", "bool");
		check_port(&t->outputs[0], "out", "bool");
	}
	block_types_reset();
}

/* ---- Compute functions --------------------------------------------------- */

static Linkable f(double v)
{
	return linkable_from_f64(v);
}

static Linkable i(int64_t v)
{
	return linkable_from_i64(v);
}

static Linkable b(bool v)
{
	return linkable_from_bool(v);
}

static Linkable s(const char *text)
{
	Linkable v = {0};
	CHECK(linkable_from_string(&v, text, strlen(text)));
	return v;
}

static const Linkable unset;

/* Run type's compute with inputs in[0..n) (taking ownership of them; inputs
 * past n hold the port defaults) and return output 0, which the caller owns.
 * Checks the context contract on the way: the return value is 0. */
static Linkable run(const char *type, Linkable *in, size_t n)
{
	const BlockTypeDef *t = block_types_get(type);
	CHECK(t != NULL);
	Linkable inputs[4] = {0};
	Linkable outputs[1] = {0};
	Linkable result = {0};
	bool usable = t != NULL && t->input_count <= 4 && t->output_count == 1 &&
		      n <= t->input_count;
	CHECK(usable);
	if (!usable) {
		for (size_t k = 0; k < n; k++)
			linkable_clear(&in[k]);
		return result;
	}
	for (uint32_t k = 0; k < t->input_count; k++)
		inputs[k] = k < n ? in[k] : linkable_copy(&t->inputs[k].default_value);
	outputs[0] = linkable_copy(&t->outputs[0].default_value);
	/* A string in the output slot checks that compute releases what it replaces. */
	linkable_clear(&outputs[0]);
	outputs[0] = s("previous");

	BlockCtx ctx = {.inputs = inputs,
			.outputs = outputs,
			.state = NULL,
			.now_ms = 1234,
			.reason = n > 0 ? REASON_INPUT : REASON_START,
			.changed_input = n > 0 ? 0 : -1};
	CHECK(t->compute(&ctx) == 0);

	for (uint32_t k = 0; k < t->input_count; k++)
		linkable_clear(&inputs[k]);
	result = outputs[0]; /* moved to the caller */
	return result;
}

static Linkable run2(const char *type, Linkable a, Linkable c)
{
	Linkable in[2] = {a, c};
	return run(type, in, 2);
}

static Linkable run4(const char *type, Linkable a, Linkable c, Linkable d, Linkable e)
{
	Linkable in[4] = {a, c, d, e};
	return run(type, in, 4);
}

/* Output checks; each releases the value. */
static bool is_f64(Linkable v, double expected)
{
	bool ok = v.type == LINKABLE_F64 && v.as.f64 == expected &&
		  signbit(v.as.f64) == signbit(expected);
	if (!ok) {
		char buf[64];
		linkable_stringify(&v, buf, sizeof(buf));
		fprintf(stderr, "  got %s (type %d), expected %.17g\n", buf, (int)v.type, expected);
	}
	linkable_clear(&v);
	return ok;
}

static bool is_nan(Linkable v)
{
	bool ok = v.type == LINKABLE_F64 && isnan(v.as.f64);
	linkable_clear(&v);
	return ok;
}

static bool is_bool(Linkable v, bool expected)
{
	bool ok = v.type == LINKABLE_BOOL && v.as.b == expected;
	linkable_clear(&v);
	return ok;
}

static void test_add_subtract_mult(void)
{
	CHECK(is_f64(run2("math/Add2", f(1.5), f(2.25)), 3.75));
	CHECK(is_f64(run2("math/Add2", f(-1), f(1)), 0.0));
	CHECK(is_f64(run4("math/Add4", f(1), f(2), f(3), f(4)), 10.0));
	CHECK(is_f64(run2("math/Subtract", f(5), f(7.5)), -2.5));
	CHECK(is_f64(run2("math/Mult2", f(-3), f(2.5)), -7.5));
	CHECK(is_nan(run2("math/Add2", f(INFINITY), f(-INFINITY))));
	CHECK(is_nan(run2("math/Mult2", f(NAN), f(2))));
	CHECK(is_f64(run2("math/Mult2", f(1e308), f(10)), INFINITY));
	/* Unconnected inputs hold their default 0. */
	CHECK(is_f64(run("math/Add4", NULL, 0), 0.0));
	Linkable one[1] = {f(7)};
	CHECK(is_f64(run("math/Add4", one, 1), 7.0));
}

static void test_mixed_input_types(void)
{
	/* string, bool and i64 inputs go through to_f64 (01 §2.1). */
	CHECK(is_f64(run2("math/Add2", s("2.5"), b(true)), 3.5));
	CHECK(is_f64(run2("math/Add2", i(40), f(2.5)), 42.5));
	CHECK(is_f64(run2("math/Add2", s("12abc"), f(1)), 1.0)); /* strict parse: 0 */
	CHECK(is_f64(run2("math/Add2", s("true"), b(false)), 0.0));
	CHECK(is_f64(run2("math/Subtract", s("20 C"), i(5)), -5.0)); /* no unit forms */
	CHECK(is_f64(run2("math/Mult2", s("-Infinity"), i(2)), -INFINITY));
	Linkable in[2] = {unset, f(3)};
	CHECK(is_f64(run("math/Mult2", in, 2), 0.0));
	CHECK(is_f64(run4("math/Add4", i(1), s("2"), b(true), s("0.5")), 4.5));
	CHECK(is_bool(run2("compare/Greater", s("10"), f(9.5)), true));
	CHECK(is_bool(run2("compare/Equal", b(true), i(1)), true));
	CHECK(is_bool(run2("compare/Equal", s("1"), s("1.0")), true));
}

static void test_divide(void)
{
	CHECK(is_f64(run2("math/Divide", f(7), f(2)), 3.5));
	CHECK(is_f64(run2("math/Divide", f(-1), f(4)), -0.25));
	/* IEEE-754 division by zero (03 §3). */
	CHECK(is_f64(run2("math/Divide", f(1), f(0)), INFINITY));
	CHECK(is_f64(run2("math/Divide", f(-1), f(0)), -INFINITY));
	CHECK(is_f64(run2("math/Divide", f(1), f(-0.0)), -INFINITY));
	CHECK(is_nan(run2("math/Divide", f(0), f(0))));
	CHECK(is_nan(run2("math/Divide", f(NAN), f(1))));
	CHECK(is_f64(run2("math/Divide", f(1), f(INFINITY)), 0.0));
	/* An unconnected divisor is 0. */
	Linkable in[1] = {f(5)};
	CHECK(is_f64(run("math/Divide", in, 1), INFINITY));
}

static void test_modulo(void)
{
	/* Floored: the result has the sign of the divisor (03 §3). */
	CHECK(is_f64(run2("math/Modulo", f(7), f(3)), 1.0));
	CHECK(is_f64(run2("math/Modulo", f(-7), f(3)), 2.0));
	CHECK(is_f64(run2("math/Modulo", f(7), f(-3)), -2.0));
	CHECK(is_f64(run2("math/Modulo", f(-7), f(-3)), -1.0));
	CHECK(is_f64(run2("math/Modulo", f(5.5), f(2)), 1.5));
	CHECK(is_f64(run2("math/Modulo", f(-5.5), f(2)), 0.5));
	/* A zero result takes the divisor's sign. */
	CHECK(is_f64(run2("math/Modulo", f(6), f(3)), 0.0));
	CHECK(is_f64(run2("math/Modulo", f(-6), f(3)), 0.0));
	CHECK(is_f64(run2("math/Modulo", f(6), f(-3)), -0.0));
	CHECK(is_f64(run2("math/Modulo", f(0), f(-3)), -0.0));
	/* Zero divisor, non-finite operands. */
	CHECK(is_nan(run2("math/Modulo", f(5), f(0))));
	CHECK(is_nan(run2("math/Modulo", f(5), f(-0.0))));
	CHECK(is_nan(run2("math/Modulo", f(INFINITY), f(3))));
	CHECK(is_nan(run2("math/Modulo", f(NAN), f(3))));
	CHECK(is_nan(run2("math/Modulo", f(3), f(NAN))));
	CHECK(is_f64(run2("math/Modulo", f(5), f(INFINITY)), 5.0));
	CHECK(is_f64(run2("math/Modulo", f(-5), f(INFINITY)), INFINITY));
	CHECK(is_f64(run2("math/Modulo", f(5), f(-INFINITY)), -INFINITY));
	CHECK(is_f64(run2("math/Modulo", f(-5), f(-INFINITY)), -5.0));
	/* Default inputs: 0 mod 0. */
	CHECK(is_nan(run("math/Modulo", NULL, 0)));
}

static void test_remainder(void)
{
	/* Truncating: the result has the sign of the dividend (fmod). */
	CHECK(is_f64(run2("math/Remainder", f(7), f(3)), 1.0));
	CHECK(is_f64(run2("math/Remainder", f(-7), f(3)), -1.0));
	CHECK(is_f64(run2("math/Remainder", f(7), f(-3)), 1.0));
	CHECK(is_f64(run2("math/Remainder", f(-7), f(-3)), -1.0));
	CHECK(is_f64(run2("math/Remainder", f(-5.5), f(2)), -1.5));
	CHECK(is_f64(run2("math/Remainder", f(-6), f(3)), -0.0));
	CHECK(is_nan(run2("math/Remainder", f(5), f(0))));
	CHECK(is_nan(run2("math/Remainder", f(-INFINITY), f(3))));
	CHECK(is_nan(run2("math/Remainder", f(NAN), f(3))));
	CHECK(is_f64(run2("math/Remainder", f(-5), f(INFINITY)), -5.0));
}

/* Former names (03 §3, plan task 47): the comparisons were math/...; the
 * file loaders map them to compare/... The removed bitwise types have no
 * alias. */
static void test_aliases(void)
{
	static const char *const moved[] = {"Greater",	 "Less",      "Equal",
					    "NotEqual", "LessEqual", "GreaterEqual"};
	for (size_t k = 0; k < sizeof(moved) / sizeof(moved[0]); k++) {
		char old_name[32], name[32];
		snprintf(old_name, sizeof(old_name), "math/%s", moved[k]);
		snprintf(name, sizeof(name), "compare/%s", moved[k]);
		CHECK_STREQ(block_types_alias(old_name), name);
		CHECK(block_types_get(old_name) == NULL);
		CHECK(block_types_get(block_types_alias(old_name)) != NULL);
	}
	static const char *const removed[] = {"math/And2", "math/Or2", "math/Xor", "math/And4",
					      "math/Or4"};
	for (size_t k = 0; k < sizeof(removed) / sizeof(removed[0]); k++) {
		CHECK_STREQ(block_types_alias(removed[k]), removed[k]);
		CHECK(block_types_get(removed[k]) == NULL);
	}
	CHECK_STREQ(block_types_alias("math/Add2"), "math/Add2");
	CHECK_STREQ(block_types_alias("compare/Less"), "compare/Less");
	CHECK(block_types_alias(NULL) == NULL);
}

static void test_comparisons(void)
{
	static const struct {
		const char *type;
		bool lt, eq, gt, nan;
	} cmp[] = {
		{"compare/Greater", false, false, true, false},
		{"compare/Less", true, false, false, false},
		{"compare/Equal", false, true, false, false},
		{"compare/NotEqual", true, false, true, true},
		{"compare/LessEqual", true, true, false, false},
		{"compare/GreaterEqual", false, true, true, false},
	};
	for (size_t k = 0; k < sizeof(cmp) / sizeof(cmp[0]); k++) {
		const char *t = cmp[k].type;
		CHECK(is_bool(run2(t, f(1), f(2)), cmp[k].lt));
		CHECK(is_bool(run2(t, f(2), f(2)), cmp[k].eq));
		CHECK(is_bool(run2(t, f(3), f(2)), cmp[k].gt));
		CHECK(is_bool(run2(t, f(-INFINITY), f(INFINITY)), cmp[k].lt));
		CHECK(is_bool(run2(t, f(0.0), f(-0.0)), cmp[k].eq));
		/* Any NaN operand: IEEE-754, only NotEqual is true. */
		CHECK(is_bool(run2(t, f(NAN), f(2)), cmp[k].nan));
		CHECK(is_bool(run2(t, f(2), f(NAN)), cmp[k].nan));
		CHECK(is_bool(run2(t, f(NAN), f(NAN)), cmp[k].nan));
		/* Both inputs at their default 0. */
		CHECK(is_bool(run(t, NULL, 0), cmp[k].eq));
	}
}

static void test_linear(void)
{
	/* Ports in, k, m: out = k*in+m. */
	Linkable in[3] = {f(3), f(2), f(1)};
	CHECK(is_f64(run("process/Linear", in, 3), 7.0));
	Linkable in2[3] = {f(-4), f(0.5), f(10)};
	CHECK(is_f64(run("process/Linear", in2, 3), 8.0));
	Linkable in3[3] = {b(true), s("30"), i(-5)};
	CHECK(is_f64(run("process/Linear", in3, 3), 25.0));
	/* k and m unconnected: 0. */
	Linkable in4[1] = {f(5)};
	CHECK(is_f64(run("process/Linear", in4, 1), 0.0));
	Linkable in5[3] = {f(INFINITY), f(0), f(1)};
	CHECK(is_nan(run("process/Linear", in5, 3)));
	/* Multiply, then add, each rounded (no fused multiply-add):
	 * (1+2^-30)^2 - 1 rounds to 2^-29 when the product is rounded first. */
	double x = 1.0 + ldexp(1.0, -30);
	Linkable in6[3] = {f(x), f(x), f(-1)};
	CHECK(is_f64(run("process/Linear", in6, 3), ldexp(1.0, -29)));
}

static void test_logic(void)
{
	static const struct {
		bool a, c, and_, or_, xor_;
	} rows[] = {
		{false, false, false, false, false},
		{false, true, false, true, true},
		{true, false, false, true, true},
		{true, true, true, true, false},
	};
	for (size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
		CHECK(is_bool(run2("logic/And", b(rows[k].a), b(rows[k].c)), rows[k].and_));
		CHECK(is_bool(run2("logic/Or", b(rows[k].a), b(rows[k].c)), rows[k].or_));
		CHECK(is_bool(run2("logic/Xor", b(rows[k].a), b(rows[k].c)), rows[k].xor_));
	}
	Linkable t1[1] = {b(true)};
	CHECK(is_bool(run("logic/Not", t1, 1), false));
	Linkable f1[1] = {b(false)};
	CHECK(is_bool(run("logic/Not", f1, 1), true));
	/* Defaults: false, so Not of nothing is true. */
	CHECK(is_bool(run("logic/Not", NULL, 0), true));
	CHECK(is_bool(run("logic/And", NULL, 0), false));
	/* to_bool (01 §2.1): a non-zero number and "true" are true. */
	CHECK(is_bool(run2("logic/And", f(2.5), s("true")), true));
	CHECK(is_bool(run2("logic/Or", i(0), s("yes")), false));
	CHECK(is_bool(run2("logic/Xor", unset, f(-1)), true));
}

int main(void)
{
	RUN(test_register_and_lookup);
	RUN(test_duplicate_refused);
	RUN(test_invalid_refused);
	RUN(test_unregister_keeps_order);
	RUN(test_full);
	RUN(test_builtins);
	RUN(test_resting_defaults);

	block_types_reset();
	if (block_types_register_builtins() != BLOCK_TYPES_OK) {
		fprintf(stderr, "registering the built-ins failed\n");
		return 1;
	}
	RUN(test_add_subtract_mult);
	RUN(test_mixed_input_types);
	RUN(test_divide);
	RUN(test_modulo);
	RUN(test_remainder);
	RUN(test_aliases);
	RUN(test_comparisons);
	RUN(test_linear);
	RUN(test_logic);
	block_types_reset();
	return test_result();
}
