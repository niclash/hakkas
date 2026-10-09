#include "graphs/runtime.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "graphs/graph_internal.h"

/* Initial size of the slot table; it doubles when full. */
#define SLOTS_INITIAL 16

void runtime_init(Runtime *rt)
{
	memset(rt, 0, sizeof(*rt));
	work_queue_init(&rt->queue);
	rt->free_head = RUNTIME_NO_SLOT;
	rt->change_threshold = RUNTIME_DEFAULT_CHANGE_THRESHOLD;
}

void runtime_deinit(Runtime *rt)
{
	work_queue_clear(&rt->queue);
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
	case WORK_TIMER_EXPIRY:
		/* Nothing produces timer expiries yet: plan task 8 adds timers
		 * and dispatches them here (03 §2 "timer expiry"). */
		break;
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
