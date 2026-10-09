/*
 * The graph model and the one generic block runtime (01 §2.2–§3.3, 03 §2).
 *
 * A Graph owns its blocks, keyed by instance name. A Block is an instance of a
 * BlockTypeDef (graphs/block_type.h): its input and output values, each
 * input's single source, each output's fan-out links and last propagated
 * value, its metadata, its state blob and its error flag. Every block type
 * runs through the same code here; only the type's compute function differs
 * (01 §2.3).
 *
 * Naming: graph_ for structural operations on a graph, block_ for one block.
 *
 * Structural operations (02 §3.4, 01 §2.2) are validated first and refused as
 * a whole on any violation, leaving the graph exactly as it was (02 §3.4.1);
 * the enum graph_status values map 1:1 to the refusal cases of 02 §3.4.1 that
 * concern a graph, plus two that never reach the wire as such (invalid
 * argument, out of memory). Checks run in this order, the first failure wins:
 * every node id argument is a valid id (GRAPH_ERR_INVALID_ID), then each
 * endpoint in argument order (source before target) is resolved, node first
 * (GRAPH_ERR_UNKNOWN_NODE), then port, then the operation's own conditions.
 * The caller (plan task 7) can therefore tell which node or port a refusal is
 * about with graph_find_block / block_*_index.
 *
 * Applied changes are reported, in applied order, to the runtime's observer
 * (graphs/runtime.h) as GraphEvents, each emitted once its structural change
 * is complete and before any resulting value delivery. A compound operation
 * reports each part: removing a block reports every removed edge and initial
 * first, then the block (02 §1.1). Starting and stopping a graph report
 * nothing (the Graphs registry, task 7, emits network:started/stopped).
 *
 * Value delivery (decided, plan task 6, 01 §2.2 / 03 §2): a value from an
 * upstream output always travels through the work queue (output_set). A value
 * delivered by a structural operation on the target input itself — an initial
 * added, a severed input reverting to its port default, a new edge delivering
 * the output's last_propagated value — is delivered at once by the same
 * update code the drain loop uses (store, live-value watches, compute step if
 * started and compute_on_input), so the input never shows a stale value after
 * the operation returns; its downstream effects are queued as usual. Callers
 * drain the queue (runtime_drain) to see them.
 *
 * Reentrancy: the observer, InputWatch callbacks and compute functions must
 * not call any operation here that changes a graph or a watch.
 *
 * Not yet here: the Graphs registry, graph ids/names/ports/uptime (task 7),
 * timers (task 8), opaque node/edge metadata and placeholder blocks for
 * unknown types (task 10, 05 §2.2), composite instances (task 21), IO
 * (task 22). A placeholder will be a block whose type is NULL: the runtime
 * already treats a NULL type as a block with no ports and no compute.
 */
#ifndef HAKKAS_GRAPHS_GRAPH_H
#define HAKKAS_GRAPHS_GRAPH_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphs/block_type.h"
#include "graphs/linkable.h"
#include "graphs/runtime.h"
#include "graphs/work_queue.h"

/* Longest valid graph or node id, in bytes (02 §3.4.1 "Invalid id"). */
#define GRAPH_ID_MAX 64

/* Canvas position of a block added without one (02 §3.4 addnode). */
#define BLOCK_DEFAULT_POSITION 50.0

enum graph_status {
	GRAPH_OK = 0,
	GRAPH_ERR_UNKNOWN_NODE = -1,	       /* "Unknown node: <node>" */
	GRAPH_ERR_UNKNOWN_PORT = -2,	       /* "Unknown port: <node>.<port>" */
	GRAPH_ERR_NODE_EXISTS = -3,	       /* "Node exists: <id>" */
	GRAPH_ERR_INVALID_EDGE_DIRECTION = -4, /* "Invalid edge direction" */
	GRAPH_ERR_INPUT_CONNECTED = -5,	       /* "Input already connected: <node>.<port>" */
	GRAPH_ERR_NO_SUCH_CONNECTION = -6,     /* "No such connection" */
	GRAPH_ERR_INVALID_ID = -7,	       /* "Invalid id" */
	/* Not wire refusal cases: a NULL type or value, or a change threshold
	 * that is NaN or negative (a protocol-level validation error), and an
	 * allocation failure. */
	GRAPH_ERR_INVALID_ARGUMENT = -8,
	GRAPH_ERR_NO_MEMORY = -9,
};

/* ---- Inputs, outputs, links ------------------------------------------ */

/* An input's single source (01 §2.2, no fan-in). */
typedef enum input_source {
	INPUT_SOURCE_NONE = 0,
	INPUT_SOURCE_EDGE,
	INPUT_SOURCE_INITIAL,
} InputSource;

/* A live-value watch on one input (03 §2 update, 02 §4.2): the protocol layer
 * (plan task 18) embeds one per subscription. on_value is called with the
 * input's value right when the watch is attached and after every update of
 * the input; on_detach, if not NULL, when the block is removed or its graph
 * destroyed, after which the runtime no longer refers to the watch. The
 * callbacks only read; see "Reentrancy" above. */
typedef struct InputWatch InputWatch;
struct InputWatch {
	void (*on_value)(InputWatch *w, const Block *block, uint32_t input, const Linkable *value);
	void (*on_detach)(InputWatch *w);
	InputWatch *next; /* owned by the runtime while attached */
};

typedef struct {
	InputSource source;
	Block *src_block;    /* INPUT_SOURCE_EDGE: the upstream block */
	uint32_t src_output; /* INPUT_SOURCE_EDGE: its output index */
	Linkable initial;    /* INPUT_SOURCE_INITIAL: the literal (owned); unset otherwise */
	/* Incremented on every source change (edge or initial added, replaced or
	 * removed, upstream block removed); queued updates sent under an older
	 * value are discarded (01 §3.2). Edge metadata (02 §3.4 changeedge),
	 * stored opaquely, will live here too, since an input has at most one
	 * edge (plan task 10). */
	uint32_t source_gen;
	InputWatch *watches;
} Input;

/* One fan-out link of an output (01 §2.2): the destination input. */
typedef struct {
	Block *block;
	uint32_t input;
} Link;

typedef struct {
	/* The value most recently pushed downstream (01 §3.3). Before the
	 * block's first start it is the output's port default. */
	Linkable last_propagated;
	Link *links; /* in connection order */
	uint32_t link_count;
	uint32_t link_cap;
} Output;

/* Node metadata (05 §2.1). change_threshold is the instance override of
 * 01 §3.3, NAN when absent. Other, opaque metadata keys are kept from plan
 * task 10 on (with the JSON library). */
typedef struct {
	double x, y;
	double change_threshold;
} BlockMetadata;

static inline BlockMetadata block_metadata_default(void)
{
	return (BlockMetadata){BLOCK_DEFAULT_POSITION, BLOCK_DEFAULT_POSITION, (double)NAN};
}

struct Block {
	Graph *graph;
	BlockHandle handle;
	char *name; /* the instance name, owned, unique in the graph */
	const BlockTypeDef *type; /* NULL only for a placeholder (task 10) */
	uint32_t input_count;
	uint32_t output_count;
	Linkable *in_values;  /* input_count current input values (BlockCtx.inputs) */
	Input *inputs;	      /* input_count */
	Linkable *out_values; /* output_count current output values (BlockCtx.outputs) */
	Output *outputs;      /* output_count */
	void *state;	      /* type->state_size bytes, zeroed at creation; NULL when 0 */
	BlockMetadata metadata;
	bool started; /* mirrors the graph's started state (03 §2) */
	bool error;   /* the error flag (01 §2.1) */
	uint32_t timer_gen; /* bumped on stop and destroy (01 §3.2); timers: task 8 */
};

struct Graph {
	Runtime *rt;
	Block **blocks; /* in creation order */
	uint32_t block_count;
	uint32_t block_cap;
	bool started;
};

/* ---- Events reported to the runtime's observer ------------------------ */

enum graph_event_type {
	GRAPH_EVENT_BLOCK_ADDED,
	GRAPH_EVENT_BLOCK_REMOVED,
	GRAPH_EVENT_BLOCK_RENAMED,
	GRAPH_EVENT_BLOCK_CHANGED, /* metadata changed; block->metadata is the result */
	GRAPH_EVENT_EDGE_ADDED,
	GRAPH_EVENT_EDGE_REMOVED,
	GRAPH_EVENT_INITIAL_ADDED,
	GRAPH_EVENT_INITIAL_REMOVED,
};

/* Pointers are valid only during the callback. */
struct GraphEvent {
	enum graph_event_type type;
	Graph *graph;
	const Block *block;	/* the block; for an edge or initial, the target block */
	uint32_t input;		/* edge, initial: the target input */
	const Block *src_block; /* edge: the source block */
	uint32_t src_output;	/* edge: its output */
	const Linkable *value;	/* initial: the literal added or removed */
	const char *old_name;	/* BLOCK_RENAMED: the previous name */
};

/* ---- Graph ------------------------------------------------------------ */

/* A new, empty, stopped graph on rt, or NULL if memory runs out. */
Graph *graph_create(Runtime *rt);

/* Stop g (as graph_stop), then free it and every block in it. Watches get
 * on_detach; no events are reported; queued entries for its blocks become
 * stale. */
void graph_destroy(Graph *g);

/* Whether id is a valid graph or node id: non-NULL, 1..GRAPH_ID_MAX bytes. */
bool graph_id_valid(const char *id);

/* The block named name (len bytes, need not be NUL-terminated), or NULL. */
Block *graph_find_block_n(const Graph *g, const char *name, size_t len);
Block *graph_find_block(const Graph *g, const char *name);

/* Create a block of type named name (02 §3.4 addnode), with metadata md
 * (NULL: block_metadata_default()); it is started at once if g is started.
 * Refused: INVALID_ID, NODE_EXISTS, INVALID_ARGUMENT (NULL type, or a
 * metadata change_threshold that is negative), NO_MEMORY. On success *out (if
 * not NULL) is the block. Reports BLOCK_ADDED. */
enum graph_status graph_create_block(Graph *g, const char *name, const BlockTypeDef *type,
				     const BlockMetadata *md, Block **out);

/* Remove a block (02 §3.4 removenode; 03 §2 destroy): stop it if started,
 * then sever every inbound edge and initial and every outbound link; each
 * downstream input reverts to its port default, which propagates. Queued
 * entries addressed to the block become stale. Reports EDGE_REMOVED /
 * INITIAL_REMOVED for each (inputs in port order, then outputs in port and
 * connection order), then BLOCK_REMOVED. Refused: INVALID_ID, UNKNOWN_NODE. */
enum graph_status graph_remove_block(Graph *g, const char *name);

/* Rename a block; links are unaffected (02 §3.4 renamenode). Refused:
 * INVALID_ID, UNKNOWN_NODE, NODE_EXISTS (to names a block, from itself
 * included), NO_MEMORY. Reports BLOCK_RENAMED. */
enum graph_status graph_rename_block(Graph *g, const char *from, const char *to);

/* A metadata change (02 §3.4 changenode): the fields to merge. */
enum threshold_change {
	THRESHOLD_KEEP = 0,
	THRESHOLD_SET,	 /* set the instance override to change_threshold */
	THRESHOLD_CLEAR, /* remove the instance override ("change_threshold": null) */
};

typedef struct {
	bool set_x, set_y;
	double x, y;
	enum threshold_change threshold;
	double change_threshold;
} BlockChange;

/* Merge chg into the block's metadata. Refused: INVALID_ID, UNKNOWN_NODE,
 * INVALID_ARGUMENT (THRESHOLD_SET with NaN or a negative value). Reports
 * BLOCK_CHANGED. A new threshold applies from the next output_set on. */
enum graph_status graph_change_block(Graph *g, const char *name, const BlockChange *chg);

/* Connect src.src_port (an output) to tgt.tgt_port (an input) (02 §3.4
 * addedge). If g is started, the input then receives the output's
 * last_propagated value at once (03 §2 connect); in a stopped graph it keeps
 * its value. Refused: INVALID_ID, UNKNOWN_NODE, UNKNOWN_PORT,
 * INVALID_EDGE_DIRECTION (src_port is an input of src, or tgt_port an output
 * of tgt), INPUT_CONNECTED (the input has an edge or an initial), NO_MEMORY.
 * Reports EDGE_ADDED. */
enum graph_status graph_connect(Graph *g, const char *src, const char *src_port, const char *tgt,
				const char *tgt_port);

/* Remove the edge src.src_port → tgt.tgt_port (02 §3.4 removeedge); the input
 * reverts to its port default, which propagates, and queued values from the
 * edge are dropped. Refused: INVALID_ID, UNKNOWN_NODE, UNKNOWN_PORT (a name
 * that is no port of the node), NO_SUCH_CONNECTION (no such edge, including a
 * port of the wrong direction). Reports EDGE_REMOVED. */
enum graph_status graph_disconnect(Graph *g, const char *src, const char *src_port,
				   const char *tgt, const char *tgt_port);

/* Wire the literal value to node.port (02 §3.4 addinitial), replacing an
 * initial already there. The value is delivered at once and propagates if g is
 * started; a stopped graph only stores it. Refused: INVALID_ID, UNKNOWN_NODE,
 * UNKNOWN_PORT, INVALID_EDGE_DIRECTION (port is an output), INPUT_CONNECTED
 * (the input has an edge), INVALID_ARGUMENT (value NULL). The graph takes its
 * own reference to value. Reports INITIAL_ADDED. */
enum graph_status graph_add_initial(Graph *g, const char *node, const char *port,
				    const Linkable *value);

/* Remove the initial on node.port (02 §3.4 removeinitial); the input reverts
 * to its port default, which propagates. Refused: INVALID_ID, UNKNOWN_NODE,
 * UNKNOWN_PORT, NO_SUCH_CONNECTION (no initial there, including an output
 * port). Reports INITIAL_REMOVED with the removed literal. */
enum graph_status graph_remove_initial(Graph *g, const char *node, const char *port);

/* Start g (01 §2.2): start every block in creation order — compute with
 * REASON_START and push every output downstream unconditionally. Nothing
 * happens if g is already started. */
void graph_start(Graph *g);

/* Stop g: for each block, cancel its timer and compute with REASON_STOP
 * without propagating (03 §2). Inputs keep accepting values; queued entries
 * stay queued and, when drained, only store their values. Nothing happens if
 * g is already stopped. */
void graph_stop(Graph *g);

/* Split a "block.port" reference on its last '.' (01 §2.4): the block name is
 * ref[0 .. *block_len) and *port the rest. False if there is no '.', or
 * either part would be empty. */
bool port_ref_split(const char *ref, size_t *block_len, const char **port);

/* ---- Block -------------------------------------------------------------- */

/* The index of the input / output called name, or -1. */
int32_t block_input_index(const Block *b, const char *name);
int32_t block_output_index(const Block *b, const char *name);

/* The port descriptors (b->type must not be NULL). */
static inline const PortDescriptor *block_input_port(const Block *b, uint32_t i)
{
	return &b->type->inputs[i];
}

static inline const PortDescriptor *block_output_port(const Block *b, uint32_t i)
{
	return &b->type->outputs[i];
}

/* The change threshold in effect for b (01 §3.3): the instance override, else
 * the type default, else the engine-wide default. */
double block_change_threshold(const Block *b);

/* Whether next differs from last enough to propagate under threshold
 * (01 §3.3): a different value type always (i64 vs f64 and number vs Metric
 * included); numbers when they are not equal and |next − last| >= threshold,
 * NaN ↔ non-NaN always, NaN → NaN never; a Metric also on any unit or
 * category change; bool and string on any difference; unset → unset never. */
bool block_value_changed(const Linkable *last, const Linkable *next, double threshold);

/* output_set (01 §3.2 item 1): store value (taking ownership) in output i and,
 * if b is started and it changed by the threshold since last_propagated, set
 * last_propagated and queue an input update for every link. A stopped block
 * only stores. */
void block_output_set(Block *b, uint32_t output, Linkable value);

/* Attach / detach a live-value watch on input i (see InputWatch).
 * block_watch_input calls w->on_value once with the current value. */
void block_watch_input(Block *b, uint32_t input, InputWatch *w);
void block_unwatch_input(Block *b, uint32_t input, InputWatch *w);

#endif
