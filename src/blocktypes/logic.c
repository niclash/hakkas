/*
 * logic/And, logic/Or, logic/Xor, logic/Not (03 §6.2): boolean logic for
 * combining calendar primitives and other bool signals. Stateless,
 * event-driven (compute_on_input, TIMER_NONE); every input is read with
 * to_bool (01 §2.1: true only for a true bool, a non-zero number or the text
 * "true"), the output is a bool.
 */
#include <math.h>
#include <stdbool.h>

#include "blocktypes/builtins.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static bool in(const BlockCtx *ctx, uint32_t i)
{
	return linkable_to_bool(&ctx->inputs[i]);
}

static int64_t and2(BlockCtx *ctx)
{
	block_ctx_set_output(ctx, 0, linkable_from_bool(in(ctx, 0) && in(ctx, 1)));
	return 0;
}

static int64_t or2(BlockCtx *ctx)
{
	block_ctx_set_output(ctx, 0, linkable_from_bool(in(ctx, 0) || in(ctx, 1)));
	return 0;
}

static int64_t xor2(BlockCtx *ctx)
{
	block_ctx_set_output(ctx, 0, linkable_from_bool(in(ctx, 0) != in(ctx, 1)));
	return 0;
}

static int64_t not1(BlockCtx *ctx)
{
	block_ctx_set_output(ctx, 0, linkable_from_bool(!in(ctx, 0)));
	return 0;
}

static const PortDescriptor two_inputs[] = {
	{.name = "in1", .description = "Operand 1", .type = "bool",
	 .default_value = LINKABLE_BOOL_INIT(false)},
	{.name = "in2", .description = "Operand 2", .type = "bool",
	 .default_value = LINKABLE_BOOL_INIT(false)},
};

static const PortDescriptor one_input[] = {
	{.name = "in", .description = "Operand", .type = "bool",
	 .default_value = LINKABLE_BOOL_INIT(false)},
};

static const PortDescriptor result[] = {
	{.name = "out", .description = "Result", .type = "bool",
	 .default_value = LINKABLE_BOOL_INIT(false)},
};

#define LOGIC_TYPE(n, desc, ins, fn)                                                               \
	{.name = n,                                                                                \
	 .icon = "logic",                                                                          \
	 .description = desc,                                                                      \
	 .inputs = ins,                                                                            \
	 .input_count = ARRAY_LEN(ins),                                                            \
	 .outputs = result,                                                                        \
	 .output_count = ARRAY_LEN(result),                                                        \
	 .compute_on_input = true,                                                                 \
	 .timer = TIMER_NONE,                                                                      \
	 .compute = fn,                                                                            \
	 .change_threshold = NAN}

const BlockTypeDef logic_block_types[] = {
	LOGIC_TYPE("logic/And", "out = in1 AND in2 (boolean)", two_inputs, and2),
	LOGIC_TYPE("logic/Or", "out = in1 OR in2 (boolean)", two_inputs, or2),
	LOGIC_TYPE("logic/Xor", "out = in1 XOR in2 (boolean)", two_inputs, xor2),
	LOGIC_TYPE("logic/Not", "out = NOT in (boolean)", one_input, not1),
};

const size_t logic_block_types_count = ARRAY_LEN(logic_block_types);
