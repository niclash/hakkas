/*
 * The engine-wide runtime context (01 §2.2, §3.2): everything the generic
 * block runtime needs beyond a single graph.
 *  - the global work queue (graphs/work_queue.h) and the drain loop that
 *    dispatches its entries;
 *  - the block slot table that resolves a BlockHandle to a live block;
 *  - the engine-wide default change threshold (01 §3.3);
 *  - the current engine time handed to compute (01 §3.4);
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
 * Threading: engine-thread only (01 §3.2), no locking.
 */
#ifndef HAKKAS_GRAPHS_RUNTIME_H
#define HAKKAS_GRAPHS_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphs/work_queue.h"

/* The engine-wide change threshold until configured (05 §1.1). */
#define RUNTIME_DEFAULT_CHANGE_THRESHOLD 0.05

typedef struct Block Block;
typedef struct Graph Graph;
typedef struct GraphEvent GraphEvent; /* graphs/graph.h */

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
	graph_event_fn on_event;
	void *event_ctx;
} Runtime;

/* Make rt an empty runtime: empty queue, no blocks, the default threshold,
 * time 0, no observer. */
void runtime_init(Runtime *rt);

/* Release the queue's entries and the slot table. Every graph created on rt
 * must have been destroyed (graph_destroy) first. */
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
 * stale is discarded; otherwise the block's update runs (03 §2). Timer
 * expiries and IO events are discarded: no producer exists yet; plan task 8
 * dispatches timer expiries and plan task 22 IO events. */
size_t runtime_drain(Runtime *rt, size_t max_entries);

#endif
