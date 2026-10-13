/*
 * Links between processes (01 §2.2 Port; plan task 21f): one graph's outport
 * feeding another graph's inport, carried exactly like an edge. See
 * graphs/graph.h "Links between processes".
 */
#include <stdlib.h>
#include <string.h>

#include "graphs/graph_internal.h"

/* Initial capacity of the runtime's link array; it doubles when full. */
#define PROCESS_LINKS_INITIAL 4

static int32_t link_index(const Runtime *rt, const Graph *src, const char *outport,
			  const Graph *tgt, const char *inport)
{
	for (uint32_t i = 0; i < rt->link_count; i++) {
		const ProcessLink *l = &rt->links[i];
		if (l->src_graph == src && l->tgt_graph == tgt &&
		    strcmp(l->src_port, outport) == 0 && strcmp(l->tgt_port, inport) == 0)
			return (int32_t)i;
	}
	return -1;
}

static bool links_reserve(Runtime *rt)
{
	if (rt->link_count < rt->link_cap)
		return true;
	if (rt->link_cap >= UINT32_MAX / 2)
		return false;
	uint32_t cap = rt->link_cap == 0 ? PROCESS_LINKS_INITIAL : rt->link_cap * 2;
	ProcessLink *links = realloc(rt->links, (size_t)cap * sizeof(*links));
	if (links == NULL)
		return false;
	rt->links = links;
	rt->link_cap = cap;
	return true;
}

/* The port of g called name in direction dir: GRAPH_OK and *out, else
 * INVALID_LINK_DIRECTION if it is a port of the other direction only, else
 * UNKNOWN_PORT. */
static enum graph_status find_port(const Graph *g, enum graph_port_dir dir, const char *name,
				   const GraphPort **out)
{
	*out = graph_find_port(g, dir, name);
	if (*out != NULL)
		return GRAPH_OK;
	enum graph_port_dir other = dir == GRAPH_INPORT ? GRAPH_OUTPORT : GRAPH_INPORT;
	return graph_find_port(g, other, name) != NULL ? GRAPH_ERR_INVALID_LINK_DIRECTION
						       : GRAPH_ERR_UNKNOWN_PORT;
}

enum graph_status graph_link_add(Graph *src, const char *outport, Graph *tgt, const char *inport)
{
	if (!graph_id_valid(outport) || !graph_id_valid(inport))
		return GRAPH_ERR_INVALID_ID;
	const GraphPort *op, *ip;
	enum graph_status st = find_port(src, GRAPH_OUTPORT, outport, &op);
	if (st != GRAPH_OK)
		return st;
	st = find_port(tgt, GRAPH_INPORT, inport, &ip);
	if (st != GRAPH_OK)
		return st;
	Runtime *rt = src->rt;
	if (tgt->rt != rt)
		return GRAPH_ERR_INVALID_ARGUMENT;
	if (link_index(rt, src, outport, tgt, inport) >= 0)
		return GRAPH_ERR_LINK_EXISTS;
	Block *tb = ip->block;
	uint32_t in = ip->port;
	Input *dst = &tb->inputs[in];
	if (dst->source != INPUT_SOURCE_NONE ||
	    graph_input_retained(tgt, tb, block_input_port(tb, in)->name))
		return GRAPH_ERR_INPUT_CONNECTED; /* no fan-in (01 §2.2) */
	Block *sb = op->block;
	uint32_t out = op->port;
	Output *o = block_links_output(sb, out); /* a composite's inner output (03 §5) */
	char *src_name = graph_str_dup(outport);
	char *tgt_name = graph_str_dup(inport);
	if (src_name == NULL || tgt_name == NULL || !links_reserve(rt) ||
	    !output_links_reserve(o)) {
		free(src_name);
		free(tgt_name);
		return GRAPH_ERR_NO_MEMORY;
	}

	ProcessLink *l = &rt->links[rt->link_count++];
	*l = (ProcessLink){src, src_name, tgt, tgt_name, sb, out, tb, in};
	o->links[o->link_count++] = (Link){tb, in};
	dst->source = INPUT_SOURCE_LINK;
	dst->src_block = sb;
	dst->src_output = out;
	input_source_changed(tb, in);
	graph_emit(src, (GraphEvent){.type = GRAPH_EVENT_LINK_ADDED, .link = l});
	/* Like connect (03 §2): the signal already on a started source's
	 * output, whose every output start pushed. */
	if (src->started)
		block_update(tb, in, linkable_copy(&o->last_propagated));
	return GRAPH_OK;
}

/* Remove link i: unlink it from its output and unsource its input; if
 * report, report LINK_REMOVED, then sever the input (it reverts to its
 * default, which propagates). */
static void link_remove_at(Runtime *rt, uint32_t i, bool report)
{
	ProcessLink l = rt->links[i];
	memmove(&rt->links[i], &rt->links[i + 1], (rt->link_count - i - 1) * sizeof(rt->links[0]));
	rt->link_count--;
	output_unlink(block_links_output(l.src_block, l.src_output), l.tgt_block, l.tgt_input);
	input_clear_source(l.tgt_block, l.tgt_input); /* queued values go stale */
	if (report) {
		graph_emit(l.src_graph, (GraphEvent){.type = GRAPH_EVENT_LINK_REMOVED, .link = &l});
		input_revert(l.tgt_block, l.tgt_input);
	}
	free(l.src_port);
	free(l.tgt_port);
}

enum graph_status graph_link_remove(Graph *src, const char *outport, Graph *tgt,
				    const char *inport)
{
	if (!graph_id_valid(outport) || !graph_id_valid(inport))
		return GRAPH_ERR_INVALID_ID;
	int32_t i = link_index(src->rt, src, outport, tgt, inport);
	if (i < 0)
		return GRAPH_ERR_NO_SUCH_LINK;
	link_remove_at(src->rt, (uint32_t)i, true);
	return GRAPH_OK;
}

/* Remove every link of g (report: as graph_link_remove; else silently). */
static void remove_graph(Graph *g, bool report)
{
	Runtime *rt = g->rt;
	uint32_t i = 0;
	while (i < rt->link_count) {
		const ProcessLink *l = &rt->links[i];
		if (l->src_graph == g || l->tgt_graph == g)
			link_remove_at(rt, i, report);
		else
			i++;
	}
}

void graph_links_remove_graph(Graph *g)
{
	remove_graph(g, true);
}

void graph_links_drop_graph(Graph *g)
{
	remove_graph(g, false);
}

void graph_links_remove_port(Graph *g, enum graph_port_dir dir, const char *name)
{
	Runtime *rt = g->rt;
	uint32_t i = 0;
	while (i < rt->link_count) {
		const ProcessLink *l = &rt->links[i];
		bool match = dir == GRAPH_OUTPORT
				     ? l->src_graph == g && strcmp(l->src_port, name) == 0
				     : l->tgt_graph == g && strcmp(l->tgt_port, name) == 0;
		if (match)
			link_remove_at(rt, i, true);
		else
			i++;
	}
}
