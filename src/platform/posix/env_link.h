/*
 * The POSIX external environment link (04 §0.1; decided, environment SPI
 * design, step 5; implemented in plan task 22): the environment SPI
 * (hakkas_env.h) implemented by talking to a separate environment program —
 * a simulator — over a Unix domain socket. The program is the server, at
 * the path of the configuration key environment.socket (05 §1.1); hakkas
 * connects as a client.
 *
 * Naming: functions are prefixed posix_env_link_; the type is PosixEnvLink.
 *
 * Messages (decided, plan task 22; 04 §0.1): newline-delimited JSON, one
 * object with one member per line, UTF-8, each line at most
 * POSIX_ENV_LINK_LINE_MAX bytes.
 *  hakkas -> environment:
 *   {"hello": {"version": 1}}                      first, on every connection
 *   {"write": {"port": "<id>", "index": <n>, "value": <number|bool>}}
 *                                                  an actuator value; "index"
 *                                                  only for an addressable
 *                                                  port; a NaN number is null
 *  environment -> hakkas:
 *   {"describe": {<the description, 04 §1>}}       first, on every connection
 *   {"value": {"port": "<id>", "index": <n>, "value": <number|bool>}}
 *                                                  a sensor value; current
 *                                                  values first, then changes
 *   {"clock": {"wall_ms": <integer>}}              optional: the wall clock
 *                                                  (UTC ms), e.g. simulated
 *  A line whose object has another member is ignored (forward
 *  compatibility); a line that is not a JSON object, or a message with
 *  invalid contents (an unknown or actuator port, an index the port does not
 *  have, a value that is not a number or bool), is logged (at most
 *  POSIX_ENV_LINK_INVALID_LOGGED per connection) and ignored. A line longer
 *  than POSIX_ENV_LINK_LINE_MAX ends the connection (as a disconnect).
 *
 * Behaviour (04 §0.1):
 *  - Startup (describe, on the engine thread inside hakkas_create): connect,
 *    send hello, read until the description. While the program is not
 *    there (no socket, connection refused), log once "waiting for
 *    environment at <path>" (warn) and retry every retry_ms
 *    (POSIX_ENV_LINK_RETRY_MS in the host; a parameter for tests); a stop
 *    request (pal_request_stop, SIGINT/SIGTERM) ends the wait:
 *    posix_env_link_interrupted is then true and startup fails. A
 *    description that does not parse or validate refuses startup. Values
 *    that come before the description are ignored; those after it are kept
 *    for start.
 *  - start: the link thread takes over the connection; sensor values reach
 *    the engine only through the sink (hakkas_env_sink_push, the PAL's
 *    handoff).
 *  - Actuator writes (write, engine thread) never block: each actuator
 *    channel holds its latest value and a pending flag, and a FIFO of
 *    pending channels (at most one entry per channel, so bounded by the
 *    description) is drained by the link thread. Overflow policy (decided):
 *    there is none to drop — when the environment reads slower than hakkas
 *    writes, a channel's values coalesce into its latest, which is always
 *    sent; channels are sent in the order they became pending.
 *  - Disconnect (end of stream, an error, an over-long line): the
 *    connection is closed, sensor-bound inputs keep their last values (the
 *    engine is told nothing), and the link reconnects every retry_ms. On
 *    reconnect it sends hello and waits for the description, which must be
 *    equal to the first one (env_description_equal; bindings were applied
 *    against it). If it is, the link sends every actuator channel's latest
 *    value (written before or during the disconnect), and the environment
 *    re-sends all its sensor values (the engine's change threshold filters
 *    the unchanged ones). If it is not, the link logs an error, closes the
 *    connection and does no more IO until hakkas is restarted
 *    (POSIX_ENV_LINK_FAILED).
 *  - Wall clock: the last "clock" received, advanced by the engine's
 *    monotonic clock since it arrived (so simulated time runs on between
 *    messages); before any, the host's clock (pal_wall_ms).
 *  - stop (at shutdown): the link thread writes out what it can of the
 *    pending writes without waiting, then ends; the connection is closed.
 *  - Logging: the link thread never logs (05 §4); it queues its messages
 *    (connected, disconnected, invalid messages, ...) and the engine thread
 *    logs them from posix_env_link_tick (a loop tick hook, so within a
 *    second) and at stop.
 *
 * Threading: create, describe, start, write, wall_ms, stop, the tick and
 * destroy are engine-thread calls; state, connections and interrupted may be
 * read from any thread. cJSON is used on the link thread too: it is
 * thread-safe as the core uses it (no cJSON_GetErrorPtr, no
 * cJSON_InitHooks, no setlocale).
 */
#ifndef HAKKAS_PLATFORM_POSIX_ENV_LINK_H
#define HAKKAS_PLATFORM_POSIX_ENV_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "hakkas_env.h"

/* The retry interval of the host: 04 §0.1, every second. */
#define POSIX_ENV_LINK_RETRY_MS 1000

/* The longest line, in bytes without the newline. */
#define POSIX_ENV_LINK_LINE_MAX (256 * 1024)

/* Invalid messages logged per connection; the rest are counted. */
#define POSIX_ENV_LINK_INVALID_LOGGED 8

/* The protocol version sent in hello. */
#define POSIX_ENV_LINK_VERSION 1

enum posix_env_link_state {
	POSIX_ENV_LINK_WAITING = 0,   /* not described yet (startup) */
	POSIX_ENV_LINK_CONNECTED,     /* described, exchanging values */
	POSIX_ENV_LINK_DISCONNECTED,  /* reconnecting */
	POSIX_ENV_LINK_FAILED,	      /* the description changed: no IO until restart */
	POSIX_ENV_LINK_STOPPED,	      /* stopped (or startup interrupted) */
};

typedef struct PosixEnvLink PosixEnvLink;

/* A link to the environment program at path, retrying every retry_ms (at
 * least 1). Nothing is connected yet. False (logged) if path does not fit a
 * Unix socket address or resources run out. */
bool posix_env_link_create(const char *path, uint32_t retry_ms, PosixEnvLink **out);

/* The link's SPI, for HakkasParams.environment. */
const HakkasEnvironment *posix_env_link_environment(PosixEnvLink *l);

/* The engine-thread side of the link's logging: a loop tick hook
 * (hakkas_add_tick) logging what the link thread queued. Returns its next
 * deadline (none). */
uint64_t posix_env_link_tick(void *ctx, uint64_t now_ms);

/* Whether a stop request ended the startup wait. Any thread. */
bool posix_env_link_interrupted(const PosixEnvLink *l);

/* The link's state and its number of described connections so far (1 after
 * startup, +1 per successful reconnect). Any thread. */
enum posix_env_link_state posix_env_link_state(const PosixEnvLink *l);
uint32_t posix_env_link_connections(const PosixEnvLink *l);

/* Free the link (stopping it if the engine did not). NULL is ignored. After
 * hakkas_destroy. */
void posix_env_link_destroy(PosixEnvLink *l);

#endif
