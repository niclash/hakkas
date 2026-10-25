/*
 * Graph-level ports: inports and outports (01 §2.2 Port, 02 §3.4). Metadata
 * only — a public name mapped to one inner block's input or output — with no
 * dataflow effect but the links between processes made through them
 * (graph_links.c). See graphs/graph.h for the refusals and the behavior on
 * block rename and removal.
 */
#include <stdlib.h>
#include <string.h>

#include "graphs/graph_internal.h"

/* Initial capacity of a port array; it doubles when full. */
#define PORTS_INITIAL 4

static const enum graph_event_type added_event[2] = {GRAPH_EVENT_INPORT_ADDED,
						     GRAPH_EVENT_OUTPORT_ADDED};
static const enum graph_event_type removed_event[2] = {GRAPH_EVENT_INPORT_REMOVED,
						       GRAPH_EVENT_OUTPORT_REMOVED};
static const enum graph_event_type renamed_event[2] = {GRAPH_EVENT_INPORT_RENAMED,
						       GRAPH_EVENT_OUTPORT_RENAMED};

static int32_t port_index(const Graph *g, enum graph_port_dir dir, const char *name)
{
	for (uint32_t i = 0; i < g->port_count[dir]; i++) {
		if (strcmp(g->ports[dir][i].name, name) == 0)
			return (int32_t)i;
	}
	return -1;
}

const GraphPort *graph_find_port(const Graph *g, enum graph_port_dir dir, const char *name)
{
	int32_t i = port_index(g, dir, name);
	return i < 0 ? NULL : &g->ports[dir][i];
}

const char *graph_port_inner_name(enum graph_port_dir dir, const GraphPort *p)
{
	return dir == GRAPH_INPORT ? block_input_port(p->block, p->port)->name
				   : block_output_port(p->block, p->port)->name;
}

static bool ports_reserve(Graph *g, enum graph_port_dir dir)
{
	if (g->port_count[dir] < g->port_cap[dir])
		return true;
	if (g->port_cap[dir] >= UINT32_MAX / 2)
		return false;
	uint32_t cap = g->port_cap[dir] == 0 ? PORTS_INITIAL : g->port_cap[dir] * 2;
	GraphPort *ports = realloc(g->ports[dir], (size_t)cap * sizeof(*ports));
	if (ports == NULL)
		return false;
	g->ports[dir] = ports;
	g->port_cap[dir] = cap;
	return true;
}

enum graph_status graph_add_port(Graph *g, enum graph_port_dir dir, const char *name,
				 const char *node, const char *port)
{
	if (!graph_id_valid(name) || !graph_id_valid(node))
		return GRAPH_ERR_INVALID_ID;
	Block *b = graph_find_block(g, node);
	if (b == NULL)
		return GRAPH_ERR_UNKNOWN_NODE;
	int32_t index = dir == GRAPH_INPORT ? block_input_index(b, port)
					    : block_output_index(b, port);
	if (index < 0)
		return GRAPH_ERR_UNKNOWN_PORT;
	if (port_index(g, dir, name) >= 0)
		return GRAPH_ERR_PORT_EXISTS;
	if (!ports_reserve(g, dir))
		return GRAPH_ERR_NO_MEMORY;
	char *copy = graph_str_dup(name);
	if (copy == NULL)
		return GRAPH_ERR_NO_MEMORY;

	graph_retained_remove_port(g, dir, name); /* the live port takes its place */
	GraphPort *p = &g->ports[dir][g->port_count[dir]++];
	*p = (GraphPort){copy, b, (uint32_t)index};
	graph_emit(g, (GraphEvent){.type = added_event[dir], .port = p});
	return GRAPH_OK;
}

/* Remove port i's links and bindings (revert: a sensor-bound input reverts
 * to its default), take it out of the array, report it, free it. */
static void port_remove_at(Graph *g, enum graph_port_dir dir, uint32_t i, bool revert)
{
	graph_links_remove_port(g, dir, g->ports[dir][i].name);
	graph_bindings_remove_port(g, dir, g->ports[dir][i].name, revert); /* plan task 70 */
	GraphPort p = g->ports[dir][i];
	memmove(&g->ports[dir][i], &g->ports[dir][i + 1],
		(g->port_count[dir] - i - 1) * sizeof(GraphPort));
	g->port_count[dir]--;
	graph_emit(g, (GraphEvent){.type = removed_event[dir], .port = &p});
	free(p.name);
}

enum graph_status graph_remove_port(Graph *g, enum graph_port_dir dir, const char *name)
{
	if (!graph_id_valid(name))
		return GRAPH_ERR_INVALID_ID;
	int32_t i = port_index(g, dir, name);
	if (i < 0)
		return GRAPH_ERR_UNKNOWN_PORT;
	port_remove_at(g, dir, (uint32_t)i, true);
	return GRAPH_OK;
}

enum graph_status graph_rename_port(Graph *g, enum graph_port_dir dir, const char *from,
				    const char *to)
{
	if (!graph_id_valid(from) || !graph_id_valid(to))
		return GRAPH_ERR_INVALID_ID;
	int32_t i = port_index(g, dir, from);
	if (i < 0)
		return GRAPH_ERR_UNKNOWN_PORT;
	if (port_index(g, dir, to) >= 0)
		return GRAPH_ERR_PORT_EXISTS;
	char *name = graph_str_dup(to);
	if (name == NULL)
		return GRAPH_ERR_NO_MEMORY;
	/* Bindings follow the rename (plan task 70), unlike links. */
	if (!graph_bindings_rename_port(g, dir, from, to)) {
		free(name);
		return GRAPH_ERR_NO_MEMORY;
	}
	/* A link names the port: the renamed port is not its port any more
	 * (decided, plan task 21f; graphs/graph.h). */
	graph_links_remove_port(g, dir, from);
	graph_retained_remove_port(g, dir, to); /* the live port takes its place */
	GraphPort *p = &g->ports[dir][i];
	char *old = p->name;
	p->name = name;
	graph_emit(g, (GraphEvent){.type = renamed_event[dir], .port = p, .old_name = old});
	free(old);
	return GRAPH_OK;
}

void graph_ports_remove_block(Graph *g, const Block *b)
{
	for (int dir = GRAPH_INPORT; dir <= GRAPH_OUTPORT; dir++) {
		uint32_t i = 0;
		while (i < g->port_count[dir]) {
			if (g->ports[dir][i].block == b)
				port_remove_at(g, (enum graph_port_dir)dir, i, false);
			else
				i++;
		}
	}
}

void graph_ports_free(Graph *g)
{
	for (int dir = GRAPH_INPORT; dir <= GRAPH_OUTPORT; dir++) {
		for (uint32_t i = 0; i < g->port_count[dir]; i++)
			free(g->ports[dir][i].name);
		free(g->ports[dir]);
		g->ports[dir] = NULL;
		g->port_count[dir] = 0;
		g->port_cap[dir] = 0;
	}
}
