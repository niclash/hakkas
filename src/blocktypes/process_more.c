/*
 * The process/ types decided on 2026-10-15 (03 §3.2-§3.9, §7.1), registered
 * right after process/PID (blocktypes/process.c) in the order of the 03 §3
 * table. Conventions as in blocktypes/math.c: every input is read with the
 * 01 §2.1 converters, and every compute but STOP's sets the outputs.
 *
 * process/Curve (03 §3.2): a piecewise linear curve of up to seven points;
 * the first and last points used are also the output's limits.
 *
 * process/AutoMan (03 §3.3): a manual override, output = override ? manual :
 * input, with no bumpless transfer.
 *
 * process/Alarm (03 §7.1): the state of an alarm condition, which a person
 * acknowledges; see the comment above its compute function.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "blocktypes/builtins.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define NUMBER(n, d) {.name = (n), .description = (d), .type = "number", \
		      .default_value = LINKABLE_F64_INIT(0.0)}
#define BOOL(n, d) {.name = (n), .description = (d), .type = "bool", \
		    .default_value = LINKABLE_BOOL_INIT(false)}

/* ---- process/Curve (03 §3.2) ------------------------------------------------ */

#define CURVE_POINTS 7

enum { CURVE_INPUT, CURVE_X1, CURVE_Y1 = CURVE_X1 + CURVE_POINTS };
enum { CURVE_OUTPUT, CURVE_MIN, CURVE_MAX };

static const PortDescriptor curve_inputs[] = {
	NUMBER("input", "The value to convert"),
	NUMBER("x1", "X coordinate of point 1"),
	NUMBER("x2", "X coordinate of point 2"),
	NUMBER("x3", "X coordinate of point 3"),
	NUMBER("x4", "X coordinate of point 4"),
	NUMBER("x5", "X coordinate of point 5"),
	NUMBER("x6", "X coordinate of point 6"),
	NUMBER("x7", "X coordinate of point 7"),
	NUMBER("y1", "Y coordinate of point 1"),
	NUMBER("y2", "Y coordinate of point 2"),
	NUMBER("y3", "Y coordinate of point 3"),
	NUMBER("y4", "Y coordinate of point 4"),
	NUMBER("y5", "Y coordinate of point 5"),
	NUMBER("y6", "Y coordinate of point 6"),
	NUMBER("y7", "Y coordinate of point 7"),
};

static const PortDescriptor curve_outputs[] = {
	NUMBER("output", "The value on the curve at input"),
	BOOL("min", "true while input is below the first point (output is held at its Y)"),
	BOOL("max", "true while input is above the last point (output is held at its Y)"),
};

/* The points used, in order: point 1 if its X is finite, each later one only
 * if its X is finite and greater than the last point used's. Then output by
 * linear interpolation, held at the end points' Y outside them; one point
 * gives its Y everywhere. A NaN input gives NaN; no point used gives 0. */
static int64_t curve(BlockCtx *ctx)
{
	if (ctx->reason == REASON_STOP)
		return 0;
	double xs[CURVE_POINTS], ys[CURVE_POINTS];
	int n = 0;
	for (int i = 0; i < CURVE_POINTS; i++) {
		double x = linkable_to_f64(&ctx->inputs[CURVE_X1 + i]);
		if (!isfinite(x) || (n > 0 && x <= xs[n - 1]))
			continue;
		xs[n] = x;
		ys[n] = linkable_to_f64(&ctx->inputs[CURVE_Y1 + i]);
		n++;
	}
	double in = linkable_to_f64(&ctx->inputs[CURVE_INPUT]);
	double out = 0.0;
	bool below = false, above = false;
	if (isnan(in)) {
		out = NAN;
	} else if (n > 0) {
		below = in < xs[0];
		above = in > xs[n - 1];
		if (below) {
			out = ys[0];
		} else if (above) {
			out = ys[n - 1];
		} else {
			out = ys[n - 1]; /* in == the last X (also one point) */
			for (int i = 0; i + 1 < n; i++) {
				if (in == xs[i]) {
					out = ys[i];
					break;
				}
				if (in < xs[i + 1]) {
					double t = (in - xs[i]) / (xs[i + 1] - xs[i]);
					out = ys[i] + t * (ys[i + 1] - ys[i]);
					break;
				}
			}
		}
	}
	block_ctx_set_output(ctx, CURVE_OUTPUT, linkable_from_f64(out));
	block_ctx_set_output(ctx, CURVE_MIN, linkable_from_bool(below));
	block_ctx_set_output(ctx, CURVE_MAX, linkable_from_bool(above));
	return 0;
}

/* ---- process/AutoMan (03 §3.3) ---------------------------------------------- */

enum { AUTOMAN_INPUT, AUTOMAN_OVERRIDE, AUTOMAN_MANUAL };

static const PortDescriptor automan_inputs[] = {
	NUMBER("input", "The automatic value"),
	BOOL("override", "Manual override: output follows manual instead of input"),
	NUMBER("manual", "The manual value"),
};

static const PortDescriptor automan_outputs[] = {
	NUMBER("output", "manual while overridden, else input"),
};

static int64_t automan(BlockCtx *ctx)
{
	if (ctx->reason == REASON_STOP)
		return 0;
	bool manual = linkable_to_bool(&ctx->inputs[AUTOMAN_OVERRIDE]);
	double v = linkable_to_f64(&ctx->inputs[manual ? AUTOMAN_MANUAL : AUTOMAN_INPUT]);
	block_ctx_set_output(ctx, 0, linkable_from_f64(v));
	return 0;
}

/* ---- process/Alarm (03 §7.1) ------------------------------------------------ */

enum { ALARM_INPUT, ALARM_ACKNOWLEDGE, ALARM_BLOCK, ALARM_DISABLE, ALARM_CATEGORY, ALARM_CLASS };

/* The states, in the order of the boolean outputs; ALARM_STATE_OUT is the
 * string output after them. */
typedef enum {
	ALARM_NORMAL,
	ALARM_ACTIVATED,
	ALARM_DEACTIVATED,
	ALARM_REACTIVATED,
	ALARM_ACKNOWLEDGED,
	ALARM_DISABLED,
	ALARM_BLOCKED,
	ALARM_STATES,
	ALARM_STATE_OUT = ALARM_STATES,
} AlarmState;

static const char *const alarm_names[ALARM_STATES] = {
	"Normal", "Activated", "Deactivated", "Reactivated", "Acknowledged", "Disabled", "Blocked",
};

static const PortDescriptor alarm_inputs[] = {
	BOOL("input", "The alarm condition: true while the alarmed situation exists"),
	BOOL("acknowledge",
	     "A push button: a person has noticed the alarm (acts on its rising edge)"),
	BOOL("block", "Blocks the alarm: no state changes; state Blocked"),
	BOOL("disable", "Disables the alarm: no state changes; state Disabled"),
	{.name = "category", .description = "Free text for the presentation", .type = "string",
	 .default_value = LINKABLE_EMPTY_STRING_INIT},
	{.name = "class", .description = "The alarm class: A, B, C or D (anything else: C)",
	 .type = "string", .default_value = LINKABLE_EMPTY_STRING_INIT},
};

static const PortDescriptor alarm_outputs[] = {
	BOOL("normal", "true while the alarm is Normal"),
	BOOL("activated", "true while the alarm is Activated"),
	BOOL("deactivated", "true while the alarm is Deactivated: gone, not acknowledged"),
	BOOL("reactivated", "true while the alarm is Reactivated: back before acknowledged"),
	BOOL("acknowledged", "true while the alarm is Acknowledged"),
	BOOL("disabled", "true while the alarm is Disabled"),
	BOOL("blocked", "true while the alarm is Blocked"),
	{.name = "state", .description = "The current state's name", .type = "string",
	 .default_value = LINKABLE_EMPTY_STRING_INIT},
};

typedef struct {
	AlarmState state;
	bool input;	  /* to_bool(input) at the last compute */
	bool acknowledge; /* to_bool(acknowledge) at the last compute */
} AlarmStateData;

/* The state after Normal once input has been evaluated. */
static AlarmState alarm_evaluate(bool input)
{
	return input ? ALARM_ACTIVATED : ALARM_NORMAL;
}

/* One step of the transition table for an input change (on/off) or an
 * acknowledgement (a rising edge of acknowledge). */
static AlarmState alarm_transition(AlarmState s, bool on, bool off, bool ack)
{
	switch (s) {
	case ALARM_NORMAL:
		return on ? ALARM_ACTIVATED : s;
	case ALARM_ACTIVATED:
		return off ? ALARM_DEACTIVATED : ack ? ALARM_ACKNOWLEDGED : s;
	case ALARM_DEACTIVATED:
		return on ? ALARM_REACTIVATED : ack ? ALARM_NORMAL : s;
	case ALARM_REACTIVATED:
		return off ? ALARM_DEACTIVATED : ack ? ALARM_ACKNOWLEDGED : s;
	case ALARM_ACKNOWLEDGED:
		return off ? ALARM_NORMAL : s;
	default:
		return s;
	}
}

/*
 * process/Alarm (03 §7.1):
 *  - disable wins over block: while disable is true the state is Disabled,
 *    else while block is true Blocked; neither input changes nor presses
 *    change it then.
 *  - Leaving Disabled/Blocked, and START, go to Normal and then evaluate
 *    input (Activated if the condition holds); the outputs, pushed once
 *    after the compute, show only that final state.
 *  - Otherwise the transition table, for the input that changed: input on or
 *    off (a change of to_bool(input) since the last compute), or a rising
 *    edge of acknowledge.
 *  - The state string is replaced only when the state changes (a string
 *    allocates). category and class change nothing: they are for alarm
 *    views, which read class as C unless it is exactly A, B, C or D.
 */
static int64_t alarm(BlockCtx *ctx)
{
	AlarmStateData *d = ctx->state;
	if (ctx->reason == REASON_STOP)
		return 0;
	bool input = linkable_to_bool(&ctx->inputs[ALARM_INPUT]);
	bool ack = linkable_to_bool(&ctx->inputs[ALARM_ACKNOWLEDGE]);
	AlarmState before = d->state;
	AlarmState s;
	if (linkable_to_bool(&ctx->inputs[ALARM_DISABLE]))
		s = ALARM_DISABLED;
	else if (linkable_to_bool(&ctx->inputs[ALARM_BLOCK]))
		s = ALARM_BLOCKED;
	else if (ctx->reason == REASON_START || before == ALARM_DISABLED ||
		 before == ALARM_BLOCKED)
		s = alarm_evaluate(input);
	else
		s = alarm_transition(before, input && !d->input, !input && d->input,
				     ack && !d->acknowledge);
	d->state = s;
	d->input = input;
	d->acknowledge = ack;

	for (int i = 0; i < ALARM_STATES; i++)
		block_ctx_set_output(ctx, (uint32_t)i, linkable_from_bool(i == (int)s));
	const Linkable *cur = &ctx->outputs[ALARM_STATE_OUT];
	if (cur->type != LINKABLE_STRING || strcmp(cur->as.s, alarm_names[s]) != 0) {
		Linkable name = {0};
		if (linkable_from_string(&name, alarm_names[s], strlen(alarm_names[s])))
			block_ctx_set_output(ctx, ALARM_STATE_OUT, name);
	}
	return 0;
}

const BlockTypeDef process_more_block_types[] = {
	{.name = "process/Curve",
	 .icon = "show_chart",
	 .description = "output follows a curve of up to 7 points",
	 .inputs = curve_inputs,
	 .input_count = ARRAY_LEN(curve_inputs),
	 .outputs = curve_outputs,
	 .output_count = ARRAY_LEN(curve_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_NONE,
	 .compute = curve,
	 .state_size = 0,
	 .change_threshold = NAN},
	{.name = "process/AutoMan",
	 .icon = "pan_tool",
	 .description = "output = override ? manual : input",
	 .inputs = automan_inputs,
	 .input_count = ARRAY_LEN(automan_inputs),
	 .outputs = automan_outputs,
	 .output_count = ARRAY_LEN(automan_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_NONE,
	 .compute = automan,
	 .state_size = 0,
	 .change_threshold = NAN},
	{.name = "process/Alarm",
	 .icon = "notifications",
	 .description = "alarm states of a condition",
	 .inputs = alarm_inputs,
	 .input_count = ARRAY_LEN(alarm_inputs),
	 .outputs = alarm_outputs,
	 .output_count = ARRAY_LEN(alarm_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_NONE,
	 .compute = alarm,
	 .state_size = sizeof(AlarmStateData),
	 .change_threshold = NAN},
};

const size_t process_more_block_types_count = ARRAY_LEN(process_more_block_types);
