/*
 * The deadline set (01 §3.2 item 7, decided in plan task 8): every timer of
 * the engine lives in the portable core as a deadline, an absolute engine
 * time (ms of uptime, 01 §3.4), kept in a binary min-heap ordered by due time.
 * There is no per-timer PAL or OS timer and no callback from another
 * execution context: the engine loop asks for the earliest deadline, uses the
 * time until it as its poll timeout (01 §3.2.1, plan task 12), and pops the
 * deadlines that are due. The runtime keeps one heap for block timers
 * (graphs/runtime.h); another subsystem with timeouts (e.g. session expiry,
 * 02 §2.5) can keep its own heap and the loop waits for the earlier of the two.
 *
 * Intrusive: a Deadline is embedded in its owner's record (a Block), and the
 * heap holds pointers to them, so the owner finds itself again from the
 * popped Deadline (offsetof). A Deadline knows its heap position, so removing
 * or moving one is O(log n) without a search.
 *
 * Order: by due time; deadlines with the same due time come out in the order
 * they were set (each set takes a sequence number), so firing is
 * deterministic.
 *
 * Memory: the heap array grows only in deadline_heap_reserve; setting,
 * removing and popping never allocate. The owner reserves room for every
 * deadline it may arm when it creates the record (a block of a timer type),
 * so arming while values propagate cannot fail (01 §2.2 "Memory").
 *
 * Naming: functions are prefixed deadline_heap_ / deadline_.
 *
 * Threading: engine-thread only (01 §3.2), no locking.
 */
#ifndef HAKKAS_GRAPHS_DEADLINES_H
#define HAKKAS_GRAPHS_DEADLINES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Deadline.index of a deadline that is in no heap. */
#define DEADLINE_UNSET UINT32_MAX

typedef struct {
	uint64_t due_ms; /* when it is due; kept after it is popped or removed */
	uint64_t seq;	 /* set order, the tie-break between equal due times */
	uint32_t index;	 /* position in the heap, DEADLINE_UNSET when not armed */
} Deadline;

typedef struct {
	Deadline **items; /* heap, cap entries */
	uint32_t len;
	uint32_t cap;
	uint64_t next_seq;
} DeadlineHeap;

/* A Deadline that is in no heap (for initialization). */
static inline Deadline deadline_none(void)
{
	return (Deadline){0, 0, DEADLINE_UNSET};
}

static inline bool deadline_armed(const Deadline *d)
{
	return d->index != DEADLINE_UNSET;
}

/* Make h an empty heap with no room. */
void deadline_heap_init(DeadlineHeap *h);

/* Free the heap array. Deadlines still in it are not touched. */
void deadline_heap_deinit(DeadlineHeap *h);

/* Make room for at least n deadlines in total. False if memory runs out (h
 * unchanged). */
bool deadline_heap_reserve(DeadlineHeap *h, uint32_t n);

/* Arm d for due_ms: insert it, or move it if it is already in h (a re-arm).
 * False, with nothing changed, only if d is not in h and h is full (room
 * not reserved). */
bool deadline_heap_set(DeadlineHeap *h, Deadline *d, uint64_t due_ms);

/* Disarm d; nothing happens if it is not armed. d must be in h, if anywhere. */
void deadline_heap_remove(DeadlineHeap *h, Deadline *d);

/* The earliest deadline, or NULL if h is empty. */
static inline Deadline *deadline_heap_peek(const DeadlineHeap *h)
{
	return h->len > 0 ? h->items[0] : NULL;
}

/* Remove and return the earliest deadline if it is due at now_ms (due_ms <=
 * now_ms), else NULL. */
Deadline *deadline_heap_pop_due(DeadlineHeap *h, uint64_t now_ms);

static inline uint32_t deadline_heap_len(const DeadlineHeap *h)
{
	return h->len;
}

#endif
