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

/* The output whose links carry output out of b (block_output_source: a
 * composite instance's mapped inner output). */
Output *block_links_output(Block *b, uint32_t out);

/* Make room for one more link in o. False if memory runs out. */
bool output_links_reserve(Output *o);

/* Remove the link to dst's input from o (nothing if there is none). */
void output_unlink(Output *o, const Block *dst, uint32_t input);

/* Increment input i's source generation (and the mapped inner input's of a
 * composite instance), so queued values from the old source go stale. */
void input_source_changed(Block *b, uint32_t i);

/* Make input i of b unsourced (input_source_changed included). */
void input_clear_source(Block *b, uint32_t i);

/* Severing (01 §2.2): input i reverts to its port default at once, which
 * propagates like any other input change. */
void input_revert(Block *b, uint32_t i);

/* ---- graph_ports.c: graph-level ports ---------------------------------- */

/* Remove every port mapped to b, reporting each (inports, then outports, in
 * creation order). Part of graph_remove_block. */
void graph_ports_remove_block(Graph *g, const Block *b);

/* Free every port of g, reporting nothing. Part of graph_destroy. */
void graph_ports_free(Graph *g);

/* ---- graph_links.c: links between processes ---------------------------- */

/* Remove every link of g's port name of direction dir, as
 * graph_link_remove (an event each, then the input reverts). Part of
 * removing and renaming a port. */
void graph_links_remove_port(Graph *g, enum graph_port_dir dir, const char *name);

/* Drop every link from or to g, reporting nothing and reverting nothing (the
 * other graph's inputs keep their values). Part of graph_destroy. */
void graph_links_drop_graph(Graph *g);

/* ---- graph_bindings.c: environment bindings (plan tasks 22, 70) -------- */

/* Drop every binding of g, reporting nothing and reverting nothing. Part of
 * graph_destroy. */
void graph_bindings_drop_graph(Graph *g);

/* Remove the bindings of g's process port name of direction dir, in creation
 * order, reporting BINDING_REMOVED for each; if revert, a sensor-bound input
 * then reverts to its default (not when the port goes with its block). Part
 * of removing a graph port, before its event. */
void graph_bindings_remove_port(Graph *g, enum graph_port_dir dir, const char *name,
				bool revert);

/* The bindings of g's process port from (direction dir) follow its rename to
 * to, without an event (04 §4). False (nothing changed): out of memory. Part
 * of renaming a graph port, before its event. */
bool graph_bindings_rename_port(Graph *g, enum graph_port_dir dir, const char *from,
				const char *to);

/* Write value, a push of output o, to every actuator channel bound to o
 * (the runtime's env_write hook). */
void runtime_env_write(Runtime *rt, const Output *o, const Linkable *value);

/* ---- graph_retained.c: retained entries of placeholders (05 §2.2) ------- */

/* Drop every retained connection and port that refers to b, reporting
 * nothing. Part of graph_remove_block. */
void graph_retained_remove_block(Graph *g, const Block *b);

/* A live source attached to input port of b, or a live graph port called
 * name, takes the place of a retained entry (graphs/graph.h): drop it,
 * reporting nothing. No-op if there is none. */
void graph_retained_remove_input(Graph *g, const Block *b, const char *port);
void graph_retained_remove_port(Graph *g, enum graph_port_dir dir, const char *name);

/* Free every retained entry of g. Part of graph_destroy. */
void graph_retained_free(Graph *g);

/* ---- block.c: the generic block runtime (03 §2) ------------------------ */

/* update(input, value): store value (taking ownership) in input i, fire the
 * input's watches, then run the compute step if b is started and its type
 * computes on input. The one path every input change takes: queued input
 * updates from the drain loop and the direct deliveries of graph.c. For a
 * composite instance, the mapped inner input is updated (03 §5). */
void block_update(Block *b, uint32_t input, Linkable value);

/* start(): mark started, arm a TIMER_PERIODIC type's timer, compute with
 * REASON_START (whose return value arms a TIMER_SCHEDULED type's timer), push
 * every output unconditionally. A composite instance starts its inner blocks
 * instead (stop likewise). No-op if started. */
void block_start(Block *b);

/* stop(): cancel the timer, compute with REASON_STOP without propagating,
 * mark stopped. No-op if stopped. */
void block_stop(Block *b);

/* A new reference to input i's port default (01 §2.1: the descriptor's
 * default, or default_value(type) if it has none). */
Linkable block_input_default(const Block *b, uint32_t input);

/* Arm b's timer for due_ms, replacing any armed one (01 §3.2 item 7). */
void block_timer_arm(Block *b, uint64_t due_ms);

/* Cancel b's timer, armed or not: a queued expiry goes stale. */
void block_timer_cancel(Block *b);

/* A live timer expiry for b, from the drain loop: the compute step with
 * REASON_TIMER (03 §2). */
void block_timer_expired(Block *b);

/* Detach every watch of every input, calling on_detach (a composite
 * instance's inner blocks' first). */
void block_detach_watches(Block *b);

#endif
