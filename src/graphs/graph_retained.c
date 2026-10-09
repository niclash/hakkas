/*
 * Retained entries of placeholder blocks (05 §2.2): the connections and graph
 * ports of a loaded graph file that reference a block whose type is not
 * registered. They cannot be live links, so the graph only keeps them, to be
 * written back unchanged on save. See graphs/graph.h.
 */
#include <stdlib.h>
#include <string.h>

#include "graphs/graph_internal.h"

/* Initial capacity of a retained array; it doubles when full. */
#define RETAINED_INITIAL 4

/* items (count used of *cap, elem bytes each) with room for one more: the
 * same or a grown array, or NULL, with items untouched, if memory runs out. */
static void *reserve(void *items, uint32_t count, uint32_t *cap, size_t elem)
{
	if (count < *cap)
		return items;
	if (*cap >= UINT32_MAX / 2)
		return NULL;
	uint32_t n = *cap == 0 ? RETAINED_INITIAL : *cap * 2;
	void *p = realloc(items, (size_t)n * elem);
	if (p != NULL)
		*cap = n;
	return p;
}

static bool in_graph(const Graph *g, const Block *b)
{
	return b != NULL && b->graph == g && graph_find_block(g, b->name) == b;
}

static void connection_free(RetainedConnection *c)
{
	free(c->src_port);
	free(c->tgt_port);
	free(c->metadata);
	linkable_clear(&c->data);
}

static void port_free(RetainedPort *p)
{
	free(p->name);
	free(p->port);
}

static int32_t retained_input_index(const Graph *g, const Block *b, const char *port)
{
	for (uint32_t i = 0; i < g->retained_count; i++) {
		const RetainedConnection *c = &g->retained[i];
		if (c->tgt == b && strcmp(c->tgt_port, port) == 0)
			return (int32_t)i;
	}
	return -1;
}

bool graph_input_retained(const Graph *g, const Block *b, const char *port)
{
	return retained_input_index(g, b, port) >= 0;
}

static int32_t retained_port_index(const Graph *g, enum graph_port_dir dir, const char *name)
{
	for (uint32_t i = 0; i < g->retained_port_count[dir]; i++) {
		if (strcmp(g->retained_ports[dir][i].name, name) == 0)
			return (int32_t)i;
	}
	return -1;
}

bool graph_port_retained(const Graph *g, enum graph_port_dir dir, const char *name)
{
	return retained_port_index(g, dir, name) >= 0;
}

/* The end of a retained connection that is a real block: its port must exist
 * with the right direction, as addedge / addinitial check it. */
static enum graph_status check_real_end(const Block *b, const char *port, bool input)
{
	if (block_is_placeholder(b))
		return GRAPH_OK;
	int32_t want = input ? block_input_index(b, port) : block_output_index(b, port);
	if (want >= 0)
		return GRAPH_OK;
	int32_t other = input ? block_output_index(b, port) : block_input_index(b, port);
	return other >= 0 ? GRAPH_ERR_INVALID_EDGE_DIRECTION : GRAPH_ERR_UNKNOWN_PORT;
}

enum graph_status graph_retain_connection(Graph *g, const RetainedConnection *c)
{
	if (c == NULL || c->tgt_port == NULL || !in_graph(g, c->tgt))
		return GRAPH_ERR_INVALID_ARGUMENT;
	bool edge = c->src != NULL;
	if (edge != (c->data.type == LINKABLE_UNSET))
		return GRAPH_ERR_INVALID_ARGUMENT; /* exactly one of src and data */
	if (edge && (c->src_port == NULL || !in_graph(g, c->src)))
		return GRAPH_ERR_INVALID_ARGUMENT;
	if (!block_is_placeholder(c->tgt) && !(edge && block_is_placeholder(c->src)))
		return GRAPH_ERR_INVALID_ARGUMENT; /* a live connection's case */
	enum graph_status st = edge ? check_real_end(c->src, c->src_port, false) : GRAPH_OK;
	if (st == GRAPH_OK)
		st = check_real_end(c->tgt, c->tgt_port, true);
	if (st != GRAPH_OK)
		return st;
	/* No fan-in (01 §2.2), also not among retained connections. */
	if (!block_is_placeholder(c->tgt) &&
	    c->tgt->inputs[block_input_index(c->tgt, c->tgt_port)].source != INPUT_SOURCE_NONE)
		return GRAPH_ERR_INPUT_CONNECTED;
	if (graph_input_retained(g, c->tgt, c->tgt_port))
		return GRAPH_ERR_INPUT_CONNECTED;

	RetainedConnection *items =
		reserve(g->retained, g->retained_count, &g->retained_cap, sizeof(*items));
	if (items == NULL)
		return GRAPH_ERR_NO_MEMORY;
	g->retained = items;

	RetainedConnection copy = {.src = c->src, .tgt = c->tgt};
	copy.tgt_port = graph_str_dup(c->tgt_port);
	bool ok = copy.tgt_port != NULL;
	if (edge) {
		copy.src_port = graph_str_dup(c->src_port);
		ok = ok && copy.src_port != NULL;
	} else {
		copy.data = linkable_copy(&c->data);
	}
	if (edge && c->metadata != NULL) {
		copy.metadata = graph_str_dup(c->metadata);
		ok = ok && copy.metadata != NULL;
	}
	if (!ok) {
		connection_free(&copy);
		return GRAPH_ERR_NO_MEMORY;
	}
	g->retained[g->retained_count++] = copy;
	return GRAPH_OK;
}

enum graph_status graph_retain_port(Graph *g, enum graph_port_dir dir, const char *name,
				    Block *block, const char *port)
{
	if (!graph_id_valid(name))
		return GRAPH_ERR_INVALID_ID;
	if (port == NULL || !in_graph(g, block) || !block_is_placeholder(block))
		return GRAPH_ERR_INVALID_ARGUMENT;
	if (graph_find_port(g, dir, name) != NULL || retained_port_index(g, dir, name) >= 0)
		return GRAPH_ERR_PORT_EXISTS;
	RetainedPort *items = reserve(g->retained_ports[dir], g->retained_port_count[dir],
				      &g->retained_port_cap[dir], sizeof(*items));
	if (items == NULL)
		return GRAPH_ERR_NO_MEMORY;
	g->retained_ports[dir] = items;
	RetainedPort p = {graph_str_dup(name), block, graph_str_dup(port)};
	if (p.name == NULL || p.port == NULL) {
		port_free(&p);
		return GRAPH_ERR_NO_MEMORY;
	}
	g->retained_ports[dir][g->retained_port_count[dir]++] = p;
	return GRAPH_OK;
}

void graph_retained_remove_input(Graph *g, const Block *b, const char *port)
{
	int32_t i = retained_input_index(g, b, port);
	if (i < 0)
		return;
	connection_free(&g->retained[i]);
	memmove(&g->retained[i], &g->retained[i + 1],
		(g->retained_count - (uint32_t)i - 1) * sizeof(g->retained[0]));
	g->retained_count--;
}

void graph_retained_remove_port(Graph *g, enum graph_port_dir dir, const char *name)
{
	int32_t i = retained_port_index(g, dir, name);
	if (i < 0)
		return;
	RetainedPort *ports = g->retained_ports[dir];
	port_free(&ports[i]);
	memmove(&ports[i], &ports[i + 1],
		(g->retained_port_count[dir] - (uint32_t)i - 1) * sizeof(ports[0]));
	g->retained_port_count[dir]--;
}

void graph_retained_remove_block(Graph *g, const Block *b)
{
	uint32_t kept = 0;
	for (uint32_t i = 0; i < g->retained_count; i++) {
		RetainedConnection *c = &g->retained[i];
		if (c->src == b || c->tgt == b)
			connection_free(c);
		else
			g->retained[kept++] = *c;
	}
	g->retained_count = kept;
	for (int dir = GRAPH_INPORT; dir <= GRAPH_OUTPORT; dir++) {
		kept = 0;
		for (uint32_t i = 0; i < g->retained_port_count[dir]; i++) {
			RetainedPort *p = &g->retained_ports[dir][i];
			if (p->block == b)
				port_free(p);
			else
				g->retained_ports[dir][kept++] = *p;
		}
		g->retained_port_count[dir] = kept;
	}
}

void graph_retained_free(Graph *g)
{
	for (uint32_t i = 0; i < g->retained_count; i++)
		connection_free(&g->retained[i]);
	free(g->retained);
	g->retained = NULL;
	g->retained_count = 0;
	g->retained_cap = 0;
	for (int dir = GRAPH_INPORT; dir <= GRAPH_OUTPORT; dir++) {
		for (uint32_t i = 0; i < g->retained_port_count[dir]; i++)
			port_free(&g->retained_ports[dir][i]);
		free(g->retained_ports[dir]);
		g->retained_ports[dir] = NULL;
		g->retained_port_count[dir] = 0;
		g->retained_port_cap[dir] = 0;
	}
}
