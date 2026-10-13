/*
 * The graph model and the one generic block runtime (01 §2.2–§3.3, 03 §2).
 *
 * A Graph owns its blocks, keyed by instance name. A Block is an instance of a
 * BlockTypeDef (graphs/block_type.h): its input and output values, each
 * input's single source, each output's fan-out links and last propagated
 * value, its metadata and its state blob. Every block type runs through the
 * same code here; only the type's compute function differs (01 §2.3).
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
 * nothing (the Graphs registry, graphs/graphs.h, emits network:started/stopped).
 *
 * Graph-level ports (inports/outports, 01 §2.2 Port) are metadata: each maps
 * a public name to one inner block's input (inport) or output (outport) and
 * has no dataflow effect of its own; a link between processes (ProcessLink,
 * plan task 21f) is made through them. A port refers to its block, not to the block's
 * name, so renaming the block carries the port along; removing the block
 * removes its ports, reported after its edges and initials and before the
 * block (decided, plan task 7, 02 §3.4).
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
 * Graph identity (id, name, description, icon) and lifecycle times are
 * fields here but are managed by the Graphs registry (graphs/graphs.h).
 *
 * Timers (plan task 8): a block of a timer type has one timer, armed and
 * cancelled by its lifecycle and compute step (03 §2) and fired by the
 * runtime (graphs/runtime.h "Timers").
 *
 * Opaque metadata (decided, plan task 10, 01 §2.2): node metadata keys other
 * than x and y (BlockMetadata.extra) and an edge's metadata
 * (Input.edge_metadata, on the destination input, which has at most one edge)
 * are kept as compact JSON object text, never interpreted here. Converting
 * them from and to JSON, and merging a changenode patch, is
 * graphs/graph_json.h.
 *
 * Placeholder blocks (05 §2.2): a block loaded from a graph file whose type
 * is not registered. Its type is NULL, so it has no ports and no compute and
 * takes part in no dataflow; it keeps its component name (block_component).
 * The connections and graph ports of the file that reference a placeholder
 * cannot be live links, so the graph keeps them as retained entries
 * (RetainedConnection, RetainedPort): they refer to their blocks (so they
 * follow a rename), are written back on save, appear in no event and in no
 * graph:connect snapshot, and are dropped, silently, with either of their
 * blocks. They keep the file free of fan-in and duplicate port names: a
 * retained connection into a real block's input is refused if the input
 * already has a source, and a later live edge or initial to that input
 * (graph_connect, graph_add_initial), or a live graph port taking a retained
 * port's name (graph_add_port, graph_rename_port), replaces the retained
 * entry, which is dropped silently.
 *
 * Composite instances (03 §5; decided, plan task 21): a block whose type is a
 * composite (type->subgraph) is one ordinary block to the graph: it is in
 * g->blocks, reported, renamed, changed, removed, saved and snapshotted as
 * any block, and its Input records hold the sources of its exposed inputs
 * (edge, initial, edge metadata) like any block's. On creation it builds its
 * inner blocks (Block.inner, in the definition's order, each with parent set
 * to the instance), wires the definition's edges and initials between them,
 * and does nothing else itself: it has no compute, no timer, and its own
 * value arrays are never read. Inner blocks are real blocks of the same
 * generic runtime (slots, timers, the queue) but are not in g->blocks: no
 * graph operation finds them by name, no event reports them, so their names
 * (the definition's) cannot clash with graph node names and never change.
 * The dataflow maps straight through, with no extra hop:
 *  - a value for an exposed input goes to the mapped inner input
 *    (block_input_target, followed through nested composites), so an edge
 *    into the instance is a link to the inner input, and an initial or a
 *    severed input's default (the inner port's default, which the exposed
 *    port descriptor carries) is delivered there;
 *  - a link from an exposed output is registered on the mapped inner output
 *    (block_output_source), keeping the outer block as its structural
 *    destination's source, so the inner block pushes to it through the queue
 *    like to any other link;
 *  - watches on an exposed input attach to the inner input;
 *  - start/stop apply to every inner block (in definition order), removal
 *    and graph destruction free them (watches detached, timers cancelled,
 *    queued entries stale).
 * The change threshold of an inner block (01 §3.3) is its own type's
 * default, else the engine-wide one (block_change_threshold), like any
 * block's: there is no per-instance override (decided, plan task 21c).
 *
 * Not yet here: IO (task 22).
 */
#ifndef HAKKAS_GRAPHS_GRAPH_H
#define HAKKAS_GRAPHS_GRAPH_H

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
	/* Not wire refusal cases: a NULL type or value (a protocol-level
	 * validation error), and an allocation failure. */
	GRAPH_ERR_INVALID_ARGUMENT = -8,
	GRAPH_ERR_NO_MEMORY = -9,
	/* Refusals of the Graphs registry (graphs/graphs.h), 02 §3.4.1. */
	GRAPH_ERR_UNKNOWN_GRAPH = -10,	   /* "Unknown graph: <id>" */
	GRAPH_ERR_UNKNOWN_COMPONENT = -11, /* "Unknown component: <name>" */
	GRAPH_ERR_PORT_EXISTS = -12,	   /* "Port exists: <name>" */
	GRAPH_ERR_GRAPH_EXISTS = -13,	   /* "Graph exists: <id>" */
	GRAPH_ERR_NAME_MISMATCH = -14,	   /* "Graph name mismatch: <id>" */
	/* Not a wire refusal case: the random source failed (no UUID). */
	GRAPH_ERR_UNAVAILABLE = -15,
	/* Refusals of the links between processes (plan task 21f; the C API
	 * only, 02 §6: no wire command yet). */
	GRAPH_ERR_INVALID_LINK_DIRECTION = -16, /* "Invalid link direction" */
	GRAPH_ERR_LINK_EXISTS = -17,		/* "Link exists" */
	GRAPH_ERR_NO_SUCH_LINK = -18,		/* "No such link" */
};

/* ---- Inputs, outputs, links ------------------------------------------ */

/* An input's single source (01 §2.2, no fan-in). */
typedef enum input_source {
	INPUT_SOURCE_NONE = 0,
	INPUT_SOURCE_EDGE,
	INPUT_SOURCE_INITIAL,
	/* A link from another process's outport, or from this graph's own
	 * (ProcessLink, "Links between processes" below). */
	INPUT_SOURCE_LINK,
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
	Block *src_block;    /* INPUT_SOURCE_EDGE, _LINK: the upstream block */
	uint32_t src_output; /* INPUT_SOURCE_EDGE, _LINK: its output index */
	Linkable initial;    /* INPUT_SOURCE_INITIAL: the literal (owned); unset otherwise */
	/* INPUT_SOURCE_EDGE: the edge's opaque metadata (02 §3.4 addedge,
	 * changeedge) as compact JSON object text, owned; NULL if it has none.
	 * Kept here since an input has at most one edge. */
	char *edge_metadata;
	/* Incremented on every source change (edge, initial or link added,
	 * replaced or removed, upstream block removed); queued updates sent
	 * under an older value are discarded (01 §3.2). */
	uint32_t source_gen;
	InputWatch *watches;
} Input;

/* One fan-out link of an output (01 §2.2): the destination input. An edge's
 * or a link's between processes (ProcessLink), whose destination is in
 * another graph: the destination input's source tells them apart. */
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

/* Node metadata (05 §2.1): the canvas position x, y. extra holds every other
 * metadata key, opaque, as
 * compact JSON object text ("{...}"), or NULL if there are none. A block owns
 * its metadata's extra; a BlockMetadata passed to graph_create_block is only
 * read (the block takes a copy). graphs/graph_json.h converts metadata from
 * and to JSON. */
typedef struct {
	double x, y;
	char *extra;
} BlockMetadata;

static inline BlockMetadata block_metadata_default(void)
{
	return (BlockMetadata){BLOCK_DEFAULT_POSITION, BLOCK_DEFAULT_POSITION, NULL};
}

/* Free md->extra (when the caller owns it) and set it NULL. */
void block_metadata_release(BlockMetadata *md);

struct Block {
	Graph *graph;
	BlockHandle handle;
	char *name; /* the instance name, owned, unique in the graph */
	const BlockTypeDef *type; /* NULL only for a placeholder (05 §2.2) */
	char *component;	  /* a placeholder's component name, owned; else NULL */
	uint32_t input_count;
	uint32_t output_count;
	Linkable *in_values;  /* input_count current input values (BlockCtx.inputs) */
	Input *inputs;	      /* input_count */
	Linkable *out_values; /* output_count current output values (BlockCtx.outputs) */
	Output *outputs;      /* output_count */
	void *state;	      /* type->state_size bytes, zeroed at creation; NULL when 0 */
	BlockMetadata metadata;
	bool started; /* mirrors the graph's started state (03 §2) */
	/* The block's one timer (TIMER_PERIODIC / TIMER_SCHEDULED types), a
	 * deadline in the runtime's heap (graphs/runtime.h "Timers"). */
	Deadline timer;
	uint32_t period_ms; /* TIMER_PERIODIC: the period, type->period_ms until set */
	/* Incremented whenever the timer is armed or cancelled (stop and
	 * destroy included), so a queued expiry of an older timer is stale
	 * (01 §3.2). A periodic timer's own continuation keeps it. */
	uint32_t timer_gen;
	/* A composite instance (03 §5, "Composite instances" above): its inner
	 * blocks, owned, in the definition's order; NULL and 0 otherwise. */
	Block **inner;
	uint32_t inner_count;
	/* An inner block: the composite instance it belongs to; NULL for a
	 * block of the graph itself. */
	Block *parent;
};

/* A graph-level port's direction: an inport maps to an inner block's input,
 * an outport to an inner block's output (01 §2.2 Port). */
enum graph_port_dir {
	GRAPH_INPORT = 0,
	GRAPH_OUTPORT = 1,
};

/* A graph-level port (01 §2.2 Port, 02 §3.4 addinport/addoutport): metadata
 * only. Its node is block->name at any time (the port follows a rename). */
typedef struct {
	char *name;    /* the public name, owned; unique per graph and direction */
	Block *block;  /* the inner block */
	uint32_t port; /* input index (inport) or output index (outport) of block */
} GraphPort;

/* A connection of a graph file that references a placeholder block (05
 * §2.2), retained as loaded so that saving writes it back unchanged: an edge
 * (src set) or an initial (src NULL, data set). At least one of its blocks is
 * a placeholder; the other one's port was checked when it was loaded. */
typedef struct {
	Block *src;	/* edge: the source block; NULL for an initial */
	char *src_port; /* edge: the source port name, owned; NULL for an initial */
	Linkable data;	/* initial: the literal as loaded (a JSON string stays a
			 * string, unparsed); unset for an edge */
	Block *tgt;
	char *tgt_port; /* owned */
	char *metadata; /* edge: its metadata as compact JSON object text, owned, or NULL */
} RetainedConnection;

/* A graph port of a graph file mapped to a placeholder block (05 §2.2). */
typedef struct {
	char *name;  /* the public name, owned */
	Block *block; /* the placeholder */
	char *port;  /* the inner port name, owned */
} RetainedPort;

struct Graph {
	Runtime *rt;
	Block **blocks; /* in creation order */
	uint32_t block_count;
	uint32_t block_cap;
	bool started;
	/* Identity (01 §2.2), owned. NULL in a graph made by graph_create alone;
	 * the Graphs registry sets all four (graphs/graphs.h). */
	char *id;
	char *name;
	char *description;
	char *icon;
	/* Graph-level ports per enum graph_port_dir, in creation order. */
	GraphPort *ports[2];
	uint32_t port_count[2];
	uint32_t port_cap[2];
	/* Retained entries of placeholder blocks (05 §2.2), in load order. */
	RetainedConnection *retained;
	uint32_t retained_count;
	uint32_t retained_cap;
	RetainedPort *retained_ports[2];
	uint32_t retained_port_count[2];
	uint32_t retained_port_cap[2];
	/* Lifecycle times (engine uptime, ms; 01 §3.4), kept by the Graphs
	 * registry: the start of the current or most recent run (0 if never
	 * started), and the duration of the most recent completed run. */
	uint64_t time_started;
	uint64_t last_run_ms;
};

/* ---- Events reported to the runtime's observer ------------------------ */

enum graph_event_type {
	GRAPH_EVENT_BLOCK_ADDED,
	GRAPH_EVENT_BLOCK_REMOVED,
	GRAPH_EVENT_BLOCK_RENAMED,
	GRAPH_EVENT_BLOCK_CHANGED, /* metadata changed; block->metadata is the result */
	GRAPH_EVENT_EDGE_ADDED,
	GRAPH_EVENT_EDGE_REMOVED,
	GRAPH_EVENT_EDGE_CHANGED, /* metadata replaced; the target input's edge_metadata */
	GRAPH_EVENT_INITIAL_ADDED,
	GRAPH_EVENT_INITIAL_REMOVED,
	GRAPH_EVENT_INPORT_ADDED,
	GRAPH_EVENT_INPORT_REMOVED,
	GRAPH_EVENT_INPORT_RENAMED,
	GRAPH_EVENT_OUTPORT_ADDED,
	GRAPH_EVENT_OUTPORT_REMOVED,
	GRAPH_EVENT_OUTPORT_RENAMED,
	/* Emitted by the Graphs registry only (graphs/graphs.h). */
	GRAPH_EVENT_GRAPH_CREATED, /* system-wide */
	GRAPH_EVENT_GRAPH_DELETED, /* system-wide */
	GRAPH_EVENT_GRAPH_RENAMED,
	GRAPH_EVENT_GRAPH_STARTED,
	GRAPH_EVENT_GRAPH_STOPPED,
	GRAPH_EVENT_GRAPH_STATUS, /* system-wide */
	GRAPH_EVENT_ERROR,	  /* system-wide */
	/* The group commands (02 §3.4.2), reported by app/type_files.h through
	 * graphs_notify_group / graphs_notify_component. */
	GRAPH_EVENT_GROUP_ADDED,
	GRAPH_EVENT_GROUP_REMOVED,
	GRAPH_EVENT_GROUP_RENAMED,
	GRAPH_EVENT_GROUP_CHANGED,
	GRAPH_EVENT_COMPONENT, /* system-wide: a new or changed block type */
	/* Links between processes (plan task 21f): system-wide; graph is the
	 * link's source process. No wire message yet (02 §6). */
	GRAPH_EVENT_LINK_ADDED,
	GRAPH_EVENT_LINK_REMOVED,
};

/* Pointers are valid only during the callback. Every event but ERROR and
 * COMPONENT has a graph, whose identity fields are the applied state. */
struct GraphEvent {
	enum graph_event_type type;
	Graph *graph;
	const Block *block;	/* the block; for an edge or initial, the target block */
	uint32_t input;		/* edge, initial: the target input */
	const Block *src_block; /* edge: the source block */
	uint32_t src_output;	/* edge: its output */
	const Linkable *value;	/* initial: the literal added or removed */
	/* BLOCK_RENAMED, *PORT_RENAMED, GRAPH_RENAMED, GROUP_RENAMED: the
	 * previous name; GROUP_REMOVED: the removed type's name */
	const char *old_name;
	/* GROUP_ADDED, GROUP_RENAMED (with its new name), GROUP_CHANGED,
	 * COMPONENT: the composite (or any) block type, as applied */
	const BlockTypeDef *block_type;
	const GraphPort *port; /* *PORT_*: the port (with its new name if renamed) */
	const ProcessLink *link; /* LINK_*: the link (as it was, when removed) */
	/* GRAPH_STARTED: time_ms = the run's start, uptime_ms = its uptime so far.
	 * GRAPH_STOPPED: the start and duration of the run that just ended (0 and
	 * 0 if the graph never ran). GRAPH_STATUS: uptime_ms of the current run,
	 * 0 when stopped. Engine uptime in ms (01 §3.4); the wire floors uptime
	 * to whole seconds (02 §3.3). */
	uint64_t time_ms;
	uint64_t uptime_ms;
	/* ERROR: the graph id the error is about (NULL if none; graph is that
	 * graph if it exists, else NULL) and the message. */
	const char *graph_id;
	const char *message;
};

/* ---- Graph ------------------------------------------------------------ */

/* A new, empty, stopped graph on rt, or NULL if memory runs out. */
Graph *graph_create(Runtime *rt);

/* Stop g (as graph_stop), then free it, every block and port in it and its
 * identity strings. Watches get
 * on_detach; no events are reported; queued entries for its blocks become
 * stale. */
void graph_destroy(Graph *g);

/* Whether id is a valid graph or node id: non-NULL, 1..GRAPH_ID_MAX bytes. */
bool graph_id_valid(const char *id);

/* The block named name (len bytes, need not be NUL-terminated), or NULL. */
Block *graph_find_block_n(const Graph *g, const char *name, size_t len);
Block *graph_find_block(const Graph *g, const char *name);

/* Create a block of type named name (02 §3.4 addnode), with metadata md
 * (NULL: block_metadata_default(); md->extra is copied); it is started at once
 * if g is started. A composite type's instance builds its inner blocks first
 * (see "Composite instances" above). Refused: INVALID_ID, NODE_EXISTS,
 * INVALID_ARGUMENT (NULL type), NO_MEMORY. On success *out (if not NULL) is the block. Reports
 * BLOCK_ADDED (only the block: inner blocks are not reported). */
enum graph_status graph_create_block(Graph *g, const char *name, const BlockTypeDef *type,
				     const BlockMetadata *md, Block **out);

/* Create a placeholder block (05 §2.2) for the unregistered type component:
 * as graph_create_block, but with no type, ports or compute. Refused:
 * INVALID_ID, NODE_EXISTS, INVALID_ARGUMENT (component NULL), NO_MEMORY. Reports BLOCK_ADDED. */
enum graph_status graph_create_placeholder(Graph *g, const char *name, const char *component,
					   const BlockMetadata *md, Block **out);

/* Remove a block (02 §3.4 removenode; 03 §2 destroy): stop it if started,
 * then sever every inbound edge and initial and every outbound link; each
 * downstream input reverts to its port default, which propagates. Queued
 * entries addressed to the block become stale. Reports EDGE_REMOVED /
 * INITIAL_REMOVED for each (inputs in port order, then outputs in port and
 * connection order), then INPORT_REMOVED / OUTPORT_REMOVED for each graph
 * port mapped to the block (inports, then outports, each in creation order;
 * each port's links removed before it, as graph_remove_port), then
 * BLOCK_REMOVED. Refused: INVALID_ID, UNKNOWN_NODE. */
enum graph_status graph_remove_block(Graph *g, const char *name);

/* Rename a block; links and graph ports mapped to it are unaffected and
 * follow it (02 §3.4 renamenode). Refused:
 * INVALID_ID, UNKNOWN_NODE, NODE_EXISTS (to names a block, from itself
 * included), NO_MEMORY. Reports BLOCK_RENAMED. */
enum graph_status graph_rename_block(Graph *g, const char *from, const char *to);

/* A metadata change (02 §3.4 changenode): the fields to merge. */
typedef struct {
	bool set_x, set_y;
	double x, y;
	/* set_extra: replace the opaque keys with extra (JSON object text, or
	 * NULL for none), which is copied. graph_json_to_change
	 * (graphs/graph_json.h) computes it from a changenode patch. */
	bool set_extra;
	char *extra;
} BlockChange;

/* Merge chg into the block's metadata. Metadata is UI-only (01 §2.2): it
 * never affects behavior. Refused: INVALID_ID, UNKNOWN_NODE, INVALID_ARGUMENT
 * (chg NULL), NO_MEMORY. Reports BLOCK_CHANGED. */
enum graph_status graph_change_block(Graph *g, const char *name, const BlockChange *chg);

/* Free chg->extra (when the caller owns it) and set it NULL. */
void block_change_release(BlockChange *chg);

/* Connect src.src_port (an output) to tgt.tgt_port (an input) (02 §3.4
 * addedge), with the edge's opaque metadata (JSON object text, copied; NULL
 * for none). If g is started, the input then receives the output's
 * last_propagated value at once (03 §2 connect); in a stopped graph it keeps
 * its value. Refused: INVALID_ID, UNKNOWN_NODE, UNKNOWN_PORT,
 * INVALID_EDGE_DIRECTION (src_port is an input of src, or tgt_port an output
 * of tgt), INPUT_CONNECTED (the input has an edge, an initial or a link),
 * NO_MEMORY.
 * Reports EDGE_ADDED. A retained connection into the input is dropped (see
 * "Placeholder blocks" above). */
enum graph_status graph_connect(Graph *g, const char *src, const char *src_port, const char *tgt,
				const char *tgt_port, const char *metadata);

/* Remove the edge src.src_port → tgt.tgt_port (02 §3.4 removeedge); the input
 * reverts to its port default, which propagates, and queued values from the
 * edge are dropped. Refused: INVALID_ID, UNKNOWN_NODE, UNKNOWN_PORT (a name
 * that is no port of the node), NO_SUCH_CONNECTION (no such edge, including a
 * port of the wrong direction). Reports EDGE_REMOVED. */
enum graph_status graph_disconnect(Graph *g, const char *src, const char *src_port,
				   const char *tgt, const char *tgt_port);

/* Replace the metadata of the edge src.src_port → tgt.tgt_port (02 §3.4
 * changeedge) with metadata (JSON object text, copied; NULL for none). No
 * value moves. Refused as graph_disconnect (NO_SUCH_CONNECTION included), and
 * NO_MEMORY. Reports EDGE_CHANGED. */
enum graph_status graph_change_edge(Graph *g, const char *src, const char *src_port,
				    const char *tgt, const char *tgt_port, const char *metadata);

/* Wire the literal value to node.port (02 §3.4 addinitial), replacing an
 * initial already there. The value is delivered at once and propagates if g is
 * started; a stopped graph only stores it. Refused: INVALID_ID, UNKNOWN_NODE,
 * UNKNOWN_PORT, INVALID_EDGE_DIRECTION (port is an output), INPUT_CONNECTED
 * (the input has an edge or a link), INVALID_ARGUMENT (value NULL). The graph takes its
 * own reference to value. Reports INITIAL_ADDED. A retained connection into
 * the input is dropped (see "Placeholder blocks" above). */
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

/* ---- Graph-level ports (01 §2.2 Port, 02 §3.4) ----------------------- */

/* Add a port called name in direction dir, mapped to node.port: an input of
 * node for GRAPH_INPORT, an output for GRAPH_OUTPORT (02 §3.4 addinport /
 * addoutport). Refused: INVALID_ID (name or node), UNKNOWN_NODE, UNKNOWN_PORT
 * (port is no input / output of node, depending on dir), PORT_EXISTS (name is
 * already a port of that direction), NO_MEMORY. Reports INPORT_ADDED /
 * OUTPORT_ADDED. */
enum graph_status graph_add_port(Graph *g, enum graph_port_dir dir, const char *name,
				 const char *node, const char *port);

/* Remove the port called name (02 §3.4 removeinport / removeoutport).
 * Refused: INVALID_ID, UNKNOWN_PORT (no such port). Removes the port's links
 * first (LINK_REMOVED each, then its input reverts), then reports
 * *PORT_REMOVED. */
enum graph_status graph_remove_port(Graph *g, enum graph_port_dir dir, const char *name);

/* Rename a port (02 §3.4 renameinport / renameoutport). Refused: INVALID_ID,
 * UNKNOWN_PORT (from is no port), PORT_EXISTS (to is a port, from itself
 * included), NO_MEMORY. Removes the port's links first (as
 * graph_remove_port), then reports *PORT_RENAMED. */
enum graph_status graph_rename_port(Graph *g, enum graph_port_dir dir, const char *from,
				    const char *to);

/* ---- Retained entries of placeholder blocks (05 §2.2) ---------------- */

/* Keep a copy of c (its strings and data copied) at the end of g's retained
 * connections, reporting nothing. The end that is a real block is checked as
 * addedge / addinitial check it, so a file entry is skipped exactly as the
 * wire command would be refused (05 §2.2): its port must exist with the right
 * direction (UNKNOWN_PORT, INVALID_EDGE_DIRECTION, source before target), and
 * the target input must have no source, live or retained (INPUT_CONNECTED;
 * no fan-in, 01 §2.2; a placeholder's input is identified by its port name).
 * Also refused: INVALID_ARGUMENT (tgt or a needed port name NULL, src and
 * data both or neither set, no placeholder among the blocks, or a block not
 * in g), NO_MEMORY. */
enum graph_status graph_retain_connection(Graph *g, const RetainedConnection *c);

/* Whether a retained connection feeds input port of b. A loader checks it
 * before a live edge or initial to b.port, which is then a fan-in. */
bool graph_input_retained(const Graph *g, const Block *b, const char *port);

/* Whether a retained graph port of direction dir is called name. */
bool graph_port_retained(const Graph *g, enum graph_port_dir dir, const char *name);

/* Keep a graph port called name in direction dir, mapped to port of the
 * placeholder block (names copied), reporting nothing. Refused: INVALID_ID
 * (name), PORT_EXISTS (a live or retained port of dir has that name),
 * INVALID_ARGUMENT (port NULL, or block is no placeholder of g), NO_MEMORY. */
enum graph_status graph_retain_port(Graph *g, enum graph_port_dir dir, const char *name,
				    Block *block, const char *port);

/* The port called name in direction dir, or NULL. */
const GraphPort *graph_find_port(const Graph *g, enum graph_port_dir dir, const char *name);

/* ---- Links between processes (01 §2.2 Port; plan task 21f) ---------------
 *
 * A link connects one graph's outport to an inport of another graph (or of
 * the same graph): the outport's inner output then feeds the inport's inner
 * input exactly like an edge. The link is an entry of the output's fan-out
 * links (Link), so a value travels through the work queue on output_set,
 * filtered by the source block's change threshold (01 §3.3); the input
 * counts the link as its one source (INPUT_SOURCE_LINK: no fan-in, so an
 * edge or initial onto it is refused as "Input already connected", and a
 * link onto an input with any source is refused); its source generation
 * changes with the link, so values queued from a removed link are dropped.
 *  - A new link delivers the output's last_propagated value at once if the
 *    source graph is started (like graph_connect); otherwise the input keeps
 *    its value until the source starts (start pushes every output).
 *  - A stopped source graph pushes nothing: the input keeps its last value.
 *  - Removing a link (graph_link_remove; the source or target graph deleted,
 *    graph_links_remove_graph; a linked graph port removed or renamed, also
 *    with its block) reports LINK_REMOVED and then severs the input: it
 *    reverts to its port default, which propagates if its graph is started
 *    (01 §2.2 Severing).
 * A link is named by its ports' public names (the process list's
 * "<process>.<port>", 05 §1.2), which is why a rename removes it: the
 * renamed port no longer is the port the link was declared with. Renaming a
 * port's inner block, or a graph, leaves it alone (the port follows the
 * block; a graph's id never changes).
 *
 * Links are engine-wide: they live in the Runtime that every graph shares
 * (rt->links, in creation order), since the graph operations that end one
 * (port removal and rename, block removal, graph destruction) are below the
 * Graphs registry. Memory (01 §2.2): the link array and the output's link
 * array grow when a link is added; nothing is allocated while values
 * propagate. Destroying a graph (graph_destroy, e.g. at shutdown) drops its
 * links silently, the other graph's inputs keeping their values. */
struct ProcessLink {
	Graph *src_graph;
	char *src_port;	     /* the outport's public name, owned */
	Graph *tgt_graph;
	char *tgt_port;	     /* the inport's public name, owned */
	Block *src_block;    /* the outport's block and output */
	uint32_t src_output;
	Block *tgt_block;    /* the inport's block and input (the link's Input) */
	uint32_t tgt_input;
};

/* Link src's outport to tgt's inport (src == tgt allowed). Refused, in this
 * order: INVALID_ID (either port name), UNKNOWN_PORT (outport is no port of
 * src, inport no port of tgt, in either direction; source first),
 * INVALID_LINK_DIRECTION (outport is only an inport of src, or inport only
 * an outport of tgt), LINK_EXISTS (this very link), INPUT_CONNECTED (the
 * inport's input has an edge, an initial, a retained connection or another
 * link), INVALID_ARGUMENT (the graphs are on different runtimes), NO_MEMORY.
 * Reports LINK_ADDED, then delivers as described above. */
enum graph_status graph_link_add(Graph *src, const char *outport, Graph *tgt,
				 const char *inport);

/* Remove the link src.outport -> tgt.inport. Refused: INVALID_ID,
 * NO_SUCH_LINK. Reports LINK_REMOVED; the input reverts to its default. */
enum graph_status graph_link_remove(Graph *src, const char *outport, Graph *tgt,
				    const char *inport);

/* Remove every link from or to g, in creation order, each as
 * graph_link_remove (an event, then the input reverts). Part of deleting a
 * graph (graphs_delete_graph), after its stop. */
void graph_links_remove_graph(Graph *g);

/* The links of rt, in creation order. */
static inline uint32_t runtime_link_count(const Runtime *rt)
{
	return rt->link_count;
}

static inline const ProcessLink *runtime_link_at(const Runtime *rt, uint32_t i)
{
	return &rt->links[i];
}

/* The name of the inner block's port p maps to (p of direction dir). */
const char *graph_port_inner_name(enum graph_port_dir dir, const GraphPort *p);

/* Split a "block.port" reference on its last '.' (01 §2.4): the block name is
 * ref[0 .. *block_len) and *port the rest. False if there is no '.', or
 * either part would be empty. */
bool port_ref_split(const char *ref, size_t *block_len, const char **port);

/* ---- Block -------------------------------------------------------------- */

/* Whether b is a placeholder for an unregistered type (05 §2.2). */
static inline bool block_is_placeholder(const Block *b)
{
	return b->type == NULL;
}

/* The block's component name: its type's name, or a placeholder's original
 * component. */
static inline const char *block_component(const Block *b)
{
	return b->type != NULL ? b->type->name : b->component;
}

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

/* Whether b is a composite instance (03 §5). */
static inline bool block_is_composite(const Block *b)
{
	return b->type != NULL && b->type->subgraph;
}

/* The block and input that a value for input *input of b is delivered to: b
 * itself, or for a composite instance the mapped inner input, followed
 * through nested composites (03 §5). Updates *input. */
Block *block_input_target(Block *b, uint32_t *input);

/* The block and output whose links carry output *output of b: b itself, or
 * for a composite instance the mapped inner output, followed through nested
 * composites. Updates *output. */
Block *block_output_source(Block *b, uint32_t *output);

/* The current value of input i of b (get_input, 03 §5: the mapped inner
 * input's for a composite instance). */
const Linkable *block_input_value(const Block *b, uint32_t input);

/* The current value of output i of b (get_output; the mapped inner output's
 * for a composite instance). */
const Linkable *block_output_value(const Block *b, uint32_t output);

/* The change threshold in effect for b (01 §3.3): its type default, else the
 * engine-wide default (also for an inner block of a composite, 03 §5). */
double block_change_threshold(const Block *b);

/* Whether next differs from last enough to propagate under threshold
 * (01 §3.3): a different value type always (i64 vs f64 included); numbers
 * when they are not equal and |next − last| >= threshold, NaN ↔ non-NaN
 * always, NaN → NaN never; bool and string on any difference; unset → unset
 * never. */
bool block_value_changed(const Linkable *last, const Linkable *next, double threshold);

/* output_set (01 §3.2 item 1): store value (taking ownership) in output i and,
 * if b is started and it changed by the threshold since last_propagated, set
 * last_propagated and queue an input update for every link. A stopped block
 * only stores. */
void block_output_set(Block *b, uint32_t output, Linkable value);

/* Set the period of a TIMER_PERIODIC block, overriding type->period_ms (01
 * §2.3; e.g. PID's sample_ms input, 03 §6.1, plan task 23). If the block is
 * started, its timer restarts from now: the next tick is period_ms after the
 * runtime's current time, and a queued expiry of the old timer is discarded;
 * 0 leaves no timer armed. Ignored for other timer modes. */
void block_set_period(Block *b, uint32_t period_ms);

/* Attach / detach a live-value watch on input i (see InputWatch).
 * block_watch_input calls w->on_value once with the current value. */
void block_watch_input(Block *b, uint32_t input, InputWatch *w);
void block_unwatch_input(Block *b, uint32_t input, InputWatch *w);

#endif
