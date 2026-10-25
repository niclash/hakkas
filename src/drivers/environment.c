/*
 * The engine's side of the environment SPI (04 §0, §4). See
 * drivers/environment.h.
 */
#include "drivers/environment.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/env_description.h"
#include "graphs/graphs.h"
#include "system/log.h"

void environment_init(Environment *env, environment_clock_fn clock)
{
	memset(env, 0, sizeof(*env));
	env->clock = clock;
}

/* The slot of a channel of out_ports[port] (env_port_has_channel holds). */
static uint32_t sensor_slot(const Environment *env, uint32_t port, uint32_t index)
{
	return env->sensor_base[port] + (index > 0 ? index - 1 : 0);
}

/* v as a value of type: an f64 for number, a bool for bool. */
static Linkable typed(enum hakkas_env_type type, const Linkable *v)
{
	return type == HAKKAS_ENV_BOOL ? linkable_from_bool(linkable_to_bool(v))
				       : linkable_from_f64(linkable_to_f64(v));
}

/* ---- The sink (any thread) ---------------------------------------------------------- */

bool hakkas_env_sink_push(HakkasEnvSink *sink, int port, int index, const HakkasValue *value)
{
	if (sink == NULL || sink->env == NULL || value == NULL)
		return false;
	Environment *env = sink->env;
	/* The description is read-only from start to stop (hakkas_env.h). */
	const HakkasEnvDescription *d = env->desc;
	if (d == NULL || port < 0 || (size_t)port >= d->out_port_count || index < 0 ||
	    !env_port_has_channel(&d->out_ports[port], (uint32_t)index))
		return false;
	if (value->type != LINKABLE_F64 && value->type != LINKABLE_I64 &&
	    value->type != LINKABLE_BOOL)
		return false; /* scalars only: they own nothing, so any thread may pass them */
	WorkEntry e = {.type = WORK_IO_EVENT};
	e.as.io.port = (uint32_t)port;
	e.as.io.index = (uint32_t)index;
	e.as.io.value = *value;
	return env->push(&e) == PAL_OK;
}

static void on_sensor(void *ctx, uint32_t port, uint32_t index, Linkable value);

bool hakkas_env_sink_current(HakkasEnvSink *sink, int port, int index, const HakkasValue *value)
{
	if (sink == NULL || sink->env == NULL || !sink->env->opening || value == NULL)
		return false;
	const HakkasEnvDescription *d = sink->env->desc;
	if (port < 0 || (size_t)port >= d->out_port_count || index < 0 ||
	    !env_port_has_channel(&d->out_ports[port], (uint32_t)index))
		return false;
	if (value->type != LINKABLE_F64 && value->type != LINKABLE_I64 &&
	    value->type != LINKABLE_BOOL)
		return false;
	/* On the engine thread: as if drained at once. */
	on_sensor(sink->env, (uint32_t)port, (uint32_t)index, *value);
	return true;
}


/* ---- The runtime's hooks (engine thread) ---------------------------------------------- */

/* An IO event drained from the work queue (01 §3.2): the change threshold
 * per channel (04 §0 rule 4), then the bound inputs. */
static void on_sensor(void *ctx, uint32_t port, uint32_t index, Linkable value)
{
	Environment *env = ctx;
	const HakkasEnvDescription *d = env->desc;
	if (d == NULL || port >= d->out_port_count ||
	    !env_port_has_channel(&d->out_ports[port], index)) {
		env->discarded++;
		linkable_clear(&value);
		return;
	}
	Linkable v = typed(d->out_ports[port].type, &value);
	linkable_clear(&value);
	uint32_t slot = sensor_slot(env, port, index);
	if (env->sensor_known[slot] &&
	    !block_value_changed(&env->sensor_last[slot], &v, runtime_change_threshold(env->rt)))
		return;
	env->sensor_known[slot] = true;
	env->sensor_last[slot] = v; /* a scalar: owns nothing */
	runtime_env_deliver(env->rt, port, index, &v);
}

/* A push of a bound block output (04 §0 rule 3). */
static void on_write(void *ctx, uint32_t port, uint32_t index, const Linkable *value)
{
	Environment *env = ctx;
	const HakkasEnvDescription *d = env->desc;
	if (d == NULL || port >= d->in_port_count)
		return;
	Linkable v = typed(d->in_ports[port].type, value);
	env->spi.write(env->spi.ctx, (int)port, (int)index, &v);
}

/* ---- Open and close ------------------------------------------------------------------ */

static void free_channels(Environment *env)
{
	free(env->sensor_base);
	free(env->sensor_last); /* scalars: nothing to clear */
	free(env->sensor_known);
	env->sensor_base = NULL;
	env->sensor_last = NULL;
	env->sensor_known = NULL;
	env->sensor_channels = 0;
	env->actuator_channels = 0;
}

static bool alloc_channels(Environment *env, const HakkasEnvDescription *d)
{
	uint32_t n = 0;
	env->sensor_base = calloc(d->out_port_count + 1, sizeof(*env->sensor_base));
	if (env->sensor_base == NULL)
		return false;
	for (size_t i = 0; i < d->out_port_count; i++) {
		env->sensor_base[i] = n;
		n += env_port_channels(&d->out_ports[i]);
	}
	env->sensor_base[d->out_port_count] = n;
	env->sensor_channels = n;
	for (size_t i = 0; i < d->in_port_count; i++)
		env->actuator_channels += env_port_channels(&d->in_ports[i]);
	env->sensor_last = calloc(n + 1, sizeof(*env->sensor_last));
	env->sensor_known = calloc(n + 1, sizeof(*env->sensor_known));
	return env->sensor_last != NULL && env->sensor_known != NULL;
}

bool environment_open(Environment *env, Runtime *rt, const HakkasEnvironment *spi,
		      environment_push_fn push)
{
	if (spi->describe == NULL || spi->start == NULL || spi->stop == NULL ||
	    spi->write == NULL || push == NULL) {
		log_error("the environment's interface is incomplete");
		return false;
	}
	env->spi = *spi;
	env->rt = rt;
	env->push = push;
	const HakkasEnvDescription *d = spi->describe(spi->ctx);
	if (d == NULL) {
		log_error("the environment did not describe itself");
		return false;
	}
	char msg[ENV_DESCRIPTION_MESSAGE_MAX];
	if (!env_description_validate(d, msg, sizeof(msg))) {
		log_error("environment description: %s", msg);
		return false;
	}
	if (!alloc_channels(env, d)) {
		log_error("environment: out of memory");
		free_channels(env);
		return false;
	}
	log_info("environment %s: %u sensor channel%s, %u actuator channel%s, %zu module%s",
		 d->name, env->sensor_channels, env->sensor_channels == 1 ? "" : "s",
		 env->actuator_channels, env->actuator_channels == 1 ? "" : "s", d->module_count,
		 d->module_count == 1 ? "" : "s");
	if (env->sensor_channels > PAL_HANDOFF_CAPACITY)
		log_warn("environment %s: %u sensor channels, more than the IO handoff holds (%d): "
			 "a burst of changes (e.g. the values re-sent after a reconnect) may lose "
			 "the oldest",
			 d->name, env->sensor_channels, PAL_HANDOFF_CAPACITY);
	env->desc = d;
	env->sink.env = env;
	runtime_set_environment(rt, on_sensor, on_write, env);
	env->opening = true; /* hakkas_env_sink_current: optional, values at hand */
	int rc = spi->start(spi->ctx, &env->sink);
	env->opening = false;
	if (rc != 0) {
		log_error("the environment did not start (%d)", rc);
		runtime_set_environment(rt, NULL, NULL, NULL);
		env->sink.env = NULL;
		env->desc = NULL;
		free_channels(env);
		return false;
	}
	env->started = true;
	return true;
}

void environment_close(Environment *env)
{
	if (env->desc == NULL)
		return;
	if (env->started)
		env->spi.stop(env->spi.ctx);
	env->started = false;
	env->sink.env = NULL; /* no push may come after stop (hakkas_env.h) */
	runtime_set_environment(env->rt, NULL, NULL, NULL);
	env->desc = NULL;
	free_channels(env);
}

int64_t environment_wall_ms(const Environment *env)
{
	if (env->started && env->spi.wall_ms != NULL)
		return env->spi.wall_ms(env->spi.ctx);
	return env->clock();
}

const Linkable *environment_sensor_value(const Environment *env, uint32_t port, uint32_t index)
{
	const HakkasEnvDescription *d = env->desc;
	if (d == NULL || port >= d->out_port_count ||
	    !env_port_has_channel(&d->out_ports[port], index))
		return NULL;
	uint32_t slot = sensor_slot(env, port, index);
	return env->sensor_known[slot] ? &env->sensor_last[slot] : NULL;
}

/* ---- Bindings (04 §4) --------------------------------------------------------------- */

static void set_msg(char *msg, size_t cap, const char *fmt, ...) LOG_PRINTF_FORMAT(3, 4);

static void set_msg(char *msg, size_t cap, const char *fmt, ...)
{
	if (msg == NULL || cap == 0)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, cap, fmt, ap);
	va_end(ap);
}

/* How an environment port and index resolve. */
enum channel_status {
	CHANNEL_OK,
	CHANNEL_UNKNOWN_PORT,
	CHANNEL_INDEX_RANGE,
	CHANNEL_INDEX_REQUIRED,
	CHANNEL_INDEX_UNEXPECTED,
};

static enum channel_status channel(const Environment *env, const EnvBindingRequest *req,
				   EnvBindingSpec *spec)
{
	bool actuator;
	uint32_t port;
	if (!env_description_find(env->desc, req->env, &actuator, &port))
		return CHANNEL_UNKNOWN_PORT;
	const HakkasEnvPort *p =
		actuator ? &env->desc->in_ports[port] : &env->desc->out_ports[port];
	if (p->addressable && req->index == 0)
		return CHANNEL_INDEX_REQUIRED;
	if (!p->addressable && req->index != 0)
		return CHANNEL_INDEX_UNEXPECTED;
	if (!env_port_has_channel(p, req->index))
		return CHANNEL_INDEX_RANGE;
	*spec = (EnvBindingSpec){req->port, actuator, req->env, port, req->index};
	return CHANNEL_OK;
}

/* The message of a channel status other than OK. */
static void channel_msg(enum channel_status st, const EnvBindingRequest *req, char *msg,
			size_t cap)
{
	switch (st) {
	case CHANNEL_OK:
		set_msg(msg, cap, "%s", "");
		break;
	case CHANNEL_UNKNOWN_PORT:
		set_msg(msg, cap, "Unknown environment port: %s", req->env);
		break;
	case CHANNEL_INDEX_RANGE:
		set_msg(msg, cap, "Index out of range: %s[%u]", req->env, (unsigned)req->index);
		break;
	case CHANNEL_INDEX_REQUIRED:
		set_msg(msg, cap, "Index required: %s", req->env);
		break;
	case CHANNEL_INDEX_UNEXPECTED:
		set_msg(msg, cap, "No index expected: %s", req->env);
		break;
	}
}

/* "<env>[<index>]", or "<env>" without an index. */
static void channel_name(const EnvBindingRequest *req, char *buf, size_t size)
{
	if (req->index > 0)
		snprintf(buf, size, "%s[%u]", req->env, (unsigned)req->index);
	else
		snprintf(buf, size, "%s", req->env);
}

/* A graph status as the binding's message. */
static enum environment_status graph_msg(enum graph_status st, const EnvBindingRequest *req,
					 char *msg, size_t cap)
{
	if (st == GRAPH_OK) {
		set_msg(msg, cap, "%s", "");
		return ENVIRONMENT_OK;
	}
	char name[2 * HAKKAS_ENV_ID_MAX + 16];
	channel_name(req, name, sizeof(name));
	GraphRefusal what = {.port = req->port, .name = name}; /* a process port: no node */
	if (msg != NULL && cap > 0)
		graph_refusal_message(st, &what, msg, cap);
	return st == GRAPH_ERR_NO_MEMORY ? ENVIRONMENT_NO_MEMORY : ENVIRONMENT_REFUSED;
}

/* The checks every binding command starts with: an environment, the
 * names. */
static bool precheck(const Environment *env, const EnvBindingRequest *req, char *msg, size_t cap)
{
	if (env->desc == NULL) {
		set_msg(msg, cap, "%s", "No environment");
		return false;
	}
	if (!graph_id_valid(req->port) || req->env == NULL) {
		set_msg(msg, cap, "%s", "Invalid id");
		return false;
	}
	return true;
}

/* The sensor channel's current value for spec (NULL for an actuator). */
static const Linkable *current(const Environment *env, const EnvBindingSpec *spec)
{
	return spec->actuator ? NULL
			      : environment_sensor_value(env, spec->env_port, spec->env_index);
}

enum environment_status environment_bind(Environment *env, Graph *g, const EnvBindingRequest *req,
					 char *msg, size_t cap)
{
	if (!precheck(env, req, msg, cap))
		return ENVIRONMENT_REFUSED;
	EnvBindingSpec spec;
	enum channel_status cst = channel(env, req, &spec);
	if (cst != CHANNEL_OK) {
		channel_msg(cst, req, msg, cap);
		return ENVIRONMENT_REFUSED;
	}
	return graph_msg(graph_bind(g, &spec, current(env, &spec)), req, msg, cap);
}

enum environment_status environment_unbind(Environment *env, Graph *g,
					   const EnvBindingRequest *req, char *msg, size_t cap)
{
	if (!precheck(env, req, msg, cap))
		return ENVIRONMENT_REFUSED;
	EnvBindingSpec spec;
	enum channel_status cst = channel(env, req, &spec);
	if (cst != CHANNEL_OK) {
		channel_msg(cst, req, msg, cap);
		return ENVIRONMENT_REFUSED;
	}
	return graph_msg(graph_unbind(g, &spec), req, msg, cap);
}

enum environment_status environment_bind_all(Environment *env, Graph *g,
					     const EnvBindingRequest *reqs, size_t n,
					     const char *origin, size_t *applied, size_t *skipped,
					     char *msg, size_t cap)
{
	if (applied != NULL)
		*applied = 0;
	if (skipped != NULL)
		*skipped = 0;
	set_msg(msg, cap, "%s", "");
	if (env->desc == NULL) {
		set_msg(msg, cap, "%s", "No environment");
		return ENVIRONMENT_REFUSED;
	}
	/* Each entry in file order, against the bindings already made, the
	 * file's earlier ones included (04 §4: warn and skip). */
	char why[ENVIRONMENT_MESSAGE_MAX];
	for (size_t i = 0; i < n; i++) {
		enum environment_status st = environment_bind(env, g, &reqs[i], why, sizeof(why));
		if (st == ENVIRONMENT_NO_MEMORY) {
			set_msg(msg, cap, "bindings[%zu]: %s", i, why);
			return st;
		}
		if (st == ENVIRONMENT_OK) {
			if (applied != NULL)
				(*applied)++;
			continue;
		}
		log_warn("%s: bindings[%zu]: %s: skipped", origin, i, why);
		if (skipped != NULL)
			(*skipped)++;
	}
	return ENVIRONMENT_OK;
}
