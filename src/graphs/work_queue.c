#include "graphs/work_queue.h"

#include <inttypes.h>
#include <string.h>

#include "system/log.h"

void work_queue_init(WorkQueue *q)
{
	memset(q, 0, sizeof(*q));
}

void work_queue_set_drop_hook(WorkQueue *q, work_queue_drop_fn fn, void *ctx)
{
	q->on_drop = fn;
	q->drop_ctx = ctx;
}

void work_queue_entry_clear(WorkEntry *e)
{
	switch (e->type) {
	case WORK_INPUT_UPDATE:
		linkable_clear(&e->as.input.value);
		break;
	case WORK_IO_EVENT:
		linkable_clear(&e->as.io.value);
		break;
	case WORK_NONE:
	case WORK_TIMER_EXPIRY:
		break;
	}
	memset(e, 0, sizeof(*e));
}

bool work_queue_pop(WorkQueue *q, WorkEntry *out)
{
	if (q->len == 0) {
		memset(out, 0, sizeof(*out));
		return false;
	}
	WorkEntry *e = &q->entries[q->head];
	*out = *e; /* a move: the slot gives up its reference */
	memset(e, 0, sizeof(*e));
	q->head = q->head + 1 == HAKKAS_QUEUE_CAPACITY ? 0 : q->head + 1;
	q->len--;
	return true;
}

void work_queue_clear(WorkQueue *q)
{
	WorkEntry e;
	while (work_queue_pop(q, &e))
		work_queue_entry_clear(&e);
	q->head = 0;
}

/* The slot for a new entry at the tail, dropping the oldest entry if the queue
 * is full (01 §3.2 item 9). */
static WorkEntry *push_slot(WorkQueue *q)
{
	if (q->len == HAKKAS_QUEUE_CAPACITY) {
		WorkEntry oldest;
		work_queue_pop(q, &oldest);
		if (q->on_drop != NULL)
			q->on_drop(q->drop_ctx, &oldest);
		work_queue_entry_clear(&oldest);
		q->drops++;
	}
	uint32_t tail = q->head + q->len;
	if (tail >= HAKKAS_QUEUE_CAPACITY)
		tail -= HAKKAS_QUEUE_CAPACITY;
	q->len++;
	return &q->entries[tail];
}

void work_queue_push_input(WorkQueue *q, BlockHandle block, uint32_t input, uint32_t source_gen,
			   Linkable value)
{
	WorkEntry *e = push_slot(q);
	e->type = WORK_INPUT_UPDATE;
	e->as.input.block = block;
	e->as.input.input = input;
	e->as.input.source_gen = source_gen;
	e->as.input.value = value;
}

void work_queue_push_timer(WorkQueue *q, BlockHandle block, uint32_t timer_gen)
{
	WorkEntry *e = push_slot(q);
	e->type = WORK_TIMER_EXPIRY;
	e->as.timer.block = block;
	e->as.timer.timer_gen = timer_gen;
}

void work_queue_push_io(WorkQueue *q, uint32_t port, uint32_t index, Linkable value)
{
	WorkEntry *e = push_slot(q);
	e->type = WORK_IO_EVENT;
	e->as.io.port = port;
	e->as.io.index = index;
	e->as.io.value = value;
}

void work_queue_push_entry(WorkQueue *q, WorkEntry *e)
{
	if (e->type == WORK_NONE)
		return;
	WorkEntry *slot = push_slot(q);
	*slot = *e; /* a move */
	memset(e, 0, sizeof(*e));
}

bool work_queue_report_drops(WorkQueue *q, uint64_t now_ms)
{
	if (q->drops == q->drops_reported)
		return false;
	if (q->reported && now_ms - q->last_report_ms < WORK_QUEUE_DROP_REPORT_INTERVAL_MS)
		return false;
	log_warn("work queue full: dropped %" PRIu64 " oldest entries since last warning (%" PRIu64
		 " total)",
		 q->drops - q->drops_reported, q->drops);
	q->drops_reported = q->drops;
	q->last_report_ms = now_ms;
	q->reported = true;
	return true;
}
