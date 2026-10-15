/*
 * process/Linear (03 §3): out = k*in+m, stateless and event-driven like the
 * math/... types (see blocktypes/math.c for the conventions).
 *
 * process/Deadband (03 §3, plan task 21c): out follows in only when it moves
 * by at least threshold. It needs no state: the value it holds is its own
 * current output (BlockCtx.outputs). Its type default change threshold is 0,
 * so the engine adds no filtering of its own.
 *
 * process/PID (03 §6.1, plan task 23): the PID regulator, a cyclic type
 * (TIMER_PERIODIC + compute_on_input) with per-instance state; see the
 * comment above its compute function.
 */
#include <math.h>
#include <stdint.h>

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

/* ---- process/PID (03 §6.1) ------------------------------------------------ */

enum {
	PID_SETPOINT,
	PID_MEASURED,
	PID_KP,
	PID_KI,
	PID_KD,
	PID_MAX_DGAIN,
	PID_OUTPUT_MIN,
	PID_OUTPUT_MAX,
	PID_REVERSE,
	PID_AUTO,
	PID_MANUAL_VALUE,
	PID_SAMPLE_MS,
};

enum { PID_OUTPUT, PID_SATURATED };


static const PortDescriptor pid_inputs[] = {
	{.name = "setpoint", .description = "Desired value of the measurement",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "input", .description = "Measured value of the controlled process",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "kp", .description = "Proportional gain (0: no proportional term)",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "ki", .description = "Integral gain, per second (0: no integral term)",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "kd", .description = "Derivative gain, in seconds (0: no derivative term)",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "max_dgain",
	 .description = "Highest gain of the derivative term (<= 0: no limit)",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "min", .description = "Lower output limit (equal limits: none)",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "max", .description = "Upper output limit (equal limits: none)",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "reverse",
	 .description = "Reverse acting (error = input - setpoint), e.g. cooling",
	 .type = "bool", .default_value = LINKABLE_BOOL_INIT(false)},
	{.name = "auto", .description = "Automatic (true) or manual (false) mode",
	 .type = "bool", .default_value = LINKABLE_BOOL_INIT(false)},
	{.name = "manual", .description = "Output in manual mode (clamped to the limits)",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "sample_ms", .description = "Sample period in milliseconds (<= 0: no sampling)",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
};

static const PortDescriptor pid_outputs[] = {
	{.name = "output", .description = "Regulator output, within the output limits",
	 .type = "number", .default_value = LINKABLE_F64_INIT(0.0)},
	{.name = "saturated", .description = "true while the output is at a limit",
	 .type = "bool", .default_value = LINKABLE_BOOL_INIT(false)},
};

typedef struct {
	double integral;       /* the integral term Ki·∫e dt, in output units */
	double dterm;	       /* the derivative term D of the last sample step */
	double pv_prev;	       /* the measurement at the last sample step */
	bool have_pv;	       /* pv_prev holds a valid measurement */
	bool auto_mode;	       /* the mode at the last compute */
	uint64_t last_step_ms; /* engine time of the last sample step (or start) */
} PidState;

/* The inputs, converted (01 §2.1). lo <= hi always (pid_read). */
typedef struct {
	double sp, pv, kp, ki, kd, max_dgain, lo, hi, manual;
	bool reverse, auto_mode;
} PidInputs;

static void pid_read(const Linkable *in, PidInputs *p)
{
	p->sp = linkable_to_f64(&in[PID_SETPOINT]);
	p->pv = linkable_to_f64(&in[PID_MEASURED]);
	p->kp = linkable_to_f64(&in[PID_KP]);
	p->ki = linkable_to_f64(&in[PID_KI]);
	p->kd = linkable_to_f64(&in[PID_KD]);
	p->max_dgain = linkable_to_f64(&in[PID_MAX_DGAIN]);
	p->manual = linkable_to_f64(&in[PID_MANUAL_VALUE]);
	p->reverse = linkable_to_bool(&in[PID_REVERSE]);
	p->auto_mode = linkable_to_bool(&in[PID_AUTO]);
	/* A NaN limit is no limit on its side; equal limits (both unset: 0)
	 * are no limits; limits given the wrong way round are swapped. */
	double lo = linkable_to_f64(&in[PID_OUTPUT_MIN]);
	double hi = linkable_to_f64(&in[PID_OUTPUT_MAX]);
	lo = isnan(lo) ? -INFINITY : lo;
	hi = isnan(hi) ? INFINITY : hi;
	if (lo == hi) {
		lo = -INFINITY;
		hi = INFINITY;
	}
	p->lo = fmin(lo, hi);
	p->hi = fmax(lo, hi);
}

/* The automatic-mode inputs are usable: every term is finite. */
static bool pid_valid(const PidInputs *p)
{
	return isfinite(p->sp) && isfinite(p->pv) && isfinite(p->kp) && isfinite(p->ki) &&
	       isfinite(p->kd);
}

static double pid_error(const PidInputs *p)
{
	return p->reverse ? p->pv - p->sp : p->sp - p->pv;
}

/* The sample period for sample_ms: rounded to whole ms, at least 1 ms and at
 * most UINT32_MAX ms; 0 (no sampling) for a value <= 0, NaN or infinite. */
static uint32_t pid_period(double ms)
{
	if (!isfinite(ms) || ms <= 0)
		return 0;
	return (uint32_t)fmin(fmax(round(ms), 1.0), (double)UINT32_MAX);
}

/* Set output (finite, clamped by the caller) and saturated: true while the
 * output is at a limit. */
static void pid_set(BlockCtx *ctx, const PidInputs *p, double u)
{
	block_ctx_set_output(ctx, PID_OUTPUT, linkable_from_f64(u));
	block_ctx_set_output(ctx, PID_SATURATED, linkable_from_bool(u <= p->lo || u >= p->hi));
}

static double pid_clamp(const PidInputs *p, double u)
{
	return fmin(fmax(u, p->lo), p->hi);
}

/* Manual mode: output = clamp(manual); a non-finite manual holds the output. */
static void pid_manual(BlockCtx *ctx, const PidInputs *p)
{
	if (isfinite(p->manual))
		pid_set(ctx, p, pid_clamp(p, p->manual));
}

/* Automatic mode without a sample step: output = clamp(P + I + D) from the
 * current proportional term and the stored integral and derivative terms.
 * Invalid inputs hold the output. */
static void pid_auto(BlockCtx *ctx, const PidInputs *p)
{
	const PidState *s = ctx->state;
	if (!pid_valid(p))
		return;
	double u = p->kp * pid_error(p) + s->integral + s->dterm;
	if (isfinite(u))
		pid_set(ctx, p, pid_clamp(p, u));
}

/* Bumpless transfer: preset the integral so that P + I + D equals the
 * current output. Without an integral term (Ki = 0) or with invalid inputs
 * the integral is 0. */
static void pid_preset(BlockCtx *ctx, const PidInputs *p)
{
	PidState *s = ctx->state;
	double u = linkable_to_f64(&ctx->outputs[PID_OUTPUT]);
	double i = u - p->kp * pid_error(p) - s->dterm;
	s->integral = p->ki != 0 && pid_valid(p) && isfinite(i) ? i : 0.0;
}

/* Advance the derivative term by one sample step of dt seconds (dt > 0).
 * D acts on the measurement: D(s) = −σ·Kd·s/(1 + Tf·s)·PV(s), σ = +1 direct,
 * −1 reverse, Tf = |Kd|/max_dgain, discretized by backward Euler
 * (s ≈ (1 − z⁻¹)/dt):
 *	D_k = (Tf·D_{k−1} + Kd·Δy) / (Tf + dt),  Δy = −σ·(pv_k − pv_{k−1})
 * It is stable for every Tf >= 0 and dt > 0, its gain for a step within one
 * sample is Kd/(Tf + dt) < max_dgain, and without a further change it decays
 * by Tf/(Tf + dt) per sample. max_dgain <= 0 or NaN (Tf = 0): the unfiltered
 * Kd·Δy/dt. The first valid sample after start (or after an invalid
 * measurement) only records the measurement (D = 0). */
static void pid_derivative(PidState *s, const PidInputs *p, double dt)
{
	if (!isfinite(p->pv) || !isfinite(p->kd)) {
		s->have_pv = false;
		s->dterm = 0.0;
		return;
	}
	if (p->kd == 0 || !s->have_pv) {
		s->dterm = 0.0;
	} else {
		double dy = p->reverse ? p->pv - s->pv_prev : s->pv_prev - p->pv;
		double tf = p->max_dgain > 0 ? fabs(p->kd) / p->max_dgain : 0.0;
		double d = (tf * s->dterm + p->kd * dy) / (tf + dt);
		s->dterm = isfinite(d) ? d : 0.0;
	}
	s->pv_prev = p->pv;
	s->have_pv = true;
}

/* The full sample step in automatic mode, dt seconds after the last one:
 * advance D, then the integral by Ki·e·dt with conditional integration — the
 * increment never drives P + I + D further beyond a limit than the limit
 * itself (it is cut to land on the limit, or to 0 when already beyond it),
 * while an increment back towards the range always applies — then output.
 * Invalid inputs hold the output and the integral, and restart the
 * derivative. */
static void pid_step(BlockCtx *ctx, const PidInputs *p, double dt)
{
	PidState *s = ctx->state;
	if (!pid_valid(p)) {
		s->have_pv = false;
		s->dterm = 0.0;
		return;
	}
	pid_derivative(s, p, dt);
	double e = pid_error(p);
	double pterm = p->kp * e;
	double integral = 0.0;
	if (p->ki != 0) {
		double u0 = pterm + s->integral + s->dterm;
		double inc = p->ki * e * dt;
		if (inc > 0)
			inc = fmin(inc, fmax(0.0, p->hi - u0));
		else if (inc < 0)
			inc = fmax(inc, fmin(0.0, p->lo - u0));
		integral = s->integral + inc;
	}
	double u = pterm + integral + s->dterm;
	if (!isfinite(u))
		return;
	s->integral = integral;
	pid_set(ctx, p, pid_clamp(p, u));
}

/*
 * process/PID (03 §6.1): out = Kp·e + Ki·∫e dt + D, parallel form.
 *  - START resets the state, then: auto → output from P alone; manual →
 *    output = clamp(manual), the integral preset from it.
 *  - TIMER (every sample_ms) is the full step, with dt the engine time since
 *    the last step or the start, so a late tick (skipped ticks, 01 §3.2)
 *    integrates the time that actually passed. In manual mode the
 *    derivative keeps tracking the measurement; the integral stays.
 *  - INPUT recomputes from the current P and the stored I and D, advancing
 *    neither. Switching to auto presets the integral (bumpless); Ki = 0
 *    clears the integral and Kd = 0 the derivative term at once.
 *  - Every compute but STOP sets the period from sample_ms
 *    (BlockCtx.period_ms); leaving "no sampling" restarts dt from now, so
 *    the idle time is not integrated.
 */
static int64_t pid(BlockCtx *ctx)
{
	PidState *s = ctx->state;
	if (ctx->reason == REASON_STOP)
		return 0;
	PidInputs p;
	pid_read(ctx->inputs, &p);
	uint32_t period = pid_period(linkable_to_f64(&ctx->inputs[PID_SAMPLE_MS]));
	if (period != ctx->period_ms) {
		if (ctx->period_ms == 0)
			s->last_step_ms = ctx->now_ms;
		ctx->period_ms = period;
	}

	switch (ctx->reason) {
	case REASON_START:
		*s = (PidState){.last_step_ms = ctx->now_ms, .auto_mode = p.auto_mode};
		if (p.auto_mode) {
			pid_auto(ctx, &p);
		} else {
			pid_manual(ctx, &p);
			pid_preset(ctx, &p);
		}
		break;
	case REASON_TIMER: {
		double dt = (double)(ctx->now_ms - s->last_step_ms) / 1000.0;
		if (dt <= 0) {
			/* No time passed since the last step: nothing to advance. */
			if (p.auto_mode)
				pid_auto(ctx, &p);
			else
				pid_manual(ctx, &p);
			break;
		}
		s->last_step_ms = ctx->now_ms;
		if (p.auto_mode) {
			pid_step(ctx, &p, dt);
		} else {
			pid_derivative(s, &p, dt);
			pid_manual(ctx, &p);
		}
		break;
	}
	case REASON_INPUT:
		if (p.ki == 0)
			s->integral = 0.0;
		if (p.kd == 0)
			s->dterm = 0.0;
		if (p.auto_mode) {
			if (!s->auto_mode)
				pid_preset(ctx, &p); /* manual → auto: bumpless */
			pid_auto(ctx, &p);
		} else {
			pid_manual(ctx, &p);
		}
		break;
	case REASON_STOP:
		break;
	}
	s->auto_mode = p.auto_mode;
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
	{.name = "process/PID",
	 .icon = "tune",
	 .description = "PID regulator",
	 .inputs = pid_inputs,
	 .input_count = ARRAY_LEN(pid_inputs),
	 .outputs = pid_outputs,
	 .output_count = ARRAY_LEN(pid_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_PERIODIC,
	 .period_ms = 0, /* sample_ms's default: no sampling until it is set */
	 .compute = pid,
	 .state_size = sizeof(PidState),
	 .change_threshold = NAN},
};

const size_t process_block_types_count = ARRAY_LEN(process_block_types);
