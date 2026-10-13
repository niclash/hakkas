#include "graphs/runtime.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "graphs/graph_internal.h"

/* Initial size of the slot table; it doubles when full. */
#define SLOTS_INITIAL 16

static void on_drop(void *ctx, const WorkEntry *e);

void runtime_init(Runtime *rt)
{
	memset(rt, 0, sizeof(*rt));
	work_queue_init(&rt->queue);
	work_queue_set_drop_hook(&rt->queue, on_drop, rt);
	deadline_heap_init(&rt->timers);
	rt->free_head = RUNTIME_NO_SLOT;
	rt->change_threshold = RUNTIME_DEFAULT_CHANGE_THRESHOLD;
}

void runtime_deinit(Runtime *rt)
{
	work_queue_clear(&rt->queue);
	deadline_heap_deinit(&rt->timers);
	rt->timer_blocks = 0;
	free(rt->links); /* empty: graph_destroy dropped every link */
	rt->links = NULL;
	rt->link_count = 0;
	rt->link_cap = 0;
	free(rt->slots);
	rt->slots = NULL;
	rt->slot_used = 0;
	rt->slot_cap = 0;
	rt->free_head = RUNTIME_NO_SLOT;
}

bool runtime_set_change_threshold(Runtime *rt, double threshold)
{
	if (isnan(threshold) || threshold < 0)
		return false;
	rt->change_threshold = threshold;
	return true;
}

void runtime_set_observer(Runtime *rt, graph_event_fn fn, void *ctx)
{
	rt->on_event = fn;
	rt->event_ctx = ctx;
}

/* ---- Block slot table ---------------------------------------------------- */

bool runtime_slot_reserve(Runtime *rt)
{
	if (rt->free_head != RUNTIME_NO_SLOT || rt->slot_used < rt->slot_cap)
		return true;
	if (rt->slot_cap >= RUNTIME_NO_SLOT / 2)
		return false;
	uint32_t cap = rt->slot_cap == 0 ? SLOTS_INITIAL : rt->slot_cap * 2;
	BlockSlot *slots = realloc(rt->slots, (size_t)cap * sizeof(*slots));
	if (slots == NULL)
		return false;
	rt->slots = slots;
	rt->slot_cap = cap;
	return true;
}

BlockHandle runtime_slot_alloc(Runtime *rt, Block *b)
{
	uint32_t i;
	if (rt->free_head != RUNTIME_NO_SLOT) {
		i = rt->free_head;
		rt->free_head = rt->slots[i].next_free;
	} else {
		i = rt->slot_used++;
		rt->slots[i].gen = 0;
	}
	rt->slots[i].block = b;
	rt->slots[i].next_free = RUNTIME_NO_SLOT;
	return (BlockHandle){i, rt->slots[i].gen};
}

void runtime_slot_free(Runtime *rt, BlockHandle h)
{
	BlockSlot *s = &rt->slots[h.slot];
	s->block = NULL;
	s->gen++;
	s->next_free = rt->free_head;
	rt->free_head = h.slot;
}

Block *runtime_block(const Runtime *rt, BlockHandle h)
{
	if (h.slot >= rt->slot_used)
		return NULL;
	const BlockSlot *s = &rt->slots[h.slot];
	return s->block != NULL && s->gen == h.gen ? s->block : NULL;
}

/* ---- Drain loop (01 §3.2) --------------------------------------------- */

static void dispatch(Runtime *rt, WorkEntry *e)
{
	switch (e->type) {
	case WORK_INPUT_UPDATE: {
		/* Stale if the block was destroyed or the input's source changed
		 * since the entry was queued (01 §3.2, 01 §2.2 "Severing"). */
		Block *b = runtime_block(rt, e->as.input.block);
		uint32_t i = e->as.input.input;
		if (b == NULL || i >= b->input_count ||
		    b->inputs[i].source_gen != e->as.input.source_gen)
			break;
		Linkable v = e->as.input.value; /* a move */
		e->as.input.value = (Linkable){0};
		block_update(b, i, v);
		break;
	}
	case WORK_TIMER_EXPIRY: {
		/* Stale if the block was destroyed or its timer was re-armed or
		 * cancelled (stop included) since the expiry was queued. */
		Block *b = runtime_block(rt, e->as.timer.block);
		if (b == NULL || b->timer_gen != e->as.timer.timer_gen)
			break;
		block_timer_expired(b);
		break;
	}
	case WORK_IO_EVENT:
		/* Nothing produces IO events yet: plan task 22 routes them
		 * through the channel binding table to io/ blocks (03 §4). */
		break;
	case WORK_NONE:
		break;
	}
	work_queue_entry_clear(e);
}

size_t runtime_drain(Runtime *rt, size_t max_entries)
{
	size_t n = 0;
	WorkEntry e;
	while (n < max_entries && work_queue_pop(&rt->queue, &e)) {
		n++;
		dispatch(rt, &e);
	}
	return n;
}

/* ---- Timers (01 §3.2 item 7) ----------------------------------------------- */

static Block *timer_block(Deadline *d)
{
	return (Block *)(void *)((char *)d - offsetof(Block, timer));
}

/* The next tick of a periodic timer that was due at due: due + period, or, if
 * that is not after now (the loop fell behind by a period or more), the first
 * tick of the same grid after now; the missed ticks are skipped. */
static uint64_t periodic_next(uint64_t due, uint32_t period, uint64_t now)
{
	uint64_t next = due + period;
	if (next <= now)
		next += ((now - next) / period + 1) * period;
	return next;
}

size_t runtime_fire_timers(Runtime *rt, uint64_t now_ms)
{
	runtime_set_now(rt, now_ms);
	size_t n = 0;
	Deadline *d;
	/* A re-armed periodic timer is due after now_ms, and a dropped expiry
	 * re-arms one ms later (on_drop), so each block fires once here. */
	while ((d = deadline_heap_pop_due(&rt->timers, now_ms)) != NULL) {
		Block *b = timer_block(d);
		work_queue_push_timer(&rt->queue, b->handle, b->timer_gen);
		n++;
		/* The periodic timer's own continuation keeps the generation:
		 * its queued expiry stays valid (01 §3.2). */
		if (b->type->timer == TIMER_PERIODIC && b->period_ms > 0) {
			uint64_t next = periodic_next(d->due_ms, b->period_ms, now_ms);
			(void)deadline_heap_set(&rt->timers, d, next);
		}
	}
	return n;
}

uint64_t runtime_timer_timeout_ms(const Runtime *rt, uint64_t now_ms)
{
	const Deadline *d = deadline_heap_peek(&rt->timers);
	if (d == NULL)
		return RUNTIME_NO_TIMEOUT;
	return d->due_ms <= now_ms ? 0 : d->due_ms - now_ms;
}

bool runtime_next_deadline(const Runtime *rt, uint64_t *due_ms)
{
	const Deadline *d = deadline_heap_peek(&rt->timers);
	if (d == NULL)
		return false;
	*due_ms = d->due_ms;
	return true;
}

/* A timer expiry dropped on queue overflow (01 §3.2 item 9) would leave a
 * scheduled block's one-shot timer disarmed for good, so if it is still the
 * block's current expiry, the timer is re-armed to fire again 1 ms from now
 * (decided, plan task 8). A periodic timer was re-armed when it fired: only
 * that one tick is lost, as a skipped tick. */
static void on_drop(void *ctx, const WorkEntry *e)
{
	Runtime *rt = ctx;
	if (e->type != WORK_TIMER_EXPIRY)
		return;
	Block *b = runtime_block(rt, e->as.timer.block);
	if (b == NULL || b->timer_gen != e->as.timer.timer_gen || deadline_armed(&b->timer))
		return;
	(void)deadline_heap_set(&rt->timers, &b->timer, rt->now_ms + 1);
}
