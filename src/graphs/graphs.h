/*
 * The Graphs registry and event notification (01 §2.2 Graphs, 01 §4).
 *
 * Graphs owns every graph of the engine, by id, in creation order (the order
 * graph:list and the status broadcasts use), and the list of subscribers
 * that every applied structural or lifecycle change is reported to.
 *
 * Naming: functions are prefixed graphs_. The per-graph model and its
 * structural operations are graphs/graph.h.
 *
 * Commands (02 §3.2–§3.4): the graphs_ command functions take the wire-level
 * arguments (graph id, node and port names, already parsed from JSON by the
 * protocol layer, plan task 18) and either apply the command, reporting its
 * events, or refuse it as a whole with no change and no event, returning the
 * status and, if msg is not NULL, the exact 02 §3.4.1 message. Checks run in
 * this order, the first failure wins: every id in the command (graph id and
 * node ids, and a graph port's public name) is valid (Invalid id), the graph
 * exists (Unknown graph), then the graph operation's own checks
 * (graphs/graph.h).
 *
 * Events (01 §4, 02 §1.1): every applied change is reported synchronously,
 * after the mutation is complete, in applied order, to every subscriber
 * whose scope admits it: graph-specific events (blocks, edges, initials,
 * ports, groups, graph renamed/started/stopped) go to subscribers with no
 * scope or scoped to that graph; system-wide events (graph created/deleted,
 * status, errors, component, and the links between processes, which concern
 * two graphs) go to every subscriber (02 §4.1). Events are GraphEvents
 * (graphs/graph.h); turning them into wire messages is the protocol layer's
 * job, and so is withholding them from connections that are not
 * authenticated (02 §2.5 item 6).
 *
 * Unsaved state (02 §3.3, plan task 45): a graph is unsaved when it differs
 * from its file as last loaded or saved — after any applied change of what
 * the file holds (an event of its blocks, connections, initials or graph
 * ports, a rename), or while its started state differs from the file's
 * started flag (so a stop and a start again leave it saved). A new graph is
 * unsaved; graphs_mark_saved records a load or a successful save. Whenever
 * the flag changes, and at every graphs_mark_saved, GRAPH_STATUS is reported
 * for the graph (after the event that changed it), so clients learn it.
 *
 * Time (decided, plan task 7): uptime is computed, not counted — the engine
 * time of the Runtime (runtime_set_now, set by the engine loop from
 * pal_uptime_ms before it dispatches commands or drains) minus the run's
 * start time. There is no per-second uptime timer (01 §2.2).
 *
 * Threading: engine-thread only (01 §3.2), no locking.
 */
#ifndef HAKKAS_GRAPHS_GRAPHS_H
#define HAKKAS_GRAPHS_GRAPHS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphs/graph.h"
#include "platform/pal.h"
#include "system/uuid.h"

/* The name of a graph created without one (02 §3.2 new_graph). */
#define GRAPHS_DEFAULT_NAME "<<new graph>>"

/* A UUID in its 36-character text form plus the NUL. */
#define GRAPHS_UUID_SIZE UUID_TEXT_SIZE

/* Room for any refusal message. A message naming a node or port longer than
 * fits is cut (it is still NUL-terminated). */
#define GRAPHS_MESSAGE_MAX 192

/* Resolves a block type name (block_types_get, blocktypes/block_types.h; the
 * registry gets it as a function so that graphs/ does not depend on
 * blocktypes/, 01 §2.3). */
typedef const BlockTypeDef *(*graphs_type_resolver)(const char *name);

/* The random source for UUIDs: pal_random_bytes (platform/pal.h). The engine
 * passes it to graphs_init, so the core itself links no PAL symbol and tests
 * can pass a fake. */
typedef pal_random_fn graphs_random_fn;

/*
 * A subscriber (01 §4): one per connection, embedded by its owner. fn is
 * called with ctx for every event in scope. The other fields are private.
 *
 * Callbacks may subscribe, unsubscribe (any subscriber, themselves included)
 * and re-scope, but must not change a graph or emit an event (no graphs_
 * command, no graph_ operation). A subscriber added during a notification
 * does not get the event being delivered; one removed during it gets
 * nothing more; a re-scope applies from the next delivery on.
 */
typedef struct GraphsSubscriber GraphsSubscriber;
struct GraphsSubscriber {
	graph_event_fn fn;
	void *ctx;
	char scope[GRAPH_ID_MAX + 1]; /* "" = every graph */
	uint64_t since;		      /* the event sequence number when added */
	GraphsSubscriber *next;
};

typedef struct Graphs {
	Runtime *rt;
	graphs_type_resolver resolve_type;
	graphs_random_fn random_bytes;
	Graph **graphs; /* in creation order */
	uint32_t count;
	uint32_t cap;
	GraphsSubscriber *subscribers; /* in subscription order */
	GraphsSubscriber *cursor;      /* the next subscriber of the delivery in progress */
	uint64_t seq;		       /* events delivered so far */
} Graphs;

/* Make gs an empty registry on rt, and make it rt's observer (so it hears
 * every structural change of the graphs on rt). resolve_type resolves
 * addnode's component name (block_types_get); random_bytes is the random
 * source for UUIDs (pal_random_bytes). */
void graphs_init(Graphs *gs, Runtime *rt, graphs_type_resolver resolve_type,
		 graphs_random_fn random_bytes);

/* Destroy every graph (reporting nothing, 01 §2.2), drop every subscriber
 * and detach from the runtime. The runtime itself is left to the caller. */
void graphs_deinit(Graphs *gs);

/* ---- Lookup and iteration ------------------------------------------------- */

/* The graph with this id, or NULL (also for a NULL id). */
Graph *graphs_find(const Graphs *gs, const char *id);

static inline uint32_t graphs_count(const Graphs *gs)
{
	return gs->count;
}

/* The i-th graph in creation order (i < graphs_count). */
static inline Graph *graphs_at(const Graphs *gs, uint32_t i)
{
	return gs->graphs[i];
}

/* The uptime of g's current run in ms (01 §2.2): the runtime's now minus its
 * start, 0 when stopped. */
uint64_t graphs_uptime_ms(const Graph *g);

/* ---- Subscribers (01 §4, 02 §4.1) ----------------------------------------- */

/* Make s a subscriber with callback fn and ctx and no scope (every graph). */
void graphs_subscriber_init(GraphsSubscriber *s, graph_event_fn fn, void *ctx);

/* Add s (at the end of the delivery order). s must not be subscribed. */
void graphs_subscribe(Graphs *gs, GraphsSubscriber *s);

/* Remove s; nothing happens if it is not subscribed. */
void graphs_unsubscribe(Graphs *gs, GraphsSubscriber *s);

/* Scope s's graph-specific events to the graph graph_id (graph:connect), or
 * to every graph for NULL. False, with no change, for an invalid id. The id
 * need not name an existing graph; the protocol layer refuses graph:connect
 * to an unknown graph before scoping. */
bool graphs_subscriber_scope(GraphsSubscriber *s, const char *graph_id);

/* ---- Refusal messages (02 §3.4.1) ----------------------------------------- */

/* What a refusal is about. Fields a status does not use may be NULL. */
typedef struct {
	const char *graph; /* UNKNOWN_GRAPH, GRAPH_EXISTS, NAME_MISMATCH */
	const char *node;  /* UNKNOWN_NODE, NODE_EXISTS; UNKNOWN_PORT and
			    * INPUT_CONNECTED: the port's node, NULL for an
			    * unknown graph port */
	const char *port;  /* UNKNOWN_PORT, INPUT_CONNECTED */
	const char *name;  /* UNKNOWN_COMPONENT, PORT_EXISTS, ACTUATOR_DRIVEN */
} GraphRefusal;

typedef struct {
	char text[GRAPHS_MESSAGE_MAX];
} GraphsMessage;

/* Render the exact 02 §3.4.1 message for st into buf (size bytes, always
 * NUL-terminated if size > 0): "Unknown graph: <graph>", "Unknown node:
 * <node>", "Unknown port: <node>.<port>" (or "Unknown port: <port>" for a
 * graph port, node NULL), "Node exists: <node>", "Unknown component:
 * <name>", "Invalid edge direction", "Input already connected:
 * <node>.<port>", "No such connection", "Port exists: <name>", "Graph name
 * mismatch: <graph>", "Graph exists: <graph>", "Invalid id", and for the
 * links between processes (02 §6) "Invalid link direction", "Link exists",
 * "No such link", for the environment bindings (02 §6) "Invalid binding
 * direction", "Actuator already driven: <name>" (name: the channel, e.g.
 * "relay[3]"), "No such binding". The statuses
 * that are no wire refusal render "Invalid argument", "Out of memory" and
 * "Random source unavailable"; GRAPH_OK renders "". Returns the length the
 * full message has (as snprintf). */
size_t graph_refusal_message(enum graph_status st, const GraphRefusal *ctx, char *buf,
			     size_t size);

/* ---- Runtime commands (02 §3.2) ------------------------------------------ */

/* new_graph: create a stopped, empty graph. id NULL: a fresh random UUID v4;
 * name NULL: GRAPHS_DEFAULT_NAME; description, icon NULL: "". Refused:
 * INVALID_ID, GRAPH_EXISTS, UNAVAILABLE (random source failed), NO_MEMORY.
 * Reports GRAPH_CREATED. On success *out (if not NULL) is the graph. */
enum graph_status graphs_new_graph(Graphs *gs, const char *id, const char *name,
				   const char *description, const char *icon, Graph **out,
				   GraphsMessage *msg);

/* delete_graph: stop the graph if started (reporting GRAPH_STOPPED), remove
 * its links from and to other graphs (reporting LINK_REMOVED for each, every
 * target input reverting to its default; graphs/graph.h) and its environment
 * bindings (BINDING_REMOVED each, plan task 22), then remove it,
 * report GRAPH_DELETED (the graph is still readable during that
 * callback) and destroy it, its blocks going without events. name must equal
 * the graph's current name. Refused: INVALID_ID, UNKNOWN_GRAPH,
 * NAME_MISMATCH. This deletes the graph in memory only: the protocol layer
 * calls graph_files_delete (app/graph_files.h), which also removes the
 * persisted file (02 §3.2, 05 §2.3). */
enum graph_status graphs_delete_graph(Graphs *gs, const char *id, const char *name,
				      GraphsMessage *msg);

/* ---- Network commands (02 §3.3) ------------------------------------------- */

/* start: start the graph if stopped, recording the runtime's now (and its
 * wall clock, for the wire's timestamps) as the run's start; report
 * GRAPH_STARTED either way (a started graph is not
 * restarted and keeps its start time). Refused: INVALID_ID, UNKNOWN_GRAPH. */
enum graph_status graphs_start(Graphs *gs, const char *id, GraphsMessage *msg);

/* stop: stop the graph if started, recording the run's duration; report
 * GRAPH_STOPPED either way, with the start and duration of the most recent
 * run (0 and 0 if it never ran). Refused: INVALID_ID, UNKNOWN_GRAPH. */
enum graph_status graphs_stop(Graphs *gs, const char *id, GraphsMessage *msg);

/* getstatus: report GRAPH_STATUS for the graph. Refused: INVALID_ID,
 * UNKNOWN_GRAPH. */
enum graph_status graphs_status(Graphs *gs, const char *id, GraphsMessage *msg);

/* graph:list, runtime:getruntime: report GRAPH_STATUS for every graph, in
 * creation order. */
void graphs_status_all(Graphs *gs);

/* g has just been loaded from its file or saved to it (app/graph_files.h);
 * started is the file's started flag. g is then unsaved only if its started
 * state differs from that flag; GRAPH_STATUS is reported (02 §3.3). */
void graphs_mark_saved(Graphs *gs, Graph *g, bool started);

/* Report ERROR, a system-wide error event (02 §1 error format, e.g. the
 * persist failure of 02 §3.3), about graph_id (NULL for none). */
void graphs_notify_error(Graphs *gs, const char *graph_id, const char *message);

/* Report a group command's event (02 §3.4.2; the commands are
 * app/type_files.h's), graph-specific to g: type is GRAPH_EVENT_GROUP_ADDED,
 * _CHANGED (def the type), _RENAMED (def the renamed type, old_name its
 * previous name) or _REMOVED (def NULL, old_name the removed type's name). */
void graphs_notify_group(Graphs *gs, Graph *g, enum graph_event_type type,
			 const BlockTypeDef *def, const char *old_name);

/* Report COMPONENT, a system-wide event (02 §4.1): the block type def was
 * added or changed (component:component, 02 §3.5). */
void graphs_notify_component(Graphs *gs, const BlockTypeDef *def);

/* ---- Graph commands (02 §3.4) ------------------------------------------- */

/* rename: set the graph's name to name (any string; the id never changes).
 * Reports GRAPH_RENAMED with the previous name. Refused: INVALID_ID,
 * UNKNOWN_GRAPH, INVALID_ARGUMENT (name NULL), NO_MEMORY. */
enum graph_status graphs_rename_graph(Graphs *gs, const char *id, const char *name,
				      GraphsMessage *msg);

/* addnode: Node exists is checked before Unknown component. md NULL: the
 * default metadata. */
enum graph_status graphs_add_node(Graphs *gs, const char *graph, const char *node,
				  const char *component, const BlockMetadata *md,
				  GraphsMessage *msg);

enum graph_status graphs_remove_node(Graphs *gs, const char *graph, const char *node,
				     GraphsMessage *msg);

enum graph_status graphs_rename_node(Graphs *gs, const char *graph, const char *from,
				     const char *to, GraphsMessage *msg);

enum graph_status graphs_change_node(Graphs *gs, const char *graph, const char *node,
				     const BlockChange *chg, GraphsMessage *msg);

/* addedge: metadata is the edge's opaque metadata as JSON object text
 * (graphs/graph_json.h), copied; NULL for none. */
enum graph_status graphs_add_edge(Graphs *gs, const char *graph, const char *src,
				  const char *src_port, const char *tgt, const char *tgt_port,
				  const char *metadata, GraphsMessage *msg);

enum graph_status graphs_remove_edge(Graphs *gs, const char *graph, const char *src,
				     const char *src_port, const char *tgt, const char *tgt_port,
				     GraphsMessage *msg);

/* changeedge: replace the edge's metadata with metadata (JSON object text,
 * copied; NULL for none). Refused like removeedge ("No such connection" for
 * an edge that does not exist). Reports EDGE_CHANGED, a graph-specific event
 * (02 §3.4: every subscriber of the graph). */
enum graph_status graphs_change_edge(Graphs *gs, const char *graph, const char *src,
				     const char *src_port, const char *tgt, const char *tgt_port,
				     const char *metadata, GraphsMessage *msg);

/* addinitial: value is the literal, already parsed for the port (02 §3.4
 * "Initial value encoding"); the graph takes its own reference. */
enum graph_status graphs_add_initial(Graphs *gs, const char *graph, const char *node,
				     const char *port, const Linkable *value, GraphsMessage *msg);

enum graph_status graphs_remove_initial(Graphs *gs, const char *graph, const char *node,
					const char *port, GraphsMessage *msg);

/* add/remove/rename in/outport (dir selects which). */
enum graph_status graphs_add_port(Graphs *gs, const char *graph, enum graph_port_dir dir,
				  const char *name, const char *node, const char *port,
				  GraphsMessage *msg);

enum graph_status graphs_remove_port(Graphs *gs, const char *graph, enum graph_port_dir dir,
				     const char *name, GraphsMessage *msg);

enum graph_status graphs_rename_port(Graphs *gs, const char *graph, enum graph_port_dir dir,
				     const char *from, const char *to, GraphsMessage *msg);

/* ---- Links between processes (plan task 21f; the C API only, 02 §6) ------ */

/* Link from_graph's outport to to_graph's inport (graphs/graph.h
 * graph_link_add). Refused, in this order: INVALID_ID (any of the four
 * names), UNKNOWN_GRAPH (from_graph, then to_graph: "Unknown graph: <id>"),
 * then as graph_link_add: UNKNOWN_PORT ("Unknown port: <graph>.<port>",
 * the source's port if it is no port of from_graph, else the target's),
 * INVALID_LINK_DIRECTION, LINK_EXISTS, INPUT_CONNECTED ("Input already
 * connected: <to_graph>.<inport>"), NO_MEMORY. Reports LINK_ADDED. */
enum graph_status graphs_add_link(Graphs *gs, const char *from_graph, const char *outport,
				  const char *to_graph, const char *inport, GraphsMessage *msg);

/* Remove the link (graph_link_remove). Refused: INVALID_ID, UNKNOWN_GRAPH,
 * NO_SUCH_LINK. Reports LINK_REMOVED; the target input reverts. */
enum graph_status graphs_remove_link(Graphs *gs, const char *from_graph, const char *outport,
				     const char *to_graph, const char *inport, GraphsMessage *msg);

#endif
