/*
 * The graph model's structural operations (01 §2.2, 02 §3.4, 02 §3.4.1). See
 * graphs/graph.h for the validation order, event reporting and value delivery
 * rules.
 */
#include "graphs/graph.h"

#include <stdlib.h>
#include <string.h>

#include "graphs/graph_internal.h"

/* Initial capacity of a graph's block array and of an output's link array;
 * both double when full. */
#define BLOCKS_INITIAL 8
#define LINKS_INITIAL 2

void graph_emit(Graph *g, GraphEvent ev)
{
	ev.graph = g;
	if (g->rt->on_event != NULL)
		g->rt->on_event(g->rt->event_ctx, &ev);
}

char *graph_str_dup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *copy = malloc(len);
	if (copy != NULL)
		memcpy(copy, s, len);
	return copy;
}

/* A heap copy of s, or NULL for NULL; *ok is false if memory runs out. */
static char *str_dup_opt(const char *s, bool *ok)
{
	if (s == NULL)
		return NULL;
	char *copy = graph_str_dup(s);
	if (copy == NULL)
		*ok = false;
	return copy;
}

void block_metadata_release(BlockMetadata *md)
{
	free(md->extra);
	md->extra = NULL;
}

void block_change_release(BlockChange *chg)
{
	free(chg->extra);
	chg->extra = NULL;
}

/* Whether blocks of type have a timer (01 §2.3). */
static bool has_timer(const BlockTypeDef *type)
{
	return type != NULL && type->timer != TIMER_NONE;
}

/* Disarm b's timer and give back its room in the timer heap; b is about to
 * be freed. */
static void block_release_timer(Block *b)
{
	block_timer_cancel(b);
	if (has_timer(b->type))
		b->graph->rt->timer_blocks--;
}

/* ---- Graph ----------------------------------------------------------------- */

Graph *graph_create(Runtime *rt)
{
	Graph *g = calloc(1, sizeof(*g));
	if (g != NULL)
		g->rt = rt;
	return g;
}

static void block_free(Block *b);

/* Free inner block c of a composite instance with everything it owns: its
 * watches detached, its timer's room given back, its slot freed (queued
 * entries for it go stale), its own inner blocks likewise. */
static void inner_dispose(Block *c)
{
	block_detach_watches(c);
	block_release_timer(c);
	runtime_slot_free(c->graph->rt, c->handle);
	block_free(c);
}

static void block_free(Block *b)
{
	for (uint32_t j = 0; j < b->inner_count; j++)
		inner_dispose(b->inner[j]);
	free(b->inner);
	for (uint32_t i = 0; i < b->input_count; i++) {
		linkable_clear(&b->in_values[i]);
		linkable_clear(&b->inputs[i].initial);
		free(b->inputs[i].edge_metadata);
	}
	for (uint32_t i = 0; i < b->output_count; i++) {
		linkable_clear(&b->out_values[i]);
		linkable_clear(&b->outputs[i].last_propagated);
		free(b->outputs[i].links);
	}
	free(b->in_values); /* out_values shares this allocation */
	free(b->inputs);
	free(b->outputs);
	free(b->state);
	free(b->name);
	free(b->component);
	block_metadata_release(&b->metadata);
	free(b);
}

void graph_destroy(Graph *g)
{
	if (g == NULL)
		return;
	graph_stop(g);
	graph_links_drop_graph(g); /* links between processes: silently */
	/* Every edge ends in this graph, so the blocks go without severing
	 * (a composite instance's inner blocks with it, block_free). */
	for (uint32_t i = 0; i < g->block_count; i++) {
		Block *b = g->blocks[i];
		block_detach_watches(b);
		block_release_timer(b);
		runtime_slot_free(g->rt, b->handle);
		block_free(b);
	}
	free(g->blocks);
	graph_ports_free(g);
	graph_retained_free(g);
	free(g->id);
	free(g->name);
	free(g->description);
	free(g->icon);
	free(g);
}

bool graph_id_valid(const char *id)
{
	if (id == NULL)
		return false;
	/* memchr stops at the first match (C17 7.24.5.1), so it never reads
	 * past a shorter string's NUL. */
	const char *nul = memchr(id, '\0', GRAPH_ID_MAX + 1);
	return nul != NULL && nul != id;
}

static int32_t block_index(const Graph *g, const char *name, size_t len)
{
	for (uint32_t i = 0; i < g->block_count; i++) {
		const char *n = g->blocks[i]->name;
		if (strncmp(n, name, len) == 0 && n[len] == '\0')
			return (int32_t)i;
	}
	return -1;
}

Block *graph_find_block_n(const Graph *g, const char *name, size_t len)
{
	int32_t i = block_index(g, name, len);
	return i < 0 ? NULL : g->blocks[i];
}

Block *graph_find_block(const Graph *g, const char *name)
{
	return graph_find_block_n(g, name, strlen(name));
}

bool port_ref_split(const char *ref, size_t *block_len, const char **port)
{
	const char *dot = strrchr(ref, '.');
	if (dot == NULL || dot == ref || dot[1] == '\0')
		return false;
	*block_len = (size_t)(dot - ref);
	*port = dot + 1;
	return true;
}

/* ---- Ports ----------------------------------------------------------------- */

static int32_t port_index(const PortDescriptor *ports, uint32_t count, const char *name)
{
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(ports[i].name, name) == 0)
			return (int32_t)i;
	}
	return -1;
}

int32_t block_input_index(const Block *b, const char *name)
{
	return b->type == NULL ? -1 : port_index(b->type->inputs, b->input_count, name);
}

int32_t block_output_index(const Block *b, const char *name)
{
	return b->type == NULL ? -1 : port_index(b->type->outputs, b->output_count, name);
}

/* Resolve an input by name: GRAPH_OK and *index, else UNKNOWN_PORT, or
 * wrong_direction if name is an output of b. */
static enum graph_status find_input(const Block *b, const char *name,
				    enum graph_status wrong_direction, uint32_t *index)
{
	int32_t i = block_input_index(b, name);
	if (i >= 0) {
		*index = (uint32_t)i;
		return GRAPH_OK;
	}
	return block_output_index(b, name) >= 0 ? wrong_direction : GRAPH_ERR_UNKNOWN_PORT;
}

static enum graph_status find_output(const Block *b, const char *name,
				     enum graph_status wrong_direction, uint32_t *index)
{
	int32_t i = block_output_index(b, name);
	if (i >= 0) {
		*index = (uint32_t)i;
		return GRAPH_OK;
	}
	return block_input_index(b, name) >= 0 ? wrong_direction : GRAPH_ERR_UNKNOWN_PORT;
}

/* ---- Blocks ---------------------------------------------------------------- */

/* A new block of type, or a placeholder for component if type is NULL. */
static Block *block_alloc(Graph *g, const char *name, const BlockTypeDef *type,
			  const char *component, const BlockMetadata *md)
{
	Block *b = calloc(1, sizeof(*b));
	if (b == NULL)
		return NULL;
	bool ok = true;
	size_t state_size = type != NULL ? type->state_size : 0;
	b->graph = g;
	b->type = type;
	b->metadata = *md;
	b->metadata.extra = str_dup_opt(md->extra, &ok);
	b->component = type == NULL ? str_dup_opt(component, &ok) : NULL;
	b->input_count = type != NULL ? type->input_count : 0;
	b->output_count = type != NULL ? type->output_count : 0;
	b->timer = deadline_none();
	b->period_ms = type != NULL ? type->period_ms : 0;
	b->name = graph_str_dup(name);
	size_t nvalues = (size_t)b->input_count + b->output_count;
	if (nvalues > 0)
		b->in_values = calloc(nvalues, sizeof(Linkable));
	if (b->input_count > 0)
		b->inputs = calloc(b->input_count, sizeof(Input));
	if (b->output_count > 0)
		b->outputs = calloc(b->output_count, sizeof(Output));
	if (state_size > 0)
		b->state = calloc(1, state_size);
	if (!ok || b->name == NULL || (nvalues > 0 && b->in_values == NULL) ||
	    (b->input_count > 0 && b->inputs == NULL) ||
	    (b->output_count > 0 && b->outputs == NULL) || (state_size > 0 && b->state == NULL)) {
		b->input_count = 0; /* nothing to clear yet */
		b->output_count = 0;
		block_free(b);
		return NULL;
	}
	if (b->in_values != NULL) /* no arithmetic on NULL for a portless type */
		b->out_values = b->in_values + b->input_count;
	for (uint32_t i = 0; i < b->input_count; i++)
		b->in_values[i] = block_input_default(b, i);
	for (uint32_t i = 0; i < b->output_count; i++) {
		const PortDescriptor *p = &b->type->outputs[i];
		b->out_values[i] = p->default_value.type == LINKABLE_UNSET
					   ? linkable_default_value(p->type)
					   : linkable_copy(&p->default_value);
		b->outputs[i].last_propagated = linkable_copy(&b->out_values[i]);
	}
	return b;
}

static bool blocks_reserve(Graph *g)
{
	if (g->block_count < g->block_cap)
		return true;
	if (g->block_cap >= UINT32_MAX / 2)
		return false;
	uint32_t cap = g->block_cap == 0 ? BLOCKS_INITIAL : g->block_cap * 2;
	Block **blocks = realloc(g->blocks, (size_t)cap * sizeof(*blocks));
	if (blocks == NULL)
		return false;
	g->blocks = blocks;
	g->block_cap = cap;
	return true;
}

/* Make input i of b sourced or unsourced: increment its source generation,
 * and for a composite instance the mapped inner input's, the one queued
 * values are sent under (03 §5), so they go stale. */
void input_source_changed(Block *b, uint32_t i)
{
	b->inputs[i].source_gen++;
	uint32_t k = i;
	Block *t = block_input_target(b, &k);
	if (t != b)
		t->inputs[k].source_gen++;
}

/* The output whose links carry output out of b (block_output_source). */
Output *block_links_output(Block *b, uint32_t out)
{
	Block *s = block_output_source(b, &out);
	return &s->outputs[out];
}

/* Build the inner blocks of the composite instance outer (03 §5 "Runtime
 * behavior" steps 1-5): create each inner block (with a slot, and room for
 * its timer), nested composites recursively, then wire the internal edges
 * and initials. Nothing is started and nothing is reported: inner blocks are
 * not graph nodes. On failure (NO_MEMORY) the inner blocks made so far are
 * in outer->inner, freed with outer (block_free). */
static enum graph_status composite_build(Block *outer)
{
	const CompositeDef *def = outer->type->composite;
	Graph *g = outer->graph;
	if (def->block_count > 0) {
		outer->inner = calloc(def->block_count, sizeof(*outer->inner));
		if (outer->inner == NULL)
			return GRAPH_ERR_NO_MEMORY;
	}
	for (uint32_t j = 0; j < def->block_count; j++) {
		const BlockTypeDef *type = def->blocks[j].type;
		BlockMetadata md = block_metadata_default();
		if (!runtime_slot_reserve(g->rt) ||
		    (has_timer(type) &&
		     !deadline_heap_reserve(&g->rt->timers, g->rt->timer_blocks + 1)))
			return GRAPH_ERR_NO_MEMORY;
		Block *c = block_alloc(g, def->blocks[j].name, type, NULL, &md);
		if (c == NULL)
			return GRAPH_ERR_NO_MEMORY;
		if (has_timer(type))
			g->rt->timer_blocks++;
		c->handle = runtime_slot_alloc(g->rt, c);
		c->parent = outer;
		outer->inner[outer->inner_count++] = c;
		if (block_is_composite(c)) {
			enum graph_status st = composite_build(c);
			if (st != GRAPH_OK)
				return st;
		}
	}
	for (uint32_t k = 0; k < def->edge_count; k++) {
		const CompositeEdge *e = &def->edges[k];
		Block *sb = outer->inner[e->src.block];
		Block *tb = outer->inner[e->tgt.block];
		Output *o = block_links_output(sb, e->src.port);
		if (!output_links_reserve(o))
			return GRAPH_ERR_NO_MEMORY;
		o->links[o->link_count++] = (Link){tb, e->tgt.port};
		Input *in = &tb->inputs[e->tgt.port];
		in->source = INPUT_SOURCE_EDGE;
		in->src_block = sb;
		in->src_output = e->src.port;
		input_source_changed(tb, e->tgt.port);
	}
	for (uint32_t k = 0; k < def->initial_count; k++) {
		const CompositeInitial *init = &def->initials[k];
		Block *tb = outer->inner[init->tgt.block];
		Input *in = &tb->inputs[init->tgt.port];
		linkable_assign(&in->initial, &init->value);
		in->source = INPUT_SOURCE_INITIAL;
		input_source_changed(tb, init->tgt.port);
		/* Not started: the value is only stored (03 §2). */
		block_update(tb, init->tgt.port, linkable_copy(&in->initial));
	}
	return GRAPH_OK;
}

/* graph_create_block (type set) and graph_create_placeholder (type NULL). */
static enum graph_status create_block(Graph *g, const char *name, const BlockTypeDef *type,
				      const char *component, const BlockMetadata *md, Block **out)
{
	if (out != NULL)
		*out = NULL;
	if (!graph_id_valid(name))
		return GRAPH_ERR_INVALID_ID;
	if (graph_find_block(g, name) != NULL)
		return GRAPH_ERR_NODE_EXISTS;
	BlockMetadata m = md != NULL ? *md : block_metadata_default();
	if (type == NULL && component == NULL)
		return GRAPH_ERR_INVALID_ARGUMENT;
	if (!blocks_reserve(g) || !runtime_slot_reserve(g->rt) ||
	    (has_timer(type) && !deadline_heap_reserve(&g->rt->timers, g->rt->timer_blocks + 1)))
		return GRAPH_ERR_NO_MEMORY;
	Block *b = block_alloc(g, name, type, component, &m);
	if (b == NULL)
		return GRAPH_ERR_NO_MEMORY;

	if (has_timer(type))
		g->rt->timer_blocks++; /* its deadline's room in the heap */
	b->handle = runtime_slot_alloc(g->rt, b);
	if (block_is_composite(b) && composite_build(b) != GRAPH_OK) {
		runtime_slot_free(g->rt, b->handle);
		block_free(b); /* and the inner blocks made so far */
		return GRAPH_ERR_NO_MEMORY;
	}
	g->blocks[g->block_count++] = b;
	if (g->started)
		block_start(b); /* 01 §2.2: a block added to a started graph starts at once */
	graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_BLOCK_ADDED, .block = b});
	if (out != NULL)
		*out = b;
	return GRAPH_OK;
}

enum graph_status graph_create_block(Graph *g, const char *name, const BlockTypeDef *type,
				     const BlockMetadata *md, Block **out)
{
	return create_block(g, name, type, NULL, md, out); /* a NULL type is refused */
}

enum graph_status graph_create_placeholder(Graph *g, const char *name, const char *component,
					   const BlockMetadata *md, Block **out)
{
	return create_block(g, name, NULL, component, md, out);
}

void output_unlink(Output *o, const Block *dst, uint32_t input)
{
	for (uint32_t k = 0; k < o->link_count; k++) {
		if (o->links[k].block == dst && o->links[k].input == input) {
			memmove(&o->links[k], &o->links[k + 1],
				(o->link_count - k - 1) * sizeof(o->links[0]));
			o->link_count--;
			return;
		}
	}
}

/* Make input i of b unsourced; queued values from the old source go stale. */
void input_clear_source(Block *b, uint32_t i)
{
	Input *in = &b->inputs[i];
	in->source = INPUT_SOURCE_NONE;
	in->src_block = NULL;
	in->src_output = 0;
	linkable_clear(&in->initial);
	free(in->edge_metadata);
	in->edge_metadata = NULL;
	input_source_changed(b, i);
}

/* Severing (01 §2.2): the input reverts to its port default at once, which
 * propagates like any other input change. */
void input_revert(Block *b, uint32_t i)
{
	block_update(b, i, block_input_default(b, i));
}

enum graph_status graph_remove_block(Graph *g, const char *name)
{
	if (!graph_id_valid(name))
		return GRAPH_ERR_INVALID_ID;
	int32_t index = block_index(g, name, strlen(name));
	if (index < 0)
		return GRAPH_ERR_UNKNOWN_NODE;
	Block *b = g->blocks[index];

	/* 03 §2 destroy: as stop() first, so the block computes no more and
	 * reverting one of its own inputs (a self-loop) cannot run it. */
	block_stop(b);
	block_release_timer(b); /* queued expiries for b go stale */

	/* Inbound edges and initials. A self-loop is removed here, so the
	 * outbound pass below only sees links to other blocks. */
	for (uint32_t i = 0; i < b->input_count; i++) {
		Input *in = &b->inputs[i];
		if (in->source == INPUT_SOURCE_EDGE) {
			Block *src = in->src_block;
			uint32_t out = in->src_output;
			output_unlink(block_links_output(src, out), b, i);
			input_clear_source(b, i);
			graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_EDGE_REMOVED,
						   .block = b,
						   .input = i,
						   .src_block = src,
						   .src_output = out});
		} else if (in->source == INPUT_SOURCE_INITIAL) {
			Linkable literal = in->initial; /* a move */
			in->initial = (Linkable){0};
			input_clear_source(b, i);
			graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_INITIAL_REMOVED,
						   .block = b,
						   .input = i,
						   .value = &literal});
			linkable_clear(&literal);
		}
	}

	/* Outbound links: each downstream input is severed and reverts. A
	 * composite instance's links are on its inner outputs (03 §5), next
	 * to the inner links, which stay: only those whose destination's
	 * source is b's output o are b's. Links between processes go with
	 * the outports below. */
	for (uint32_t o = 0; o < b->output_count; o++) {
		Output *out = block_links_output(b, o);
		uint32_t k = 0;
		while (k < out->link_count) {
			Link l = out->links[k];
			const Input *dst = &l.block->inputs[l.input];
			if (dst->source != INPUT_SOURCE_EDGE || dst->src_block != b ||
			    dst->src_output != o) {
				k++;
				continue;
			}
			output_unlink(out, l.block, l.input);
			input_clear_source(l.block, l.input);
			graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_EDGE_REMOVED,
						   .block = l.block,
						   .input = l.input,
						   .src_block = b,
						   .src_output = o});
			input_revert(l.block, l.input);
		}
	}

	/* Graph ports mapped to the block go before it (decided, 02 §3.4),
	 * each with its links between processes (plan task 21f). */
	graph_ports_remove_block(g, b);
	graph_retained_remove_block(g, b); /* no events (05 §2.2) */
	block_detach_watches(b);
	memmove(&g->blocks[index], &g->blocks[index + 1],
		(g->block_count - (uint32_t)index - 1) * sizeof(g->blocks[0]));
	g->block_count--;
	runtime_slot_free(g->rt, b->handle); /* queued entries for b go stale */
	graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_BLOCK_REMOVED, .block = b});
	block_free(b);
	return GRAPH_OK;
}

enum graph_status graph_rename_block(Graph *g, const char *from, const char *to)
{
	if (!graph_id_valid(from) || !graph_id_valid(to))
		return GRAPH_ERR_INVALID_ID;
	Block *b = graph_find_block(g, from);
	if (b == NULL)
		return GRAPH_ERR_UNKNOWN_NODE;
	if (graph_find_block(g, to) != NULL)
		return GRAPH_ERR_NODE_EXISTS;
	char *name = graph_str_dup(to);
	if (name == NULL)
		return GRAPH_ERR_NO_MEMORY;
	char *old = b->name;
	b->name = name;
	graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_BLOCK_RENAMED, .block = b, .old_name = old});
	free(old);
	return GRAPH_OK;
}

enum graph_status graph_change_block(Graph *g, const char *name, const BlockChange *chg)
{
	if (!graph_id_valid(name))
		return GRAPH_ERR_INVALID_ID;
	Block *b = graph_find_block(g, name);
	if (b == NULL)
		return GRAPH_ERR_UNKNOWN_NODE;
	if (chg == NULL)
		return GRAPH_ERR_INVALID_ARGUMENT;
	bool ok = true;
	char *extra = chg->set_extra ? str_dup_opt(chg->extra, &ok) : NULL;
	if (!ok)
		return GRAPH_ERR_NO_MEMORY;
	if (chg->set_extra) {
		free(b->metadata.extra);
		b->metadata.extra = extra;
	}
	if (chg->set_x)
		b->metadata.x = chg->x;
	if (chg->set_y)
		b->metadata.y = chg->y;
	graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_BLOCK_CHANGED, .block = b});
	return GRAPH_OK;
}

/* ---- Edges and initials ------------------------------------------------- */

bool output_links_reserve(Output *o)
{
	if (o->link_count < o->link_cap)
		return true;
	if (o->link_cap >= UINT32_MAX / 2)
		return false;
	uint32_t cap = o->link_cap == 0 ? LINKS_INITIAL : o->link_cap * 2;
	Link *links = realloc(o->links, (size_t)cap * sizeof(*links));
	if (links == NULL)
		return false;
	o->links = links;
	o->link_cap = cap;
	return true;
}

enum graph_status graph_connect(Graph *g, const char *src, const char *src_port, const char *tgt,
				const char *tgt_port, const char *metadata)
{
	if (!graph_id_valid(src) || !graph_id_valid(tgt))
		return GRAPH_ERR_INVALID_ID;
	Block *sb = graph_find_block(g, src);
	if (sb == NULL)
		return GRAPH_ERR_UNKNOWN_NODE;
	uint32_t out;
	enum graph_status st = find_output(sb, src_port, GRAPH_ERR_INVALID_EDGE_DIRECTION, &out);
	if (st != GRAPH_OK)
		return st;
	Block *tb = graph_find_block(g, tgt);
	if (tb == NULL)
		return GRAPH_ERR_UNKNOWN_NODE;
	uint32_t in;
	st = find_input(tb, tgt_port, GRAPH_ERR_INVALID_EDGE_DIRECTION, &in);
	if (st != GRAPH_OK)
		return st;
	if (tb->inputs[in].source != INPUT_SOURCE_NONE)
		return GRAPH_ERR_INPUT_CONNECTED; /* no fan-in (01 §2.2) */
	Output *o = block_links_output(sb, out); /* a composite's inner output (03 §5) */
	bool ok = true;
	char *md = str_dup_opt(metadata, &ok);
	if (!ok || !output_links_reserve(o)) {
		free(md);
		return GRAPH_ERR_NO_MEMORY;
	}

	graph_retained_remove_input(g, tb, tgt_port); /* the live edge takes its place */
	o->links[o->link_count++] = (Link){tb, in};
	Input *dst = &tb->inputs[in];
	dst->edge_metadata = md;
	dst->source = INPUT_SOURCE_EDGE;
	dst->src_block = sb;
	dst->src_output = out;
	input_source_changed(tb, in);
	graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_EDGE_ADDED,
				   .block = tb,
				   .input = in,
				   .src_block = sb,
				   .src_output = out});
	/* 03 §2 connect: wiring delivers the signal already on the output. In
	 * a started graph every output was pushed by start, so last_propagated
	 * is a value that was actually propagated. */
	if (g->started)
		block_update(tb, in, linkable_copy(&o->last_propagated));
	return GRAPH_OK;
}

/* Resolve the existing edge src.src_port → tgt.tgt_port for removeedge and
 * changeedge: GRAPH_OK with *sbp, *out, *tbp, *in, else the refusal. */
static enum graph_status find_edge(Graph *g, const char *src, const char *src_port,
				   const char *tgt, const char *tgt_port, Block **sbp,
				   uint32_t *outp, Block **tbp, uint32_t *inp)
{
	if (!graph_id_valid(src) || !graph_id_valid(tgt))
		return GRAPH_ERR_INVALID_ID;
	Block *sb = graph_find_block(g, src);
	if (sb == NULL)
		return GRAPH_ERR_UNKNOWN_NODE;
	uint32_t out;
	enum graph_status st = find_output(sb, src_port, GRAPH_ERR_NO_SUCH_CONNECTION, &out);
	if (st != GRAPH_OK)
		return st;
	Block *tb = graph_find_block(g, tgt);
	if (tb == NULL)
		return GRAPH_ERR_UNKNOWN_NODE;
	uint32_t in;
	st = find_input(tb, tgt_port, GRAPH_ERR_NO_SUCH_CONNECTION, &in);
	if (st != GRAPH_OK)
		return st;
	const Input *dst = &tb->inputs[in];
	if (dst->source != INPUT_SOURCE_EDGE || dst->src_block != sb || dst->src_output != out)
		return GRAPH_ERR_NO_SUCH_CONNECTION;
	*sbp = sb;
	*outp = out;
	*tbp = tb;
	*inp = in;
	return GRAPH_OK;
}

enum graph_status graph_disconnect(Graph *g, const char *src, const char *src_port,
				   const char *tgt, const char *tgt_port)
{
	Block *sb, *tb;
	uint32_t out, in;
	enum graph_status st = find_edge(g, src, src_port, tgt, tgt_port, &sb, &out, &tb, &in);
	if (st != GRAPH_OK)
		return st;

	output_unlink(block_links_output(sb, out), tb, in);
	input_clear_source(tb, in);
	graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_EDGE_REMOVED,
				   .block = tb,
				   .input = in,
				   .src_block = sb,
				   .src_output = out});
	input_revert(tb, in);
	return GRAPH_OK;
}

enum graph_status graph_change_edge(Graph *g, const char *src, const char *src_port,
				    const char *tgt, const char *tgt_port, const char *metadata)
{
	Block *sb, *tb;
	uint32_t out, in;
	enum graph_status st = find_edge(g, src, src_port, tgt, tgt_port, &sb, &out, &tb, &in);
	if (st != GRAPH_OK)
		return st;
	bool ok = true;
	char *md = str_dup_opt(metadata, &ok);
	if (!ok)
		return GRAPH_ERR_NO_MEMORY;
	Input *dst = &tb->inputs[in];
	free(dst->edge_metadata);
	dst->edge_metadata = md;
	graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_EDGE_CHANGED,
				   .block = tb,
				   .input = in,
				   .src_block = sb,
				   .src_output = out});
	return GRAPH_OK;
}

enum graph_status graph_add_initial(Graph *g, const char *node, const char *port,
				    const Linkable *value)
{
	if (!graph_id_valid(node))
		return GRAPH_ERR_INVALID_ID;
	Block *b = graph_find_block(g, node);
	if (b == NULL)
		return GRAPH_ERR_UNKNOWN_NODE;
	uint32_t i;
	enum graph_status st = find_input(b, port, GRAPH_ERR_INVALID_EDGE_DIRECTION, &i);
	if (st != GRAPH_OK)
		return st;
	Input *in = &b->inputs[i];
	if (in->source == INPUT_SOURCE_EDGE || in->source == INPUT_SOURCE_LINK)
		return GRAPH_ERR_INPUT_CONNECTED; /* no fan-in (01 §2.2) */
	if (value == NULL)
		return GRAPH_ERR_INVALID_ARGUMENT;

	/* Adding or replacing: either way a source change (01 §3.2). */
	graph_retained_remove_input(g, b, port); /* the live initial takes its place */
	linkable_assign(&in->initial, value);
	in->source = INPUT_SOURCE_INITIAL;
	input_source_changed(b, i);
	graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_INITIAL_ADDED,
				   .block = b,
				   .input = i,
				   .value = &in->initial});
	block_update(b, i, linkable_copy(&in->initial));
	return GRAPH_OK;
}

enum graph_status graph_remove_initial(Graph *g, const char *node, const char *port)
{
	if (!graph_id_valid(node))
		return GRAPH_ERR_INVALID_ID;
	Block *b = graph_find_block(g, node);
	if (b == NULL)
		return GRAPH_ERR_UNKNOWN_NODE;
	uint32_t i;
	enum graph_status st = find_input(b, port, GRAPH_ERR_NO_SUCH_CONNECTION, &i);
	if (st != GRAPH_OK)
		return st;
	Input *in = &b->inputs[i];
	if (in->source != INPUT_SOURCE_INITIAL)
		return GRAPH_ERR_NO_SUCH_CONNECTION;

	Linkable literal = in->initial; /* a move */
	in->initial = (Linkable){0};
	input_clear_source(b, i);
	graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_INITIAL_REMOVED,
				   .block = b,
				   .input = i,
				   .value = &literal});
	linkable_clear(&literal);
	input_revert(b, i);
	return GRAPH_OK;
}

/* ---- Start / stop (01 §2.2) ------------------------------------------------ */

void graph_start(Graph *g)
{
	if (g->started)
		return;
	g->started = true;
	for (uint32_t i = 0; i < g->block_count; i++)
		block_start(g->blocks[i]);
}

void graph_stop(Graph *g)
{
	if (!g->started)
		return;
	for (uint32_t i = 0; i < g->block_count; i++)
		block_stop(g->blocks[i]);
	g->started = false;
}
