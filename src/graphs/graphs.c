/*
 * The Graphs registry and event notification (01 §2.2, §4; 02 §3.2–§3.4,
 * §4.1). See graphs/graphs.h.
 */
#include "graphs/graphs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphs/graph_internal.h"

/* Initial capacity of the graph array; it doubles when full. */
#define GRAPHS_INITIAL 4

/* Attempts at drawing a UUID that is not already a graph id. */
#define UUID_ATTEMPTS 4

/* ---- Event delivery (01 §4, 02 §4.1) ----------------------------------- */

/* System-wide events reach every subscriber, whatever its scope (02 §4.1). */
static bool system_wide(enum graph_event_type type)
{
	return type == GRAPH_EVENT_GRAPH_CREATED || type == GRAPH_EVENT_GRAPH_DELETED ||
	       type == GRAPH_EVENT_GRAPH_STATUS || type == GRAPH_EVENT_ERROR ||
	       type == GRAPH_EVENT_COMPONENT || type == GRAPH_EVENT_LINK_ADDED ||
	       type == GRAPH_EVENT_LINK_REMOVED;
}

static bool in_scope(const GraphsSubscriber *s, const GraphEvent *ev)
{
	if (s->scope[0] == '\0' || system_wide(ev->type))
		return true;
	return ev->graph != NULL && ev->graph->id != NULL && strcmp(ev->graph->id, s->scope) == 0;
}

/* Whether an event changes what the graph file holds (05 §2.1; 02 §3.3
 * "Unsaved state"). Start and stop are marked where they change the state
 * (graphs_start, graphs_stop): their events are also sent when nothing
 * changed. Groups and links are not in the graph file; bindings are saved
 * with it (its bindings file, plan task 62), so they count. */
static bool changes_file(enum graph_event_type type)
{
	switch (type) {
	case GRAPH_EVENT_BLOCK_ADDED:
	case GRAPH_EVENT_BLOCK_REMOVED:
	case GRAPH_EVENT_BLOCK_RENAMED:
	case GRAPH_EVENT_BLOCK_CHANGED:
	case GRAPH_EVENT_EDGE_ADDED:
	case GRAPH_EVENT_EDGE_REMOVED:
	case GRAPH_EVENT_EDGE_CHANGED:
	case GRAPH_EVENT_INITIAL_ADDED:
	case GRAPH_EVENT_INITIAL_REMOVED:
	case GRAPH_EVENT_INPORT_ADDED:
	case GRAPH_EVENT_INPORT_REMOVED:
	case GRAPH_EVENT_INPORT_RENAMED:
	case GRAPH_EVENT_OUTPORT_ADDED:
	case GRAPH_EVENT_OUTPORT_REMOVED:
	case GRAPH_EVENT_OUTPORT_RENAMED:
	case GRAPH_EVENT_GRAPH_RENAMED:
	case GRAPH_EVENT_BINDING_ADDED:
	case GRAPH_EVENT_BINDING_REMOVED:
		return true;
	default:
		return false;
	}
}

static void mark_changed(Graphs *gs, Graph *g);
static void update_unsaved(Graphs *gs, Graph *g);

/* Deliver ev to every subscriber in scope, in subscription order. gs->cursor
 * holds the next subscriber, so a callback may unsubscribe any subscriber;
 * one added during the delivery has since == seq and is skipped. Then, if ev
 * made its graph unsaved, that graph's status follows. */
static void notify(Graphs *gs, const GraphEvent *ev)
{
	if (gs->quiet > 0)
		return;
	uint64_t seq = ++gs->seq;
	for (GraphsSubscriber *s = gs->subscribers; s != NULL; s = gs->cursor) {
		gs->cursor = s->next;
		if (s->since < seq && in_scope(s, ev))
			s->fn(s->ctx, ev);
	}
	gs->cursor = NULL;
	if (ev->graph != NULL && changes_file(ev->type))
		mark_changed(gs, ev->graph);
}

/* The runtime's observer: every structural change of every graph. */
static void on_graph_event(void *ctx, const GraphEvent *ev)
{
	notify(ctx, ev);
}

static void emit(Graphs *gs, Graph *g, GraphEvent ev)
{
	ev.graph = g;
	notify(gs, &ev);
}

void graphs_subscriber_init(GraphsSubscriber *s, graph_event_fn fn, void *ctx)
{
	memset(s, 0, sizeof(*s));
	s->fn = fn;
	s->ctx = ctx;
}

void graphs_subscribe(Graphs *gs, GraphsSubscriber *s)
{
	s->since = gs->seq;
	s->next = NULL;
	GraphsSubscriber **p = &gs->subscribers;
	while (*p != NULL)
		p = &(*p)->next;
	*p = s;
}

void graphs_unsubscribe(Graphs *gs, GraphsSubscriber *s)
{
	for (GraphsSubscriber **p = &gs->subscribers; *p != NULL; p = &(*p)->next) {
		if (*p == s) {
			*p = s->next;
			if (gs->cursor == s)
				gs->cursor = s->next;
			s->next = NULL;
			return;
		}
	}
}

bool graphs_subscriber_scope(GraphsSubscriber *s, const char *graph_id)
{
	if (graph_id == NULL) {
		s->scope[0] = '\0';
		return true;
	}
	if (!graph_id_valid(graph_id))
		return false;
	memcpy(s->scope, graph_id, strlen(graph_id) + 1);
	return true;
}

/* ---- Registry ------------------------------------------------------------- */

void graphs_init(Graphs *gs, Runtime *rt, graphs_type_resolver resolve_type,
		 graphs_random_fn random_bytes)
{
	memset(gs, 0, sizeof(*gs));
	gs->rt = rt;
	gs->resolve_type = resolve_type;
	gs->random_bytes = random_bytes;
	runtime_set_observer(rt, on_graph_event, gs);
}

void graphs_deinit(Graphs *gs)
{
	runtime_set_observer(gs->rt, NULL, NULL);
	for (uint32_t i = 0; i < gs->count; i++)
		graph_destroy(gs->graphs[i]);
	free(gs->graphs);
	gs->graphs = NULL;
	gs->count = 0;
	gs->cap = 0;
	gs->subscribers = NULL;
	gs->cursor = NULL;
}

static int32_t graph_index(const Graphs *gs, const char *id)
{
	if (id == NULL)
		return -1;
	for (uint32_t i = 0; i < gs->count; i++) {
		if (strcmp(gs->graphs[i]->id, id) == 0)
			return (int32_t)i;
	}
	return -1;
}

Graph *graphs_find(const Graphs *gs, const char *id)
{
	int32_t i = graph_index(gs, id);
	return i < 0 ? NULL : gs->graphs[i];
}

uint64_t graphs_uptime_ms(const Graph *g)
{
	uint64_t now = g->rt->now_ms;
	if (!g->started || now < g->time_started)
		return 0;
	return now - g->time_started;
}

static bool graphs_reserve(Graphs *gs)
{
	if (gs->count < gs->cap)
		return true;
	if (gs->cap >= UINT32_MAX / 2)
		return false;
	uint32_t cap = gs->cap == 0 ? GRAPHS_INITIAL : gs->cap * 2;
	Graph **graphs = realloc(gs->graphs, (size_t)cap * sizeof(*graphs));
	if (graphs == NULL)
		return false;
	gs->graphs = graphs;
	gs->cap = cap;
	return true;
}

/* ---- Refusal messages (02 §3.4.1) ---------------------------------------- */

static const char *or_empty(const char *s)
{
	return s != NULL ? s : "";
}

size_t graph_refusal_message(enum graph_status st, const GraphRefusal *ctx, char *buf,
			     size_t size)
{
	static const GraphRefusal none;
	if (ctx == NULL)
		ctx = &none;
	const char *graph = or_empty(ctx->graph), *node = or_empty(ctx->node);
	const char *port = or_empty(ctx->port), *name = or_empty(ctx->name);
	int n = 0;
	switch (st) {
	case GRAPH_OK:
		n = snprintf(buf, size, "%s", "");
		break;
	case GRAPH_ERR_UNKNOWN_GRAPH:
		n = snprintf(buf, size, "Unknown graph: %s", graph);
		break;
	case GRAPH_ERR_UNKNOWN_NODE:
		n = snprintf(buf, size, "Unknown node: %s", node);
		break;
	case GRAPH_ERR_UNKNOWN_PORT:
		if (ctx->node != NULL)
			n = snprintf(buf, size, "Unknown port: %s.%s", node, port);
		else
			n = snprintf(buf, size, "Unknown port: %s", port);
		break;
	case GRAPH_ERR_NODE_EXISTS:
		n = snprintf(buf, size, "Node exists: %s", node);
		break;
	case GRAPH_ERR_UNKNOWN_COMPONENT:
		n = snprintf(buf, size, "Unknown component: %s", name);
		break;
	case GRAPH_ERR_INVALID_EDGE_DIRECTION:
		n = snprintf(buf, size, "%s", "Invalid edge direction");
		break;
	case GRAPH_ERR_INPUT_CONNECTED:
		if (ctx->node != NULL)
			n = snprintf(buf, size, "Input already connected: %s.%s", node, port);
		else /* a process port (an environment binding, 04 §4) */
			n = snprintf(buf, size, "Input already connected: %s", port);
		break;
	case GRAPH_ERR_NO_SUCH_CONNECTION:
		n = snprintf(buf, size, "%s", "No such connection");
		break;
	case GRAPH_ERR_PORT_EXISTS:
		n = snprintf(buf, size, "Port exists: %s", name);
		break;
	case GRAPH_ERR_NAME_MISMATCH:
		n = snprintf(buf, size, "Graph name mismatch: %s", graph);
		break;
	case GRAPH_ERR_GRAPH_EXISTS:
		n = snprintf(buf, size, "Graph exists: %s", graph);
		break;
	case GRAPH_ERR_INVALID_ID:
		n = snprintf(buf, size, "%s", "Invalid id");
		break;
	case GRAPH_ERR_INVALID_ARGUMENT:
		n = snprintf(buf, size, "%s", "Invalid argument");
		break;
	case GRAPH_ERR_NO_MEMORY:
		n = snprintf(buf, size, "%s", "Out of memory");
		break;
	case GRAPH_ERR_UNAVAILABLE:
		n = snprintf(buf, size, "%s", "Random source unavailable");
		break;
	case GRAPH_ERR_INVALID_LINK_DIRECTION:
		n = snprintf(buf, size, "%s", "Invalid link direction");
		break;
	case GRAPH_ERR_LINK_EXISTS:
		n = snprintf(buf, size, "%s", "Link exists");
		break;
	case GRAPH_ERR_NO_SUCH_LINK:
		n = snprintf(buf, size, "%s", "No such link");
		break;
	case GRAPH_ERR_INVALID_BINDING_DIRECTION:
		n = snprintf(buf, size, "%s", "Invalid binding direction");
		break;
	case GRAPH_ERR_ACTUATOR_DRIVEN:
		n = snprintf(buf, size, "Actuator already driven: %s", name);
		break;
	case GRAPH_ERR_NO_SUCH_BINDING:
		n = snprintf(buf, size, "%s", "No such binding");
		break;
	case GRAPH_ERR_NOT_A_PROCESS:
		n = snprintf(buf, size, "Not a process: %s", graph);
		break;
	}
	return n < 0 ? 0 : (size_t)n;
}

/* Fill msg (if any) for st and return st. */
static enum graph_status finish(enum graph_status st, const GraphRefusal *ctx, GraphsMessage *msg)
{
	if (msg != NULL)
		graph_refusal_message(st, ctx, msg->text, sizeof(msg->text));
	return st;
}

/* The command's graph: Invalid id, then Unknown graph. */
static enum graph_status lookup(const Graphs *gs, const char *id, Graph **out)
{
	*out = NULL;
	if (!graph_id_valid(id))
		return GRAPH_ERR_INVALID_ID;
	*out = graphs_find(gs, id);
	return *out == NULL ? GRAPH_ERR_UNKNOWN_GRAPH : GRAPH_OK;
}

/* Whether name is a port of b in either direction. */
static bool has_port(const Block *b, const char *name)
{
	return block_input_index(b, name) >= 0 || block_output_index(b, name) >= 0;
}

/* The endpoint a refusal of an operation on src.src_port → tgt.tgt_port is
 * about (src NULL for a single-endpoint operation), re-derived from the
 * check order of graphs/graph.h: an unknown node is the first endpoint whose
 * node is missing; an unknown port the first whose node exists but has no
 * port of that name (a wrong-direction port is reported as such before the
 * next endpoint is looked at); the input of a fan-in is the target. */
static void endpoint_refusal(const Graph *g, enum graph_status st, const char *src,
			     const char *src_port, const char *tgt, const char *tgt_port,
			     GraphRefusal *ctx)
{
	const char *node = tgt, *port = tgt_port;
	if (src != NULL) {
		const Block *sb = graph_find_block(g, src);
		if ((st == GRAPH_ERR_UNKNOWN_NODE && sb == NULL) ||
		    (st == GRAPH_ERR_UNKNOWN_PORT && sb != NULL && !has_port(sb, src_port))) {
			node = src;
			port = src_port;
		}
	}
	ctx->node = node;
	ctx->port = port;
}

/* ---- Runtime commands (02 §3.2) ---------------------------------------- */

/* new_graph and new_component: a process, or a component graph (no
 * GRAPH_CREATED: it is no process, 02 §3.4.3). */
static enum graph_status new_graph(Graphs *gs, const char *id, const char *name,
				   const char *description, const char *icon, bool component,
				   Graph **out, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = id};
	char uuid[GRAPHS_UUID_SIZE];
	if (out != NULL)
		*out = NULL;
	if (id == NULL) {
		int attempt = 0;
		do {
			if (!uuid_v4(gs->random_bytes, uuid))
				return finish(GRAPH_ERR_UNAVAILABLE, &ctx, msg);
		} while (graphs_find(gs, uuid) != NULL && ++attempt < UUID_ATTEMPTS);
		id = uuid;
		ctx.graph = id;
	}
	if (!graph_id_valid(id))
		return finish(GRAPH_ERR_INVALID_ID, &ctx, msg);
	if (graphs_find(gs, id) != NULL)
		return finish(GRAPH_ERR_GRAPH_EXISTS, &ctx, msg);
	if (!graphs_reserve(gs))
		return finish(GRAPH_ERR_NO_MEMORY, &ctx, msg);
	Graph *g = graph_create(gs->rt);
	if (g == NULL)
		return finish(GRAPH_ERR_NO_MEMORY, &ctx, msg);
	g->id = graph_str_dup(id);
	g->name = graph_str_dup(name != NULL ? name : GRAPHS_DEFAULT_NAME);
	g->description = graph_str_dup(or_empty(description));
	g->icon = graph_str_dup(or_empty(icon));
	if (g->id == NULL || g->name == NULL || g->description == NULL || g->icon == NULL) {
		graph_destroy(g);
		return finish(GRAPH_ERR_NO_MEMORY, &ctx, msg);
	}

	g->component = component;
	gs->graphs[gs->count++] = g;
	if (!component)
		emit(gs, g, (GraphEvent){.type = GRAPH_EVENT_GRAPH_CREATED});
	mark_changed(gs, g); /* until saved; a load clears it (graphs_mark_saved) */
	if (out != NULL)
		*out = g;
	return finish(GRAPH_OK, &ctx, msg);
}

enum graph_status graphs_new_graph(Graphs *gs, const char *id, const char *name,
				   const char *description, const char *icon, Graph **out,
				   GraphsMessage *msg)
{
	return new_graph(gs, id, name, description, icon, false, out, msg);
}

enum graph_status graphs_new_component(Graphs *gs, const char *id, const char *name,
				       const char *description, const char *icon, Graph **out,
				       GraphsMessage *msg)
{
	if (id == NULL) { /* a component graph's id is its type's name */
		GraphRefusal ctx = {0};
		return finish(GRAPH_ERR_INVALID_ID, &ctx, msg);
	}
	return new_graph(gs, id, name, description, icon, true, out, msg);
}

/* Replace s (owned) by a copy of v, if v is not NULL. */
static bool set_str(char **s, const char *v)
{
	if (v == NULL)
		return true;
	char *copy = graph_str_dup(v);
	if (copy == NULL)
		return false;
	free(*s);
	*s = copy;
	return true;
}

enum graph_status graphs_clear_component(Graphs *gs, const char *id, const char *name,
					 const char *description, const char *icon,
					 GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = id};
	Graph *g;
	enum graph_status st = lookup(gs, id, &g);
	if (st != GRAPH_OK)
		return finish(st, &ctx, msg);
	if (!g->component)
		return finish(GRAPH_ERR_GRAPH_EXISTS, &ctx, msg);
	/* Its graph ports, then its blocks (each with its connections and
	 * initials), newest first, reported as their removals. */
	for (int dir = GRAPH_OUTPORT; dir >= GRAPH_INPORT; dir--) {
		while (g->port_count[dir] > 0)
			graph_remove_port(g, (enum graph_port_dir)dir,
					  g->ports[dir][g->port_count[dir] - 1].name);
	}
	while (g->block_count > 0)
		graph_remove_block(g, g->blocks[g->block_count - 1]->name);
	if (!set_str(&g->name, name) || !set_str(&g->description, description) ||
	    !set_str(&g->icon, icon))
		return finish(GRAPH_ERR_NO_MEMORY, &ctx, msg);
	return finish(GRAPH_OK, &ctx, msg);
}

/* Stop g if started, recording the run's duration (no event). */
static void stop_graph(Graph *g)
{
	if (!g->started)
		return;
	g->last_run_ms = graphs_uptime_ms(g);
	graph_stop(g);
}

static void emit_stopped(Graphs *gs, Graph *g)
{
	emit(gs, g,
	     (GraphEvent){.type = GRAPH_EVENT_GRAPH_STOPPED,
			  .time_ms = g->time_started,
			  .uptime_ms = g->last_run_ms,
			  .wall_ms = g->wall_started});
}

enum graph_status graphs_delete_graph(Graphs *gs, const char *id, const char *name,
				      GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = id};
	Graph *g;
	enum graph_status st = lookup(gs, id, &g);
	if (st != GRAPH_OK)
		return finish(st, &ctx, msg);
	if (name == NULL || strcmp(name, g->name) != 0)
		return finish(GRAPH_ERR_NAME_MISMATCH, &ctx, msg);

	/* A compound effect is reported as its parts (02 §1.1): the stop, if
	 * the graph ran, then its links between processes (each target input
	 * reverting, plan task 21f), then its environment bindings (plan task
	 * 22), then the deletion. */
	if (g->started) {
		stop_graph(g);
		emit_stopped(gs, g);
	}
	graph_links_remove_graph(g);
	graph_bindings_remove_graph(g); /* plan task 22 */
	int32_t i = graph_index(gs, id);
	memmove(&gs->graphs[i], &gs->graphs[i + 1],
		(gs->count - (uint32_t)i - 1) * sizeof(gs->graphs[0]));
	gs->count--;
	emit(gs, g, (GraphEvent){.type = GRAPH_EVENT_GRAPH_DELETED});
	graph_destroy(g); /* reports nothing (01 §2.2) */
	return finish(GRAPH_OK, &ctx, msg);
}

/* ---- Network commands (02 §3.3) ------------------------------------------ */

/* lookup, then: a component graph is no process (start, stop, getstatus). */
static enum graph_status lookup_process(const Graphs *gs, const char *id, Graph **out)
{
	enum graph_status st = lookup(gs, id, out);
	if (st == GRAPH_OK && (*out)->component)
		return GRAPH_ERR_NOT_A_PROCESS;
	return st;
}

enum graph_status graphs_start(Graphs *gs, const char *id, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = id};
	Graph *g;
	enum graph_status st = lookup_process(gs, id, &g);
	if (st != GRAPH_OK)
		return finish(st, &ctx, msg);
	bool changed = !g->started;
	if (changed) {
		g->time_started = gs->rt->now_ms;
		g->wall_started = runtime_wall_ms(gs->rt);
		graph_start(g);
	}
	emit(gs, g,
	     (GraphEvent){.type = GRAPH_EVENT_GRAPH_STARTED,
			  .time_ms = g->time_started,
			  .uptime_ms = graphs_uptime_ms(g),
			  .wall_ms = g->wall_started});
	if (changed)
		update_unsaved(gs, g); /* the file keeps started (05 §2.1) */
	return finish(GRAPH_OK, &ctx, msg);
}

enum graph_status graphs_stop(Graphs *gs, const char *id, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = id};
	Graph *g;
	enum graph_status st = lookup_process(gs, id, &g);
	if (st != GRAPH_OK)
		return finish(st, &ctx, msg);
	bool changed = g->started;
	stop_graph(g);
	emit_stopped(gs, g);
	if (changed)
		update_unsaved(gs, g);
	return finish(GRAPH_OK, &ctx, msg);
}

/* GRAPH_STATUS for g; a component graph has none (it is no process). */
static void emit_status(Graphs *gs, Graph *g)
{
	if (g->component)
		return;
	emit(gs, g,
	     (GraphEvent){.type = GRAPH_EVENT_GRAPH_STATUS,
			  .time_ms = g->time_started,
			  .uptime_ms = graphs_uptime_ms(g)});
}

enum graph_status graphs_status(Graphs *gs, const char *id, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = id};
	Graph *g;
	enum graph_status st = lookup_process(gs, id, &g);
	if (st != GRAPH_OK)
		return finish(st, &ctx, msg);
	emit_status(gs, g);
	return finish(GRAPH_OK, &ctx, msg);
}

void graphs_status_all(Graphs *gs)
{
	for (uint32_t i = 0; i < gs->count; i++)
		emit_status(gs, gs->graphs[i]);
}

/* Recompute g's unsaved flag (02 §3.3), reporting its status if it changed. */
static void update_unsaved(Graphs *gs, Graph *g)
{
	bool unsaved = g->changed || g->started != g->saved_started;
	if (unsaved == g->unsaved)
		return;
	g->unsaved = unsaved;
	emit_status(gs, g);
}

static void mark_changed(Graphs *gs, Graph *g)
{
	g->changed = true;
	update_unsaved(gs, g);
}

void graphs_mark_saved(Graphs *gs, Graph *g, bool started)
{
	g->changed = false;
	g->saved_started = started;
	g->unsaved = g->started != started;
	emit_status(gs, g);
}

void graphs_notify_error(Graphs *gs, const char *graph_id, const char *message)
{
	emit(gs, graphs_find(gs, graph_id),
	     (GraphEvent){.type = GRAPH_EVENT_ERROR,
			  .graph_id = graph_id,
			  .message = or_empty(message)});
}

void graphs_notify_group(Graphs *gs, Graph *g, enum graph_event_type type,
			 const BlockTypeDef *def, const char *old_name)
{
	emit(gs, g, (GraphEvent){.type = type, .block_type = def, .old_name = old_name});
}

void graphs_notify_component(Graphs *gs, const BlockTypeDef *def)
{
	emit(gs, NULL, (GraphEvent){.type = GRAPH_EVENT_COMPONENT, .block_type = def});
}

/* ---- Graph commands (02 §3.4) ------------------------------------------- */

enum graph_status graphs_rename_graph(Graphs *gs, const char *id, const char *name,
				      GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = id};
	Graph *g;
	enum graph_status st = lookup(gs, id, &g);
	if (st != GRAPH_OK)
		return finish(st, &ctx, msg);
	if (name == NULL)
		return finish(GRAPH_ERR_INVALID_ARGUMENT, &ctx, msg);
	char *copy = graph_str_dup(name);
	if (copy == NULL)
		return finish(GRAPH_ERR_NO_MEMORY, &ctx, msg);
	char *old = g->name;
	g->name = copy;
	emit(gs, g, (GraphEvent){.type = GRAPH_EVENT_GRAPH_RENAMED, .old_name = old});
	free(old);
	return finish(GRAPH_OK, &ctx, msg);
}

/* The graph of a command naming nodes or a graph port: every id first (the
 * graph id, then the nids ids in ids), then the graph (graphs/graphs.h). */
static enum graph_status lookup_with(const Graphs *gs, const char *id, int nids,
				     const char *const ids[], Graph **out)
{
	*out = NULL;
	bool valid = graph_id_valid(id);
	for (int i = 0; i < nids; i++)
		valid = valid && graph_id_valid(ids[i]);
	if (!valid)
		return GRAPH_ERR_INVALID_ID;
	return lookup(gs, id, out);
}

enum graph_status graphs_add_node(Graphs *gs, const char *graph, const char *node,
				  const char *component, const BlockMetadata *md,
				  GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = graph, .node = node, .name = component};
	Graph *g;
	enum graph_status st = lookup_with(gs, graph, 1, (const char *const[]){node}, &g);
	if (st != GRAPH_OK)
		return finish(st, &ctx, msg);
	if (graph_find_block(g, node) != NULL)
		return finish(GRAPH_ERR_NODE_EXISTS, &ctx, msg);
	const BlockTypeDef *type = component != NULL ? gs->resolve_type(component) : NULL;
	if (type == NULL)
		return finish(GRAPH_ERR_UNKNOWN_COMPONENT, &ctx, msg);
	return finish(graph_create_block(g, node, type, md, NULL), &ctx, msg);
}

enum graph_status graphs_remove_node(Graphs *gs, const char *graph, const char *node,
				     GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = graph, .node = node};
	Graph *g;
	enum graph_status st = lookup_with(gs, graph, 1, (const char *const[]){node}, &g);
	if (st == GRAPH_OK)
		st = graph_remove_block(g, node);
	return finish(st, &ctx, msg);
}

enum graph_status graphs_rename_node(Graphs *gs, const char *graph, const char *from,
				     const char *to, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = graph, .node = from};
	Graph *g;
	enum graph_status st = lookup_with(gs, graph, 2, (const char *const[]){from, to}, &g);
	if (st == GRAPH_OK)
		st = graph_rename_block(g, from, to);
	if (st == GRAPH_ERR_NODE_EXISTS)
		ctx.node = to;
	return finish(st, &ctx, msg);
}

enum graph_status graphs_change_node(Graphs *gs, const char *graph, const char *node,
				     const BlockChange *chg, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = graph, .node = node};
	Graph *g;
	enum graph_status st = lookup_with(gs, graph, 1, (const char *const[]){node}, &g);
	if (st == GRAPH_OK)
		st = graph_change_block(g, node, chg);
	return finish(st, &ctx, msg);
}

enum graph_status graphs_add_edge(Graphs *gs, const char *graph, const char *src,
				  const char *src_port, const char *tgt, const char *tgt_port,
				  const char *metadata, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = graph};
	Graph *g;
	enum graph_status st = lookup_with(gs, graph, 2, (const char *const[]){src, tgt}, &g);
	if (st == GRAPH_OK) {
		st = graph_connect(g, src, src_port, tgt, tgt_port, metadata);
		endpoint_refusal(g, st, src, src_port, tgt, tgt_port, &ctx);
	}
	return finish(st, &ctx, msg);
}

enum graph_status graphs_remove_edge(Graphs *gs, const char *graph, const char *src,
				     const char *src_port, const char *tgt, const char *tgt_port,
				     GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = graph};
	Graph *g;
	enum graph_status st = lookup_with(gs, graph, 2, (const char *const[]){src, tgt}, &g);
	if (st == GRAPH_OK) {
		st = graph_disconnect(g, src, src_port, tgt, tgt_port);
		endpoint_refusal(g, st, src, src_port, tgt, tgt_port, &ctx);
	}
	return finish(st, &ctx, msg);
}

enum graph_status graphs_change_edge(Graphs *gs, const char *graph, const char *src,
				     const char *src_port, const char *tgt, const char *tgt_port,
				     const char *metadata, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = graph};
	Graph *g;
	enum graph_status st = lookup_with(gs, graph, 2, (const char *const[]){src, tgt}, &g);
	if (st == GRAPH_OK) {
		st = graph_change_edge(g, src, src_port, tgt, tgt_port, metadata);
		endpoint_refusal(g, st, src, src_port, tgt, tgt_port, &ctx);
	}
	return finish(st, &ctx, msg);
}

enum graph_status graphs_add_initial(Graphs *gs, const char *graph, const char *node,
				     const char *port, const Linkable *value, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = graph, .node = node, .port = port};
	Graph *g;
	enum graph_status st = lookup_with(gs, graph, 1, (const char *const[]){node}, &g);
	if (st == GRAPH_OK)
		st = graph_add_initial(g, node, port, value);
	return finish(st, &ctx, msg);
}

enum graph_status graphs_remove_initial(Graphs *gs, const char *graph, const char *node,
					const char *port, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = graph, .node = node, .port = port};
	Graph *g;
	enum graph_status st = lookup_with(gs, graph, 1, (const char *const[]){node}, &g);
	if (st == GRAPH_OK)
		st = graph_remove_initial(g, node, port);
	return finish(st, &ctx, msg);
}

enum graph_status graphs_add_port(Graphs *gs, const char *graph, enum graph_port_dir dir,
				  const char *name, const char *node, const char *port,
				  GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = graph, .node = node, .port = port, .name = name};
	Graph *g;
	enum graph_status st = lookup_with(gs, graph, 2, (const char *const[]){name, node}, &g);
	if (st == GRAPH_OK)
		st = graph_add_port(g, dir, name, node, port);
	return finish(st, &ctx, msg);
}

enum graph_status graphs_remove_port(Graphs *gs, const char *graph, enum graph_port_dir dir,
				     const char *name, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = graph, .port = name, .name = name};
	Graph *g;
	enum graph_status st = lookup_with(gs, graph, 1, (const char *const[]){name}, &g);
	if (st == GRAPH_OK)
		st = graph_remove_port(g, dir, name);
	return finish(st, &ctx, msg);
}

enum graph_status graphs_rename_port(Graphs *gs, const char *graph, enum graph_port_dir dir,
				     const char *from, const char *to, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = graph, .port = from, .name = to};
	Graph *g;
	enum graph_status st = lookup_with(gs, graph, 2, (const char *const[]){from, to}, &g);
	if (st == GRAPH_OK)
		st = graph_rename_port(g, dir, from, to);
	return finish(st, &ctx, msg);
}

/* ---- Links between processes (plan task 21f) ---------------------------- */

/* The two graphs of a link command: every id first (both graph ids and both
 * port names), then the source graph, then the target graph. */
static enum graph_status lookup_link(const Graphs *gs, const char *from_graph,
				     const char *outport, const char *to_graph, const char *inport,
				     Graph **src, Graph **tgt, GraphRefusal *ctx)
{
	*src = *tgt = NULL;
	if (!graph_id_valid(from_graph) || !graph_id_valid(outport) ||
	    !graph_id_valid(to_graph) || !graph_id_valid(inport))
		return GRAPH_ERR_INVALID_ID;
	ctx->graph = from_graph;
	if ((*src = graphs_find(gs, from_graph)) == NULL)
		return GRAPH_ERR_UNKNOWN_GRAPH;
	if ((*src)->component)
		return GRAPH_ERR_NOT_A_PROCESS;
	ctx->graph = to_graph;
	if ((*tgt = graphs_find(gs, to_graph)) == NULL)
		return GRAPH_ERR_UNKNOWN_GRAPH;
	if ((*tgt)->component)
		return GRAPH_ERR_NOT_A_PROCESS;
	return GRAPH_OK;
}

enum graph_status graphs_add_link(Graphs *gs, const char *from_graph, const char *outport,
				  const char *to_graph, const char *inport, GraphsMessage *msg)
{
	GraphRefusal ctx = {0};
	Graph *src, *tgt;
	enum graph_status st =
		lookup_link(gs, from_graph, outport, to_graph, inport, &src, &tgt, &ctx);
	if (st != GRAPH_OK)
		return finish(st, &ctx, msg);
	st = graph_link_add(src, outport, tgt, inport);
	/* "<process>.<port>": the source's port if it is no port of src at
	 * all (checked first), else the target's. */
	bool src_unknown = graph_find_port(src, GRAPH_OUTPORT, outport) == NULL &&
			   graph_find_port(src, GRAPH_INPORT, outport) == NULL;
	ctx.node = st == GRAPH_ERR_UNKNOWN_PORT && src_unknown ? from_graph : to_graph;
	ctx.port = st == GRAPH_ERR_UNKNOWN_PORT && src_unknown ? outport : inport;
	return finish(st, &ctx, msg);
}

enum graph_status graphs_remove_link(Graphs *gs, const char *from_graph, const char *outport,
				     const char *to_graph, const char *inport, GraphsMessage *msg)
{
	GraphRefusal ctx = {0};
	Graph *src, *tgt;
	enum graph_status st =
		lookup_link(gs, from_graph, outport, to_graph, inport, &src, &tgt, &ctx);
	if (st == GRAPH_OK)
		st = graph_link_remove(src, outport, tgt, inport);
	return finish(st, &ctx, msg);
}
