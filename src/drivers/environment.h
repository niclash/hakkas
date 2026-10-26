/*
 * The engine's side of the environment SPI (04 §0, §4; hakkas_env.h;
 * plan task 22): everything above the SPI, platform-independent (01 §6
 * drivers/).
 *  - environment_open (startup step 5, 05 §1.4): describe, validate the
 *    description (drivers/env_description.h), install the runtime's hooks,
 *    start the environment with the sink;
 *  - the sink: hakkas_env_sink_push (any thread) checks the port, channel
 *    and value type against the description (read-only while started) and
 *    hands an IO event to the engine through the push function (the PAL's
 *    handoff, pal_handoff_push, given by the engine so this module links no
 *    PAL symbol); hakkas_env_sink_current (optional, within start, engine
 *    thread) takes a value the environment already has at once, as a
 *    drained IO event (nothing waits for values, 04 §0 rule 2);
 *  - sensor values (the IO events, from the work queue's drain): converted
 *    to the port's type (number: f64, bool: bool), filtered per channel by
 *    the engine-wide change threshold (04 §0 rule 4, 01 §3.3: the first
 *    value of a channel always passes), then delivered to every bound block
 *    input (runtime_env_deliver). A value of a channel no binding uses is
 *    still the channel's current value, which a later binding delivers;
 *  - actuator writes: every push of a bound block output, converted to the
 *    actuator port's type, through the SPI's write (04 §0 rule 3); the value
 *    is kept as the channel's current value (filtered like a sensor's);
 *  - every channel's current value for the frontend (02 §3.7
 *    environment:watch, plan task 76): channels are numbered sensors first
 *    (out_ports order), then actuators; each change of a channel's current
 *    value takes the next change number and is reported to the watcher;
 *  - the wall clock (04 §1): the environment's wall_ms, or the platform's
 *    clock (pal_wall_ms, given by the engine) without one;
 *  - applying bindings (04 §4) with the refusals and skips of a bindings
 *    file, and single bindings for the C API (02 §6).
 *
 * Naming: functions are prefixed environment_; the type is Environment.
 *
 * Threading: engine thread only, except hakkas_env_sink_push.
 */
#ifndef HAKKAS_DRIVERS_ENVIRONMENT_H
#define HAKKAS_DRIVERS_ENVIRONMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphs/graph.h"
#include "hakkas_env.h"
#include "platform/pal.h"

/* Room for a binding refusal message. */
#define ENVIRONMENT_MESSAGE_MAX 192

/* Hands an IO event to the engine from any context (pal_handoff_push). */
typedef enum pal_err (*environment_push_fn)(WorkEntry *e);

/* The platform's wall clock (pal_wall_ms). */
typedef int64_t (*environment_clock_fn)(void);

typedef struct Environment Environment;

/* A channel's current value changed (environment_watch). */
typedef void (*environment_change_fn)(void *ctx, uint32_t channel);

struct HakkasEnvSink {
	Environment *env;
};

struct Environment {
	HakkasEnvironment spi;
	const HakkasEnvDescription *desc; /* NULL: no environment (not open) */
	Runtime *rt;
	environment_push_fn push;
	environment_clock_fn clock;
	/* Per sensor port (out_ports) and per actuator port (in_ports), the
	 * first of its channels; sensors are channels 0..sensor_channels - 1,
	 * actuators follow. A channel holds its last propagated value (01
	 * §3.3), sensor or written, and the change number of its last change
	 * (0: none). */
	uint32_t *sensor_base;
	uint32_t *actuator_base;
	uint32_t sensor_channels;
	uint32_t actuator_channels;
	Linkable *last;
	bool *known;
	uint32_t *changed;
	uint32_t seq; /* the last change number given, never 0 once one is */
	environment_change_fn on_change;
	void *change_ctx;
	HakkasEnvSink sink;
	bool opening; /* inside the SPI's start: hakkas_env_sink_current */
	bool started;
	uint64_t discarded; /* IO events naming no channel */
};

/* Make env "no environment", with clock as the wall clock. */
void environment_init(Environment *env, environment_clock_fn clock);

/* Startup step 5 (05 §1.4): describe, validate, start (see above); push is
 * the engine's handoff. False (logged: "the environment did not describe
 * itself", "environment description: <why>", "the environment did not
 * start") with env left without an environment. */
bool environment_open(Environment *env, Runtime *rt, const HakkasEnvironment *spi,
		      environment_push_fn push);

/* Stop the environment (the SPI's stop), remove the runtime's hooks and free
 * the channel state. Nothing if not open. */
void environment_close(Environment *env);

/* The description, NULL without an environment. */
static inline const HakkasEnvDescription *environment_description(const Environment *env)
{
	return env->desc;
}

/* The wall clock (UTC ms): the environment's, or the platform's. */
int64_t environment_wall_ms(const Environment *env);

/* The current (last propagated) value of sensor channel index of
 * out_ports[port], NULL if none has been delivered. */
const Linkable *environment_sensor_value(const Environment *env, uint32_t port, uint32_t index);

/* Report every change of a channel's current value to fn (NULL: none), on
 * the engine thread, after the bound inputs have it. One watcher; it stays
 * across open and close. */
void environment_watch(Environment *env, environment_change_fn fn, void *ctx);

/* The number of channels: sensors, then actuators (0 without an
 * environment). */
static inline uint32_t environment_channel_count(const Environment *env)
{
	return env->desc != NULL ? env->sensor_channels + env->actuator_channels : 0;
}

/* Channel ch: its port's id and its index (0 for a port that is not
 * addressable), and its current value; false (and nothing set) if it has
 * none yet or there is no such channel. An actuator's is the last value
 * written to it. */
bool environment_channel(const Environment *env, uint32_t ch, const char **id, uint32_t *index,
			 const Linkable **value);

/* The last change number given (0: none yet). */
static inline uint32_t environment_seq(const Environment *env)
{
	return env->seq;
}

/* Whether channel ch changed after change number seq. Change numbers wrap:
 * one more than 2^31 changes old may count as newer, which only sends a
 * current value once more. */
static inline bool environment_changed_after(const Environment *env, uint32_t ch, uint32_t seq)
{
	return env->changed[ch] != 0 && (int32_t)(env->changed[ch] - seq) > 0;
}

/* A binding as a bindings file or the C API names it (04 §4): a block port
 * and an environment port id with its channel index (0: none). */
typedef struct {
	const char *port; /* the process port (04 §4, plan task 70) */
	const char *env;
	uint32_t index;
} EnvBindingRequest;

enum environment_status {
	ENVIRONMENT_OK = 0,
	ENVIRONMENT_REFUSED = -1,   /* msg holds the message */
	ENVIRONMENT_NO_MEMORY = -2, /* "Out of memory" */
};

/* One binding (the C API, 02 §6) on g: refused, with msg (cap bytes) set, in
 * this order: "No environment"; "Invalid id" (a block or port name empty or
 * too long, or env NULL); "Unknown environment port: <env>"; "Index
 * required: <env>" (an addressable port without one), "No index expected:
 * <env>", "Index out of range: <env>[<index>]"; then as graph_bind_check:
 * "Unknown node: <block>", "Unknown port: <block>.<port>", "Invalid binding
 * direction", "Input already connected: <block>.<port>", "Actuator already
 * driven: <channel>" (<channel>: "<env>[<index>]", or "<env>" without an
 * index). Reports BINDING_ADDED, then delivers (graphs/graph.h). */
enum environment_status environment_bind(Environment *env, Graph *g, const EnvBindingRequest *req,
					 char *msg, size_t cap);

/* Remove a binding: "No environment", "Invalid id", the environment port's
 * refusals as environment_bind, "No such binding". A sensor-bound input
 * reverts to its default; an actuator keeps its last value. */
enum environment_status environment_unbind(Environment *env, Graph *g,
					   const EnvBindingRequest *req, char *msg, size_t cap);

/* Apply the n bindings of a bindings file (04 §4) to g, warning and
 * skipping: each is made in file order as environment_bind makes it, against
 * the bindings already there, the file's earlier ones included (a second
 * binding of one input, a second driver of one channel); one that is refused
 * is skipped with the warning "<origin>: bindings[<i>]: <refusal>: skipped"
 * and the rest are made (a channel the environment does not provide leaves
 * its block port inert, 04 §4 item 6). *applied and *skipped (if not NULL)
 * count them. Only two things stop it: no environment (ENVIRONMENT_REFUSED,
 * "No environment", nothing made) and running out of memory
 * (ENVIRONMENT_NO_MEMORY, "bindings[<i>]: <message>", the bindings made so
 * far kept). */
enum environment_status environment_bind_all(Environment *env, Graph *g,
					     const EnvBindingRequest *reqs, size_t n,
					     const char *origin, size_t *applied, size_t *skipped,
					     char *msg, size_t cap);

#endif
