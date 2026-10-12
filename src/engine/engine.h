/*
 * Engine startup and shutdown (05 §1.4, 01 §5): the startup sequence wired
 * end to end, written once against the PAL and the core modules, for every
 * platform. The host reaches it through the local C API (hakkas.h:
 * hakkas_create, hakkas_run, hakkas_destroy), which the POSIX command
 * line's `run` uses (platform/posix/host.c) and the Zephyr application
 * will (05 §1.3).
 *
 * Naming: functions are prefixed engine_, the types are Engine and
 * EngineParams.
 *
 * Before engine_start the platform has initialized logging (its console
 * sink) and the PAL (pal_init: reading the configuration goes through it),
 * and has built the configuration: defaults < file < options (05 §1.4
 * step 1). engine_start then runs (decided, plan task 19; 05 §1.4):
 *  1. the log threshold from log.level; engine_id generated if unset
 *     (config_generate_engine_id); auth.server_secret generated and written
 *     to the configuration file if absent (config_ensure_server_secret: a
 *     failure refuses startup);
 *  2. <datadir>/graphs and <datadir>/types created if missing (a failure
 *     refuses startup); the leftover *.tmp files deleted in those two and in
 *     the directories of the configuration and users files (05 §2.3 step 4;
 *     a failure is logged, never fatal); the runtime, the graphs registry and
 *     the engine loop (loop_init), then the platform's start hook (POSIX: the
 *     SIGINT/SIGTERM handlers);
 *  3. the users file (users_load_startup, never fatal) and the authorizer
 *     (auth_create, re-reading the users file at every challenge through
 *     users_loader), its tick hook (auth_tick);
 *  4. the built-in block types (block_types_register_builtins);
 *  5. the IO provider: not implemented yet (plan task 22);
 *  6. the composite block types of <datadir>/types, in dependency order
 *     (type_files_load_all; a type that does not load is logged and skipped,
 *     never fatal);
 *  7. nothing: loading and starting processes is the host's job (decided,
 *     plan task 21e; 05 §1.4 step 7), through the C API (hakkas.h) after
 *     engine_start and before engine_run. The engine starts no process by
 *     itself and does not act on a graph file's started flag;
 *  8. the protocol layer (protocol_create), the web server on port
 *     (web_server_open_config) and the WebSocket listener on port + 1
 *     (ws_server_open_config, which refuses tls.enabled); any failure
 *     refuses startup.
 * Then one info line: the engine id, host and both ports (never the server
 * secret). engine_run is step 9, the loop until a stop request; engine_stop
 * shuts down in reverse order: the servers (their listeners and
 * connections), the protocol layer, the authorizer's tick hook and the
 * authorizer, the platform's stop hook (before loop_deinit, which closes the
 * PAL's wakeup a signal handler writes to), the loop, the graphs (nothing is
 * saved: graphs are persisted only by network:persist), the runtime, the
 * composite block types and the built-in ones.
 *
 * A failing step logs why and releases everything made before it, so
 * engine_start returns false with nothing left over.
 *
 * Memory: the Engine (runtime with its work queue, users store, a copy of
 * the configuration) is one heap allocation.
 *
 * Threading: engine thread only; a stop request may come from any context
 * (pal_request_stop).
 */
#ifndef HAKKAS_ENGINE_ENGINE_H
#define HAKKAS_ENGINE_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "app/graph_files.h"
#include "engine/loop.h"
#include "graphs/graphs.h"
#include "system/config.h"

typedef struct {
	/* The platform's hooks, either may be NULL: start is called right
	 * after the loop is created (startup step 2; POSIX installs its stop
	 * signal handlers, posix_stop_signals_install), and a false return
	 * refuses startup; stop is called at shutdown before the loop is
	 * destroyed (posix_stop_signals_restore), if start succeeded. */
	bool (*platform_start)(void *ctx);
	void (*platform_stop)(void *ctx);
	void *platform_ctx;
	/* Tests only: both listeners on ephemeral ports (engine_web_port,
	 * engine_ws_port) instead of port and port + 1, the WebSocket's
	 * Origin port being the web server's actual port. Never set by a real
	 * startup; the configuration's port keeps its 1-65534 range. */
	bool ephemeral_ports;
} EngineParams;

typedef struct Engine Engine;

/* Startup steps 1-8 (see above) with cfg (copied; the copy gets the
 * generated engine_id and server secret). True with *out the running
 * engine; false (logged) with *out NULL and everything released. */
bool engine_start(const Config *cfg, const EngineParams *params, Engine **out);

/* Step 9: runs the loop until a stop request (pal_request_stop, or loop_stop
 * on engine_loop), then closes every connection and listener and returns. */
void engine_run(Engine *e);

/* Shuts down and frees the engine (see above). NULL is ignored. */
void engine_stop(Engine *e);

/* The configuration in use (with engine_id and auth.server_secret set). */
const Config *engine_config(const Engine *e);

/* The engine loop, e.g. for tests driving loop_run_once. */
Loop *engine_loop(Engine *e);

/* The graphs registry and the graph files (for the C API, hakkas.h). */
Graphs *engine_graphs(Engine *e);
GraphFiles *engine_files(Engine *e);

/* The ports the web server and the WebSocket listener listen on (0 once
 * closed). */
uint16_t engine_web_port(const Engine *e);
uint16_t engine_ws_port(const Engine *e);

#endif
