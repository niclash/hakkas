/*
 * The work queue (01 §3.2): the engine's single global FIFO of typed entries,
 * a fixed-capacity ring buffer. Every crossing from one block to another, every
 * timer expiry and every IO event becomes an entry here; the engine's drain
 * loop (plan task 6) pops and dispatches them one at a time.
 *
 * Naming: functions are prefixed work_queue_, the entry type enum WORK_. The
 * type names WorkQueue, WorkEntry and BlockHandle follow the spec's style.
 *
 * Entry types (01 §3.2):
 *  - input update (block, input index, value, source generation): produced by
 *    output_set, by initials and by severing (01 §2.2);
 *  - timer expiry (block, timer generation): produced via a block's timer;
 *  - IO event (sensor port, channel index, value): a sensor value delivered
 *    by the environment (04 §0) through the PAL's handoff (decided, plan
 *    task 22: the port is the index of the sensor port in the environment's
 *    description, the index its channel, 1..count, 0 for a port that is not
 *    addressable; hakkas_env.h).
 *
 * Staleness: entries never hold a pointer to a block. They carry a BlockHandle
 * and generation numbers, and the dispatcher (task 6) discards an entry, with
 * work_queue_entry_clear, when any of them no longer matches:
 *  - block handle: the block table slot's generation is incremented whenever
 *    the block in that slot is destroyed, so a handle whose gen differs from
 *    its slot's current gen (or whose slot is free) names a removed block;
 *  - source generation: each input has a counter, incremented whenever its
 *    source changes (edge connected or removed, initial added, replaced or
 *    removed, upstream block removed). An input update carries the counter
 *    value its producer saw when it was enqueued; on mismatch the update came
 *    from a severed or replaced source and is dropped (01 §2.2 "Severing");
 *  - timer generation: each block has a counter, incremented whenever its
 *    timer is re-armed or cancelled (and on stop/destroy); an expiry carrying
 *    an older value belongs to a cancelled or superseded timer and is dropped.
 * The queue itself only carries the numbers; it never checks them. Because of
 * this, no "remove entries for block X" operation is needed. Generations are
 * 32-bit and wrap; a stale entry could only be mistaken for a live one after
 * 2^32 changes while it sits in the queue, which a bounded queue rules out in
 * practice.
 *
 * Ownership: an entry owns a reference to its value (graphs/linkable.h). The
 * push functions take ownership of the value passed in (a move: the caller
 * must not clear it afterwards; pass linkable_copy(&v) to keep v). Popping
 * moves the entry, and its reference, to the caller, who must eventually
 * release it with work_queue_entry_clear (or move the value on). Entries
 * dropped on overflow or discarded by work_queue_clear are released here.
 *
 * Capacity: fixed at build time (01 §3.2 item 9): HAKKAS_QUEUE_CAPACITY
 * entries, default 1024. Override it from CMake with
 * -DHAKKAS_QUEUE_CAPACITY=<n> (a cache variable, passed to hakkas_core and its
 * users as a public compile definition so every translation unit agrees on
 * the struct size), or, in a port's own build, by defining the macro. The ring
 * is an array inside WorkQueue, never heap-allocated; the engine owns one
 * (static) instance, and tests create their own.
 *
 * Overflow: pushing onto a full queue drops the oldest entry (releasing its
 * value) to make room and counts it. The queue only counts and never reads a
 * clock; work_queue_report_drops logs the rate-limited warning. An optional
 * drop hook sees each dropped entry first: the runtime uses it so that a
 * dropped timer expiry re-arms its timer instead of losing it (decided, plan
 * task 8, 01 §3.2).
 *
 * Threading: engine-thread only, no locking (01 §3.2). Timers live in the core
 * and are fired by the engine loop itself (graphs/deadlines.h), so they push
 * here directly. IO workers never do: they use the PAL's cross-context
 * enqueue (01 §0, plan tasks 12 and 22), whose handoff is drained into this
 * queue on the engine thread.
 */
#ifndef HAKKAS_GRAPHS_WORK_QUEUE_H
#define HAKKAS_GRAPHS_WORK_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphs/linkable.h"

#ifndef HAKKAS_QUEUE_CAPACITY
#define HAKKAS_QUEUE_CAPACITY 1024
#endif

_Static_assert(HAKKAS_QUEUE_CAPACITY >= 1 && HAKKAS_QUEUE_CAPACITY <= UINT32_MAX / 2,
	       "HAKKAS_QUEUE_CAPACITY out of range");

/* Minimum time between two drop warnings of work_queue_report_drops. */
#ifndef WORK_QUEUE_DROP_REPORT_INTERVAL_MS
#define WORK_QUEUE_DROP_REPORT_INTERVAL_MS 10000
#endif

/* A reference to a block: its slot in the block table (task 6) and the slot's
 * generation when the handle was taken (see "Staleness" above). */
typedef struct {
	uint32_t slot;
	uint32_t gen;
} BlockHandle;

static inline bool block_handle_eq(BlockHandle a, BlockHandle b)
{
	return a.slot == b.slot && a.gen == b.gen;
}

enum work_type {
	WORK_NONE = 0, /* an empty entry; never queued */
	WORK_INPUT_UPDATE,
	WORK_TIMER_EXPIRY,
	WORK_IO_EVENT,
};

typedef struct {
	enum work_type type;
	union {
		struct {
			BlockHandle block;
			uint32_t input;	     /* index into the block's inputs */
			uint32_t source_gen; /* the input's source generation when sent */
			Linkable value;	     /* owned */
		} input;
		struct {
			BlockHandle block;
			uint32_t timer_gen; /* the block's timer generation when armed */
		} timer;
		struct {
			uint32_t port;	/* the sensor port: out_ports[port] (hakkas_env.h) */
			uint32_t index; /* its channel: 1..count, 0 if not addressable */
			Linkable value; /* owned */
		} io;
	} as;
} WorkEntry;

/* Called with each entry dropped on overflow, before it is released; it may
 * read the entry but must not push onto or pop from the queue. */
typedef void (*work_queue_drop_fn)(void *ctx, const WorkEntry *e);

typedef struct {
	WorkEntry entries[HAKKAS_QUEUE_CAPACITY];
	uint32_t head; /* index of the oldest entry */
	uint32_t len;
	uint64_t drops;		 /* entries dropped on overflow, ever */
	uint64_t drops_reported; /* drops already reported by work_queue_report_drops */
	uint64_t last_report_ms;
	bool reported;		 /* whether a report was ever logged */
	/* The drop hook (work_queue_set_drop_hook), NULL when none. */
	work_queue_drop_fn on_drop;
	void *drop_ctx;
} WorkQueue;

/* Make q an empty queue with no drops. Call once on a fresh queue: entries
 * already in q are not released (use work_queue_clear for that). */
void work_queue_init(WorkQueue *q);

/* Set the drop hook (NULL to remove it); work_queue_init clears it. */
void work_queue_set_drop_hook(WorkQueue *q, work_queue_drop_fn fn, void *ctx);

/* Release every queued entry's value and empty the queue (shutdown). The drop
 * counters are kept. */
void work_queue_clear(WorkQueue *q);

/* Append an entry, taking ownership of value. If the queue is full, the
 * oldest entry is dropped first (released and counted). */
void work_queue_push_input(WorkQueue *q, BlockHandle block, uint32_t input, uint32_t source_gen,
			   Linkable value);
void work_queue_push_timer(WorkQueue *q, BlockHandle block, uint32_t timer_gen);
void work_queue_push_io(WorkQueue *q, uint32_t port, uint32_t index, Linkable value);

/* Append *e (any type but WORK_NONE, which is ignored), taking ownership of
 * its value; *e is left empty. Used for entries handed over from another
 * context (pal_handoff_take, engine/loop.h). Overflow as above. */
void work_queue_push_entry(WorkQueue *q, WorkEntry *e);

/* Move the oldest entry into *out, which must not hold a value, and return
 * true; the caller now owns it. On an empty queue, return false and set *out
 * to an empty WORK_NONE entry. */
bool work_queue_pop(WorkQueue *q, WorkEntry *out);

/* Release e's value, if any, and make it an empty WORK_NONE entry. Used by
 * the dispatcher to discard a stale entry. */
void work_queue_entry_clear(WorkEntry *e);

static inline size_t work_queue_len(const WorkQueue *q)
{
	return q->len;
}

static inline bool work_queue_empty(const WorkQueue *q)
{
	return q->len == 0;
}

static inline size_t work_queue_capacity(void)
{
	return HAKKAS_QUEUE_CAPACITY;
}

/* Number of entries dropped on overflow since work_queue_init. */
static inline uint64_t work_queue_drops(const WorkQueue *q)
{
	return q->drops;
}

/* The rate-limited overflow warning (01 §3.2 item 9). If entries were dropped
 * since the last report, and no report was logged in the last
 * WORK_QUEUE_DROP_REPORT_INTERVAL_MS before now_ms, log one warn line with the
 * number of drops since the last report and the total, and return true.
 * Otherwise do nothing and return false; drops not yet reported are carried
 * into the next report. The engine loop (plan tasks 6/12) calls it once per
 * iteration, after draining the queue, with pal_uptime_ms(). */
bool work_queue_report_drops(WorkQueue *q, uint64_t now_ms);

#endif
