/*
 * The compare/... block types (03 §3): stateless, event-driven
 * (compute_on_input, TIMER_NONE, state_size 0, the engine-wide change
 * threshold), ports in1, in2 → out, each input a "number" with default 0, the
 * output a "bool" with default false.
 *
 * Inputs are normalized with linkable_to_f64 (01 §2.1), so a string, bool or
 * i64 input compares like the number it converts to. Comparisons follow
 * IEEE-754: with a NaN operand every comparison is false except NotEqual,
 * which is true; 0 == -0.
 *
 * Until plan task 47 these were math/Greater ... math/GreaterEqual; the file
 * loaders still accept those names (block_types_alias).
 *
 * Every compute ignores ctx->reason (03 §2) and returns 0 (ignored for
 * TIMER_NONE, 01 §2.3).
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

static const PortDescriptor out_bool[] = {
	{.name = "out", .description = "Result", .type = "bool",
	 .default_value = LINKABLE_BOOL_INIT(false)},
};

static double in_f64(const BlockCtx *ctx, int i)
{
	return linkable_to_f64(&ctx->inputs[i]);
}

static int64_t out_bool_value(BlockCtx *ctx, bool v)
{
	block_ctx_set_output(ctx, 0, linkable_from_bool(v));
	return 0;
}

static int64_t greater(BlockCtx *ctx)
{
	return out_bool_value(ctx, in_f64(ctx, 0) > in_f64(ctx, 1));
}

static int64_t less(BlockCtx *ctx)
{
	return out_bool_value(ctx, in_f64(ctx, 0) < in_f64(ctx, 1));
}

static int64_t equal(BlockCtx *ctx)
{
	return out_bool_value(ctx, in_f64(ctx, 0) == in_f64(ctx, 1));
}

static int64_t not_equal(BlockCtx *ctx)
{
	return out_bool_value(ctx, in_f64(ctx, 0) != in_f64(ctx, 1));
}

static int64_t less_equal(BlockCtx *ctx)
{
	return out_bool_value(ctx, in_f64(ctx, 0) <= in_f64(ctx, 1));
}

static int64_t greater_equal(BlockCtx *ctx)
{
	return out_bool_value(ctx, in_f64(ctx, 0) >= in_f64(ctx, 1));
}

/* ---- Definitions, in the order of the 03 §3 table ---------------------- */

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define COMPARE_TYPE(type_name, desc, fn)                                                          \
	{.name = (type_name), .icon = "compare", .description = (desc), .inputs = in2,             \
	 .input_count = ARRAY_LEN(in2), .outputs = out_bool, .output_count = ARRAY_LEN(out_bool),  \
	 .compute_on_input = true, .timer = TIMER_NONE, .compute = (fn), .state_size = 0,          \
	 .change_threshold = NAN}

const BlockTypeDef compare_block_types[] = {
	COMPARE_TYPE("compare/Greater", "out = in1 > in2", greater),
	COMPARE_TYPE("compare/Less", "out = in1 < in2", less),
	COMPARE_TYPE("compare/Equal", "out = in1 == in2", equal),
	COMPARE_TYPE("compare/NotEqual", "out = in1 != in2", not_equal),
	COMPARE_TYPE("compare/LessEqual", "out = in1 <= in2", less_equal),
	COMPARE_TYPE("compare/GreaterEqual", "out = in1 >= in2", greater_equal),
};

const size_t compare_block_types_count = ARRAY_LEN(compare_block_types);
