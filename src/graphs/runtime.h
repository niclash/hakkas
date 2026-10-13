/*
 * The engine-wide runtime context (01 §2.2, §3.2): everything the generic
 * block runtime needs beyond a single graph.
 *  - the global work queue (graphs/work_queue.h) and the drain loop that
 *    dispatches its entries;
 *  - the block slot table that resolves a BlockHandle to a live block;
 *  - the engine-wide default change threshold (01 §3.3);
 *  - the current engine time handed to compute (01 §3.4);
 *  - the timer service: one deadline per block of a timer type, in a
 *    deadline heap (graphs/deadlines.h), fired by the engine loop
 *    (runtime_fire_timers) into timer-expiry queue entries (01 §3.2 item 7);
 *  - the observer that hears every applied structural change of every graph
 *    (01 §4), set by the Graphs registry (plan task 7).
 *
 * Naming: functions are prefixed runtime_. The graph model built on top of it
 * (Graph, Block, the structural operations) is graphs/graph.h.
 *
 * Memory (decided, plan task 6): the work queue is a fixed array inside
 * Runtime (01 §3.2 item 9). Blocks, graphs, names, port value arrays, link
 * lists and state blobs are heap-allocated, sized exactly when a block is
 * created or a link is added; nothing is allocated while values propagate
 * (strings excepted, which Linkable already reference-counts on the heap). The
 * slot table is one heap array that grows by doubling and never shrinks;
 * freed slots are reused. An allocation failure refuses the operation that
 * needed it, with no change (GRAPH_ERR_NO_MEMORY).
 *
 * Time: the runtime never reads a clock. The engine loop (plan task 12) calls
 * runtime_set_now with pal_uptime_ms() before draining; tests set it directly.
 *
 * Wall clock (decided, plan task 24; 01 §3.4): the runtime keeps the wall
 * clock as an offset from engine time, so it advances with runtime_set_now
 * (and runtime_fire_timers) and the tests drive both with one number. The
 * engine loop sets it from the PAL (pal_wall_ms, the interim source until the
 * environment interface, plan task 22) each iteration, before firing the
 * timers. Calendar primitives arm their timers in engine time for an instant
 * computed on the wall clock, so a step of the wall clock (set by the user, an
 * NTP correction, the first synchronization) would leave them armed for the
 * wrong instant: when the offset has moved by RUNTIME_WALL_STEP_MS or more
 * since the last such check, every armed timer of a wall_clock type is made
 * due at once, and its compute re-evaluates and re-arms it. Drift (an NTP
 * slew) is caught the same way once it adds up to the threshold, so a
 * calendar boundary is never more than about RUNTIME_WALL_STEP_MS off.
 * Until it is set the wall clock is engine time (offset 0, 1970). The time
 * zone (05 §1.1 time_zone) is UTC until runtime_set_time_zone.
 *
 * Timers (decided, plan task 8, 01 §3.2 item 7): each block of a
 * TIMER_PERIODIC or TIMER_SCHEDULED type has one timer, a Deadline in the
 * runtime's heap. No PAL timer is involved: each loop iteration the engine
 * loop calls runtime_fire_timers with the current time, which queues a timer
 * expiry for every block whose deadline has passed, and it waits for sockets
 * at most runtime_timer_timeout_ms. Arming or cancelling a block's timer
 * increments its timer generation, so a queued expiry of a superseded or
 * cancelled timer is discarded when drained. A periodic timer continues
 * drift-free when it fires: the next deadline is the previous one plus the
 * period, and if the loop fell behind by a period or more, the missed ticks
 * are skipped (the next deadline is the first one of the same grid after
 * now). A scheduled timer is one-shot; the compute it causes re-arms it.
 *
 * Threading: engine-thread only (01 §3.2), no locking.
 */
#ifndef HAKKAS_GRAPHS_RUNTIME_H
#define HAKKAS_GRAPHS_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphs/deadlines.h"
#include "graphs/work_queue.h"
#include "system/tz.h"

/* The engine-wide change threshold until configured (05 §1.1). */
#define RUNTIME_DEFAULT_CHANGE_THRESHOLD 0.05

/* A move of the wall clock against engine time by this much (ms, either
 * way) re-evaluates the calendar primitives (see "Wall clock" above). */
#define RUNTIME_WALL_STEP_MS 1000

typedef struct Block Block;
typedef struct Graph Graph;
typedef struct GraphEvent GraphEvent; /* graphs/graph.h */
typedef struct ProcessLink ProcessLink; /* graphs/graph.h */

/* Receives every applied structural change (graphs/graph.h, GraphEvent). */
typedef void (*graph_event_fn)(void *ctx, const GraphEvent *ev);

/* One entry of the block table. gen is incremented whenever the block in the
 * slot is destroyed, so a BlockHandle taken before is stale (01 §3.2). */
typedef struct {
	Block *block; /* NULL when the slot is free */
	uint32_t gen;
	uint32_t next_free; /* free list link, RUNTIME_NO_SLOT at the end */
} BlockSlot;

#define RUNTIME_NO_SLOT UINT32_MAX

typedef struct Runtime {
	WorkQueue queue;
	BlockSlot *slots;   /* heap, slot_cap entries */
	uint32_t slot_used; /* slots handed out at least once */
	uint32_t slot_cap;
	uint32_t free_head; /* first free slot below slot_used, or RUNTIME_NO_SLOT */
	double change_threshold; /* engine-wide default (01 §3.3) */
	uint64_t now_ms;	 /* engine uptime handed to compute (01 §3.4) */
	int64_t wall_offset_ms;	 /* the wall clock minus now_ms */
	int64_t wall_checked_ms; /* wall_offset_ms at the last step check */
	TimeZone tz;		 /* local time for the calendar primitives */
	DeadlineHeap timers;	 /* the armed block timers */
	uint32_t timer_blocks;	 /* blocks of a timer type: the room reserved in timers */
	graph_event_fn on_event;
	void *event_ctx;
	/* The links between processes (graphs/graph.h), in creation order;
	 * heap, link_cap entries. */
	ProcessLink *links;
	uint32_t link_count;
	uint32_t link_cap;
} Runtime;

/* Make rt an empty runtime: empty queue, no blocks, no timers, the default
 * threshold, time 0, no observer. rt must not move afterwards (the queue's drop
 * hook refers to it). */
void runtime_init(Runtime *rt);

/* Release the queue's entries, the timer heap, the slot table and the link
 * array. Every graph created on rt must have been destroyed (graph_destroy)
 * first, which removed their links. */
void runtime_deinit(Runtime *rt);

/* Set the engine-wide change threshold (05 §1.1 change_threshold). Refused
 * (false, no change) for NaN or a negative value; +Infinity is allowed. */
bool runtime_set_change_threshold(Runtime *rt, double threshold);

static inline double runtime_change_threshold(const Runtime *rt)
{
	return rt->change_threshold;
}

/* Set the engine time (ms of uptime, 01 §3.4) that compute sees as now_ms. */
static inline void runtime_set_now(Runtime *rt, uint64_t now_ms)
{
	rt->now_ms = now_ms;
}

/* The wall clock at the engine time: UTC ms since the Unix epoch. */
static inline int64_t runtime_wall_ms(const Runtime *rt)
{
	return (int64_t)rt->now_ms + rt->wall_offset_ms;
}

/* Set the wall clock at the current engine time (runtime_set_now first) to
 * wall_ms, UTC ms since the Unix epoch. If that moves it against engine time
 * by RUNTIME_WALL_STEP_MS or more since the last step, every armed timer of
 * a wall_clock type is made due now (see "Wall clock" above); returns true
 * then. */
bool runtime_set_wall_ms(Runtime *rt, int64_t wall_ms);

/* Set the time zone (05 §1.1 time_zone) for the calendar primitives' local
 * time; it applies from their next compute. */
static inline void runtime_set_time_zone(Runtime *rt, const TimeZone *tz)
{
	rt->tz = *tz;
}

/* Set the one observer of structural changes (NULL to remove it). The Graphs
 * registry (plan task 7) fans each event out to its subscribers. */
void runtime_set_observer(Runtime *rt, graph_event_fn fn, void *ctx);

/* The live block h refers to, or NULL if h is stale or out of range. */
Block *runtime_block(const Runtime *rt, BlockHandle h);

/* The drain loop (01 §3.2 item 2): pop and dispatch queue entries, oldest
 * first, until the queue is empty or max_entries entries were handled,
 * whichever comes first. Entries pushed while draining are handled in the
 * same call. Returns the number handled, stale ones included.
 *
 * The budget (decided, plan task 6) keeps the engine loop responsive: a
 * feedback cycle in a graph enqueues forever (01 §3.2 item 4), and the loop
 * must still service sockets and timers between drains (01 §3.2.1). Pass
 * SIZE_MAX to drain until empty.
 *
 * Dispatch: an input update whose block handle or input source generation is
 * stale is discarded; otherwise the block's update runs (03 §2). A timer
 * expiry whose block handle or timer generation is stale is discarded;
 * otherwise the block's compute step runs with REASON_TIMER (03 §2). IO
 * events are discarded: no producer exists yet (plan task 22). */
size_t runtime_drain(Runtime *rt, size_t max_entries);

/* runtime_timer_timeout_ms when no timer is armed. */
#define RUNTIME_NO_TIMEOUT UINT64_MAX

/* Set the engine time to now_ms (as runtime_set_now), then queue a timer
 * expiry for every block whose timer is due (deadline <= now_ms), earliest
 * deadline first, equal deadlines in arming order. A block gets at most one
 * expiry per call. A periodic timer is re-armed at once, drift-free, skipping
 * missed ticks (see "Timers" above); a scheduled one is disarmed until its
 * next compute re-arms it. Returns the number of expiries queued; the caller
 * drains them (runtime_drain). */
size_t runtime_fire_timers(Runtime *rt, uint64_t now_ms);

/* The time from now_ms until the earliest armed timer is due: 0 if one is
 * due already, RUNTIME_NO_TIMEOUT if none is armed. The engine loop's upper
 * bound for waiting on sockets (01 §3.2.1). */
uint64_t runtime_timer_timeout_ms(const Runtime *rt, uint64_t now_ms);

/* The earliest armed timer's deadline in *due_ms; false if none is armed. */
bool runtime_next_deadline(const Runtime *rt, uint64_t *due_ms);

#endif
