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
	default:
		/* The graph:* events: plan task 18. */
		return NULL;
	}
}
