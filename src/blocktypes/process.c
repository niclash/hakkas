/*
 * process/Linear (03 §3): out = k*in+m, stateless and event-driven like the
 * math/... types (see blocktypes/math.c for the conventions, including the
 * Metric note: Metric inputs convert to their magnitude, no unit rules).
 * process/PID (03 §6.1) will join this family (plan task 23).
 */
#include <math.h>

#include "blocktypes/builtins.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static const PortDescriptor linear_inputs[] = {
	{.name = "in", .description = "Input value", .type = "number",
	 .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "k", .description = "Gain", .type = "number",
	 .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "m", .description = "Offset", .type = "number",
	 .default_value = LINKABLE_F64_INIT(0.0)},
};

static const PortDescriptor linear_outputs[] = {
	{.name = "out", .description = "k*in+m", .type = "number",
	 .default_value = LINKABLE_F64_INIT(0.0)},
};

/* (to_f64(in)*to_f64(k))+to_f64(m): a multiply then an add, each rounded.
 * Two statements, so that no compiler contracts them into a fused
 * multiply-add (clang may within one expression), which would give a
 * different result on targets with an FMA instruction. */
static int64_t linear(BlockCtx *ctx)
{
	double in = linkable_to_f64(&ctx->inputs[0]);
	double k = linkable_to_f64(&ctx->inputs[1]);
	double m = linkable_to_f64(&ctx->inputs[2]);
	double product = in * k;
	block_ctx_set_output(ctx, 0, linkable_from_f64(product + m));
	return 0;
}

const BlockTypeDef process_block_types[] = {
	{.name = "process/Linear",
	 .icon = "function",
	 .description = "out = k*in+m",
	 .inputs = linear_inputs,
	 .input_count = ARRAY_LEN(linear_inputs),
	 .outputs = linear_outputs,
	 .output_count = ARRAY_LEN(linear_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_NONE,
	 .compute = linear,
	 .state_size = 0,
	 .change_threshold = NAN},
};

const size_t process_block_types_count = ARRAY_LEN(process_block_types);
