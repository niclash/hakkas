/*
 * Internal to graphs/: what the runtime (runtime.c), the structural
 * operations (graph.c, graph_ports.c) and the generic block runtime (block.c)
 * share.
 */
#ifndef HAKKAS_GRAPHS_GRAPH_INTERNAL_H
#define HAKKAS_GRAPHS_GRAPH_INTERNAL_H

#include "graphs/graph.h"

/* ---- runtime.c: the block slot table ---------------------------------- */

/* Make sure runtime_slot_alloc will find a slot (may grow the table). False if
 * memory runs out. */
bool runtime_slot_reserve(Runtime *rt);

/* Put b into a free slot and return its handle. Call runtime_slot_reserve
 * first; this never fails. */
BlockHandle runtime_slot_alloc(Runtime *rt, Block *b);

/* Free h's slot, making every handle to it stale. */
void runtime_slot_free(Runtime *rt, BlockHandle h);

/* ---- graph.c ------------------------------------------------------------ */

/* Report ev (with ev.graph set to g) to the runtime's observer. */
void graph_emit(Graph *g, GraphEvent ev);

/* A heap copy of s, or NULL if memory runs out. */
char *graph_str_dup(const char *s);

/* ---- graph_ports.c: graph-level ports ---------------------------------- */

/* Remove every port mapped to b, reporting each (inports, then outports, in
 * creation order). Part of graph_remove_block. */
void graph_ports_remove_block(Graph *g, const Block *b);

/* Free every port of g, reporting nothing. Part of graph_destroy. */
void graph_ports_free(Graph *g);

/* ---- block.c: the generic block runtime (03 §2) ------------------------ */

/* update(input, value): store value (taking ownership) in input i, fire the
 * input's watches, then run the compute step if b is started and its type
 * computes on input. The one path every input change takes: queued input
 * updates from the drain loop and the direct deliveries of graph.c. */
void block_update(Block *b, uint32_t input, Linkable value);

/* start(): mark started, compute with REASON_START, push every output
 * unconditionally. No-op if started. */
void block_start(Block *b);

/* stop(): cancel the timer, compute with REASON_STOP without propagating,
 * mark stopped. No-op if stopped. */
void block_stop(Block *b);

/* A new reference to input i's port default (01 §2.1: the descriptor's
 * default, or default_value(type) if it has none). */
Linkable block_input_default(const Block *b, uint32_t input);

/* Detach every watch of every input, calling on_detach. */
void block_detach_watches(Block *b);

#endif
