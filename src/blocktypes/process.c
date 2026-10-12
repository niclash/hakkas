/*
 * process/Linear (03 §3): out = k*in+m, stateless and event-driven like the
 * math/... types (see blocktypes/math.c for the conventions).
 *
 * process/Deadband (03 §3, plan task 21c): out follows in only when it moves
 * by at least threshold. It needs no state: the value it holds is its own
 * current output (BlockCtx.outputs). Its type default change threshold is 0,
 * so the engine adds no filtering of its own.
 *
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

static const PortDescriptor deadband_inputs[] = {
	{.name = "in", .description = "Input value", .type = "number",
	 .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "threshold", .description = "Smallest change of in that moves out",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
};

static const PortDescriptor deadband_outputs[] = {
	{.name = "out", .description = "in, held while it moves less than threshold",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
};

/* Whether out should move from cur to next under threshold t (03 §3): on a
 * NaN transition (NaN → NaN is no move), or when |next − cur| >= t. A NaN or
 * negative threshold counts as 0 (decided, plan task 21c). */
static bool deadband_moves(double cur, double next, double t)
{
	bool cur_nan = isnan(cur), next_nan = isnan(next);
	if (cur_nan || next_nan)
		return cur_nan != next_nan;
	if (isnan(t) || t < 0)
		t = 0;
	return fabs(next - cur) >= t; /* inf − inf is NaN: no move between equal infinities */
}

/* START sets out to in unconditionally (the first value after start); an
 * input change, of in or of threshold alone, compares the current in with
 * the current out; STOP changes nothing. */
static int64_t deadband(BlockCtx *ctx)
{
	if (ctx->reason == REASON_STOP)
		return 0;
	double in = linkable_to_f64(&ctx->inputs[0]);
	if (ctx->reason == REASON_START ||
	    deadband_moves(linkable_to_f64(&ctx->outputs[0]), in,
			   linkable_to_f64(&ctx->inputs[1])))
		block_ctx_set_output(ctx, 0, linkable_from_f64(in));
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
	{.name = "process/Deadband",
	 .icon = "filter",
	 .description = "out follows in only when it moves by at least threshold",
	 .inputs = deadband_inputs,
	 .input_count = ARRAY_LEN(deadband_inputs),
	 .outputs = deadband_outputs,
	 .output_count = ARRAY_LEN(deadband_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_NONE,
	 .compute = deadband,
	 .state_size = 0,
	 .change_threshold = 0.0},
};

const size_t process_block_types_count = ARRAY_LEN(process_block_types);
