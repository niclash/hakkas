/*
 * The local C API (02 §6; decided, plan task 21e): hakkas embedded in a host
 * program. The host — the POSIX command line (platform/posix/host.c), the
 * Zephyr application — creates the runtime, loads its processes, starts
 * and stops them, edits them, and follows every change as typed events,
 * exactly like a WebSocket client does over the protocol of 02.
 *
 * Terms (00-README Terminology): a process is one dataflow network (the
 * wire's "graph"), made of blocks (the wire's "nodes") and connections (the
 * wire's "edges") and initials. A process is named by its id.
 *
 * One command layer: every function is a thin layer over the same model
 * commands as the WebSocket dispatcher (graphs/graphs.h graphs_*,
 * app/graph_files.h, app/type_files.h), so validation, refusals and events
 * are those of the wire, by construction:
 *  - a refused command changes nothing, reports no event and returns
 *    HAKKAS_REFUSED with the wire's network:error message in msg (if not
 *    NULL): the 02 §3.4.1 texts ("Unknown graph: <id>", "Invalid id", ...)
 *    and "Invalid field: <name>" (02 §1.2) for a missing (NULL) argument
 *    or invalid metadata, <name> being the wire's payload field (graph, id,
 *    src, metadata, ...), checked in the order 02 lists the fields;
 *  - an applied command reports its events (below) before it returns, the
 *    same events, in the same order, as the wire command. The host gets no
 *    other acknowledgement (02 §1.1).
 * Metadata is UI-only (01 §2.2) and passed as JSON object text, the wire's
 * "metadata" member: a block's {"x","y",...} (addnode; changenode takes the
 * merge patch of 02 §3.4) and a connection's opaque object. Values are typed
 * (HakkasValue): an initial's string is parsed for the target port's type
 * like the wire's JSON string data; a number or bool is taken as it is (an
 * i64 initial can only come from here, 05 §2.1).
 *
 * Events (01 §4, 02 §1.1): hakkas_subscribe registers a callback that
 * receives a HakkasEvent for every model event — the same GraphEvent stream
 * the wire messages are built from (protocol/messages.h proto_msg_event),
 * in the same order, unscoped: the host sees every process, whatever a
 * client has connected to. The host is trusted: there is no login and no
 * capability check (02 §6). Events about a process carry its state as
 * applied (02 §1.1: "events describe what was applied").
 *
 * Threading (01 §3.2): the engine is single-threaded and so is this API.
 * Call it on the engine thread only: from the host before hakkas_run (e.g.
 * loading processes) or after it returns, from a tick hook (hakkas_add_tick)
 * while the loop runs, or from an event callback for the read-only
 * functions (hakkas_config, hakkas_process_count, _at, _status,
 * hakkas_link_count, _at). A command
 * that changes the model, called from inside an event callback, is refused
 * with HAKKAS_ERR_BUSY (01 §4: a subscriber must not change a graph while
 * events are delivered). Another context (a signal handler, an IO worker)
 * may only request a stop, with pal_request_stop (platform/pal.h).
 *
 * Time: every command first sets the runtime's engine time from the PAL's
 * clock, as the loop does before it dispatches a command (01 §3.4), so a
 * process started before hakkas_run reports the right start time.
 *
 * Naming: functions are prefixed hakkas_; the runtime is a Hakkas.
 */
#ifndef HAKKAS_HAKKAS_H
#define HAKKAS_HAKKAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphs/block_type.h"
#include "graphs/linkable.h"
#include "system/config.h"

/* The longest process id, block name or port name, in bytes (02 §3.4.1). */
#define HAKKAS_ID_MAX 64

/* Room for any refusal message (as GRAPHS_MESSAGE_MAX). */
#define HAKKAS_MESSAGE_MAX 192

/* The most event subscribers (hakkas_subscribe). */
#define HAKKAS_SUBSCRIBERS_MAX 8

typedef struct Hakkas Hakkas;

/* A value (01 §2.1): a string, an i64, an f64 or a bool. */
typedef Linkable HakkasValue;

enum hakkas_status {
	HAKKAS_OK = 0,
	HAKKAS_REFUSED = -1,	    /* refused as the wire refuses it; msg holds the message */
	HAKKAS_ERR_NO_MEMORY = -2,  /* "Out of memory"; nothing changed */
	HAKKAS_ERR_STORAGE = -3,    /* a file could not be read or written (logged) */
	HAKKAS_ERR_NOT_FOUND = -4,  /* load: no such file */
	HAKKAS_ERR_INVALID = -5,    /* load: an invalid path (empty, too long) */
	HAKKAS_ERR_TOO_BIG = -6,    /* load: the file is larger than HAKKAS_GRAPH_FILE_MAX */
	HAKKAS_ERR_MALFORMED = -7,  /* load: not a graph file (05 §2.2) */
	HAKKAS_ERR_EXISTS = -8,	    /* load: a process with the file's id is loaded */
	HAKKAS_ERR_BUSY = -9,	    /* a command called from inside an event callback */
};

/* A short, static description of st, for log lines. */
const char *hakkas_status_str(enum hakkas_status st);

/* A command's message: the wire's network:error text of a refusal (02
 * §3.4.1, §1.2), or a short description of any other failure; "" on
 * success. */
typedef struct {
	char text[HAKKAS_MESSAGE_MAX];
} HakkasMessage;

/* ---- The runtime ---------------------------------------------------------------- */

typedef struct {
	/* The platform's hooks around the loop, either may be NULL (as in
	 * engine/engine.h): start right after the loop is created (POSIX
	 * installs its SIGINT/SIGTERM handlers; false refuses startup), stop
	 * at shutdown before the loop is destroyed. */
	bool (*platform_start)(void *ctx);
	void (*platform_stop)(void *ctx);
	void *platform_ctx;
	/* Tests only: both listeners on ephemeral ports (hakkas_web_port,
	 * hakkas_ws_port). */
	bool ephemeral_ports;
} HakkasParams;

/* Create the runtime from cfg (copied): the engine's startup sequence (05
 * §1.4 steps 1-8: the configuration's generated values, the data
 * directories, the loop, the authorization, the built-in and the composite
 * block types of <datadir>/types, the protocol layer and both listeners).
 * No process is loaded or started: that is the host's part (step 7).
 * params may be NULL (no hooks). The PAL is initialized (pal_init) and
 * logging has its console sink before this call. True with *out the
 * runtime; false (logged) with *out NULL and nothing left over. */
bool hakkas_create(const Config *cfg, const HakkasParams *params, Hakkas **out);

/* Run the engine loop (05 §1.4 step 9) until a stop request
 * (hakkas_request_stop, pal_request_stop); it then closes every connection
 * and listener and returns. Processes and subscriptions are kept. */
void hakkas_run(Hakkas *h);

/* One loop iteration (engine/loop.h loop_run_once), for hosts that drive
 * the loop themselves, and tests. */
void hakkas_run_once(Hakkas *h);

/* Ask hakkas_run to return after the current iteration. Engine thread (a
 * tick hook, an event callback); other contexts call pal_request_stop. */
void hakkas_request_stop(Hakkas *h);

/* Shut down and free the runtime (engine_stop): every process is destroyed
 * without events and nothing is saved (05 §1.4 "Shutdown"). NULL is
 * ignored. Not from inside hakkas_run. */
void hakkas_destroy(Hakkas *h);

/* The configuration in use (with the generated engine_id and secret). */
const Config *hakkas_config(const Hakkas *h);

/* The ports the web server and the WebSocket listener listen on (0 once
 * closed). */
uint16_t hakkas_web_port(const Hakkas *h);
uint16_t hakkas_ws_port(const Hakkas *h);

/* A tick hook of the loop (engine/loop.h loop_add_tick): called every
 * iteration with the engine time; returns its next deadline (ms of engine
 * time; UINT64_MAX for none), which bounds the loop's poll. Commands may be
 * called from it. False if the loop has no free slot. */
typedef uint64_t (*hakkas_tick_fn)(void *ctx, uint64_t now_ms);
bool hakkas_add_tick(Hakkas *h, hakkas_tick_fn fn, void *ctx);
bool hakkas_remove_tick(Hakkas *h, hakkas_tick_fn fn, void *ctx);

/* ---- Events (01 §4, 02 §1.1) --------------------------------------------------- */

/* One per GraphEvent type, with its wire message (02 §3). */
enum hakkas_event_type {
	HAKKAS_EVENT_PROCESS_CREATED,	 /* runtime:new_graph */
	HAKKAS_EVENT_PROCESS_DELETED,	 /* runtime:delete_graph */
	HAKKAS_EVENT_PROCESS_RENAMED,	 /* graph:rename */
	HAKKAS_EVENT_PROCESS_STARTED,	 /* network:started */
	HAKKAS_EVENT_PROCESS_STOPPED,	 /* network:stopped */
	HAKKAS_EVENT_PROCESS_STATUS,	 /* network:status */
	HAKKAS_EVENT_BLOCK_ADDED,	 /* graph:addnode */
	HAKKAS_EVENT_BLOCK_REMOVED,	 /* graph:removenode */
	HAKKAS_EVENT_BLOCK_RENAMED,	 /* graph:renamenode */
	HAKKAS_EVENT_BLOCK_CHANGED,	 /* graph:changenode */
	HAKKAS_EVENT_CONNECTION_ADDED,	 /* graph:addedge */
	HAKKAS_EVENT_CONNECTION_REMOVED, /* graph:removeedge */
	HAKKAS_EVENT_CONNECTION_CHANGED, /* graph:changeedge */
	HAKKAS_EVENT_INITIAL_ADDED,	 /* graph:addinitial */
	HAKKAS_EVENT_INITIAL_REMOVED,	 /* graph:removeinitial */
	HAKKAS_EVENT_INPORT_ADDED,	 /* graph:addinport */
	HAKKAS_EVENT_INPORT_REMOVED,	 /* graph:removeinport */
	HAKKAS_EVENT_INPORT_RENAMED,	 /* graph:renameinport */
	HAKKAS_EVENT_OUTPORT_ADDED,	 /* graph:addoutport */
	HAKKAS_EVENT_OUTPORT_REMOVED,	 /* graph:removeoutport */
	HAKKAS_EVENT_OUTPORT_RENAMED,	 /* graph:renameoutport */
	HAKKAS_EVENT_GROUP_ADDED,	 /* graph:addgroup */
	HAKKAS_EVENT_GROUP_REMOVED,	 /* graph:removegroup */
	HAKKAS_EVENT_GROUP_RENAMED,	 /* graph:renamegroup */
	HAKKAS_EVENT_GROUP_CHANGED,	 /* graph:changegroup */
	HAKKAS_EVENT_COMPONENT,		 /* component:component: a new or changed block type */
	HAKKAS_EVENT_ERROR,		 /* network:error, e.g. a failed persist (02 §3.3) */
	/* Links between processes (plan task 21f): no wire message yet. */
	HAKKAS_EVENT_LINK_ADDED,
	HAKKAS_EVENT_LINK_REMOVED,
};

/*
 * An event. Every pointer is valid only during the callback. Fields an
 * event type does not list are NULL, 0 or false.
 *  process:     the process id; ERROR: the id the error is about, or NULL;
 *               COMPONENT: NULL; LINK_*: the link's source process.
 *  name:        PROCESS_*: the process's name (RENAMED: the new one);
 *               *PORT_*: the public port name (RENAMED: the new one);
 *               LINK_*: the source process's outport;
 *               GROUP_*, COMPONENT: the block type's name (RENAMED: the new
 *               one; REMOVED: the removed one).
 *  old_name:    *_RENAMED: the previous name (of the process, block, port
 *               or type).
 *  description: PROCESS_CREATED, PROCESS_STATUS: the process's;
 *               GROUP_ADDED, GROUP_CHANGED: the type's.
 *  icon:        PROCESS_CREATED: the process's; GROUP_ADDED, GROUP_CHANGED:
 *               the type's.
 *  block, port: BLOCK_*: the block (RENAMED: its new name), no port;
 *               CONNECTION_*, INITIAL_*: the target block and input;
 *               INPORT_ADDED / OUTPORT_ADDED: the inner block and port.
 *  src_block, src_port: CONNECTION_*: the source block and output.
 *  component:   BLOCK_ADDED: the block's type name (a placeholder's
 *               component, 05 §2.2).
 *  x, y, metadata: BLOCK_ADDED, BLOCK_CHANGED: the canvas position and the
 *               other metadata keys as JSON object text (NULL for none), as
 *               applied; CONNECTION_ADDED, CONNECTION_CHANGED: metadata is
 *               the connection's metadata (NULL for none).
 *  value:       INITIAL_*: the literal added or removed.
 *  started:     PROCESS_STARTED (true), PROCESS_STATUS.
 *  time_ms, uptime_ms: engine time in ms (01 §3.4). PROCESS_STARTED: the
 *               run's start, its uptime so far; PROCESS_STOPPED: the start
 *               and duration of the run that ended (0, 0 if it never ran);
 *               PROCESS_STATUS: uptime_ms of the current run (0 stopped).
 *  message:     ERROR: the message.
 *  block_type:  GROUP_ADDED, GROUP_RENAMED, GROUP_CHANGED, COMPONENT: the
 *               type as applied (graphs/block_type.h; a group's inner
 *               blocks are block_type->composite->blocks).
 *  to_process, to_port: LINK_*: the link's target process and its inport.
 */
typedef struct {
	enum hakkas_event_type type;
	const char *process;
	const char *name;
	const char *old_name;
	const char *description;
	const char *icon;
	const char *block;
	const char *port;
	const char *src_block;
	const char *src_port;
	const char *component;
	double x, y;
	const char *metadata;
	const HakkasValue *value;
	bool started;
	uint64_t time_ms;
	uint64_t uptime_ms;
	const char *message;
	const BlockTypeDef *block_type;
	const char *to_process;
	const char *to_port;
} HakkasEvent;

typedef void (*hakkas_event_fn)(void *ctx, const HakkasEvent *ev);

/* Subscribe fn with ctx to every event, from the next one on, after the
 * subscribers already there. False if HAKKAS_SUBSCRIBERS_MAX are
 * subscribed or (fn, ctx) already is. May be called from a callback (the
 * event being delivered is not delivered to the new subscriber). */
bool hakkas_subscribe(Hakkas *h, hakkas_event_fn fn, void *ctx);

/* Unsubscribe (fn, ctx); nothing happens if it is not subscribed. May be
 * called from a callback (it then gets nothing more). */
void hakkas_unsubscribe(Hakkas *h, hakkas_event_fn fn, void *ctx);

/* ---- Processes: lifetime (02 §3.2, §3.3; 05 §2) -------------------------------- */

/* A process loaded from a file. */
typedef struct {
	char id[HAKKAS_ID_MAX + 1];
	/* The file's started flag (05 §2.1): information for the host, which
	 * decides whether to start the process. Loading never starts it. */
	bool started;
} HakkasLoaded;

/* Load the graph file at path (any path) as a new, stopped process (05
 * §2.1-§2.2): its blocks, connections, initials and ports are loaded
 * through the same commands as the wire's, reporting PROCESS_CREATED and
 * an event per entry; an invalid entry is logged and skipped. On success
 * *out (if not NULL) holds its id and started flag. Failures are logged and
 * create nothing: HAKKAS_ERR_NOT_FOUND, _INVALID, _TOO_BIG, _MALFORMED,
 * _EXISTS (a process with the file's id is loaded), _STORAGE, _NO_MEMORY,
 * with a description in msg. */
enum hakkas_status hakkas_process_load(Hakkas *h, const char *path, HakkasLoaded *out,
				       HakkasMessage *msg);

/* Called by hakkas_process_load_saved for each process loaded. It may call
 * commands (hakkas_process_start, ...). */
typedef void (*hakkas_loaded_fn)(void *ctx, const HakkasLoaded *loaded);

/* Load every saved process: each graph file in <datadir>/graphs, in byte
 * order of the file names (05 §2.3), calling fn (if not NULL) after each
 * one that loaded. A file that does not load is logged and skipped; the
 * others still load. HAKKAS_OK if all loaded, else the first failure's
 * status. *loaded (if not NULL) is the number loaded. */
enum hakkas_status hakkas_process_load_saved(Hakkas *h, hakkas_loaded_fn fn, void *ctx,
					     size_t *loaded);

/* runtime:new_graph (02 §3.2): a new, stopped, empty process. Every
 * argument may be NULL: id a fresh random UUID, name "<<new graph>>",
 * description and icon "". Refused: "Invalid id", "Graph exists: <id>".
 * On success id_out (if not NULL, HAKKAS_ID_MAX + 1 bytes) holds its id.
 * Not persisted until hakkas_process_persist. Reports PROCESS_CREATED. */
enum hakkas_status hakkas_process_create(Hakkas *h, const char *id, const char *name,
					 const char *description, const char *icon, char *id_out,
					 HakkasMessage *msg);

/* runtime:delete_graph (02 §3.2): stop the process if started (reporting
 * PROCESS_STOPPED), delete it (PROCESS_DELETED) and its file. name must be
 * its current name (a guard). Refused: "Invalid field: id" / "name",
 * "Invalid id", "Unknown graph: <id>", "Graph name mismatch: <id>". A file
 * that cannot be removed is logged; the deletion stands. */
enum hakkas_status hakkas_process_delete(Hakkas *h, const char *id, const char *name,
					 HakkasMessage *msg);

/* graph:rename (02 §3.4): set the process's name (the id never changes).
 * Refused: "Invalid field: id" / "to", "Invalid id", "Unknown graph: <id>".
 * Reports PROCESS_RENAMED. */
enum hakkas_status hakkas_process_rename(Hakkas *h, const char *id, const char *name,
					 HakkasMessage *msg);

/* network:start / stop (02 §3.3): start or stop the process; a started one
 * is not restarted, a stopped one not stopped again, but the event is
 * reported either way (PROCESS_STARTED / PROCESS_STOPPED). Refused:
 * "Invalid field: graph", "Invalid id", "Unknown graph: <id>". */
enum hakkas_status hakkas_process_start(Hakkas *h, const char *id, HakkasMessage *msg);
enum hakkas_status hakkas_process_stop(Hakkas *h, const char *id, HakkasMessage *msg);

/* network:persist (02 §3.3): write the process, with its started state, to
 * <datadir>/graphs (05 §2.3). Refused: "Invalid field: graph", "Invalid
 * id", "Unknown graph: <id>". A failed save is HAKKAS_ERR_STORAGE and is
 * reported as an ERROR event "Unable to save graph: <name> (<id>)", like
 * the wire's broadcast. Success reports nothing. */
enum hakkas_status hakkas_process_persist(Hakkas *h, const char *id, HakkasMessage *msg);

/* A process's state (01 §2.2). The strings are valid until the next
 * command. */
typedef struct {
	const char *id;
	const char *name;
	const char *description;
	const char *icon;
	bool started;
	uint64_t uptime_ms;	/* of the current run; 0 when stopped */
	uint64_t time_started_ms; /* the start of the current or last run (0: never) */
} HakkasProcessStatus;

/* The process's state, read without an event (network:getstatus is a
 * request for a network:status broadcast; the host reads the state
 * directly). Refused: "Invalid field: graph", "Invalid id", "Unknown
 * graph: <id>". */
enum hakkas_status hakkas_process_status(Hakkas *h, const char *id, HakkasProcessStatus *out,
					 HakkasMessage *msg);

/* The processes in creation order (graph:list's order): their number, and
 * the i-th one's state (false if i is out of range). */
size_t hakkas_process_count(const Hakkas *h);
bool hakkas_process_at(const Hakkas *h, size_t i, HakkasProcessStatus *out);

/* ---- Processes: editing (02 §3.4) -------------------------------------------- */

/* graph:addnode: a block of type component named block (started at once if
 * the process is), with metadata (JSON object text {"x","y",...}; NULL:
 * x = y = 50). Fields: id (block), graph, component, metadata. Reports
 * BLOCK_ADDED. */
enum hakkas_status hakkas_block_add(Hakkas *h, const char *process, const char *block,
				    const char *component, const char *metadata,
				    HakkasMessage *msg);

/* graph:removenode: fields id (block), graph. Reports CONNECTION_REMOVED /
 * INITIAL_REMOVED / INPORT_REMOVED / OUTPORT_REMOVED for what was attached,
 * then BLOCK_REMOVED (02 §3.4). */
enum hakkas_status hakkas_block_remove(Hakkas *h, const char *process, const char *block,
				       HakkasMessage *msg);

/* graph:renamenode: fields from, to, graph. Reports BLOCK_RENAMED. */
enum hakkas_status hakkas_block_rename(Hakkas *h, const char *process, const char *from,
				       const char *to, HakkasMessage *msg);

/* graph:changenode: merge patch (JSON object text: "x"/"y" set or, null,
 * reset the position; another key null removes it, any other value sets
 * it) into the block's metadata. Fields id (block), metadata, graph; the
 * patch is checked before the process and block. Reports BLOCK_CHANGED with
 * the resulting metadata. */
enum hakkas_status hakkas_block_change(Hakkas *h, const char *process, const char *block,
				       const char *patch, HakkasMessage *msg);

/* graph:addedge: connect src_block.src_port (an output) to
 * tgt_block.tgt_port (an input), with metadata (JSON object text, NULL for
 * none). Fields src (block and port), tgt, graph, metadata. Reports
 * CONNECTION_ADDED. */
enum hakkas_status hakkas_connection_add(Hakkas *h, const char *process, const char *src_block,
					 const char *src_port, const char *tgt_block,
					 const char *tgt_port, const char *metadata,
					 HakkasMessage *msg);

/* graph:removeedge: fields src, tgt, graph. Reports CONNECTION_REMOVED. */
enum hakkas_status hakkas_connection_remove(Hakkas *h, const char *process,
					    const char *src_block, const char *src_port,
					    const char *tgt_block, const char *tgt_port,
					    HakkasMessage *msg);

/* graph:changeedge: replace the connection's metadata (JSON object text;
 * NULL removes it). Fields src, tgt, metadata, graph. Reports
 * CONNECTION_CHANGED. */
enum hakkas_status hakkas_connection_change(Hakkas *h, const char *process,
					    const char *src_block, const char *src_port,
					    const char *tgt_block, const char *tgt_port,
					    const char *metadata, HakkasMessage *msg);

/* graph:addinitial: wire the literal value to block.port (an input),
 * replacing a literal there. A string value is parsed for the port's type
 * (02 §3.4 "Initial value encoding"); a number or bool is used as it is.
 * Fields tgt (block and port), src (value), graph. Reports INITIAL_ADDED. */
enum hakkas_status hakkas_initial_add(Hakkas *h, const char *process, const char *block,
				      const char *port, const HakkasValue *value,
				      HakkasMessage *msg);

/* graph:removeinitial: fields tgt, graph. Reports INITIAL_REMOVED. */
enum hakkas_status hakkas_initial_remove(Hakkas *h, const char *process, const char *block,
					 const char *port, HakkasMessage *msg);

/* A process-level port's direction (01 §2.2 Port). */
enum hakkas_port_dir {
	HAKKAS_INPORT = 0,
	HAKKAS_OUTPORT = 1,
};

/* graph:addinport / addoutport: the public name mapped to block.port (an
 * input / an output). Fields graph, node (block), port, public (name).
 * Reports INPORT_ADDED / OUTPORT_ADDED. */
enum hakkas_status hakkas_port_add(Hakkas *h, const char *process, enum hakkas_port_dir dir,
				   const char *name, const char *block, const char *port,
				   HakkasMessage *msg);

/* graph:removeinport / removeoutport: fields graph, public. */
enum hakkas_status hakkas_port_remove(Hakkas *h, const char *process, enum hakkas_port_dir dir,
				      const char *name, HakkasMessage *msg);

/* graph:renameinport / renameoutport: fields graph, from, to. */
enum hakkas_status hakkas_port_rename(Hakkas *h, const char *process, enum hakkas_port_dir dir,
				      const char *from, const char *to, HakkasMessage *msg);

/* ---- Groups: composite block types (02 §3.4.2, 03 §5) ------------------------- */

/* graph:addgroup: "save as component" — a new block type name built from
 * the blocks (count names, at least one) of process, with description and
 * icon (NULL: ""), registered and saved to <datadir>/types. Fields graph,
 * name, nodes (blocks), metadata (description). Reports GROUP_ADDED, then
 * COMPONENT. */
enum hakkas_status hakkas_group_add(Hakkas *h, const char *process, const char *name,
				    const char *const *blocks, size_t count,
				    const char *description, const char *icon, HakkasMessage *msg);

/* graph:removegroup: fields graph, name. Reports GROUP_REMOVED. */
enum hakkas_status hakkas_group_remove(Hakkas *h, const char *process, const char *name,
				       HakkasMessage *msg);

/* graph:renamegroup: fields graph, from, to. Reports GROUP_RENAMED, then
 * COMPONENT. */
enum hakkas_status hakkas_group_rename(Hakkas *h, const char *process, const char *from,
				       const char *to, HakkasMessage *msg);

/* graph:changegroup: set the type's description and, if icon is not NULL,
 * its icon. Fields graph, name, metadata (description). Reports
 * GROUP_CHANGED, then COMPONENT. */
enum hakkas_status hakkas_group_change(Hakkas *h, const char *process, const char *name,
				       const char *description, const char *icon,
				       HakkasMessage *msg);

/* ---- Links between processes (01 §2.2 Port; plan task 21f) ------------------- */

/* A link from one process's outport to another's (or its own) inport: the
 * outport's inner output feeds the inport's inner input like a connection —
 * through the work queue, filtered by the source block's change threshold,
 * as the input's one source (no fan-in). A new link delivers the source's
 * current output value if the source process is started. While the source
 * is stopped the input keeps its last value; when the link goes (removed,
 * either process deleted, either port removed or renamed, also with its
 * block) LINK_REMOVED is reported and the input reverts to its default,
 * which propagates if its process is started. A deleted process's links
 * are removed after its stop and before PROCESS_DELETED (02 §1.1). Links
 * are not saved in graph files: they are the deployment's (the POSIX
 * host's process list, 05 §1.2). There is no wire command or message for
 * them yet: clients see nothing of a link but its values.
 *
 * Refusals (02 §6): "Invalid field: from" (from_process or from_outport
 * NULL), "Invalid field: to" (to_process or to_inport NULL), "Invalid id"
 * (any of the four empty or longer than HAKKAS_ID_MAX), "Unknown graph:
 * <process>" (the source, then the target), "Unknown port:
 * <process>.<port>" (no port of that process in either direction, source
 * first), "Invalid link direction" (from_outport is only an inport, or
 * to_inport only an outport), "Link exists", "Input already connected:
 * <to_process>.<to_inport>" (the inport's input has a connection, an
 * initial or another link); hakkas_link_remove: "No such link". Reports
 * LINK_ADDED / LINK_REMOVED. */
enum hakkas_status hakkas_link_add(Hakkas *h, const char *from_process, const char *from_outport,
				   const char *to_process, const char *to_inport,
				   HakkasMessage *msg);
enum hakkas_status hakkas_link_remove(Hakkas *h, const char *from_process,
				      const char *from_outport, const char *to_process,
				      const char *to_inport, HakkasMessage *msg);

/* A link, read without an event. The strings are valid until the next
 * command. */
typedef struct {
	const char *from_process;
	const char *from_outport;
	const char *to_process;
	const char *to_inport;
} HakkasLink;

/* The links in creation order: their number, and the i-th one (false if i
 * is out of range). Read-only (allowed from an event callback). */
size_t hakkas_link_count(const Hakkas *h);
bool hakkas_link_at(const Hakkas *h, size_t i, HakkasLink *out);

#endif
