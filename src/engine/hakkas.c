/*
 * The local C API (02 §6). See hakkas.h.
 */
#include "hakkas.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/graph_files.h"
#include "app/type_files.h"
#include "cJSON.h"
#include "engine/engine.h"
#include "graphs/graph_json.h"
#include "graphs/graphs.h"
#include "platform/pal.h"
#include "system/log.h"

typedef struct {
	hakkas_event_fn fn; /* NULL: unsubscribed during a delivery */
	void *ctx;
} Subscription;

struct Hakkas {
	Engine *engine;
	GraphsSubscriber sub; /* the host's one subscriber of the registry */
	Subscription subs[HAKKAS_SUBSCRIBERS_MAX];
	size_t sub_count;
	unsigned delivering; /* > 0 while an event is delivered */
	bool compact;	     /* an entry was unsubscribed during a delivery */
};

const char *hakkas_status_str(enum hakkas_status st)
{
	switch (st) {
	case HAKKAS_OK:
		return "ok";
	case HAKKAS_REFUSED:
		return "refused";
	case HAKKAS_ERR_NO_MEMORY:
		return "out of memory";
	case HAKKAS_ERR_STORAGE:
		return "storage error";
	case HAKKAS_ERR_NOT_FOUND:
		return "not found";
	case HAKKAS_ERR_INVALID:
		return "invalid path";
	case HAKKAS_ERR_TOO_BIG:
		return "file too big";
	case HAKKAS_ERR_MALFORMED:
		return "malformed graph file";
	case HAKKAS_ERR_EXISTS:
		return "process exists";
	case HAKKAS_ERR_BUSY:
		return "not allowed during event delivery";
	}
	return "?";
}

/* ---- Messages and statuses -------------------------------------------------------- */

static void set_msg(HakkasMessage *msg, const char *text)
{
	if (msg != NULL)
		snprintf(msg->text, sizeof(msg->text), "%s", text);
}

/* "Invalid field: <name>" (02 §1.2): a missing argument or invalid
 * metadata. */
static enum hakkas_status invalid_field(HakkasMessage *msg, const char *name)
{
	if (msg != NULL)
		snprintf(msg->text, sizeof(msg->text), "Invalid field: %s", name);
	return HAKKAS_REFUSED;
}

/* A graphs_ command's result. */
static enum hakkas_status from_graph(enum graph_status st, const GraphsMessage *gm,
				     HakkasMessage *msg)
{
	set_msg(msg, st == GRAPH_OK ? "" : gm->text);
	if (st == GRAPH_OK)
		return HAKKAS_OK;
	return st == GRAPH_ERR_NO_MEMORY ? HAKKAS_ERR_NO_MEMORY : HAKKAS_REFUSED;
}

/* A refusal rendered by the registry (graph_refusal_message). */
static enum hakkas_status refusal(enum graph_status st, const GraphRefusal *what,
				  HakkasMessage *msg)
{
	if (msg != NULL)
		graph_refusal_message(st, what, msg->text, sizeof(msg->text));
	return st == GRAPH_ERR_NO_MEMORY ? HAKKAS_ERR_NO_MEMORY : HAKKAS_REFUSED;
}

static enum hakkas_status no_memory(HakkasMessage *msg)
{
	return refusal(GRAPH_ERR_NO_MEMORY, NULL, msg);
}

/* A command of app/ (persist, the group commands): a refusal is
 * GRAPH_FILES_ERR_INVALID or _NOT_FOUND, a failed write _STORAGE. */
static enum hakkas_status from_files(enum graph_files_status st, const GraphsMessage *gm,
				     HakkasMessage *msg)
{
	set_msg(msg, st == GRAPH_FILES_OK ? "" : gm->text);
	switch (st) {
	case GRAPH_FILES_OK:
		return HAKKAS_OK;
	case GRAPH_FILES_ERR_STORAGE:
		return HAKKAS_ERR_STORAGE;
	case GRAPH_FILES_ERR_NO_MEMORY:
		return HAKKAS_ERR_NO_MEMORY;
	default:
		return HAKKAS_REFUSED;
	}
}

/* A load's result (graph_files_load). */
static enum hakkas_status from_load(enum graph_files_status st)
{
	switch (st) {
	case GRAPH_FILES_OK:
		return HAKKAS_OK;
	case GRAPH_FILES_ERR_INVALID:
		return HAKKAS_ERR_INVALID;
	case GRAPH_FILES_ERR_NOT_FOUND:
		return HAKKAS_ERR_NOT_FOUND;
	case GRAPH_FILES_ERR_TOO_BIG:
		return HAKKAS_ERR_TOO_BIG;
	case GRAPH_FILES_ERR_MALFORMED:
		return HAKKAS_ERR_MALFORMED;
	case GRAPH_FILES_ERR_EXISTS:
		return HAKKAS_ERR_EXISTS;
	case GRAPH_FILES_ERR_STORAGE:
		return HAKKAS_ERR_STORAGE;
	case GRAPH_FILES_ERR_NO_MEMORY:
		return HAKKAS_ERR_NO_MEMORY;
	}
	return HAKKAS_ERR_STORAGE;
}

static Graphs *graphs_of(const Hakkas *h)
{
	return engine_graphs(h->engine);
}

static GraphFiles *files_of(const Hakkas *h)
{
	return engine_files(h->engine);
}

/* Every command that changes the model starts here: refused during an
 * event delivery (01 §4), else the engine time is brought up to date, as
 * the loop does before it dispatches a command (01 §3.4). */
static bool enter(Hakkas *h, HakkasMessage *msg)
{
	if (h->delivering > 0) {
		set_msg(msg, hakkas_status_str(HAKKAS_ERR_BUSY));
		return false;
	}
	runtime_set_now(graphs_of(h)->rt, pal_uptime_ms());
	set_msg(msg, "");
	return true;
}

/* JSON text given for a metadata argument: true with *out the parsed item
 * (NULL for NULL text or a JSON null, the same as absent, 02 §1.2), false
 * if it is not one JSON value (or out of memory). The caller deletes it. */
static bool parse_json(const char *text, cJSON **out)
{
	*out = NULL;
	if (text == NULL)
		return true;
	const char *end = NULL;
	cJSON *v = cJSON_ParseWithOpts(text, &end, true);
	if (v == NULL)
		return false;
	if (cJSON_IsNull(v)) {
		cJSON_Delete(v);
		return true;
	}
	*out = v;
	return true;
}

/* ---- The runtime ---------------------------------------------------------------- */

/* The registry's events, as typed events (hakkas.h "An event"). */
static void convert(const GraphEvent *ev, HakkasEvent *out)
{
	static const enum hakkas_event_type types[] = {
		[GRAPH_EVENT_BLOCK_ADDED] = HAKKAS_EVENT_BLOCK_ADDED,
		[GRAPH_EVENT_BLOCK_REMOVED] = HAKKAS_EVENT_BLOCK_REMOVED,
		[GRAPH_EVENT_BLOCK_RENAMED] = HAKKAS_EVENT_BLOCK_RENAMED,
		[GRAPH_EVENT_BLOCK_CHANGED] = HAKKAS_EVENT_BLOCK_CHANGED,
		[GRAPH_EVENT_EDGE_ADDED] = HAKKAS_EVENT_CONNECTION_ADDED,
		[GRAPH_EVENT_EDGE_REMOVED] = HAKKAS_EVENT_CONNECTION_REMOVED,
		[GRAPH_EVENT_EDGE_CHANGED] = HAKKAS_EVENT_CONNECTION_CHANGED,
		[GRAPH_EVENT_INITIAL_ADDED] = HAKKAS_EVENT_INITIAL_ADDED,
		[GRAPH_EVENT_INITIAL_REMOVED] = HAKKAS_EVENT_INITIAL_REMOVED,
		[GRAPH_EVENT_INPORT_ADDED] = HAKKAS_EVENT_INPORT_ADDED,
		[GRAPH_EVENT_INPORT_REMOVED] = HAKKAS_EVENT_INPORT_REMOVED,
		[GRAPH_EVENT_INPORT_RENAMED] = HAKKAS_EVENT_INPORT_RENAMED,
		[GRAPH_EVENT_OUTPORT_ADDED] = HAKKAS_EVENT_OUTPORT_ADDED,
		[GRAPH_EVENT_OUTPORT_REMOVED] = HAKKAS_EVENT_OUTPORT_REMOVED,
		[GRAPH_EVENT_OUTPORT_RENAMED] = HAKKAS_EVENT_OUTPORT_RENAMED,
		[GRAPH_EVENT_GRAPH_CREATED] = HAKKAS_EVENT_PROCESS_CREATED,
		[GRAPH_EVENT_GRAPH_DELETED] = HAKKAS_EVENT_PROCESS_DELETED,
		[GRAPH_EVENT_GRAPH_RENAMED] = HAKKAS_EVENT_PROCESS_RENAMED,
		[GRAPH_EVENT_GRAPH_STARTED] = HAKKAS_EVENT_PROCESS_STARTED,
		[GRAPH_EVENT_GRAPH_STOPPED] = HAKKAS_EVENT_PROCESS_STOPPED,
		[GRAPH_EVENT_GRAPH_STATUS] = HAKKAS_EVENT_PROCESS_STATUS,
		[GRAPH_EVENT_ERROR] = HAKKAS_EVENT_ERROR,
		[GRAPH_EVENT_GROUP_ADDED] = HAKKAS_EVENT_GROUP_ADDED,
		[GRAPH_EVENT_GROUP_REMOVED] = HAKKAS_EVENT_GROUP_REMOVED,
		[GRAPH_EVENT_GROUP_RENAMED] = HAKKAS_EVENT_GROUP_RENAMED,
		[GRAPH_EVENT_GROUP_CHANGED] = HAKKAS_EVENT_GROUP_CHANGED,
		[GRAPH_EVENT_COMPONENT] = HAKKAS_EVENT_COMPONENT,
		[GRAPH_EVENT_LINK_ADDED] = HAKKAS_EVENT_LINK_ADDED,
		[GRAPH_EVENT_LINK_REMOVED] = HAKKAS_EVENT_LINK_REMOVED,
	};
	memset(out, 0, sizeof(*out));
	out->type = types[ev->type];
	const Graph *g = ev->graph;
	const Block *b = ev->block;
	const BlockTypeDef *def = ev->block_type;
	out->process = g != NULL ? g->id : NULL;
	switch (ev->type) {
	case GRAPH_EVENT_GRAPH_CREATED:
		out->name = g->name;
		out->description = g->description;
		out->icon = g->icon;
		break;
	case GRAPH_EVENT_GRAPH_STATUS:
		out->name = g->name;
		out->description = g->description;
		out->started = g->started;
		out->uptime_ms = ev->uptime_ms;
		break;
	case GRAPH_EVENT_GRAPH_DELETED:
		out->name = g->name;
		break;
	case GRAPH_EVENT_GRAPH_RENAMED:
		out->name = g->name;
		out->old_name = ev->old_name;
		break;
	case GRAPH_EVENT_GRAPH_STARTED:
	case GRAPH_EVENT_GRAPH_STOPPED:
		out->started = ev->type == GRAPH_EVENT_GRAPH_STARTED;
		out->time_ms = ev->time_ms;
		out->uptime_ms = ev->uptime_ms;
		break;
	case GRAPH_EVENT_ERROR:
		out->process = ev->graph_id;
		out->message = ev->message;
		break;
	case GRAPH_EVENT_BLOCK_ADDED:
		out->component = block_component(b);
		/* fall through */
	case GRAPH_EVENT_BLOCK_CHANGED:
		out->x = b->metadata.x;
		out->y = b->metadata.y;
		out->metadata = b->metadata.extra;
		/* fall through */
	case GRAPH_EVENT_BLOCK_REMOVED:
		out->block = b->name;
		break;
	case GRAPH_EVENT_BLOCK_RENAMED:
		out->block = b->name;
		out->old_name = ev->old_name;
		break;
	case GRAPH_EVENT_EDGE_ADDED:
	case GRAPH_EVENT_EDGE_CHANGED:
		out->metadata = b->inputs[ev->input].edge_metadata;
		/* fall through */
	case GRAPH_EVENT_EDGE_REMOVED:
		out->src_block = ev->src_block->name;
		out->src_port = block_output_port(ev->src_block, ev->src_output)->name;
		/* fall through */
	case GRAPH_EVENT_INITIAL_ADDED:
	case GRAPH_EVENT_INITIAL_REMOVED:
		out->block = b->name;
		out->port = block_input_port(b, ev->input)->name;
		out->value = ev->value;
		break;
	case GRAPH_EVENT_INPORT_ADDED:
	case GRAPH_EVENT_OUTPORT_ADDED:
		out->block = ev->port->block->name;
		out->port = ev->type == GRAPH_EVENT_INPORT_ADDED
				    ? block_input_port(ev->port->block, ev->port->port)->name
				    : block_output_port(ev->port->block, ev->port->port)->name;
		/* fall through */
	case GRAPH_EVENT_INPORT_REMOVED:
	case GRAPH_EVENT_OUTPORT_REMOVED:
		out->name = ev->port->name;
		break;
	case GRAPH_EVENT_INPORT_RENAMED:
	case GRAPH_EVENT_OUTPORT_RENAMED:
		out->name = ev->port->name;
		out->old_name = ev->old_name;
		break;
	case GRAPH_EVENT_GROUP_ADDED:
	case GRAPH_EVENT_GROUP_CHANGED:
		out->description = def->description;
		out->icon = def->icon;
		/* fall through */
	case GRAPH_EVENT_COMPONENT:
		out->name = def->name;
		out->block_type = def;
		break;
	case GRAPH_EVENT_GROUP_RENAMED:
		out->name = def->name;
		out->old_name = ev->old_name;
		out->block_type = def;
		break;
	case GRAPH_EVENT_GROUP_REMOVED:
		out->name = ev->old_name;
		break;
	case GRAPH_EVENT_LINK_ADDED:
	case GRAPH_EVENT_LINK_REMOVED:
		out->name = ev->link->src_port;
		out->to_process = ev->link->tgt_graph->id;
		out->to_port = ev->link->tgt_port;
		break;
	}
}

/* Drop the entries unsubscribed during a delivery, keeping the order. */
static void compact(Hakkas *h)
{
	size_t n = 0;
	for (size_t i = 0; i < h->sub_count; i++) {
		if (h->subs[i].fn != NULL)
			h->subs[n++] = h->subs[i];
	}
	h->sub_count = n;
	h->compact = false;
}

/* The registry's subscriber: every event, unscoped, to every host
 * subscriber in subscription order. One added meanwhile (after count) gets
 * the next event. */
static void on_event(void *ctx, const GraphEvent *ev)
{
	Hakkas *h = ctx;
	if (h->sub_count == 0)
		return;
	HakkasEvent he;
	convert(ev, &he);
	h->delivering++;
	size_t count = h->sub_count;
	for (size_t i = 0; i < count; i++) {
		if (h->subs[i].fn != NULL)
			h->subs[i].fn(h->subs[i].ctx, &he);
	}
	h->delivering--;
	if (h->delivering == 0 && h->compact)
		compact(h);
}

bool hakkas_create(const Config *cfg, const HakkasParams *params, Hakkas **out)
{
	*out = NULL;
	Hakkas *h = calloc(1, sizeof(*h));
	if (h == NULL) {
		log_error("cannot start the engine: out of memory");
		return false;
	}
	EngineParams ep = {0};
	if (params != NULL) {
		ep.platform_start = params->platform_start;
		ep.platform_stop = params->platform_stop;
		ep.platform_ctx = params->platform_ctx;
		ep.ephemeral_ports = params->ephemeral_ports;
	}
	if (!engine_start(cfg, &ep, &h->engine)) {
		free(h);
		return false; /* logged */
	}
	/* Subscribed before any process exists, so the host sees everything
	 * (01 §4: in subscription order, before every connection). */
	graphs_subscriber_init(&h->sub, on_event, h);
	graphs_subscribe(graphs_of(h), &h->sub);
	*out = h;
	return true;
}

void hakkas_run(Hakkas *h)
{
	engine_run(h->engine);
}

void hakkas_run_once(Hakkas *h)
{
	loop_run_once(engine_loop(h->engine));
}

void hakkas_request_stop(Hakkas *h)
{
	loop_stop(engine_loop(h->engine));
}

void hakkas_destroy(Hakkas *h)
{
	if (h == NULL)
		return;
	graphs_unsubscribe(graphs_of(h), &h->sub);
	engine_stop(h->engine);
	free(h);
}

const Config *hakkas_config(const Hakkas *h)
{
	return engine_config(h->engine);
}

uint16_t hakkas_web_port(const Hakkas *h)
{
	return engine_web_port(h->engine);
}

uint16_t hakkas_ws_port(const Hakkas *h)
{
	return engine_ws_port(h->engine);
}

bool hakkas_add_tick(Hakkas *h, hakkas_tick_fn fn, void *ctx)
{
	return loop_add_tick(engine_loop(h->engine), fn, ctx);
}

bool hakkas_remove_tick(Hakkas *h, hakkas_tick_fn fn, void *ctx)
{
	return loop_remove_tick(engine_loop(h->engine), fn, ctx);
}

/* ---- Events ----------------------------------------------------------------------- */

static Subscription *find_sub(Hakkas *h, hakkas_event_fn fn, void *ctx)
{
	for (size_t i = 0; i < h->sub_count; i++) {
		if (h->subs[i].fn == fn && h->subs[i].ctx == ctx)
			return &h->subs[i];
	}
	return NULL;
}

bool hakkas_subscribe(Hakkas *h, hakkas_event_fn fn, void *ctx)
{
	if (fn == NULL || find_sub(h, fn, ctx) != NULL)
		return false;
	if (h->sub_count == HAKKAS_SUBSCRIBERS_MAX && h->compact && h->delivering == 0)
		compact(h);
	if (h->sub_count == HAKKAS_SUBSCRIBERS_MAX)
		return false;
	h->subs[h->sub_count++] = (Subscription){fn, ctx};
	return true;
}

void hakkas_unsubscribe(Hakkas *h, hakkas_event_fn fn, void *ctx)
{
	Subscription *s = fn != NULL ? find_sub(h, fn, ctx) : NULL;
	if (s == NULL)
		return;
	s->fn = NULL;
	h->compact = true;
	if (h->delivering == 0)
		compact(h);
}

/* ---- Processes: lifetime ------------------------------------------------------------- */

static void loaded_info(const Graph *g, bool started, HakkasLoaded *out)
{
	snprintf(out->id, sizeof(out->id), "%s", g->id);
	out->started = started;
}

enum hakkas_status hakkas_process_load(Hakkas *h, const char *path, HakkasLoaded *out,
				       HakkasMessage *msg)
{
	if (out != NULL)
		memset(out, 0, sizeof(*out));
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (path == NULL || path[0] == '\0') {
		set_msg(msg, hakkas_status_str(HAKKAS_ERR_INVALID));
		return HAKKAS_ERR_INVALID;
	}
	Graph *g;
	bool started;
	enum graph_files_status st = graph_files_load(files_of(h), path, &g, &started);
	if (st != GRAPH_FILES_OK) {
		set_msg(msg, graph_files_status_str(st));
		return from_load(st);
	}
	if (out != NULL)
		loaded_info(g, started, out);
	return HAKKAS_OK;
}

typedef struct {
	hakkas_loaded_fn fn;
	void *ctx;
} LoadSaved;

static void on_loaded(void *ctx, Graph *g, bool started)
{
	LoadSaved *ls = ctx;
	if (ls->fn == NULL)
		return;
	HakkasLoaded info;
	loaded_info(g, started, &info);
	ls->fn(ls->ctx, &info);
}

enum hakkas_status hakkas_process_load_saved(Hakkas *h, hakkas_loaded_fn fn, void *ctx,
					     size_t *loaded)
{
	if (loaded != NULL)
		*loaded = 0;
	if (!enter(h, NULL))
		return HAKKAS_ERR_BUSY;
	LoadSaved ls = {fn, ctx};
	return from_load(graph_files_load_all(files_of(h), on_loaded, &ls, loaded));
}

enum hakkas_status hakkas_process_create(Hakkas *h, const char *id, const char *name,
					 const char *description, const char *icon, char *id_out,
					 HakkasMessage *msg)
{
	if (id_out != NULL)
		id_out[0] = '\0';
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	Graph *g = NULL;
	GraphsMessage gm;
	enum graph_status st = graphs_new_graph(graphs_of(h), id, name, description, icon, &g, &gm);
	if (st == GRAPH_OK && id_out != NULL)
		snprintf(id_out, HAKKAS_ID_MAX + 1, "%s", g->id);
	return from_graph(st, &gm, msg);
}

enum hakkas_status hakkas_process_delete(Hakkas *h, const char *id, const char *name,
					 HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (id == NULL)
		return invalid_field(msg, "id");
	if (name == NULL)
		return invalid_field(msg, "name");
	GraphsMessage gm;
	return from_graph(graph_files_delete(files_of(h), id, name, &gm), &gm, msg);
}

enum hakkas_status hakkas_process_rename(Hakkas *h, const char *id, const char *name,
					 HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (id == NULL)
		return invalid_field(msg, "id");
	if (name == NULL)
		return invalid_field(msg, "to");
	GraphsMessage gm;
	return from_graph(graphs_rename_graph(graphs_of(h), id, name, &gm), &gm, msg);
}

enum hakkas_status hakkas_process_start(Hakkas *h, const char *id, HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (id == NULL)
		return invalid_field(msg, "graph");
	GraphsMessage gm;
	return from_graph(graphs_start(graphs_of(h), id, &gm), &gm, msg);
}

enum hakkas_status hakkas_process_stop(Hakkas *h, const char *id, HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (id == NULL)
		return invalid_field(msg, "graph");
	GraphsMessage gm;
	return from_graph(graphs_stop(graphs_of(h), id, &gm), &gm, msg);
}

enum hakkas_status hakkas_process_persist(Hakkas *h, const char *id, HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (id == NULL)
		return invalid_field(msg, "graph");
	GraphsMessage gm;
	return from_files(graph_files_persist(files_of(h), id, &gm), &gm, msg);
}

static void status_of(const Graph *g, HakkasProcessStatus *out)
{
	out->id = g->id;
	out->name = g->name;
	out->description = g->description;
	out->icon = g->icon;
	out->started = g->started;
	out->uptime_ms = graphs_uptime_ms(g);
	out->time_started_ms = g->time_started;
}

enum hakkas_status hakkas_process_status(Hakkas *h, const char *id, HakkasProcessStatus *out,
					 HakkasMessage *msg)
{
	memset(out, 0, sizeof(*out));
	set_msg(msg, "");
	if (id == NULL)
		return invalid_field(msg, "graph");
	if (!graph_id_valid(id))
		return refusal(GRAPH_ERR_INVALID_ID, NULL, msg);
	const Graph *g = graphs_find(graphs_of(h), id);
	if (g == NULL)
		return refusal(GRAPH_ERR_UNKNOWN_GRAPH, &(GraphRefusal){.graph = id}, msg);
	status_of(g, out);
	return HAKKAS_OK;
}

size_t hakkas_process_count(const Hakkas *h)
{
	return graphs_count(graphs_of(h));
}

bool hakkas_process_at(const Hakkas *h, size_t i, HakkasProcessStatus *out)
{
	memset(out, 0, sizeof(*out));
	if (i >= graphs_count(graphs_of(h)))
		return false;
	status_of(graphs_at(graphs_of(h), (uint32_t)i), out);
	return true;
}

/* ---- Processes: editing --------------------------------------------------------------- */

enum hakkas_status hakkas_block_add(Hakkas *h, const char *process, const char *block,
				    const char *component, const char *metadata,
				    HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (block == NULL)
		return invalid_field(msg, "id");
	if (process == NULL)
		return invalid_field(msg, "graph");
	if (component == NULL)
		return invalid_field(msg, "component");
	cJSON *json;
	if (!parse_json(metadata, &json))
		return invalid_field(msg, "metadata");
	BlockMetadata md;
	enum graph_status st = graph_json_to_metadata(json, &md);
	cJSON_Delete(json);
	if (st == GRAPH_ERR_NO_MEMORY)
		return no_memory(msg);
	if (st != GRAPH_OK)
		return invalid_field(msg, "metadata");
	GraphsMessage gm;
	st = graphs_add_node(graphs_of(h), process, block, component, &md, &gm);
	block_metadata_release(&md);
	return from_graph(st, &gm, msg);
}

enum hakkas_status hakkas_block_remove(Hakkas *h, const char *process, const char *block,
				       HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (block == NULL)
		return invalid_field(msg, "id");
	if (process == NULL)
		return invalid_field(msg, "graph");
	GraphsMessage gm;
	return from_graph(graphs_remove_node(graphs_of(h), process, block, &gm), &gm, msg);
}

enum hakkas_status hakkas_block_rename(Hakkas *h, const char *process, const char *from,
				       const char *to, HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (from == NULL)
		return invalid_field(msg, "from");
	if (to == NULL)
		return invalid_field(msg, "to");
	if (process == NULL)
		return invalid_field(msg, "graph");
	GraphsMessage gm;
	return from_graph(graphs_rename_node(graphs_of(h), process, from, to, &gm), &gm, msg);
}

enum hakkas_status hakkas_block_change(Hakkas *h, const char *process, const char *block,
				       const char *patch, HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (block == NULL)
		return invalid_field(msg, "id");
	cJSON *json;
	if (patch == NULL || !parse_json(patch, &json))
		return invalid_field(msg, "metadata");
	enum hakkas_status rc;
	if (!cJSON_IsObject(json)) {
		rc = invalid_field(msg, "metadata");
		goto done;
	}
	if (process == NULL) {
		rc = invalid_field(msg, "graph");
		goto done;
	}
	/* The patch's values are checked before the process and block, as on
	 * the wire (02 §1.2); the merge itself needs the block's metadata. */
	BlockMetadata none = block_metadata_default();
	BlockChange chg;
	enum graph_status st = graph_json_to_change(&none, json, &chg);
	if (st != GRAPH_OK) {
		rc = st == GRAPH_ERR_NO_MEMORY ? no_memory(msg) : invalid_field(msg, "metadata");
		goto done;
	}
	block_change_release(&chg);
	Graph *g = graph_id_valid(process) ? graphs_find(graphs_of(h), process) : NULL;
	Block *b = g != NULL && graph_id_valid(block) ? graph_find_block(g, block) : NULL;
	GraphsMessage gm;
	if (b == NULL) {
		/* Refused by the registry: Invalid id, Unknown graph or node. */
		BlockChange nothing = {0};
		rc = from_graph(graphs_change_node(graphs_of(h), process, block, &nothing, &gm),
				&gm, msg);
		goto done;
	}
	st = graph_json_to_change(&b->metadata, json, &chg);
	if (st != GRAPH_OK) {
		rc = no_memory(msg); /* the patch was checked above */
		goto done;
	}
	rc = from_graph(graphs_change_node(graphs_of(h), process, block, &chg, &gm), &gm, msg);
	block_change_release(&chg);
done:
	cJSON_Delete(json);
	return rc;
}

/* A connection's metadata text (NULL for none) as model text in *out (NULL
 * for none): GRAPH_OK, INVALID_ARGUMENT (not one JSON object) or
 * NO_MEMORY. */
static enum graph_status edge_metadata(const char *text, char **out)
{
	*out = NULL;
	cJSON *json;
	if (!parse_json(text, &json))
		return GRAPH_ERR_INVALID_ARGUMENT;
	enum graph_status st = graph_json_to_edge_metadata(json, out);
	cJSON_Delete(json);
	return st;
}

enum hakkas_status hakkas_connection_add(Hakkas *h, const char *process, const char *src_block,
					 const char *src_port, const char *tgt_block,
					 const char *tgt_port, const char *metadata,
					 HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (src_block == NULL || src_port == NULL)
		return invalid_field(msg, "src");
	if (tgt_block == NULL || tgt_port == NULL)
		return invalid_field(msg, "tgt");
	if (process == NULL)
		return invalid_field(msg, "graph");
	char *md;
	enum graph_status st = edge_metadata(metadata, &md);
	if (st != GRAPH_OK)
		return st == GRAPH_ERR_NO_MEMORY ? no_memory(msg) : invalid_field(msg, "metadata");
	GraphsMessage gm;
	st = graphs_add_edge(graphs_of(h), process, src_block, src_port, tgt_block, tgt_port, md,
			     &gm);
	free(md);
	return from_graph(st, &gm, msg);
}

enum hakkas_status hakkas_connection_remove(Hakkas *h, const char *process,
					    const char *src_block, const char *src_port,
					    const char *tgt_block, const char *tgt_port,
					    HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (src_block == NULL || src_port == NULL)
		return invalid_field(msg, "src");
	if (tgt_block == NULL || tgt_port == NULL)
		return invalid_field(msg, "tgt");
	if (process == NULL)
		return invalid_field(msg, "graph");
	GraphsMessage gm;
	return from_graph(graphs_remove_edge(graphs_of(h), process, src_block, src_port, tgt_block,
					     tgt_port, &gm),
			  &gm, msg);
}

enum hakkas_status hakkas_connection_change(Hakkas *h, const char *process,
					    const char *src_block, const char *src_port,
					    const char *tgt_block, const char *tgt_port,
					    const char *metadata, HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (src_block == NULL || src_port == NULL)
		return invalid_field(msg, "src");
	if (tgt_block == NULL || tgt_port == NULL)
		return invalid_field(msg, "tgt");
	char *md;
	enum graph_status st = edge_metadata(metadata, &md);
	if (st != GRAPH_OK)
		return st == GRAPH_ERR_NO_MEMORY ? no_memory(msg) : invalid_field(msg, "metadata");
	if (process == NULL) {
		free(md);
		return invalid_field(msg, "graph");
	}
	GraphsMessage gm;
	st = graphs_change_edge(graphs_of(h), process, src_block, src_port, tgt_block, tgt_port, md,
				&gm);
	free(md);
	return from_graph(st, &gm, msg);
}

/* The declared type of the input block.port of process, or NULL if there is
 * no such input (the registry then refuses the command, 02 §3.4.1). */
static const char *input_type(const Hakkas *h, const char *process, const char *block,
			      const char *port)
{
	const Graph *g = graph_id_valid(process) ? graphs_find(graphs_of(h), process) : NULL;
	const Block *b = g != NULL && graph_id_valid(block) ? graph_find_block(g, block) : NULL;
	int32_t i = b != NULL && !block_is_placeholder(b) ? block_input_index(b, port) : -1;
	return i >= 0 ? block_input_port(b, (uint32_t)i)->type : NULL;
}

enum hakkas_status hakkas_initial_add(Hakkas *h, const char *process, const char *block,
				      const char *port, const HakkasValue *value,
				      HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (block == NULL || port == NULL)
		return invalid_field(msg, "tgt");
	if (value == NULL || value->type == LINKABLE_UNSET)
		return invalid_field(msg, "src");
	if (process == NULL)
		return invalid_field(msg, "graph");
	/* A string is parsed for the port's type, like the wire's string data
	 * (02 §3.4); a number or bool is taken as it is. */
	Linkable v;
	if (value->type == LINKABLE_STRING) {
		if (!linkable_parse(&v, input_type(h, process, block, port), value->as.s))
			return no_memory(msg);
	} else {
		v = linkable_copy(value);
	}
	GraphsMessage gm;
	enum graph_status st = graphs_add_initial(graphs_of(h), process, block, port, &v, &gm);
	linkable_clear(&v);
	return from_graph(st, &gm, msg);
}

enum hakkas_status hakkas_initial_remove(Hakkas *h, const char *process, const char *block,
					 const char *port, HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (block == NULL || port == NULL)
		return invalid_field(msg, "tgt");
	if (process == NULL)
		return invalid_field(msg, "graph");
	GraphsMessage gm;
	return from_graph(graphs_remove_initial(graphs_of(h), process, block, port, &gm), &gm,
			  msg);
}

static enum graph_port_dir port_dir(enum hakkas_port_dir dir)
{
	return dir == HAKKAS_OUTPORT ? GRAPH_OUTPORT : GRAPH_INPORT;
}

enum hakkas_status hakkas_port_add(Hakkas *h, const char *process, enum hakkas_port_dir dir,
				   const char *name, const char *block, const char *port,
				   HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (process == NULL)
		return invalid_field(msg, "graph");
	if (block == NULL)
		return invalid_field(msg, "node");
	if (port == NULL)
		return invalid_field(msg, "port");
	if (name == NULL)
		return invalid_field(msg, "public");
	GraphsMessage gm;
	return from_graph(graphs_add_port(graphs_of(h), process, port_dir(dir), name, block, port,
					  &gm),
			  &gm, msg);
}

enum hakkas_status hakkas_port_remove(Hakkas *h, const char *process, enum hakkas_port_dir dir,
				      const char *name, HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (process == NULL)
		return invalid_field(msg, "graph");
	if (name == NULL)
		return invalid_field(msg, "public");
	GraphsMessage gm;
	return from_graph(graphs_remove_port(graphs_of(h), process, port_dir(dir), name, &gm), &gm,
			  msg);
}

enum hakkas_status hakkas_port_rename(Hakkas *h, const char *process, enum hakkas_port_dir dir,
				      const char *from, const char *to, HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (process == NULL)
		return invalid_field(msg, "graph");
	if (from == NULL)
		return invalid_field(msg, "from");
	if (to == NULL)
		return invalid_field(msg, "to");
	GraphsMessage gm;
	return from_graph(graphs_rename_port(graphs_of(h), process, port_dir(dir), from, to, &gm),
			  &gm, msg);
}

/* ---- Groups --------------------------------------------------------------------------- */

enum hakkas_status hakkas_group_add(Hakkas *h, const char *process, const char *name,
				    const char *const *blocks, size_t count,
				    const char *description, const char *icon, HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (process == NULL)
		return invalid_field(msg, "graph");
	if (name == NULL)
		return invalid_field(msg, "name");
	bool nodes_ok = blocks != NULL && count > 0;
	for (size_t i = 0; nodes_ok && i < count; i++)
		nodes_ok = blocks[i] != NULL;
	if (!nodes_ok)
		return invalid_field(msg, "nodes");
	if (description == NULL)
		return invalid_field(msg, "metadata");
	CompositeSelection sel = {.name = name,
				  .description = description,
				  .icon = icon,
				  .nodes = blocks,
				  .node_count = count};
	GraphsMessage gm;
	return from_files(type_files_add_group(files_of(h), process, &sel, &gm), &gm, msg);
}

enum hakkas_status hakkas_group_remove(Hakkas *h, const char *process, const char *name,
				       HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (process == NULL)
		return invalid_field(msg, "graph");
	if (name == NULL)
		return invalid_field(msg, "name");
	GraphsMessage gm;
	return from_files(type_files_remove_group(files_of(h), process, name, &gm), &gm, msg);
}

enum hakkas_status hakkas_group_rename(Hakkas *h, const char *process, const char *from,
				       const char *to, HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (process == NULL)
		return invalid_field(msg, "graph");
	if (from == NULL)
		return invalid_field(msg, "from");
	if (to == NULL)
		return invalid_field(msg, "to");
	GraphsMessage gm;
	return from_files(type_files_rename_group(files_of(h), process, from, to, &gm), &gm, msg);
}

enum hakkas_status hakkas_group_change(Hakkas *h, const char *process, const char *name,
				       const char *description, const char *icon,
				       HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (process == NULL)
		return invalid_field(msg, "graph");
	if (name == NULL)
		return invalid_field(msg, "name");
	if (description == NULL)
		return invalid_field(msg, "metadata");
	GraphsMessage gm;
	return from_files(
		type_files_change_group(files_of(h), process, name, description, icon, &gm), &gm,
		msg);
}

/* ---- Links between processes ------------------------------------------------------- */

enum hakkas_status hakkas_link_add(Hakkas *h, const char *from_process, const char *from_outport,
				   const char *to_process, const char *to_inport,
				   HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (from_process == NULL || from_outport == NULL)
		return invalid_field(msg, "from");
	if (to_process == NULL || to_inport == NULL)
		return invalid_field(msg, "to");
	GraphsMessage gm;
	return from_graph(graphs_add_link(graphs_of(h), from_process, from_outport, to_process,
					  to_inport, &gm),
			  &gm, msg);
}

enum hakkas_status hakkas_link_remove(Hakkas *h, const char *from_process,
				      const char *from_outport, const char *to_process,
				      const char *to_inport, HakkasMessage *msg)
{
	if (!enter(h, msg))
		return HAKKAS_ERR_BUSY;
	if (from_process == NULL || from_outport == NULL)
		return invalid_field(msg, "from");
	if (to_process == NULL || to_inport == NULL)
		return invalid_field(msg, "to");
	GraphsMessage gm;
	return from_graph(graphs_remove_link(graphs_of(h), from_process, from_outport, to_process,
					     to_inport, &gm),
			  &gm, msg);
}

size_t hakkas_link_count(const Hakkas *h)
{
	return runtime_link_count(graphs_of(h)->rt);
}

bool hakkas_link_at(const Hakkas *h, size_t i, HakkasLink *out)
{
	memset(out, 0, sizeof(*out));
	const Runtime *rt = graphs_of(h)->rt;
	if (i >= runtime_link_count(rt))
		return false;
	const ProcessLink *l = runtime_link_at(rt, (uint32_t)i);
	out->from_process = l->src_graph->id;
	out->from_outport = l->src_port;
	out->to_process = l->tgt_graph->id;
	out->to_inport = l->tgt_port;
	return true;
}
