/*
 * Environment bindings (04 §4; plan tasks 22, 70): a process inport whose
 * block input is fed by a sensor channel, a process outport whose block
 * output is written to an actuator channel. See graphs/graph.h
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

/* Resolve spec's process port — the inport of its name for a sensor, the
 * outport for an actuator (04 §4: the environment port decides) — to the
 * block port behind it: GRAPH_OK with *bp and *port, else the refusal (the
 * order of graph_bind_check, up to the direction). */
static enum graph_status resolve(const Graph *g, const EnvBindingSpec *spec, Block **bp,
				 uint32_t *port)
{
	if (spec == NULL)
		return GRAPH_ERR_INVALID_ARGUMENT;
	if (!graph_id_valid(spec->port))
		return GRAPH_ERR_INVALID_ID;
	enum graph_port_dir dir = spec->actuator ? GRAPH_OUTPORT : GRAPH_INPORT;
	const GraphPort *p = graph_find_port(g, dir, spec->port);
	if (p == NULL) {
		enum graph_port_dir other = spec->actuator ? GRAPH_INPORT : GRAPH_OUTPORT;
		return graph_find_port(g, other, spec->port) != NULL
			       ? GRAPH_ERR_INVALID_BINDING_DIRECTION
			       : GRAPH_ERR_UNKNOWN_PORT;
	}
	*bp = p->block;
	*port = p->port;
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
		    graph_input_retained(g, b, block_input_port(b, port)->name))
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
	char *name = graph_str_dup(spec->port);
	if (env == NULL || name == NULL || !bindings_reserve(rt)) {
		free(env);
		free(name);
		return GRAPH_ERR_NO_MEMORY;
	}

	EnvBinding *eb = &rt->bindings[rt->binding_count++];
	*eb = (EnvBinding){g,    name,		 b, port, spec->actuator,
			   env,	 spec->env_port, spec->env_index, NULL};
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
	free(eb.name);
}

enum graph_status graph_unbind(Graph *g, const EnvBindingSpec *spec)
{
	if (spec == NULL)
		return GRAPH_ERR_INVALID_ARGUMENT;
	if (!graph_id_valid(spec->port))
		return GRAPH_ERR_INVALID_ID;
	Runtime *rt = g->rt;
	for (uint32_t i = 0; i < rt->binding_count; i++) {
		const EnvBinding *eb = &rt->bindings[i];
		if (eb->graph != g || eb->actuator != spec->actuator ||
		    eb->env_port != spec->env_port || eb->env_index != spec->env_index ||
		    strcmp(eb->name, spec->port) != 0)
			continue;
		/* 01 §2.2 Severing: the input falls to its default; an actuator
		 * keeps its last value (03 §4). */
		binding_remove_at(rt, i, true, true);
		return GRAPH_OK;
	}
	return GRAPH_ERR_NO_SUCH_BINDING;
}

/* Whether eb binds g's process port name of direction dir. */
static bool binds_port(const EnvBinding *eb, const Graph *g, enum graph_port_dir dir,
		       const char *name)
{
	return eb->graph == g && eb->actuator == (dir == GRAPH_OUTPORT) &&
	       strcmp(eb->name, name) == 0;
}

/* Remove every binding of g (report: with events), in creation order.
 * Nothing reverts. */
static void remove_all(Graph *g, bool report)
{
	Runtime *rt = g->rt;
	uint32_t i = 0;
	while (i < rt->binding_count) {
		if (rt->bindings[i].graph == g)
			binding_remove_at(rt, i, report, false);
		else
			i++;
	}
}

void graph_bindings_remove_graph(Graph *g)
{
	remove_all(g, true);
}

void graph_bindings_drop_graph(Graph *g)
{
	remove_all(g, false);
}

void graph_bindings_remove_port(Graph *g, enum graph_port_dir dir, const char *name,
				bool revert)
{
	Runtime *rt = g->rt;
	uint32_t i = 0;
	while (i < rt->binding_count) {
		if (binds_port(&rt->bindings[i], g, dir, name))
			binding_remove_at(rt, i, true, revert);
		else
			i++;
	}
}

bool graph_bindings_rename_port(Graph *g, enum graph_port_dir dir, const char *from,
				const char *to)
{
	Runtime *rt = g->rt;
	uint32_t n = 0;
	for (uint32_t i = 0; i < rt->binding_count; i++)
		n += binds_port(&rt->bindings[i], g, dir, from);
	if (n == 0)
		return true;
	/* Every new name first, so a failure changes nothing. */
	char **names = calloc(n, sizeof(*names));
	bool ok = names != NULL;
	for (uint32_t k = 0; ok && k < n; k++)
		ok = (names[k] = graph_str_dup(to)) != NULL;
	if (!ok) {
		for (uint32_t k = 0; names != NULL && k < n; k++)
			free(names[k]);
		free(names);
		return false;
	}
	uint32_t k = 0;
	for (uint32_t i = 0; i < rt->binding_count; i++) {
		EnvBinding *eb = &rt->bindings[i];
		if (binds_port(eb, g, dir, from)) {
			free(eb->name);
			eb->name = names[k++];
		}
	}
	free(names);
	return true;
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
