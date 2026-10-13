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
 *    PAL symbol); hakkas_env_sink_current (within start, engine thread)
 *    takes a channel's current value at once, as a drained IO event;
 *  - sensor values (the IO events, from the work queue's drain): converted
 *    to the port's type (number: f64, bool: bool), filtered per channel by
 *    the engine-wide change threshold (04 §0 rule 4, 01 §3.3: the first
 *    value of a channel always passes), then delivered to every bound block
 *    input (runtime_env_deliver). A value of a channel no binding uses is
 *    still the channel's current value, which a later binding delivers;
 *  - actuator writes: every push of a bound block output, converted to the
 *    actuator port's type, through the SPI's write (04 §0 rule 3);
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

struct HakkasEnvSink {
	Environment *env;
};

struct Environment {
	HakkasEnvironment spi;
	const HakkasEnvDescription *desc; /* NULL: no environment (not open) */
	Runtime *rt;
	environment_push_fn push;
	environment_clock_fn clock;
	/* Per sensor port (out_ports), the first of its channels' slots; the
	 * slots hold each channel's last propagated value (01 §3.3). */
	uint32_t *sensor_base;
	uint32_t sensor_channels;
	uint32_t actuator_channels;
	Linkable *sensor_last;
	bool *sensor_known;
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

/* A binding as a bindings file or the C API names it (04 §4): a block port
 * and an environment port id with its channel index (0: none). */
typedef struct {
	const char *block;
	const char *port;
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

/* Apply the n bindings of a bindings file (04 §4) to g, all or none: first
 * every binding is checked — an environment port the description does not
 * have, or an index beyond its count, is skipped with a warning naming
 * origin and the entry (04 §4 item 6: the block port stays inert); any other
 * refusal of environment_bind, also against an earlier binding of the same
 * file (a second binding of one input, a second driver of one channel),
 * refuses the whole file with msg "bindings[<i>]: <refusal>" and nothing
 * made — then they are made in order (only running out of memory can stop
 * that half-way: ENVIRONMENT_NO_MEMORY, the bindings made so far kept). *applied and *skipped (if not NULL)
 * count them. Without an environment the file is refused ("No
 * environment"). */
enum environment_status environment_bind_all(Environment *env, Graph *g,
					     const EnvBindingRequest *reqs, size_t n,
					     const char *origin, size_t *applied, size_t *skipped,
					     char *msg, size_t cap);

#endif
