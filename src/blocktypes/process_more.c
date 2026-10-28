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
 *
 * process/Delay (03 §3.4): input reaches output once it has been stable for
 * milliseconds, on both edges or (mode) only the rising or the falling one; a
 * TIMER_SCHEDULED type woken when the wait ends.
 *
 * process/Trigger (03 §3.5): output false on a rising edge of arm, true on a
 * rising edge of fire, each regardless of the other input.
 *
 * process/Limit (03 §3.6): output = input capped to [min, max], the limits
 * read as process/PID's (limits_read).
 *
 * process/Hysteresis (03 §3.7): a Schmitt trigger, on above threshold +
 * hysteresis and off below threshold - hysteresis; inverted negates output.
 *
 * process/Exponential and process/Logarithmic (03 §3.8): output =
 * a*exp(b*input) and a*ln(b*input), IEEE-754 results as C gives them.
 *
 * process/Integrator (03 §3.9): the integral of input over time, a cyclic
 * type like process/PID (TIMER_PERIODIC + compute_on_input, the period from
 * sample_ms); see the comment above its compute function.
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

/* The limits [*lo, *hi] from the inputs min and max, as process/PID reads
 * its output limits (03 §6.1): a NaN limit is no limit on its side, equal
 * limits (both at rest: 0) are no limits, and limits given the wrong way
 * round are swapped. */
static void limits_read(const Linkable *min, const Linkable *max, double *lo, double *hi)
{
	double a = linkable_to_f64(min), b = linkable_to_f64(max);
	a = isnan(a) ? -INFINITY : a;
	b = isnan(b) ? INFINITY : b;
	if (a == b) {
		a = -INFINITY;
		b = INFINITY;
	}
	*lo = fmin(a, b);
	*hi = fmax(a, b);
}

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
	{.name = "state", .description = "The name of the current state", .type = "string",
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

/* ---- process/Delay (03 §3.4) ------------------------------------------------ */

/* The longest delay, in ms: 2^53, exact as a double (as timing/Interval). */
#define DELAY_MAX_MS 9007199254740992.0

enum { DELAY_INPUT, DELAY_MILLISECONDS, DELAY_MODE };

/* The edges mode delays: 0 both, 1 rising (on-delay), 2 falling (off-delay). */
enum { DELAY_BOTH, DELAY_RISING, DELAY_FALLING };

static const PortDescriptor delay_inputs[] = {
	BOOL("input", "The signal to delay"),
	NUMBER("milliseconds",
	       "How long input must be stable before output takes it (<= 0: no delay)"),
	NUMBER("mode", "The edges delayed: 0 both, 1 rising, 2 falling; the other passes at once"),
};

static const PortDescriptor delay_outputs[] = {
	BOOL("output", "input, once it has been stable for milliseconds"),
};

typedef struct {
	bool input;	    /* to_bool(input) at the last compute */
	uint64_t changed_ms; /* engine time of the last change of input */
} DelayState;

/* milliseconds rounded to whole ms, at most DELAY_MAX_MS; 0 (no delay) for
 * a value <= 0, NaN or infinite. */
static uint64_t delay_ms(double ms)
{
	if (!isfinite(ms) || ms <= 0)
		return 0;
	return (uint64_t)fmin(round(ms), DELAY_MAX_MS);
}

/* mode rounded; anything but 1 or 2 is 0, both edges. */
static int delay_mode(double mode)
{
	double m = round(mode);
	return m == DELAY_RISING ? DELAY_RISING : m == DELAY_FALLING ? DELAY_FALLING : DELAY_BOTH;
}

/*
 * process/Delay (03 §3.4): START in mode 0 sets output = input with nothing
 * pending; in modes 1 and 2 output starts false and input counts as a change
 * from false at the start, whose wait, if already due, ends at the first timer
 * expiry rather than in the START compute: a milliseconds computed by other
 * blocks arrives only after it. Afterwards a change of to_bool(input) records its
 * time; while input differs from output a change is pending. An edge mode
 * delays takes effect once milliseconds have passed since that time (at once
 * if they already have, also after a change of milliseconds or mode); the
 * other edge at once. Every compute returns the delay to the end of the
 * pending wait, or -1 (no timer) when nothing is pending.
 */
static int64_t delay(BlockCtx *ctx)
{
	DelayState *d = ctx->state;
	if (ctx->reason == REASON_STOP)
		return -1;
	bool in = linkable_to_bool(&ctx->inputs[DELAY_INPUT]);
	bool out = linkable_to_bool(&ctx->outputs[0]);
	int mode = delay_mode(linkable_to_f64(&ctx->inputs[DELAY_MODE]));
	if (ctx->reason == REASON_START) {
		out = mode == DELAY_BOTH ? in : false;
		d->changed_ms = ctx->now_ms;
	} else if (in != d->input) {
		d->changed_ms = ctx->now_ms;
	}
	d->input = in;
	int64_t next = -1;
	if (in != out && mode != DELAY_BOTH && (mode == DELAY_RISING) != in) {
		out = in; /* the edge mode does not delay */
	} else if (in != out) {
		double ms = linkable_to_f64(&ctx->inputs[DELAY_MILLISECONDS]);
		uint64_t due = d->changed_ms + delay_ms(ms);
		if (ctx->reason == REASON_START && ctx->now_ms >= due)
			next = 0; /* after the values sent at the start, milliseconds' among them */
		else if (ctx->now_ms >= due)
			out = in;
		else
			next = (int64_t)(due - ctx->now_ms);
	}
	block_ctx_set_output(ctx, 0, linkable_from_bool(out));
	return next;
}

/* ---- process/Trigger (03 §3.5) ---------------------------------------------- */

enum { TRIGGER_ARM, TRIGGER_FIRE };

static const PortDescriptor trigger_inputs[] = {
	BOOL("arm", "Its rising edge sets output to false"),
	BOOL("fire", "Its rising edge sets output to true"),
};

static const PortDescriptor trigger_outputs[] = {
	BOOL("output", "false since the last rising arm, true since the last rising fire"),
};

typedef struct {
	bool arm, fire; /* their to_bool values at the last compute */
} TriggerState;

/* START: output false, the inputs recorded without counting as edges. Then
 * only the rising edges change output; one input changes per compute, so
 * they never coincide. */
static int64_t trigger(BlockCtx *ctx)
{
	TriggerState *d = ctx->state;
	if (ctx->reason == REASON_STOP)
		return 0;
	bool arm = linkable_to_bool(&ctx->inputs[TRIGGER_ARM]);
	bool fire = linkable_to_bool(&ctx->inputs[TRIGGER_FIRE]);
	if (ctx->reason == REASON_START || (arm && !d->arm))
		block_ctx_set_output(ctx, 0, linkable_from_bool(false));
	else if (fire && !d->fire)
		block_ctx_set_output(ctx, 0, linkable_from_bool(true));
	d->arm = arm;
	d->fire = fire;
	return 0;
}

/* ---- process/Limit (03 §3.6) ------------------------------------------------ */

enum { LIMIT_INPUT, LIMIT_MIN, LIMIT_MAX };

static const PortDescriptor limit_inputs[] = {
	NUMBER("input", "The value to limit"),
	NUMBER("min", "Lower limit (equal limits: none)"),
	NUMBER("max", "Upper limit (equal limits: none)"),
};

static const PortDescriptor limit_outputs[] = {
	NUMBER("output", "input, capped to [min, max]"),
};

/* A NaN input gives a NaN output (fmin/fmax would drop it). */
static int64_t limit(BlockCtx *ctx)
{
	if (ctx->reason == REASON_STOP)
		return 0;
	double lo, hi;
	limits_read(&ctx->inputs[LIMIT_MIN], &ctx->inputs[LIMIT_MAX], &lo, &hi);
	double in = linkable_to_f64(&ctx->inputs[LIMIT_INPUT]);
	double out = isnan(in) ? in : fmin(fmax(in, lo), hi);
	block_ctx_set_output(ctx, 0, linkable_from_f64(out));
	return 0;
}

/* ---- process/Hysteresis (03 §3.7) ------------------------------------------- */

enum { HYST_INPUT, HYST_THRESHOLD, HYST_HYSTERESIS, HYST_INVERTED };

static const PortDescriptor hysteresis_inputs[] = {
	NUMBER("input", "The value compared"),
	NUMBER("threshold", "The middle of the switching band"),
	NUMBER("hysteresis", "Half the width of the band: on above threshold + hysteresis, "
			     "off below threshold - hysteresis"),
	BOOL("inverted", "Negates output"),
};

static const PortDescriptor hysteresis_outputs[] = {
	BOOL("output", "The state (on = true), negated when inverted"),
};

typedef struct {
	bool on; /* the state, before inversion */
} HysteresisState;

/* hysteresis is used as its absolute value, NaN as 0; a NaN input or
 * threshold holds the state. START: on only above the band (off inside it). */
static int64_t hysteresis(BlockCtx *ctx)
{
	HysteresisState *d = ctx->state;
	if (ctx->reason == REASON_STOP)
		return 0;
	double in = linkable_to_f64(&ctx->inputs[HYST_INPUT]);
	double thr = linkable_to_f64(&ctx->inputs[HYST_THRESHOLD]);
	double h = fabs(linkable_to_f64(&ctx->inputs[HYST_HYSTERESIS]));
	if (isnan(h))
		h = 0.0;
	if (ctx->reason == REASON_START)
		d->on = false;
	if (!isnan(in) && !isnan(thr)) {
		if (in > thr + h)
			d->on = true;
		else if (in < thr - h)
			d->on = false;
	}
	bool inverted = linkable_to_bool(&ctx->inputs[HYST_INVERTED]);
	block_ctx_set_output(ctx, 0, linkable_from_bool(d->on != inverted));
	return 0;
}

/* ---- process/Exponential, process/Logarithmic (03 §3.8) --------------------- */

enum { FN_INPUT, FN_A, FN_B };

static const PortDescriptor exp_inputs[] = {
	NUMBER("input", "Input value"),
	NUMBER("a", "Scale"),
	NUMBER("b", "Rate"),
};

static const PortDescriptor exponential_outputs[] = {
	NUMBER("output", "a*exp(b*input)"),
};

static const PortDescriptor logarithmic_outputs[] = {
	NUMBER("output", "a*ln(b*input)"),
};

static int64_t exp_or_log(BlockCtx *ctx, double (*fn)(double))
{
	if (ctx->reason == REASON_STOP)
		return 0;
	double in = linkable_to_f64(&ctx->inputs[FN_INPUT]);
	double a = linkable_to_f64(&ctx->inputs[FN_A]);
	double b = linkable_to_f64(&ctx->inputs[FN_B]);
	block_ctx_set_output(ctx, 0, linkable_from_f64(a * fn(b * in)));
	return 0;
}

static int64_t exponential(BlockCtx *ctx)
{
	return exp_or_log(ctx, exp);
}

static int64_t logarithmic(BlockCtx *ctx)
{
	return exp_or_log(ctx, log);
}

/* ---- process/Integrator (03 §3.9) ------------------------------------------- */

enum { INTEG_INPUT, INTEG_MIN, INTEG_MAX, INTEG_DIVISOR, INTEG_RESET, INTEG_SAMPLE_MS };

static const PortDescriptor integrator_inputs[] = {
	NUMBER("input", "The value integrated"),
	NUMBER("min", "Lower limit of output (equal limits: none)"),
	NUMBER("max", "Upper limit of output (equal limits: none)"),
	NUMBER("divisor", "Time scale: 1 seconds, 3600 hours, 86400 days (<= 0: 1)"),
	BOOL("reset", "While true, output is 0 (within the limits) and nothing integrates"),
	NUMBER("sample_ms",
	       "How often output is updated, in milliseconds (<= 0: only on input changes)"),
};

static const PortDescriptor integrator_outputs[] = {
	NUMBER("output", "The integral, within [min, max]"),
};

typedef struct {
	double value;	  /* the accumulated integral, within the limits */
	double input;	  /* input since the last step */
	double divisor;	  /* divisor since the last step (>= its minimum, finite) */
	bool reset;	  /* reset since the last step */
	uint64_t last_ms; /* engine time of the last step (or the start) */
} IntegratorState;

/* The period for sample_ms, as process/PID's: whole ms, 1 to UINT32_MAX; 0
 * (no timer) for a value <= 0, NaN or infinite. */
static uint32_t integrator_period(double ms)
{
	if (!isfinite(ms) || ms <= 0)
		return 0;
	return (uint32_t)fmin(fmax(round(ms), 1.0), (double)UINT32_MAX);
}

/*
 * process/Integrator (03 §3.9). Every compute but STOP is a step: the time
 * since the last step is integrated with the values that held during it —
 * input (taken as constant between changes, so the result does not depend
 * on sample_ms), divisor (<= 0, NaN or infinite: 1) and reset (true: nothing
 * integrated) — and the current ones are recorded for the next step. A
 * non-finite input integrates nothing. The value is then clamped to the
 * current limits (anti-windup: the stored value itself), or set to clamp(0)
 * while reset is true. START begins at clamp(0). The period follows
 * sample_ms through BlockCtx.period_ms, as for process/PID.
 */
static int64_t integrator(BlockCtx *ctx)
{
	IntegratorState *d = ctx->state;
	if (ctx->reason == REASON_STOP)
		return 0;
	ctx->period_ms = integrator_period(linkable_to_f64(&ctx->inputs[INTEG_SAMPLE_MS]));
	double lo, hi;
	limits_read(&ctx->inputs[INTEG_MIN], &ctx->inputs[INTEG_MAX], &lo, &hi);
	double divisor = linkable_to_f64(&ctx->inputs[INTEG_DIVISOR]);
	if (!isfinite(divisor) || divisor <= 0)
		divisor = 1.0;
	bool reset = linkable_to_bool(&ctx->inputs[INTEG_RESET]);

	if (ctx->reason == REASON_START) {
		d->value = 0.0;
	} else if (!d->reset && isfinite(d->input) && ctx->now_ms > d->last_ms) {
		double seconds = (double)(ctx->now_ms - d->last_ms) / 1000.0;
		double v = d->value + d->input * seconds / d->divisor;
		if (!isnan(v))
			d->value = v;
	}
	d->value = fmin(fmax(reset ? 0.0 : d->value, lo), hi);
	d->input = linkable_to_f64(&ctx->inputs[INTEG_INPUT]);
	d->divisor = divisor;
	d->reset = reset;
	d->last_ms = ctx->now_ms;
	block_ctx_set_output(ctx, 0, linkable_from_f64(d->value));
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
	{.name = "process/Delay",
	 .icon = "hourglass_empty",
	 .description = "output takes input once it has been stable for milliseconds",
	 .inputs = delay_inputs,
	 .input_count = ARRAY_LEN(delay_inputs),
	 .outputs = delay_outputs,
	 .output_count = ARRAY_LEN(delay_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_SCHEDULED,
	 .compute = delay,
	 .state_size = sizeof(DelayState),
	 .change_threshold = NAN},
	{.name = "process/Trigger",
	 .icon = "bolt",
	 .description = "output false when arm rises, true when fire rises",
	 .inputs = trigger_inputs,
	 .input_count = ARRAY_LEN(trigger_inputs),
	 .outputs = trigger_outputs,
	 .output_count = ARRAY_LEN(trigger_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_NONE,
	 .compute = trigger,
	 .state_size = sizeof(TriggerState),
	 .change_threshold = NAN},
	{.name = "process/Limit",
	 .icon = "compress",
	 .description = "output = input capped to [min, max]",
	 .inputs = limit_inputs,
	 .input_count = ARRAY_LEN(limit_inputs),
	 .outputs = limit_outputs,
	 .output_count = ARRAY_LEN(limit_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_NONE,
	 .compute = limit,
	 .state_size = 0,
	 .change_threshold = NAN},
	{.name = "process/Hysteresis",
	 .icon = "swap_vert",
	 .description = "Schmitt trigger: output on above threshold+hysteresis, "
			"off below threshold-hysteresis",
	 .inputs = hysteresis_inputs,
	 .input_count = ARRAY_LEN(hysteresis_inputs),
	 .outputs = hysteresis_outputs,
	 .output_count = ARRAY_LEN(hysteresis_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_NONE,
	 .compute = hysteresis,
	 .state_size = sizeof(HysteresisState),
	 .change_threshold = NAN},
	{.name = "process/Exponential",
	 .icon = "function",
	 .description = "output = a*exp(b*input)",
	 .inputs = exp_inputs,
	 .input_count = ARRAY_LEN(exp_inputs),
	 .outputs = exponential_outputs,
	 .output_count = ARRAY_LEN(exponential_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_NONE,
	 .compute = exponential,
	 .state_size = 0,
	 .change_threshold = NAN},
	{.name = "process/Logarithmic",
	 .icon = "function",
	 .description = "output = a*ln(b*input)",
	 .inputs = exp_inputs,
	 .input_count = ARRAY_LEN(exp_inputs),
	 .outputs = logarithmic_outputs,
	 .output_count = ARRAY_LEN(logarithmic_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_NONE,
	 .compute = logarithmic,
	 .state_size = 0,
	 .change_threshold = NAN},
	{.name = "process/Integrator",
	 .icon = "functions",
	 .description = "output = integral of input dt / divisor, within [min, max]",
	 .inputs = integrator_inputs,
	 .input_count = ARRAY_LEN(integrator_inputs),
	 .outputs = integrator_outputs,
	 .output_count = ARRAY_LEN(integrator_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_PERIODIC,
	 .period_ms = 0, /* sample_ms at rest: only on input changes */
	 .compute = integrator,
	 .state_size = sizeof(IntegratorState),
	 .change_threshold = NAN},
};

const size_t process_more_block_types_count = ARRAY_LEN(process_more_block_types);
