/*
 * The server's wire messages (02 §1, §3). See protocol/messages.h.
 */
#include "protocol/messages.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/version.h"
#include "graphs/graph_json.h"
#include "protocol/users.h"

/* Add item under key to obj; on failure (item NULL or no memory) item is
 * freed and false returned. */
static bool add(cJSON *obj, const char *key, cJSON *item)
{
	if (item == NULL)
		return false;
	if (!cJSON_AddItemToObject(obj, key, item)) {
		cJSON_Delete(item);
		return false;
	}
	return true;
}

static bool add_str(cJSON *obj, const char *key, const char *s)
{
	return add(obj, key, cJSON_CreateString(s != NULL ? s : ""));
}

static bool add_num(cJSON *obj, const char *key, double v)
{
	return add(obj, key, cJSON_CreateNumber(v));
}

static bool add_bool(cJSON *obj, const char *key, bool b)
{
	return add(obj, key, cJSON_CreateBool(b));
}

/* ---- Time (02 §3.3 "Timestamps") ----------------------------------------------------- */

void proto_msg_time(uint64_t ms, char buf[PROTO_TIME_SIZE])
{
	uint64_t s = ms / 1000;
	uint64_t days = s / 86400;
	uint32_t rem = (uint32_t)(s % 86400);
	/* The proleptic Gregorian date of day number days since 1970-01-01
	 * (H. Hinnant's civil_from_days, for non-negative days). */
	uint64_t z = days + 719468;
	uint64_t era = z / 146097;
	uint64_t doe = z - era * 146097;
	uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	uint64_t y = yoe + era * 400;
	uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint64_t mp = (5 * doy + 2) / 153;
	unsigned d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
	unsigned m = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
	if (m <= 2)
		y++;
	if (y > 9999) { /* beyond the format; never reached by an uptime */
		snprintf(buf, PROTO_TIME_SIZE, "9999-12-31T23:59:59Z");
		return;
	}
	/* Every field is in range, so the text is 20 characters; the larger
	 * buffer only keeps the compiler's truncation check quiet. */
	char text[48];
	snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", (unsigned)y, m, d,
		 (unsigned)(rem / 3600), (unsigned)(rem / 60 % 60), (unsigned)(rem % 60));
	memcpy(buf, text, PROTO_TIME_SIZE - 1);
	buf[PROTO_TIME_SIZE - 1] = '\0';
}

/* ---- The envelope (02 §1) -------------------------------------------------------------- */

char *proto_msg_build(const char *protocol, const char *command, cJSON *payload)
{
	cJSON *msg = cJSON_CreateObject();
	char *text = NULL;
	if (msg == NULL || !add_str(msg, "protocol", protocol) ||
	    !add_str(msg, "command", command)) {
		cJSON_Delete(payload);
		goto out;
	}
	if (payload != NULL && !add(msg, "payload", payload))
		goto out;
	text = graph_json_print(msg);
out:
	cJSON_Delete(msg);
	return text;
}

cJSON *proto_msg_capabilities(uint32_t caps)
{
	cJSON *arr = cJSON_CreateArray();
	if (arr == NULL)
		return NULL;
	for (unsigned i = 0; i < AUTH_CAP_COUNT; i++) {
		if ((caps & (1u << i)) == 0)
			continue;
		cJSON *name = cJSON_CreateString(auth_capability_name(i));
		if (name == NULL || !cJSON_AddItemToArray(arr, name)) {
			cJSON_Delete(name);
			cJSON_Delete(arr);
			return NULL;
		}
	}
	return arr;
}

/* An empty payload object, or NULL. */
static cJSON *object(void)
{
	return cJSON_CreateObject();
}

/* Build protocol:command around payload if ok, else free payload and give
 * NULL (a member could not be added). */
static char *finish(const char *protocol, const char *command, cJSON *payload, bool ok)
{
	if (!ok) {
		cJSON_Delete(payload);
		return NULL;
	}
	return proto_msg_build(protocol, command, payload);
}

char *proto_msg_error(const char *message, const char *graph)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "stack", "") && add_str(p, "message", message);
	if (ok && graph != NULL)
		ok = add_str(p, "graph", graph);
	return finish("network", "error", p, ok);
}

char *proto_msg_output(const char *text)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "type", "message") && add_str(p, "message", text);
	return finish("network", "output", p, ok);
}

char *proto_msg_logout(void)
{
	return proto_msg_build("environment", "logout", NULL);
}

char *proto_msg_challenge(const AuthChallenge *ch)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "salt", ch->salt) &&
		  add_num(p, "iterations", ch->iterations) && add_str(p, "nonce", ch->nonce);
	return finish("environment", "challenge", p, ok);
}

char *proto_msg_login(const char *secret, const AuthProof *proof)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "secret", secret) &&
		  add_str(p, "role", users_role_name(proof->role)) &&
		  add(p, "capabilities", proto_msg_capabilities(proof->capabilities)) &&
		  add_str(p, "server_signature", proof->server_signature);
	return finish("environment", "login", p, ok);
}

char *proto_msg_runtime(const char *id, const char *label, uint32_t caps)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "id", id) && add_str(p, "label", label) &&
		  add_str(p, "version", hakkas_version()) &&
		  add(p, "allCapabilities", proto_msg_capabilities(AUTH_CAPS_ALL)) &&
		  add(p, "capabilities", proto_msg_capabilities(caps)) &&
		  add_str(p, "type", "hakkas") && add_str(p, "namespace", "hakkas") &&
		  add_str(p, "repository", "") && add_str(p, "repositoryVersion", "");
	return finish("runtime", "runtime", p, ok);
}

char *proto_msg_component(const BlockTypeDef *def)
{
	cJSON *p = graph_json_component(def);
	return finish("component", "component", p, p != NULL);
}

char *proto_msg_componentsready(size_t count)
{
	cJSON *n = cJSON_CreateNumber((double)count);
	return finish("component", "componentsready", n, n != NULL);
}

char *proto_msg_source(const char *name, const char *code)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "name", name) && add_str(p, "language", "json") &&
		  add_str(p, "library", "hakkas") && add_str(p, "code", code) &&
		  add_str(p, "tests", "");
	return finish("component", "source", p, ok);
}

char *proto_msg_trace(const char *command, const char *graph)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", graph);
	if (ok && strcmp(command, "dump") == 0)
		ok = add_str(p, "type", "flowtrace.json") && add_str(p, "flowtrace", "");
	return finish("trace", command, p, ok);
}

/* ---- Events (01 §4, 02 §3.2-§3.3, §4.1) -------------------------------------------- */

/* Whole seconds of an uptime (02 §3.3 "uptime": int seconds). */
static double seconds(uint64_t ms)
{
	return (double)(ms / 1000);
}

static char *graph_created(const Graph *g)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", g->id) && add_str(p, "name", g->name) &&
		  add_str(p, "description", g->description) && add_str(p, "icon", g->icon);
	return finish("runtime", "new_graph", p, ok);
}

static char *graph_deleted(const Graph *g)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", g->id) && add_str(p, "name", g->name);
	return finish("runtime", "delete_graph", p, ok);
}

static char *graph_status(const Graph *g, const GraphEvent *ev)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", g->id) && add_str(p, "name", g->name) &&
		  add_str(p, "description", g->description) &&
		  add_num(p, "uptime", seconds(ev->uptime_ms)) &&
		  add_bool(p, "running", g->started) && add_bool(p, "started", g->started) &&
		  add_bool(p, "debug", false);
	return finish("network", "status", p, ok);
}

static char *graph_started(const Graph *g, const GraphEvent *ev)
{
	char time[PROTO_TIME_SIZE];
	proto_msg_time(ev->time_ms, time);
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", g->id) && add_str(p, "time", time) &&
		  add_bool(p, "started", true) && add_bool(p, "running", true) &&
		  add_bool(p, "debug", false);
	return finish("network", "started", p, ok);
}

static char *graph_stopped(const Graph *g, const GraphEvent *ev)
{
	char time[PROTO_TIME_SIZE];
	proto_msg_time(ev->time_ms, time);
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", g->id) && add_str(p, "time", time) &&
		  add_num(p, "uptime", seconds(ev->uptime_ms)) && add_bool(p, "started", false) &&
		  add_bool(p, "running", false) && add_bool(p, "debug", false);
	return finish("network", "stopped", p, ok);
}

/* ---- graph:* messages (02 §3.4, §4.1.1) ------------------------------------------- */

/* {"node","port"}, or NULL if memory runs out. */
static cJSON *endpoint(const char *node, const char *port)
{
	cJSON *e = cJSON_CreateObject();
	if (e == NULL || !add_str(e, "node", node) || !add_str(e, "port", port)) {
		cJSON_Delete(e);
		return NULL;
	}
	return e;
}

static const char *input_name(const Block *b, uint32_t input)
{
	return block_input_port(b, input)->name;
}

static const char *output_name(const Block *b, uint32_t output)
{
	return block_output_port(b, output)->name;
}

/* v's text form (linkable_stringify) in small, or in a heap string for a
 * longer one (*heap, to free). NULL if memory runs out. */
static const char *stringified(const Linkable *v, char small[LINKABLE_NUMBER_TEXT_MAX],
			       char **heap)
{
	*heap = NULL;
	size_t len = linkable_stringify(v, small, LINKABLE_NUMBER_TEXT_MAX);
	if (len < LINKABLE_NUMBER_TEXT_MAX)
		return small;
	*heap = malloc(len + 1);
	if (*heap != NULL)
		linkable_stringify(v, *heap, len + 1);
	return *heap;
}

/* Add v stringified under key (02 §3.4, §3.3: values go out as strings). */
static bool add_value(cJSON *obj, const char *key, const Linkable *v)
{
	char small[LINKABLE_NUMBER_TEXT_MAX], *heap;
	const char *text = stringified(v, small, &heap);
	bool ok = text != NULL && add_str(obj, key, text);
	free(heap);
	return ok;
}

/* {"data":"<v stringified>"}, or NULL. */
static cJSON *data_object(const Linkable *v)
{
	cJSON *d = cJSON_CreateObject();
	if (d == NULL || !add_value(d, "data", v)) {
		cJSON_Delete(d);
		return NULL;
	}
	return d;
}

char *proto_msg_graph_clear(const Graph *g)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "id", g->id) && add_str(p, "name", g->name) &&
		  add_bool(p, "main", true) && add_str(p, "description", g->description) &&
		  add_str(p, "icon", g->icon);
	return finish("graph", "clear", p, ok);
}

char *proto_msg_addnode(const Graph *g, const Block *b)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", g->id) &&
		  add_str(p, "component", block_component(b)) && add_str(p, "id", b->name) &&
		  add(p, "metadata", graph_json_from_metadata(&b->metadata));
	return finish("graph", "addnode", p, ok);
}

char *proto_msg_addedge(const Graph *g, const Block *src, uint32_t output, const Block *tgt,
			uint32_t input)
{
	const char *md = tgt->inputs[input].edge_metadata;
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", g->id) &&
		  add(p, "src", endpoint(src->name, output_name(src, output))) &&
		  add(p, "tgt", endpoint(tgt->name, input_name(tgt, input)));
	if (ok && md != NULL)
		ok = add(p, "metadata", graph_json_from_edge_metadata(md));
	return finish("graph", "addedge", p, ok);
}

char *proto_msg_addinitial(const Graph *g, const Block *b, uint32_t input, const Linkable *v)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", g->id) && add(p, "src", data_object(v)) &&
		  add(p, "tgt", endpoint(b->name, input_name(b, input)));
	return finish("graph", "addinitial", p, ok);
}

char *proto_msg_addport(const Graph *g, enum graph_port_dir dir, const GraphPort *port)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", g->id) && add_str(p, "name", port->name) &&
		  add_str(p, "node", port->block->name) &&
		  add_str(p, "port", graph_port_inner_name(dir, port));
	return finish("graph", dir == GRAPH_INPORT ? "addinport" : "addoutport", p, ok);
}

char *proto_msg_data(const char *graph, const char *src_node, const char *src_port,
		     const char *tgt_node, const char *tgt_port, const Linkable *v)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "id", "unknown") && add_str(p, "graph", graph) &&
		  add(p, "src", endpoint(src_node, src_port)) &&
		  add(p, "tgt", endpoint(tgt_node, tgt_port)) && add_value(p, "data", v);
	return finish("network", "data", p, ok);
}

/* The events' own shapes (02 §3.4 "broadcast"). */

static char *graph_renamed(const GraphEvent *ev)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "id", ev->graph->id) &&
		  add_str(p, "from", ev->old_name) && add_str(p, "to", ev->graph->name);
	return finish("graph", "rename", p, ok);
}

static char *node_removed(const GraphEvent *ev)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "id", ev->block->name) &&
		  add_str(p, "graph", ev->graph->id);
	return finish("graph", "removenode", p, ok);
}

/* graph:renamenode, rename{in,out}port: {"graph","from","to"}. */
static char *renamed(const char *command, const GraphEvent *ev, const char *to)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", ev->graph->id) &&
		  add_str(p, "from", ev->old_name) && add_str(p, "to", to);
	return finish("graph", command, p, ok);
}

static char *node_changed(const GraphEvent *ev)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "id", ev->block->name) &&
		  add_str(p, "graph", ev->graph->id) &&
		  add(p, "metadata", graph_json_from_metadata(&ev->block->metadata));
	return finish("graph", "changenode", p, ok);
}

/* graph:removeedge {"graph","src","tgt"}; graph:changeedge also "metadata",
 * the edge's resulting metadata, {} for none (decided, plan task 18). */
static char *edge_event(const char *command, const GraphEvent *ev, bool with_metadata)
{
	const Block *src = ev->src_block, *tgt = ev->block;
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", ev->graph->id) &&
		  add(p, "src", endpoint(src->name, output_name(src, ev->src_output))) &&
		  add(p, "tgt", endpoint(tgt->name, input_name(tgt, ev->input)));
	if (ok && with_metadata) {
		const char *md = tgt->inputs[ev->input].edge_metadata;
		ok = add(p, "metadata",
			 md != NULL ? graph_json_from_edge_metadata(md) : cJSON_CreateObject());
	}
	return finish("graph", command, p, ok);
}

static char *initial_removed(const GraphEvent *ev)
{
	cJSON *p = object();
	bool ok = p != NULL && add(p, "src", data_object(ev->value)) &&
		  add(p, "tgt", endpoint(ev->block->name, input_name(ev->block, ev->input))) &&
		  add_str(p, "graph", ev->graph->id);
	return finish("graph", "removeinitial", p, ok);
}

static char *port_removed(const char *command, const GraphEvent *ev)
{
	cJSON *p = object();
	bool ok = p != NULL && add_str(p, "graph", ev->graph->id) &&
		  add_str(p, "name", ev->port->name);
	return finish("graph", command, p, ok);
}

char *proto_msg_event(const GraphEvent *ev)
{
	switch (ev->type) {
	case GRAPH_EVENT_GRAPH_CREATED:
		return graph_created(ev->graph);
	case GRAPH_EVENT_GRAPH_DELETED:
		return graph_deleted(ev->graph);
	case GRAPH_EVENT_GRAPH_STATUS:
		return graph_status(ev->graph, ev);
	case GRAPH_EVENT_GRAPH_STARTED:
		return graph_started(ev->graph, ev);
	case GRAPH_EVENT_GRAPH_STOPPED:
		return graph_stopped(ev->graph, ev);
	case GRAPH_EVENT_ERROR:
		return proto_msg_error(ev->message, ev->graph_id);
	case GRAPH_EVENT_GRAPH_RENAMED:
		return graph_renamed(ev);
	case GRAPH_EVENT_BLOCK_ADDED:
		return proto_msg_addnode(ev->graph, ev->block);
	case GRAPH_EVENT_BLOCK_REMOVED:
		return node_removed(ev);
	case GRAPH_EVENT_BLOCK_RENAMED:
		return renamed("renamenode", ev, ev->block->name);
	case GRAPH_EVENT_BLOCK_CHANGED:
		return node_changed(ev);
	case GRAPH_EVENT_EDGE_ADDED:
		return proto_msg_addedge(ev->graph, ev->src_block, ev->src_output, ev->block,
					 ev->input);
	case GRAPH_EVENT_EDGE_REMOVED:
		return edge_event("removeedge", ev, false);
	case GRAPH_EVENT_EDGE_CHANGED:
		return edge_event("changeedge", ev, true);
	case GRAPH_EVENT_INITIAL_ADDED:
		return proto_msg_addinitial(ev->graph, ev->block, ev->input, ev->value);
	case GRAPH_EVENT_INITIAL_REMOVED:
		return initial_removed(ev);
	case GRAPH_EVENT_INPORT_ADDED:
		return proto_msg_addport(ev->graph, GRAPH_INPORT, ev->port);
	case GRAPH_EVENT_INPORT_REMOVED:
		return port_removed("removeinport", ev);
	case GRAPH_EVENT_INPORT_RENAMED:
		return renamed("renameinport", ev, ev->port->name);
	case GRAPH_EVENT_OUTPORT_ADDED:
		return proto_msg_addport(ev->graph, GRAPH_OUTPORT, ev->port);
	case GRAPH_EVENT_OUTPORT_REMOVED:
		return port_removed("removeoutport", ev);
	case GRAPH_EVENT_OUTPORT_RENAMED:
		return renamed("renameoutport", ev, ev->port->name);
	}
	return NULL;
}
