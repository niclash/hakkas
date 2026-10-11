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
 * network:error. Plan task 18 adds the graph:* events (blocks, edges,
 * initials, ports, rename) to the same switch.
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

/* The ISO 8601 UTC timestamp (02 §3.3 "Timestamps") of the engine time ms,
 * counted from the epoch and floored to the second, e.g. 90000 ->
 * "1970-01-01T00:01:30Z". buf holds PROTO_TIME_SIZE bytes. */
void proto_msg_time(uint64_t ms, char buf[PROTO_TIME_SIZE]);

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

/* environment:challenge {"salt","iterations","nonce"} (02 §2.3). */
char *proto_msg_challenge(const AuthChallenge *ch);

/* environment:login {"secret","role","capabilities","server_signature"}
 * (02 §2.3). */
char *proto_msg_login(const char *secret, const AuthProof *proof);

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

/* The wire message of ev (see "Event mapping" above), or NULL. */
char *proto_msg_event(const GraphEvent *ev);

#endif
