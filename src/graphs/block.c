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
	if (!isnan(b->metadata.change_threshold))
		return b->metadata.change_threshold;
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
	case LINKABLE_METRIC:
		if (last->as.metric.category != next->as.metric.category ||
		    strcmp(last->as.metric.unit, next->as.metric.unit) != 0)
			return true;
		return f64_changed(last->as.metric.value, next->as.metric.value, threshold);
	}
	return true;
}

/* ---- Propagation (01 §3.2) ---------------------------------------------- */

/* Set last_propagated and queue an input update for every link, carrying the
 * destination input's current source generation. */
static void push_output(Block *b, uint32_t i)
{
	Output *o = &b->outputs[i];
	const Linkable *v = &b->out_values[i];
	WorkQueue *q = &b->graph->rt->queue;

	linkable_assign(&o->last_propagated, v);
	for (uint32_t k = 0; k < o->link_count; k++) {
		const Link *l = &o->links[k];
		work_queue_push_input(q, l->block->handle, l->input,
				      l->block->inputs[l->input].source_gen, linkable_copy(v));
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

static void run_compute(Block *b, Reason reason, int changed_input)
{
	if (b->type == NULL || b->type->compute == NULL)
		return;
	BlockCtx ctx = {
		.inputs = b->in_values,
		.outputs = b->out_values,
		.state = b->state,
		.now_ms = b->graph->rt->now_ms,
		.reason = reason,
		.changed_input = changed_input,
		.error = &b->error,
	};
	/* The return value re-arms a TIMER_SCHEDULED type's timer; timers
	 * arrive with plan task 8. */
	(void)b->type->compute(&ctx);
}

/* The compute step (03 §2): compute, then output_set every output. force
 * pushes every output regardless of the threshold (start). */
static void compute_step(Block *b, Reason reason, int changed_input, bool force)
{
	if (!b->started)
		return;
	run_compute(b, reason, changed_input);
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
	/* Arming a TIMER_PERIODIC type's timer: plan task 8. The START push
	 * also makes "the first value after start always propagates" (01 §3.3)
	 * hold: every output is pushed here, whatever its last_propagated. */
	compute_step(b, REASON_START, -1, true);
}

void block_stop(Block *b)
{
	if (!b->started)
		return;
	b->timer_gen++; /* cancels the timer: a pending expiry is stale */
	run_compute(b, REASON_STOP, -1);
	b->started = false;
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
