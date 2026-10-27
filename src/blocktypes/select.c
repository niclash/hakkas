/*
 * The selectors (03 §3.11; plan task 81), registered after process/Integrator
 * (blocktypes/process_more.c): stateless, event-driven (compute_on_input,
 * TIMER_NONE), number values only, the engine-wide change threshold.
 *
 * process/SelectIn and process/SelectOut: a bool select chooses which of two
 * inputs reaches the output, or which of two outputs the input reaches; an
 * unfed select is false, so the false side.
 *
 * process/MultiplexIn and process/MultiplexOut: the same with a number select
 * among 8, rounded to the nearest whole number; 1-8 chooses, anything else
 * (an unfed select's 0 included) chooses none, which reads as 0.
 *
 * Outputs not chosen are 0. As in blocktypes/process_more.c, inputs are read
 * with the 01 §2.1 converters and every compute but STOP's sets the outputs.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "blocktypes/builtins.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define NUMBER(n, d) {.name = (n), .description = (d), .type = "number", \
		      .default_value = LINKABLE_F64_INIT(0.0)}
#define BOOL(n, d) {.name = (n), .description = (d), .type = "bool", \
		    .default_value = LINKABLE_BOOL_INIT(false)}

#define WAYS 8

static double in_f64(const BlockCtx *ctx, int i)
{
	return linkable_to_f64(&ctx->inputs[i]);
}

/* Set every output to 0 but output chosen (-1: none), which gets v. */
static void outputs_one(BlockCtx *ctx, uint32_t count, int chosen, double v)
{
	for (uint32_t i = 0; i < count; i++)
		block_ctx_set_output(ctx, i, linkable_from_f64((int)i == chosen ? v : 0.0));
}

/* A number select's choice, 0-based: round to the nearest whole number
 * (halves away from zero); 1-WAYS chooses, anything else (NaN too) -1. */
static int choice(double select)
{
	double r = round(select);
	return r >= 1 && r <= WAYS ? (int)r - 1 : -1;
}

/* ---- process/SelectIn and process/SelectOut ---------------------------------- */

static const PortDescriptor select_in_inputs[] = {
	BOOL("select", "Which input reaches output: in_true when true, else in_false"),
	NUMBER("in_false", "Reaches output while select is false"),
	NUMBER("in_true", "Reaches output while select is true"),
};

static const PortDescriptor select_in_outputs[] = {
	NUMBER("output", "in_true or in_false, as select chooses"),
};

static int64_t select_in(BlockCtx *ctx)
{
	if (ctx->reason == REASON_STOP)
		return 0;
	bool sel = linkable_to_bool(&ctx->inputs[0]);
	block_ctx_set_output(ctx, 0, linkable_from_f64(in_f64(ctx, sel ? 2 : 1)));
	return 0;
}

static const PortDescriptor select_out_inputs[] = {
	BOOL("select", "Which output input reaches: out_true when true, else out_false"),
	NUMBER("input", "The value passed on"),
};

static const PortDescriptor select_out_outputs[] = {
	NUMBER("out_false", "input while select is false, else 0"),
	NUMBER("out_true", "input while select is true, else 0"),
};

static int64_t select_out(BlockCtx *ctx)
{
	if (ctx->reason == REASON_STOP)
		return 0;
	bool sel = linkable_to_bool(&ctx->inputs[0]);
	outputs_one(ctx, 2, sel ? 1 : 0, in_f64(ctx, 1));
	return 0;
}

/* ---- process/MultiplexIn and process/MultiplexOut ----------------------------- */

static const PortDescriptor multiplex_in_inputs[] = {
	NUMBER("select", "Which input reaches output, 1-8; any other value: none (output 0)"),
	NUMBER("in1", "Reaches output while select is 1"),
	NUMBER("in2", "Reaches output while select is 2"),
	NUMBER("in3", "Reaches output while select is 3"),
	NUMBER("in4", "Reaches output while select is 4"),
	NUMBER("in5", "Reaches output while select is 5"),
	NUMBER("in6", "Reaches output while select is 6"),
	NUMBER("in7", "Reaches output while select is 7"),
	NUMBER("in8", "Reaches output while select is 8"),
};

static const PortDescriptor multiplex_in_outputs[] = {
	NUMBER("output", "The input select chooses, or 0"),
};

static int64_t multiplex_in(BlockCtx *ctx)
{
	if (ctx->reason == REASON_STOP)
		return 0;
	int c = choice(in_f64(ctx, 0));
	block_ctx_set_output(ctx, 0, linkable_from_f64(c < 0 ? 0.0 : in_f64(ctx, 1 + c)));
	return 0;
}

static const PortDescriptor multiplex_out_inputs[] = {
	NUMBER("select", "Which output input reaches, 1-8; any other value: none"),
	NUMBER("input", "The value passed on"),
};

static const PortDescriptor multiplex_out_outputs[] = {
	NUMBER("out1", "input while select is 1, else 0"),
	NUMBER("out2", "input while select is 2, else 0"),
	NUMBER("out3", "input while select is 3, else 0"),
	NUMBER("out4", "input while select is 4, else 0"),
	NUMBER("out5", "input while select is 5, else 0"),
	NUMBER("out6", "input while select is 6, else 0"),
	NUMBER("out7", "input while select is 7, else 0"),
	NUMBER("out8", "input while select is 8, else 0"),
};

static int64_t multiplex_out(BlockCtx *ctx)
{
	if (ctx->reason == REASON_STOP)
		return 0;
	outputs_one(ctx, WAYS, choice(in_f64(ctx, 0)), in_f64(ctx, 1));
	return 0;
}

/* ---- Definitions, in the order of the 03 §3 table ------------------------------ */

#define SELECT_TYPE(type_name, type_icon, desc, ins, outs, fn)                                     \
	{.name = (type_name), .icon = (type_icon), .description = (desc), .inputs = (ins),         \
	 .input_count = ARRAY_LEN(ins), .outputs = (outs), .output_count = ARRAY_LEN(outs),        \
	 .compute_on_input = true, .timer = TIMER_NONE, .compute = (fn), .state_size = 0,          \
	 .change_threshold = NAN}

const BlockTypeDef select_block_types[] = {
	SELECT_TYPE("process/SelectIn", "call_merge", "output = select ? in_true : in_false",
		    select_in_inputs, select_in_outputs, select_in),
	SELECT_TYPE("process/SelectOut", "call_split",
		    "input goes to out_true or out_false, as select chooses; the other is 0",
		    select_out_inputs, select_out_outputs, select_out),
	SELECT_TYPE("process/MultiplexIn", "merge", "output = the input select chooses, 1-8",
		    multiplex_in_inputs, multiplex_in_outputs, multiplex_in),
	SELECT_TYPE("process/MultiplexOut", "alt_route",
		    "input goes to the output select chooses, 1-8; the others are 0",
		    multiplex_out_inputs, multiplex_out_outputs, multiplex_out),
};

const size_t select_block_types_count = ARRAY_LEN(select_block_types);
