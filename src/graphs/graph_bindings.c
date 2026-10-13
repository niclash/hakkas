/*
 * Environment bindings (04 §4; plan task 22): a block input fed by a sensor
 * channel, a block output written to an actuator channel. See graphs/graph.h
 * "Environment bindings".
 */
#include <stdlib.h>
#include <string.h>

#include "graphs/graph_internal.h"

/* Initial capacity of the runtime's binding array; it doubles when full. */
#define BINDINGS_INITIAL 8

static bool bindings_reserve(Runtime *rt)
{
	if (rt->binding_count < rt->binding_cap)
		return true;
	if (rt->binding_cap >= UINT32_MAX / 2)
		return false;
	uint32_t cap = rt->binding_cap == 0 ? BINDINGS_INITIAL : rt->binding_cap * 2;
	EnvBinding *bindings = realloc(rt->bindings, (size_t)cap * sizeof(*bindings));
	if (bindings == NULL)
		return false;
	rt->bindings = bindings;
	rt->binding_cap = cap;
	return true;
}

/* Whether the actuator channel (env_port, env_index) has a driver. */
static bool actuator_driven(const Runtime *rt, uint32_t env_port, uint32_t env_index)
{
	for (uint32_t i = 0; i < rt->binding_count; i++) {
		const EnvBinding *b = &rt->bindings[i];
		if (b->actuator && b->env_port == env_port && b->env_index == env_index)
			return true;
	}
	return false;
}

/* Resolve spec's block and port: GRAPH_OK with *bp and *port, else the
 * refusal (the order of graph_bind_check, up to the direction). */
static enum graph_status resolve(const Graph *g, const EnvBindingSpec *spec, Block **bp,
				 uint32_t *port)
{
	if (spec == NULL)
		return GRAPH_ERR_INVALID_ARGUMENT;
	if (!graph_id_valid(spec->block) || !graph_id_valid(spec->port))
		return GRAPH_ERR_INVALID_ID;
	Block *b = graph_find_block(g, spec->block);
	if (b == NULL)
		return GRAPH_ERR_UNKNOWN_NODE;
	int32_t in = block_input_index(b, spec->port);
	int32_t out = block_output_index(b, spec->port);
	if (in < 0 && out < 0)
		return GRAPH_ERR_UNKNOWN_PORT;
	int32_t i = spec->actuator ? out : in;
	if (i < 0)
		return GRAPH_ERR_INVALID_BINDING_DIRECTION;
	*bp = b;
	*port = (uint32_t)i;
	return GRAPH_OK;
}

enum graph_status graph_bind_check(const Graph *g, const EnvBindingSpec *spec)
{
	Block *b;
	uint32_t port;
	enum graph_status st = resolve(g, spec, &b, &port);
	if (st != GRAPH_OK)
		return st;
	if (!spec->actuator) {
		/* No fan-in (01 §2.2): the channel is the input's one source. */
		if (b->inputs[port].source != INPUT_SOURCE_NONE ||
		    graph_input_retained(g, b, spec->port))
			return GRAPH_ERR_INPUT_CONNECTED;
	} else if (actuator_driven(g->rt, spec->env_port, spec->env_index)) {
		return GRAPH_ERR_ACTUATOR_DRIVEN; /* one driver per channel (04 §4) */
	}
	return GRAPH_OK;
}

enum graph_status graph_bind(Graph *g, const EnvBindingSpec *spec, const Linkable *current)
{
	enum graph_status st = graph_bind_check(g, spec);
	if (st != GRAPH_OK)
		return st;
	Block *b;
	uint32_t port;
	(void)resolve(g, spec, &b, &port);
	Runtime *rt = g->rt;
	if (spec->env == NULL)
		return GRAPH_ERR_INVALID_ARGUMENT;
	char *env = graph_str_dup(spec->env);
	if (env == NULL || !bindings_reserve(rt)) {
		free(env);
		return GRAPH_ERR_NO_MEMORY;
	}

	EnvBinding *eb = &rt->bindings[rt->binding_count++];
	*eb = (EnvBinding){g, b, port, spec->actuator, env, spec->env_port, spec->env_index, NULL};
	if (spec->actuator) {
		eb->out = block_links_output(b, port); /* a composite's inner output (03 §5) */
		eb->out->actuators++;
	} else {
		b->inputs[port].source = INPUT_SOURCE_ENV;
		input_source_changed(b, port);
	}
	/* The event first (01 §2.2: a report precedes the value delivery it
	 * causes); eb may move if a later binding grows the array, so the
	 * delivery below reads its own copies. */
	graph_emit(g, (GraphEvent){.type = GRAPH_EVENT_BINDING_ADDED, .binding = eb});
	if (!spec->actuator) {
		/* Like a new link from a started source: the signal already on
		 * the channel (04 §0 rule 2), if the environment delivered one. */
		if (current != NULL)
			block_update(b, port, linkable_copy(current));
	} else if (b->started && rt->env_write != NULL) {
		Output *o = block_links_output(b, port);
		rt->env_write(rt->env_ctx, spec->env_port, spec->env_index, &o->last_propagated);
	}
	return GRAPH_OK;
}

/* Remove binding i: detach it from its output or input; if report, report
 * BINDING_REMOVED; if revert, a sensor-bound input then reverts to its
 * default (which propagates). */
static void binding_remove_at(Runtime *rt, uint32_t i, bool report, bool revert)
{
	EnvBinding eb = rt->bindings[i];
	memmove(&rt->bindings[i], &rt->bindings[i + 1],
		(rt->binding_count - i - 1) * sizeof(rt->bindings[0]));
	rt->binding_count--;
	if (eb.actuator)
		eb.out->actuators--;
	else
		input_clear_source(eb.block, eb.port); /* queued values go stale */
	if (report)
		graph_emit(eb.graph,
			   (GraphEvent){.type = GRAPH_EVENT_BINDING_REMOVED, .binding = &eb});
	if (revert && !eb.actuator)
		input_revert(eb.block, eb.port);
	free(eb.env);
}

enum graph_status graph_unbind(Graph *g, const EnvBindingSpec *spec)
{
	if (spec == NULL)
		return GRAPH_ERR_INVALID_ARGUMENT;
	if (!graph_id_valid(spec->block) || !graph_id_valid(spec->port))
		return GRAPH_ERR_INVALID_ID;
	Runtime *rt = g->rt;
	for (uint32_t i = 0; i < rt->binding_count; i++) {
		const EnvBinding *eb = &rt->bindings[i];
		if (eb->graph != g || eb->actuator != spec->actuator ||
		    eb->env_port != spec->env_port || eb->env_index != spec->env_index ||
		    strcmp(eb->block->name, spec->block) != 0)
			continue;
		const PortDescriptor *p = eb->actuator ? block_output_port(eb->block, eb->port)
						       : block_input_port(eb->block, eb->port);
		if (strcmp(p->name, spec->port) != 0)
			continue;
		/* 01 §2.2 Severing: the input falls to its default; an actuator
		 * keeps its last value (03 §4). */
		binding_remove_at(rt, i, true, true);
		return GRAPH_OK;
	}
	return GRAPH_ERR_NO_SUCH_BINDING;
}

/* Remove every binding of g (report: with events) whose block is b, or every
 * one of g for b NULL; sensor bindings only, actuator bindings only, or both
 * (which: 0 sensors, 1 actuators, 2 both). Nothing reverts. */
static void remove_matching(Graph *g, const Block *b, int which, bool report)
{
	Runtime *rt = g->rt;
	uint32_t i = 0;
	while (i < rt->binding_count) {
		const EnvBinding *eb = &rt->bindings[i];
		bool kind = which == 2 || (which == 1) == eb->actuator;
		if (eb->graph == g && (b == NULL || eb->block == b) && kind)
			binding_remove_at(rt, i, report, false);
		else
			i++;
	}
}

void graph_bindings_remove_graph(Graph *g)
{
	remove_matching(g, NULL, 2, true);
}

void graph_bindings_drop_graph(Graph *g)
{
	remove_matching(g, NULL, 2, false);
}

void graph_bindings_remove_input(Graph *g, const Block *b, uint32_t input)
{
	Runtime *rt = g->rt;
	for (uint32_t i = 0; i < rt->binding_count; i++) {
		const EnvBinding *eb = &rt->bindings[i];
		if (!eb->actuator && eb->block == b && eb->port == input) {
			binding_remove_at(rt, i, true, false);
			return;
		}
	}
}

void graph_bindings_remove_outputs(Graph *g, const Block *b)
{
	remove_matching(g, b, 1, true);
}

void runtime_env_deliver(Runtime *rt, uint32_t env_port, uint32_t env_index,
			 const Linkable *value)
{
	/* block_update changes no binding, so the array stays as it is. */
	for (uint32_t i = 0; i < rt->binding_count; i++) {
		const EnvBinding *eb = &rt->bindings[i];
		if (!eb->actuator && eb->env_port == env_port && eb->env_index == env_index)
			block_update(eb->block, eb->port, linkable_copy(value));
	}
}

void runtime_env_write(Runtime *rt, const Output *o, const Linkable *value)
{
	if (rt->env_write == NULL)
		return;
	for (uint32_t i = 0; i < rt->binding_count; i++) {
		const EnvBinding *eb = &rt->bindings[i];
		if (eb->actuator && eb->out == o)
			rt->env_write(rt->env_ctx, eb->env_port, eb->env_index, value);
	}
}
