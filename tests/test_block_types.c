/*
 * Block type registry and built-in stateless types (03 §3); timing/Interval's
 * descriptor (its behavior: tests/test_timers.c). Every compute
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
	int inputs;	     /* in1..inN; 0 for process/Linear's in, k, m */
	const char *out_type;
};

static const struct expected_type table[] = {
	{"math/Add4", "add", "out = in1+in2+in3+in4", 4, "number"},
	{"math/Add2", "add", "out = in1+in2", 2, "number"},
	{"math/Mult2", "close", "out = in1*in2", 2, "number"},
	{"math/Divide", "divide", "out = in1/in2", 2, "number"},
	{"math/Subtract", "remove", "out = in1-in2", 2, "number"},
	{"math/Modulo", "percent", "out = in1 MOD in2", 2, "number"},
	{"math/Remainder", "percent", "out = in1 REMAINDER in2", 2, "number"},
	{"math/And2", "logic", "out = in1 AND in2", 2, "number"},
	{"math/Or2", "logic", "out = in1 OR in2", 2, "number"},
	{"math/Xor", "logic", "out = in1 XOR in2", 2, "number"},
	{"math/And4", "logic", "out = in1 AND in2 AND in3 AND in4", 4, "number"},
	{"math/Or4", "logic", "out = in1 OR in2 OR in3 OR in4", 4, "number"},
	{"math/Greater", "compare", "out = in1 > in2", 2, "bool"},
	{"math/Less", "compare", "out = in1 < in2", 2, "bool"},
	{"math/Equal", "compare", "out = in1 == in2", 2, "bool"},
	{"math/NotEqual", "compare", "out = in1 != in2", 2, "bool"},
	{"math/LessEqual", "compare", "out = in1 <= in2", 2, "bool"},
	{"math/GreaterEqual", "compare", "out = in1 >= in2", 2, "bool"},
	{"process/Linear", "function", "out = k*in+m", 0, "number"},
};

#define EXPECTED_COUNT (sizeof(table) / sizeof(table[0]))

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

static void test_builtins(void)
{
	block_types_reset();
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	CHECK(block_types_count() == EXPECTED_COUNT + 1);
	/* Registering them twice is refused. */
	CHECK(block_types_register_builtins() == BLOCK_TYPES_ERR_EXISTS);
	CHECK(block_types_count() == EXPECTED_COUNT + 1);

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
		CHECK(isnan(t->change_threshold));
		CHECK(!t->subgraph);
		CHECK(t->composite == NULL);

		if (e->inputs == 0) {
			static const char *const linear_ins[] = {"in", "k", "m"};
			CHECK(t->input_count == 3);
			for (uint32_t j = 0; j < 3 && j < t->input_count; j++)
				check_port(&t->inputs[j], linear_ins[j], "number");
		} else {
			CHECK(t->input_count == (uint32_t)e->inputs);
			for (uint32_t j = 0; j < t->input_count; j++) {
				char name[16];
				snprintf(name, sizeof(name), "in%u", (unsigned)(j + 1));
				check_port(&t->inputs[j], name, "number");
			}
		}
		CHECK(t->output_count == 1);
		if (t->output_count >= 1)
			check_port(&t->outputs[0], "out", e->out_type);
	}

	/* timing/Interval (03 §3.1), after the stateless types. */
	const BlockTypeDef *t = block_types_get("timing/Interval");
	CHECK(t != NULL && block_types_at(EXPECTED_COUNT) == t);
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
			      t->inputs[0].default_value.as.f64 == 1000.0);
			CHECK_STREQ(t->inputs[1].name, "duty-cycle");
			CHECK_STREQ(t->inputs[1].type, "number");
			CHECK(t->inputs[1].default_value.type == LINKABLE_F64 &&
			      t->inputs[1].default_value.as.f64 == 50.0);
			check_port(&t->outputs[0], "out", "bool");
			CHECK(!t->inputs[0].addressable && !t->inputs[1].addressable);
		}
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

static Linkable m(double value, const char *unit)
{
	Linkable v = {0};
	CHECK(linkable_from_metric(&v, value, unit, strlen(unit), METRIC_UNCLASSIFIED));
	return v;
}

static const Linkable unset;

/* Run type's compute with inputs in[0..n) (taking ownership of them; inputs
 * past n hold the port defaults) and return output 0, which the caller owns.
 * Checks the context contract on the way: the error flag stays clear and the
 * return value is 0. */
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

	bool error = false;
	BlockCtx ctx = {.inputs = inputs,
			.outputs = outputs,
			.state = NULL,
			.now_ms = 1234,
			.reason = n > 0 ? REASON_INPUT : REASON_START,
			.changed_input = n > 0 ? 0 : -1,
			.error = &error};
	CHECK(t->compute(&ctx) == 0);
	CHECK(!error);

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

static bool is_i64(Linkable v, int64_t expected)
{
	bool ok = v.type == LINKABLE_I64 && v.as.i64 == expected;
	if (!ok)
		fprintf(stderr, "  got type %d, expected i64 %lld\n", (int)v.type,
			(long long)expected);
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
	/* string, bool, i64 and Metric inputs go through to_f64 (01 §2.1). */
	CHECK(is_f64(run2("math/Add2", s("2.5"), b(true)), 3.5));
	CHECK(is_f64(run2("math/Add2", i(40), m(2.5, "V")), 42.5));
	CHECK(is_f64(run2("math/Add2", s("12abc"), f(1)), 1.0)); /* strict parse: 0 */
	CHECK(is_f64(run2("math/Add2", s("true"), b(false)), 0.0));
	CHECK(is_f64(run2("math/Subtract", m(20, "C"), m(5, "K")), 15.0)); /* no unit rules */
	CHECK(is_f64(run2("math/Mult2", s("-Infinity"), i(2)), -INFINITY));
	Linkable in[2] = {unset, f(3)};
	CHECK(is_f64(run("math/Mult2", in, 2), 0.0));
	CHECK(is_f64(run4("math/Add4", i(1), s("2"), b(true), m(0.5, "mm")), 4.5));
	/* Bitwise inputs go through to_i64: strings parse as integers only. */
	CHECK(is_i64(run2("math/Or2", s("12"), s("1.5")), 12));
	CHECK(is_i64(run2("math/Or2", b(true), m(6.9, "A")), 7));
	CHECK(is_bool(run2("math/Greater", s("10"), m(9.5, "V")), true));
	CHECK(is_bool(run2("math/Equal", b(true), i(1)), true));
	CHECK(is_bool(run2("math/Equal", s("1"), s("1.0")), true));
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

static void test_bitwise(void)
{
	CHECK(is_i64(run2("math/And2", i(0xC), i(0xA)), 0x8));
	CHECK(is_i64(run2("math/Or2", i(0xC), i(0xA)), 0xE));
	CHECK(is_i64(run2("math/Xor", i(0xC), i(0xA)), 0x6));
	CHECK(is_i64(run4("math/And4", i(0xF), i(0xE), i(0x7), i(0x6)), 0x6));
	CHECK(is_i64(run4("math/Or4", i(1), i(2), i(4), i(8)), 0xF));
	/* Negative values are two's complement. */
	CHECK(is_i64(run2("math/And2", i(-1), i(0x55)), 0x55));
	CHECK(is_i64(run2("math/Xor", i(-1), i(0)), -1));
	CHECK(is_i64(run2("math/Or2", i(INT64_MIN), i(1)), INT64_MIN + 1));
	/* Full 64-bit values survive (i64 output, not f64). */
	CHECK(is_i64(run2("math/Or2", i(INT64_MAX - 1), i(1)), INT64_MAX));
	/* Doubles truncate toward zero and saturate (01 §2.1). */
	CHECK(is_i64(run2("math/And2", f(7.9), f(-1)), 7));
	CHECK(is_i64(run2("math/Or2", f(-2.5), f(0)), -2));
	CHECK(is_i64(run2("math/Or2", f(NAN), f(0)), 0));
	CHECK(is_i64(run2("math/Or2", f(INFINITY), f(0)), INT64_MAX));
	CHECK(is_i64(run2("math/Or2", f(-1e300), f(0)), INT64_MIN));
	/* And4 with unconnected inputs (default 0) is 0; Or4 is the OR of the rest. */
	Linkable in[2] = {i(-1), i(-1)};
	CHECK(is_i64(run("math/And4", in, 2), 0));
	Linkable in2[2] = {i(3), i(4)};
	CHECK(is_i64(run("math/Or4", in2, 2), 7));
}

static void test_comparisons(void)
{
	static const struct {
		const char *type;
		bool lt, eq, gt, nan;
	} cmp[] = {
		{"math/Greater", false, false, true, false},
		{"math/Less", true, false, false, false},
		{"math/Equal", false, true, false, false},
		{"math/NotEqual", true, false, true, true},
		{"math/LessEqual", true, true, false, false},
		{"math/GreaterEqual", false, true, true, false},
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
	Linkable in3[3] = {m(10, "V"), s("30"), i(-5)};
	CHECK(is_f64(run("process/Linear", in3, 3), 295.0));
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

int main(void)
{
	RUN(test_register_and_lookup);
	RUN(test_duplicate_refused);
	RUN(test_invalid_refused);
	RUN(test_unregister_keeps_order);
	RUN(test_full);
	RUN(test_builtins);

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
	RUN(test_bitwise);
	RUN(test_comparisons);
	RUN(test_linear);
	block_types_reset();
	return test_result();
}
