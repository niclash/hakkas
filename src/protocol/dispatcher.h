/*
 * The protocol dispatcher (02 §1-§3, plan task 17): one protocol session per
 * WebSocket connection, between the WebSocket server (engine/ws_server.h)
 * and the engine. It parses each client message's envelope, gates it on the
 * connection's secret and the session's capabilities through the Authorizer
 * interface (protocol/auth.h), runs the command, and turns the graphs
 * registry's events into the broadcasts every authenticated connection
 * receives (01 §4, 02 §1.1, §4.1).
 *
 * Naming: the layer is a Protocol (protocol_), a connection's session a
 * ProtoConn (protocol_conn_). Wire messages are built by protocol/messages.h.
 *
 * Composition (plan task 19): protocol_create from the graphs registry (its
 * runtime is gs->rt), the graph files, the configuration (engine_id, label,
 * log.mirror) and the Authorizer; block types come from the global registry
 * (blocktypes/block_types.h). Then
 *	ws_server_open_config(&loop, &cfg, protocol_ws_handler(), proto, &ws)
 * and, after loop_run: ws_server_close(ws), then protocol_destroy(proto).
 *
 * The connection is reached through a ProtoTransport (send, space, pending,
 * close), so tests drive the dispatcher in-process with a fake one;
 * protocol_ws_handler adapts it to a WsConn (protocol/protocol_ws.c).
 *
 * Message handling, in order (decided, plan task 17, 02 §1 "Implementation
 * decisions"):
 *  1. ops->activity for every message (the idle timeout, 02 §2.5 item 3).
 *  2. The envelope (02 §1): one JSON object (nothing but whitespace after
 *     it) whose "protocol" is one of the six names, "command" a string,
 *     "payload" an object and "secret" a string; other members are ignored.
 *     Otherwise network:error "Badly formatted request: <raw text>", the raw
 *     text cut to its first PROTO_RAW_ECHO_MAX bytes (at a character
 *     boundary, and at a U+0000) followed by "..." when cut.
 *  3. Secret gating (02 §2.5 item 5): a gated command (auth_command_rule)
 *     whose secret ops->validate_secret refuses: "Not authenticated" (the
 *     value is never echoed).
 *  4. A command not in 02 §3: the generic error "Unknown command:
 *     <protocol>:<command>" (the command cut to PROTO_COMMAND_ECHO_MAX
 *     bytes, "..." appended).
 *  5. Capabilities (02 §2.4): "Not permitted: <protocol>:<command>".
 *  6. The command. A missing or mistyped payload field is "Invalid field: <name>"
 *     (checked in the order 02 lists the fields, before any refusal of
 *     02 §3.4.1); of environment:challenge and login it is "Login failed"
 *     (02 §2.3).
 *
 * Errors carry "graph" only when the command named a valid graph id (02 §1).
 *
 * Replies and broadcasts (02 §1.1): runtime:new_graph and delete_graph, the
 * graph-mutating graph:* commands and network:start/stop are acknowledged
 * only through their broadcasts (the registry's events, mapped by
 * proto_msg_event), which reach every subscribed authenticated connection,
 * the sender included; a refusal reaches the sender only. graph:list,
 * network:getstatus and getruntime after login trigger network:status
 * broadcasts; network:persist has no reply on success (a failed save is a
 * broadcast network:error). getruntime, component:*, trace:*,
 * environment:*, graph:connect and network:edges reply to the sender.
 *
 * Group commands (02 §3.4.2, plan task 21b): graph:addgroup, removegroup,
 * renamegroup and changegroup run app/type_files.h's group commands with the
 * Protocol's GraphFiles; their events are the graph-specific graph:*group
 * broadcast, then the system-wide component:component of a new, renamed or
 * changed type.
 *
 * Graph commands (02 §3.4, decided in plan task 18): payload fields are
 * mapped onto the graphs_ commands (graphs/graphs.h), whose refusal messages
 * are sent as they are. Node metadata whose x or y is invalid is "Invalid
 * field: metadata" (for changenode checked before the node is looked up); an
 * initial's data is parsed for the target input's declared type
 * (graph_json_to_initial).
 *
 * graph:connect (02 §4.1.1): the snapshot (graph:clear, the blocks, the
 * inports and outports, the initials, the edges; never the retained entries
 * of placeholders) is built only up to the free outgoing space. If it fits,
 * the connection is re-scoped and the snapshot sent at once, atomically. If
 * not while the buffer holds data, it is deferred to on_drained
 * (PENDING_CONNECT) and made then, with the state of that moment; the scope
 * changes only when it is sent, so no event of the graph comes between the
 * re-scope and the snapshot. If it does not fit even an empty buffer:
 * "Graph too large to send: <id>" and the scope stays.
 *
 * network:edges (02 §4.2): the request replaces the connection's whole set
 * of live watches (graphs/graph.h InputWatch) on existing tgt inputs; the
 * payload is echoed, then each watch sends network:data with the current
 * value and again on every update. A network:data that does not fit the
 * free space above the reserve, or comes while a burst is deferred, marks
 * its watch dirty; the value at on_drained is sent then (coalesced). The
 * set ends on replacement, logout, expiry and close; a watch ends with its
 * block.
 *
 * Reserve (02 §0 "Outgoing buffer", §4.2; decided, plan task 20 fix): the
 * lossy messages, network:data and the log mirror's lines, are sent only if
 * they leave at least the reserve free (send_capacity /
 * PROTO_DATA_RESERVE_DIV, a quarter: 16 KiB of the default 64 KiB). Replies
 * and events may use it, so a client that stops reading while values
 * stream is not closed by its own next reply or the next event. While a
 * burst is deferred, no lossy message is sent at all (values coalesce, log
 * lines are dropped), so a stream cannot keep the buffer from draining:
 * at on_drained the deferred burst goes first, the dirty values after it.
 *
 * Event delivery (02 §2.5 item 6, §4.1): a connection is a graphs subscriber
 * from its login until its logout, expiry or close, and an event is only
 * delivered while ops->capabilities_for is non-zero (so not after a session's
 * deadline, before the tick hook ends it). A login starts unscoped (every
 * graph); protocol_conn_scope is graph:connect's re-scoping.
 * Each event's message is built once for all connections.
 *
 * Bursts (02 §0 "Outgoing buffer"): the getruntime and component:list
 * replies are sent whole only if they fit the free space (send_space, with
 * PROTO_FRAME_OVERHEAD per message); otherwise the reply is made again on
 * on_drained (protocol_conn_drained), when the buffer is empty. A reply that
 * does not fit even an empty buffer is network:error "Reply too large".
 * Other replies meanwhile may overtake the deferred one.
 *
 * Log mirroring (05 §4, 02 §3.3): with log.mirror, a log sink sends each log
 * line, as formatted, to every authenticated connection: network:error
 * {"stack":"","message"} for error, else network:output {"type":"message",
 * "message"}. A line is dropped for a connection whose outgoing buffer
 * cannot hold it above the reserve, or while a burst is deferred
 * (mirroring never closes a connection). A line logged while
 * the layer is sending (from inside the transport) or mirroring is dropped,
 * so mirroring never recurses.
 *
 * Threading: engine thread only.
 */
#ifndef HAKKAS_PROTOCOL_DISPATCHER_H
#define HAKKAS_PROTOCOL_DISPATCHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/graph_files.h"
#include "engine/ws_server.h"
#include "graphs/graphs.h"
#include "protocol/auth.h"
#include "system/config.h"

/* Bytes of the raw text echoed in "Badly formatted request: ...". */
#define PROTO_RAW_ECHO_MAX 256
/* Bytes of a command echoed in the generic error. */
#define PROTO_COMMAND_ECHO_MAX 64
/* What a burst reserves per message beyond its text (a WebSocket frame
 * header is at most 10 bytes). */
#define PROTO_FRAME_OVERHEAD 10
/* The reserve of the outgoing buffer that lossy traffic (network:data, the
 * log mirror) never takes: send_capacity / PROTO_DATA_RESERVE_DIV (see
 * "Reserve" above). A per-build override. */
#ifndef PROTO_DATA_RESERVE_DIV
#define PROTO_DATA_RESERVE_DIV 4
#endif

/* A connection, as the dispatcher sees it; conn is the transport's own. */
typedef struct {
	/* Sends one text message. False if it was not sent: the connection is
	 * then closing (e.g. the message did not fit, 02 §0). */
	bool (*send_text)(void *conn, const char *text, size_t len);
	/* The longest message send_text takes now. */
	size_t (*send_space)(void *conn);
	/* The longest message send_text takes into the empty buffer. */
	size_t (*send_capacity)(void *conn);
	/* Bytes waiting to be written; 0 means no on_drained will follow. */
	size_t (*send_pending)(void *conn);
	/* Starts closing with a WebSocket close code and reason. */
	void (*close)(void *conn, uint16_t code, const char *reason);
} ProtoTransport;

typedef struct {
	Graphs *graphs;	   /* borrowed, must outlive the Protocol */
	GraphFiles *files; /* borrowed: delete_graph, persist and the group commands */
	/* Read at creation: engine_id (must be set, config_generate_engine_id),
	 * label, log.mirror. */
	const Config *cfg;
	Authorizer auth;
} ProtocolConfig;

typedef struct Protocol Protocol;
typedef struct ProtoConn ProtoConn;

/* Makes the protocol layer; with log.mirror it registers its log sink.
 * False (logged) for an invalid cfg (a NULL member, no engine_id), out of
 * memory, or no free log sink. */
bool protocol_create(const ProtocolConfig *cfg, Protocol **out);

/* Removes the log sink and frees the layer and any connection still open
 * (closing its session and its network:edges watches). After
 * ws_server_close; before graphs_deinit, or after it (the watches are then
 * detached already). NULL is ignored. */
void protocol_destroy(Protocol *p);

/* The WebSocket server's callbacks; their ctx is the Protocol. */
const WsHandler *protocol_ws_handler(void);

/* ---- Sessions (one per connection) ----------------------------------------------- */

/* A connection opened (on_open): its session, with ops->open for peer_addr.
 * NULL (out of memory, or the authorizer refused): close the connection. */
ProtoConn *protocol_conn_open(Protocol *p, const ProtoTransport *t, void *conn,
			      const char *peer_addr);

/* One client message of len bytes, NUL-terminated after them (on_message). */
void protocol_conn_message(ProtoConn *c, const char *text, size_t len);

/* The outgoing buffer has been written out (on_drained): a deferred reply
 * is made now. */
void protocol_conn_drained(ProtoConn *c);

/* The connection is gone (on_close): its session ends and c is freed. NULL
 * is ignored. */
void protocol_conn_close(ProtoConn *c);

/* Scopes c's graph-specific events to graph_id, or to every graph for NULL
 * (02 §4.1; graph:connect does it with its snapshot). False for an invalid
 * id. */
bool protocol_conn_scope(ProtoConn *c, const char *graph_id);

#endif
