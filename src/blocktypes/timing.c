/*
 * timing/Interval (03 §3.1): a periodic pulse generator, PWM source or edge
 * generator. A TIMER_SCHEDULED type: every compute returns the delay to the
 * cycle's next transition, which the runtime arms as the block's one-shot
 * timer (03 §2), so the block is woken exactly at each edge.
 *
 * Decisions (plan task 8, recorded in 03 §3.1):
 *  - Times are whole engine milliseconds. The latched period is
 *    to_f64(milliseconds) rounded to the nearest ms, at least 1 ms and at most
 *    INTERVAL_MAX_MS; on_time is period × duty / 100 rounded to the nearest
 *    ms.
 *  - "Non-number": milliseconds whose to_f64 is NaN or ±Infinity makes the
 *    block idle, like a value <= 0. Text that is not a number, and unset,
 *    convert to 0 (01 §2.1) and are idle by the <= 0 rule. A NaN duty-cycle
 *    clamps to 0.
 *  - Every transition time is computed from the cycle start, never from the
 *    time a timer actually fired. A late timer brings the output to the
 *    waveform's value at the current time; whole cycles missed meanwhile are
 *    skipped, not replayed, and the next cycle starts on the original grid
 *    (start + k × period), latching the inputs current then.
 *  - An input change while running only applies a transition already due and
 *    re-arms the timer for the next one (any compute re-arms a scheduled
 *    timer, 03 §2); an input change while idle re-latches and starts a fresh
 *    cycle if the period is now valid.
 *  - stop() leaves out as it was (it is not propagated anyway); start()
 *    always begins a fresh cycle, so nothing of a cycle interrupted by a
 *    stop survives.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "blocktypes/builtins.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* The longest period, in ms (about 285 000 years): 2^53, so every period and
 * on-time is exact as a double. */
#define INTERVAL_MAX_MS 9007199254740992.0

enum { IN_MILLISECONDS, IN_DUTY_CYCLE };

static const PortDescriptor interval_inputs[] = {
	{.name = "milliseconds",
	 .description = "Period of one full cycle, in milliseconds.",
	 .type = "number",
	 .default_value = LINKABLE_F64_INIT(1000.0)},
	{.name = "duty-cycle",
	 .description = "Percentage of each period during which out is true, 0-100 "
			"(clamped to that range).",
	 .type = "number",
	 .default_value = LINKABLE_F64_INIT(50.0)},
};

static const PortDescriptor interval_outputs[] = {
	{.name = "out", .description = "The generated pulse train.", .type = "bool",
	 .default_value = LINKABLE_BOOL_INIT(false)},
};

typedef struct {
	bool running;	      /* false: idle, out false, no timer */
	bool on;	      /* in the cycle's on phase */
	uint64_t cycle_start; /* engine time the current cycle started */
	uint64_t period;      /* the cycle's latched period, ms, >= 1 */
	uint64_t on_time;     /* the cycle's latched on-time, ms, 0..period */
} IntervalState;

/* Latch the inputs for a cycle (03 §3.1). False, leaving the block idle, if
 * the period is not a positive number. */
static bool latch(IntervalState *s, const Linkable *inputs)
{
	double ms = linkable_to_f64(&inputs[IN_MILLISECONDS]);
	if (!isfinite(ms) || ms <= 0) {
		s->running = false;
		return false;
	}
	double period = fmin(fmax(round(ms), 1.0), INTERVAL_MAX_MS);
	/* fmax/fmin treat NaN as missing, so a NaN duty-cycle clamps to 0. */
	double duty = fmin(fmax(linkable_to_f64(&inputs[IN_DUTY_CYCLE]), 0.0), 100.0);
	double on_time = fmin(round(period * duty / 100.0), period);
	s->period = (uint64_t)period;
	s->on_time = (uint64_t)on_time;
	s->running = true;
	return true;
}

/* Start a cycle at time t, latching the current inputs. */
static void begin_cycle(IntervalState *s, const Linkable *inputs, uint64_t t)
{
	if (!latch(s, inputs))
		return;
	s->cycle_start = t;
	s->on = s->on_time > 0;
}

/* The time of the running cycle's next transition. */
static uint64_t next_transition(const IntervalState *s)
{
	return s->cycle_start + (s->on ? s->on_time : s->period);
}

/* Apply every transition due at now, each at its own time from the cycle
 * start. A cycle that is over starts the next one on the grid; cycles that
 * ended entirely before now are skipped. */
static void advance(IntervalState *s, const Linkable *inputs, uint64_t now)
{
	while (s->running && next_transition(s) <= now) {
		if (s->on && s->on_time < s->period) {
			s->on = false; /* on-time elapsed */
			continue;
		}
		/* The period elapsed (at 100 % the on-time ends with it). */
		uint64_t next = s->cycle_start + s->period;
		next += (now - next) / s->period * s->period;
		begin_cycle(s, inputs, next);
	}
}

static int64_t interval(BlockCtx *ctx)
{
	IntervalState *s = ctx->state;
	uint64_t now = ctx->now_ms;

	switch (ctx->reason) {
	case REASON_START:
		begin_cycle(s, ctx->inputs, now);
		break;
	case REASON_INPUT:
		/* Takes effect at the next cycle start, unless idle. */
		if (!s->running)
			begin_cycle(s, ctx->inputs, now);
		advance(s, ctx->inputs, now);
		break;
	case REASON_TIMER:
		advance(s, ctx->inputs, now);
		break;
	case REASON_STOP:
		s->running = false; /* start() begins afresh; out is kept */
		return -1;
	}
	block_ctx_set_output(ctx, 0, linkable_from_bool(s->running && s->on));
	return s->running ? (int64_t)(next_transition(s) - now) : -1;
}

const BlockTypeDef timing_block_types[] = {
	{.name = "timing/Interval",
	 .icon = "timer",
	 .description = "periodic pulse generator: PWM or edge generator",
	 .inputs = interval_inputs,
	 .input_count = ARRAY_LEN(interval_inputs),
	 .outputs = interval_outputs,
	 .output_count = ARRAY_LEN(interval_outputs),
	 .compute_on_input = true,
	 .timer = TIMER_SCHEDULED,
	 .compute = interval,
	 .state_size = sizeof(IntervalState),
	 .change_threshold = NAN},
};

const size_t timing_block_types_count = ARRAY_LEN(timing_block_types);
