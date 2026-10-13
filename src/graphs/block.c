/*
 * The generic block runtime (01 §2.3, 03 §2): update, the compute step,
 * output_set with the change threshold (01 §3.3), start and stop, and the
 * live-value watches. Implemented once for every block type; only
 * type->compute differs.
 */
#include <math.h>
#include <string.h>

#include "graphs/graph_internal.h"

/* ---- Change threshold (01 §3.3) ---------------------------------------- */

double block_change_threshold(const Block *b)
{
	/* No per-instance override (decided, plan task 21c): an inner block of
	 * a composite resolves the same way, from its own type (03 §5). */
	if (b->type != NULL && !isnan(b->type->change_threshold))
		return b->type->change_threshold;
	return b->graph->rt->change_threshold;
}

static bool f64_changed(double last, double next, double threshold)
{
	bool last_nan = isnan(last), next_nan = isnan(next);
	if (last_nan || next_nan)
		return last_nan != next_nan; /* into or out of NaN; NaN → NaN is no change */
	if (last == next)
		return false; /* so threshold 0 propagates every distinct value */
	return fabs(next - last) >= threshold;
}

static bool i64_changed(int64_t last, int64_t next, double threshold)
{
	if (last == next)
		return false;
	/* |next − last| without overflow, as an unsigned 64-bit distance. */
	uint64_t d = next > last ? (uint64_t)next - (uint64_t)last
				 : (uint64_t)last - (uint64_t)next;
	return (double)d >= threshold;
}

bool block_value_changed(const Linkable *last, const Linkable *next, double threshold)
{
	if (last->type != next->type)
		return true;
	switch (next->type) {
	case LINKABLE_UNSET:
		return false;
	case LINKABLE_BOOL:
		return last->as.b != next->as.b;
	case LINKABLE_STRING:
		return last->as.s != next->as.s && strcmp(last->as.s, next->as.s) != 0;
	case LINKABLE_I64:
		return i64_changed(last->as.i64, next->as.i64, threshold);
	case LINKABLE_F64:
		return f64_changed(last->as.f64, next->as.f64, threshold);
	}
	return true;
}

/* ---- Composite mapping (03 §5) ----------------------------------------- */

Block *block_input_target(Block *b, uint32_t *input)
{
	while (block_is_composite(b)) {
		const CompositeRef *r = &b->type->composite->inports[*input];
		*input = r->port;
		b = b->inner[r->block];
	}
	return b;
}

Block *block_output_source(Block *b, uint32_t *output)
{
	while (block_is_composite(b)) {
		const CompositeRef *r = &b->type->composite->outports[*output];
		*output = r->port;
		b = b->inner[r->block];
	}
	return b;
}

const Linkable *block_input_value(const Block *b, uint32_t input)
{
	/* The mapping only reads; b itself is not changed. */
	const Block *t = block_input_target((Block *)b, &input);
	return &t->in_values[input];
}

const Linkable *block_output_value(const Block *b, uint32_t output)
{
	const Block *s = block_output_source((Block *)b, &output);
	return &s->out_values[output];
}

/* ---- Propagation (01 §3.2) ---------------------------------------------- */

/* Set last_propagated and queue an input update for every link, carrying the
 * destination input's current source generation. A link's block is its
 * structural destination; a composite instance's input is delivered to the
 * mapped inner input (03 §5), with that input's generation. */
static void push_output(Block *b, uint32_t i)
{
	Output *o = &b->outputs[i];
	const Linkable *v = &b->out_values[i];
	WorkQueue *q = &b->graph->rt->queue;

	linkable_assign(&o->last_propagated, v);
	for (uint32_t k = 0; k < o->link_count; k++) {
		const Link *l = &o->links[k];
		uint32_t in = l->input;
		Block *t = block_input_target(l->block, &in);
		work_queue_push_input(q, t->handle, in, t->inputs[in].source_gen,
				      linkable_copy(v));
	}
}

static void propagate(Block *b, uint32_t i, double threshold)
{
	if (block_value_changed(&b->outputs[i].last_propagated, &b->out_values[i], threshold))
		push_output(b, i);
}

void block_output_set(Block *b, uint32_t output, Linkable value)
{
	linkable_clear(&b->out_values[output]);
	b->out_values[output] = value;
	if (b->started)
		propagate(b, output, block_change_threshold(b));
}

/* ---- Timers (01 §3.2 item 7, 03 §2) ----------------------------------------- */

void block_timer_arm(Block *b, uint64_t due_ms)
{
	b->timer_gen++;
	/* Room for every timer block is reserved when it is created
	 * (graph_create_block), so this cannot fail. */
	(void)deadline_heap_set(&b->graph->rt->timers, &b->timer, due_ms);
}

void block_timer_cancel(Block *b)
{
	b->timer_gen++;
	deadline_heap_remove(&b->graph->rt->timers, &b->timer);
}

/* Arm a started periodic block's timer one period from now, or cancel it if
 * the period is 0. */
static void periodic_arm(Block *b)
{
	if (b->period_ms > 0)
		block_timer_arm(b, b->graph->rt->now_ms + b->period_ms);
	else
		block_timer_cancel(b);
}

/* Re-arm (delay >= 0) or cancel (delay < 0) a TIMER_SCHEDULED block's one-shot
 * timer from compute's return value, relative to now (01 §2.3). */
static void scheduled_rearm(Block *b, int64_t delay)
{
	if (delay < 0) {
		block_timer_cancel(b);
		return;
	}
	uint64_t now = b->graph->rt->now_ms;
	uint64_t d = (uint64_t)delay;
	block_timer_arm(b, d > UINT64_MAX - now ? UINT64_MAX : now + d);
}

void block_set_period(Block *b, uint32_t period_ms)
{
	if (b->type == NULL || b->type->timer != TIMER_PERIODIC)
		return;
	b->period_ms = period_ms;
	if (b->started)
		periodic_arm(b);
}

/* ---- Compute step (03 §2) ----------------------------------------------------- */

static int64_t run_compute(Block *b, Reason reason, int changed_input)
{
	if (b->type == NULL || b->type->compute == NULL)
		return -1;
	BlockCtx ctx = {
		.inputs = b->in_values,
		.outputs = b->out_values,
		.state = b->state,
		.now_ms = b->graph->rt->now_ms,
		.wall_ms = runtime_wall_ms(b->graph->rt),
		.tz = &b->graph->rt->tz,
		.reason = reason,
		.changed_input = changed_input,
	};
	return b->type->compute(&ctx);
}

/* The compute step (03 §2): compute, re-arm or cancel a TIMER_SCHEDULED type's
 * timer from its return value (after every compute of a started block, for
 * any reason), then output_set every output. force pushes every output
 * regardless of the threshold (start). */
static void compute_step(Block *b, Reason reason, int changed_input, bool force)
{
	if (!b->started)
		return;
	int64_t delay = run_compute(b, reason, changed_input);
	if (b->type != NULL && b->type->timer == TIMER_SCHEDULED)
		scheduled_rearm(b, delay);
	double threshold = block_change_threshold(b);
	for (uint32_t i = 0; i < b->output_count; i++) {
		if (force)
			push_output(b, i);
		else
			propagate(b, i, threshold);
	}
}

/* ---- Events (03 §2 table) --------------------------------------------------- */

static void fire_watches(Block *b, uint32_t i)
{
	InputWatch *w = b->inputs[i].watches;
	while (w != NULL) {
		InputWatch *next = w->next;
		w->on_value(w, b, i, &b->in_values[i]);
		w = next;
	}
}

void block_update(Block *b, uint32_t input, Linkable value)
{
	b = block_input_target(b, &input); /* a composite's inner input (03 §5) */
	linkable_clear(&b->in_values[input]);
	b->in_values[input] = value;
	fire_watches(b, input);
	if (b->started && b->type != NULL && b->type->compute_on_input)
		compute_step(b, REASON_INPUT, (int)input, false);
}

void block_start(Block *b)
{
	if (b->started)
		return;
	b->started = true;
	if (block_is_composite(b)) {
		/* 03 §5: the instance itself computes nothing; its inner
		 * blocks start in the definition's order. */
		for (uint32_t j = 0; j < b->inner_count; j++)
			block_start(b->inner[j]);
		return;
	}
	/* 03 §2 start: the periodic timer is armed before the START compute;
	 * its first tick is one period after the start. A scheduled type arms
	 * from START's return value (compute_step). */
	if (b->type != NULL && b->type->timer == TIMER_PERIODIC)
		periodic_arm(b);
	/* The START push also makes "the first value after start always
	 * propagates" (01 §3.3) hold: every output is pushed here, whatever
	 * its last_propagated. */
	compute_step(b, REASON_START, -1, true);
}

void block_stop(Block *b)
{
	if (!b->started)
		return;
	if (block_is_composite(b)) {
		for (uint32_t j = 0; j < b->inner_count; j++)
			block_stop(b->inner[j]);
		b->started = false;
		return;
	}
	/* A stopped block has no armed timer; a queued expiry goes stale.
	 * STOP's return value is ignored (decided, plan task 8). */
	block_timer_cancel(b);
	(void)run_compute(b, REASON_STOP, -1);
	b->started = false;
}

void block_timer_expired(Block *b)
{
	/* A periodic timer re-armed itself when it fired (runtime.c); a
	 * scheduled one is re-armed by this compute step. */
	compute_step(b, REASON_TIMER, -1, false);
}

Linkable block_input_default(const Block *b, uint32_t input)
{
	const PortDescriptor *p = block_input_port(b, input);
	if (p->default_value.type == LINKABLE_UNSET)
		return linkable_default_value(p->type);
	return linkable_copy(&p->default_value);
}

/* ---- Live-value watches (03 §2 subscribe_link, 02 §4.2) ------------------- */

void block_watch_input(Block *b, uint32_t input, InputWatch *w)
{
	b = block_input_target(b, &input); /* a composite's inner input (03 §5) */
	Input *in = &b->inputs[input];
	/* Appended, so watches fire in attach order. */
	InputWatch **pp = &in->watches;
	while (*pp != NULL)
		pp = &(*pp)->next;
	w->next = NULL;
	*pp = w;
	w->on_value(w, b, input, &b->in_values[input]);
}

void block_unwatch_input(Block *b, uint32_t input, InputWatch *w)
{
	b = block_input_target(b, &input);
	for (InputWatch **pp = &b->inputs[input].watches; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == w) {
			*pp = w->next;
			w->next = NULL;
			return;
		}
	}
}

void block_detach_watches(Block *b)
{
	/* A composite instance's watches are on its inner blocks' inputs. */
	for (uint32_t j = 0; j < b->inner_count; j++)
		block_detach_watches(b->inner[j]);
	for (uint32_t i = 0; i < b->input_count; i++) {
		InputWatch *w = b->inputs[i].watches;
		b->inputs[i].watches = NULL;
		while (w != NULL) {
			InputWatch *next = w->next;
			w->next = NULL;
			if (w->on_detach != NULL)
				w->on_detach(w);
			w = next;
		}
	}
}
