/*
 * The math/... block types (03 §3): stateless, event-driven (compute_on_input,
 * TIMER_NONE, state_size 0, the engine-wide change threshold), ports
 * in1..inN → out, every input a "number" with default 0.
 *
 * Inputs are normalized with the linkable_to_* converters (01 §2.1), so a
 * string, bool or i64 input works like the number it converts to.
 * Results: every block outputs an f64 (03 §3). The comparisons are
 * compare/... (blocktypes/compare.c); the bitwise And2, Or2, Xor, And4 and
 * Or4 were removed in plan task 47 (boolean logic is logic/...).
 *
 * Every compute ignores ctx->reason: a stateless type computes the same
 * result for every trigger, and the runtime decides what is propagated
 * (03 §2). Each returns 0 (ignored for TIMER_NONE, 01 §2.3).
 */
#include <math.h>

#include "blocktypes/builtins.h"

#define NUMBER_IN(port_name, desc)                                                                 \
	{.name = (port_name), .description = (desc), .type = "number",                             \
	 .default_value = LINKABLE_F64_INIT(0.0)}

static const PortDescriptor in2[] = {
	NUMBER_IN("in1", "Operand 1"),
	NUMBER_IN("in2", "Operand 2"),
};

static const PortDescriptor in4[] = {
	NUMBER_IN("in1", "Operand 1"),
	NUMBER_IN("in2", "Operand 2"),
	NUMBER_IN("in3", "Operand 3"),
	NUMBER_IN("in4", "Operand 4"),
};

static const PortDescriptor out_number[] = {
	{.name = "out", .description = "Result", .type = "number",
	 .default_value = LINKABLE_F64_INIT(0.0)},
};

static double in_f64(const BlockCtx *ctx, int i)
{
	return linkable_to_f64(&ctx->inputs[i]);
}

static int64_t out_f64(BlockCtx *ctx, double v)
{
	block_ctx_set_output(ctx, 0, linkable_from_f64(v));
	return 0;
}

/* ---- Arithmetic (f64 result) ------------------------------------------- */

static int64_t add4(BlockCtx *ctx)
{
	return out_f64(ctx, in_f64(ctx, 0) + in_f64(ctx, 1) + in_f64(ctx, 2) + in_f64(ctx, 3));
}

static int64_t add2(BlockCtx *ctx)
{
	return out_f64(ctx, in_f64(ctx, 0) + in_f64(ctx, 1));
}

static int64_t mult2(BlockCtx *ctx)
{
	return out_f64(ctx, in_f64(ctx, 0) * in_f64(ctx, 1));
}

/* IEEE-754: x/0 is ±Infinity, 0/0 and NaN operands give NaN (03 §3). */
static int64_t divide(BlockCtx *ctx)
{
	return out_f64(ctx, in_f64(ctx, 0) / in_f64(ctx, 1));
}

static int64_t subtract(BlockCtx *ctx)
{
	return out_f64(ctx, in_f64(ctx, 0) - in_f64(ctx, 1));
}

/* Floored modulo (03 §3): a - b*floor(a/b), computed from fmod so that it is
 * exact. A non-zero result has the sign of the divisor; a zero result is +0
 * for a positive divisor and -0 for a negative one. NaN if either operand is
 * NaN, b is zero, or a is infinite. For an infinite b: a when a has b's sign
 * (or is zero), else b (±Infinity). Same as Python's float %; like it, a
 * tiny result of the sign opposite to b can round up to b itself. */
static double floored_mod(double a, double b)
{
	double r = fmod(a, b);
	if (isnan(r))
		return r;
	if (r == 0.0)
		return copysign(0.0, b);
	if ((r < 0.0) != (b < 0.0))
		r += b;
	return r;
}

static int64_t modulo(BlockCtx *ctx)
{
	return out_f64(ctx, floored_mod(in_f64(ctx, 0), in_f64(ctx, 1)));
}

/* Truncating remainder (03 §3): C fmod, a - b*trunc(a/b), with the sign of
 * the dividend. NaN if either operand is NaN, b is zero, or a is infinite;
 * a for an infinite b. */
static int64_t remainder_trunc(BlockCtx *ctx)
{
	return out_f64(ctx, fmod(in_f64(ctx, 0), in_f64(ctx, 1)));
}

/* ---- Definitions, in the order of the 03 §3 table ---------------------- */

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define MATH_TYPE(type_name, type_icon, desc, ins, outs, fn)                                       \
	{.name = (type_name), .icon = (type_icon), .description = (desc), .inputs = (ins),         \
	 .input_count = ARRAY_LEN(ins), .outputs = (outs), .output_count = ARRAY_LEN(outs),        \
	 .compute_on_input = true, .timer = TIMER_NONE, .compute = (fn), .state_size = 0,          \
	 .change_threshold = NAN}

const BlockTypeDef math_block_types[] = {
	MATH_TYPE("math/Add4", "add", "out = in1+in2+in3+in4", in4, out_number, add4),
	MATH_TYPE("math/Add2", "add", "out = in1+in2", in2, out_number, add2),
	MATH_TYPE("math/Mult2", "close", "out = in1*in2", in2, out_number, mult2),
	MATH_TYPE("math/Divide", "divide", "out = in1/in2", in2, out_number, divide),
	MATH_TYPE("math/Subtract", "remove", "out = in1-in2", in2, out_number, subtract),
	MATH_TYPE("math/Modulo", "percent", "out = in1 MOD in2", in2, out_number, modulo),
	MATH_TYPE("math/Remainder", "percent", "out = in1 REMAINDER in2", in2, out_number,
		  remainder_trunc),
};

const size_t math_block_types_count = ARRAY_LEN(math_block_types);
