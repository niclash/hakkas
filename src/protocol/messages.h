/*
 * The server's wire messages (02 §1, §3): the envelope, the error format,
 * the replies of the environment, runtime, component and trace commands,
 * and the mapping from the graphs registry's events (GraphEvent, 01 §4) to
 * the broadcasts of 02 §3.2-§3.3.
 *
 * Naming: functions are prefixed proto_msg_.
 *
 * Every message is one compact JSON object {"protocol","command","payload"}
 * built with cJSON and printed by graph_json_print, so every number that
 * carries an engine value is exact (graphs/graph_json.h). A server message
 * never has a "secret" member (02 §1). The two special shapes of 02 §1:
 * component:componentsready has a bare integer payload
 * (proto_msg_componentsready) and environment:logout has no payload at all
 * (proto_msg_logout).
 *
 * Event mapping (proto_msg_event): one GraphEvent becomes one wire message,
 * or none (NULL) for an event that has no wire form yet. Plan task 17 maps
 * the events its commands produce: GRAPH_CREATED -> runtime:new_graph,
 * GRAPH_DELETED -> runtime:delete_graph, GRAPH_STATUS -> network:status,
 * GRAPH_STARTED -> network:started, GRAPH_STOPPED -> network:stopped (a
 * started graph's delete_graph reports it first, 02 §3.2) and ERROR ->
 * network:error. Plan task 18 maps the graph:* events: GRAPH_RENAMED ->
 * graph:rename, BLOCK_ADDED/REMOVED/RENAMED/CHANGED -> graph:addnode,
 * removenode, renamenode, changenode, EDGE_ADDED/REMOVED/CHANGED ->
 * graph:addedge, removeedge, changeedge, INITIAL_ADDED/REMOVED ->
 * graph:addinitial, removeinitial, and the in/outport events ->
 * graph:add/remove/rename{in,out}port, with the broadcast shapes of 02 §3.4
 * (the request's "public" is the broadcast's "name"; an initial's data and a
 * network:data value as a string, linkable_stringify). Plan task 21b maps
 * the group events (02 §3.4.2): GROUP_ADDED -> graph:addgroup {"graph",
 * "name","nodes","metadata":{"description","icon"}} (nodes: the type's
 * inner blocks), GROUP_REMOVED -> graph:removegroup {"graph","name"},
 * GROUP_RENAMED -> graph:renamegroup {"graph","from","to"}, GROUP_CHANGED ->
 * graph:changegroup {"graph","name","metadata"}, and COMPONENT ->
 * component:component. The links between processes (LINK_ADDED/REMOVED,
 * plan task 21f) have no wire form yet: NULL, nothing is sent (02 §6).
 *
 * Every function returns a heap string to free with free(), or NULL when
 * memory runs out (or, for proto_msg_event, when the event has no wire
 * form).
 */
#ifndef HAKKAS_PROTOCOL_MESSAGES_H
#define HAKKAS_PROTOCOL_MESSAGES_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "graphs/block_type.h"
#include "graphs/graph.h"
#include "protocol/auth.h"

/* "1970-01-01T00:00:00Z" and its NUL. */
#define PROTO_TIME_SIZE 21

/* The ISO 8601 UTC timestamp (02 §3.3 "Timestamps") of the wall-clock time
 * ms (UTC ms since the epoch, 01 §3.4), floored to the second, e.g. 90000 ->
 * "1970-01-01T00:01:30Z"; a time before the epoch is the epoch. buf holds
 * PROTO_TIME_SIZE bytes. */
void proto_msg_time(int64_t ms, char buf[PROTO_TIME_SIZE]);

/* The envelope {"protocol","command","payload"} around payload, which it
 * takes over (freed in every case); payload NULL leaves the "payload" key
 * out (environment:logout only). */
char *proto_msg_build(const char *protocol, const char *command, cJSON *payload);

/* The array of the capability names of caps, in 02 §3.2.1 order. NULL if
 * memory runs out. */
cJSON *proto_msg_capabilities(uint32_t caps);

/* network:error {"stack":"","message":message[,"graph":graph]} (02 §1);
 * graph NULL leaves "graph" out. */
char *proto_msg_error(const char *message, const char *graph);

/* network:output {"type":"message","message":text} (02 §3.3, log mirroring,
 * 05 §4). */
char *proto_msg_output(const char *text);

/* environment:logout without a payload (02 §1). */
char *proto_msg_logout(void);

/* environment:challenge {"salt","iterations","nonce"} (02 §2.3); only
 * {"nonce"} for a resume (02 §2.3.1). */
char *proto_msg_challenge(const AuthChallenge *ch);

/* environment:login {"secret","role","capabilities","server_signature",
 * "resume"} (02 §2.3, §2.3.1). */
char *proto_msg_login(const char *secret, const char *resume, const AuthProof *proof);

/* runtime:runtime (02 §3.2.1): id, label, the build version,
 * allCapabilities, caps (none before login), type and namespace "hakkas",
 * repository and repositoryVersion "". */
char *proto_msg_runtime(const char *id, const char *label, uint32_t caps);

/* component:component for def (02 §3.5, graph_json_component). */
char *proto_msg_component(const BlockTypeDef *def);

/* component:componentsready with the bare integer count (02 §1). */
char *proto_msg_componentsready(size_t count);

/* component:source {"name","language":"json","library":"hakkas","code",
 * "tests":""} (02 §3.5). */
char *proto_msg_source(const char *name, const char *code);

/* trace:<command> {"graph"}, and for dump also "type":"flowtrace.json",
 * "flowtrace":"" (02 §3.6). */
char *proto_msg_trace(const char *command, const char *graph);

/* ---- graph:* (02 §3.4, the graph:connect snapshot of 02 §4.1.1) ---------------------- */

/* graph:clear {"id","name","main":true,"description","icon"}. */
char *proto_msg_graph_clear(const Graph *g);

/* graph:addnode {"graph","component","id","metadata"}: the block's full
 * metadata (x, y, the opaque keys); a placeholder's original component
 * name. */
char *proto_msg_addnode(const Graph *g, const Block *b);

/* graph:addedge {"graph","src","tgt"[,"metadata"]} for the link from
 * src.output to tgt.input; "metadata" only if the edge has any. */
char *proto_msg_addedge(const Graph *g, const Block *src, uint32_t output, const Block *tgt,
			uint32_t input);

/* graph:addinitial {"graph","src":{"data":"<v stringified>"},"tgt"}. */
char *proto_msg_addinitial(const Graph *g, const Block *b, uint32_t input, const Linkable *v);

/* graph:addinport / addoutport {"graph","name","node","port"}. */
char *proto_msg_addport(const Graph *g, enum graph_port_dir dir, const GraphPort *port);

/* network:data {"id":"unknown","graph","src","tgt","data":"<v stringified>"}
 * (02 §3.3, §4.2). */
char *proto_msg_data(const char *graph, const char *src_node, const char *src_port,
		     const char *tgt_node, const char *tgt_port, const Linkable *v);

/* The wire message of ev (see "Event mapping" above), or NULL. */
char *proto_msg_event(const GraphEvent *ev);

#endif
