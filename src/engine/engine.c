/*
 * Engine startup and shutdown (05 §1.4). See engine/engine.h.
 */
#include "engine/engine.h"

#include <stdlib.h>
#include <string.h>

#include "app/graph_files.h"
#include "app/type_files.h"
#include "blocktypes/block_types.h"
#include "blocktypes/composite_types.h"
#include "engine/web_server.h"
#include "engine/ws_server.h"
#include "graphs/graphs.h"
#include "graphs/runtime.h"
#include "platform/pal.h"
#include "protocol/auth.h"
#include "protocol/dispatcher.h"
#include "protocol/users.h"
#include "system/crypto.h"

struct Engine {
	Config cfg; /* the copy in use: users_loader's path lives here */
	EngineParams params;
	Runtime rt;
	Graphs gs;
	GraphFiles gf;
	Loop loop;
	UserStore users;
	Auth *auth;
	Protocol *proto;
	WebServer *web;
	WsServer *ws;
	/* What exists, for the shutdown of a partial startup. */
	bool rt_ready, loop_ready, platform_started, tick_added, types_registered;
};

/* ---- Helpers ------------------------------------------------------------------ */

/* The authorizer's clock: the loop's engine time (02 §2.5, 01 §3.2.1). */
static uint64_t loop_clock(void *ctx)
{
	return loop_now_ms(ctx);
}

/* The directory part of path into buf: "." without a '/', "/" for a file in
 * the root. False if it does not fit. */
static bool dir_of(const char *path, char *buf, size_t size)
{
	const char *slash = strrchr(path, '/');
	const char *src = ".";
	size_t len = 1;
	if (slash != NULL) {
		src = path;
		len = slash == path ? 1 : (size_t)(slash - path);
	}
	if (len >= size)
		return false;
	memcpy(buf, src, len);
	buf[len] = '\0';
	return true;
}

/* Deletes the leftover *.tmp files of interrupted atomic writes in dir
 * (05 §2.3 step 4). A missing directory has none; any other failure is
 * logged and startup goes on (the leftovers are harmless, only untidy). */
static void remove_tmp(const char *dir)
{
	size_t removed = 0;
	enum pal_err err = pal_storage_remove_tmp(dir, &removed);
	if (removed > 0)
		log_info("%s: removed %zu leftover .tmp file%s", dir, removed,
			 removed == 1 ? "" : "s");
	if (err != PAL_OK && err != PAL_ERR_NOT_FOUND)
		log_warn("cannot remove the leftover .tmp files in %s: %s", dir, pal_err_str(err));
}

/* Step 2's cleanup in every directory the engine writes to (05 §2.3 step 4):
 * <datadir>/graphs, <datadir>/types, the configuration file's and the users
 * file's directories (once if they are the same). */
static void remove_tmp_all(const Engine *e)
{
	char dir[PAL_PATH_MAX], users_dir[PAL_PATH_MAX];
	const char *subdirs[] = {GRAPH_FILES_GRAPHS_DIR, GRAPH_FILES_TYPES_DIR};
	for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
		if (graph_files_dir(&e->gf, subdirs[i], dir, sizeof(dir)) == GRAPH_FILES_OK)
			remove_tmp(dir);
	}
	bool have_conf = dir_of(e->cfg.file, dir, sizeof(dir));
	if (have_conf)
		remove_tmp(dir);
	if (dir_of(e->cfg.auth.users_file, users_dir, sizeof(users_dir)) &&
	    (!have_conf || strcmp(dir, users_dir) != 0))
		remove_tmp(users_dir);
}

/* Step 8: the protocol layer and both listeners. False refuses startup. */
static bool open_servers(Engine *e)
{
	ProtocolConfig pc = {&e->gs, &e->gf, &e->cfg, auth_authorizer(e->auth)};
	if (!protocol_create(&pc, &e->proto))
		return false;
	enum pal_err err;
	if (e->params.ephemeral_ports)
		err = web_server_open_config_at(&e->loop, &e->cfg, 0, &e->web);
	else
		err = web_server_open_config(&e->loop, &e->cfg, &e->web);
	if (err != PAL_OK) {
		log_error("cannot open the web server on %s port %u: %s", e->cfg.host,
			  (unsigned)e->cfg.port, pal_err_str(err));
		return false;
	}
	if (e->params.ephemeral_ports)
		err = ws_server_open_config_at(&e->loop, &e->cfg, 0, web_server_port(e->web),
					       protocol_ws_handler(), e->proto, &e->ws);
	else
		err = ws_server_open_config(&e->loop, &e->cfg, protocol_ws_handler(), e->proto,
					    &e->ws);
	if (err != PAL_OK) {
		log_error("cannot open the WebSocket listener on %s port %u: %s", e->cfg.host,
			  (unsigned)e->cfg.port + 1, pal_err_str(err));
		return false;
	}
	return true;
}

/* ---- Startup and shutdown -------------------------------------------------------- */

bool engine_start(const Config *cfg, const EngineParams *params, Engine **out)
{
	*out = NULL;
	Engine *e = calloc(1, sizeof(*e));
	if (e == NULL) {
		log_error("cannot start the engine: out of memory");
		return false;
	}
	e->cfg = *cfg;
	e->params = *params;

	/* Step 1: logging, engine_id, auth.server_secret (05 §1.1). */
	log_set_threshold(e->cfg.log.level);
	enum config_status cst = config_generate_engine_id(&e->cfg, pal_random_bytes);
	if (cst != CONFIG_OK) {
		log_error("cannot generate the engine id: %s", config_status_str(cst));
		goto fail;
	}
	if (config_ensure_server_secret(&e->cfg, pal_random_bytes) != CONFIG_OK)
		goto fail; /* logged */

	/* Step 2: the data directories, the .tmp cleanup, the runtime and the
	 * graphs registry, the loop, the platform's hook. (The platform has
	 * initialized the PAL before step 1, engine/engine.h.) */
	if (!graph_files_init(&e->gf, &e->gs, e->cfg.datadir)) {
		log_error("datadir too long: %s", e->cfg.datadir);
		goto fail;
	}
	if (graph_files_make_dirs(&e->gf) != GRAPH_FILES_OK)
		goto fail; /* logged */
	remove_tmp_all(e);
	runtime_init(&e->rt);
	runtime_set_change_threshold(&e->rt, e->cfg.change_threshold);
	graphs_init(&e->gs, &e->rt, block_types_get, pal_random_bytes);
	e->rt_ready = true;
	enum pal_err err = loop_init(&e->loop, &e->rt);
	if (err != PAL_OK) {
		log_error("cannot create the engine loop: %s", pal_err_str(err));
		goto fail;
	}
	e->loop_ready = true;
	if (e->params.platform_start != NULL) {
		if (!e->params.platform_start(e->params.platform_ctx))
			goto fail; /* logged by the platform */
		e->platform_started = true;
	}

	/* Step 3: the users and the authorizer (02 §2.2: the users file never
	 * refuses startup). */
	users_load_startup(&e->users, e->cfg.auth.users_file);
	AuthConfig ac = {
		.users = &e->users,
		.load_users = users_loader,
		.load_users_ctx = e->cfg.auth.users_file,
		.server_secret = e->cfg.auth.server_secret,
		.idle_timeout_s = e->cfg.auth.idle_timeout_s,
		.max_lifetime_s = e->cfg.auth.max_lifetime_s,
		.clock = loop_clock,
		.clock_ctx = &e->loop,
		.random_bytes = pal_random_bytes,
	};
	if (!auth_create(&ac, &e->auth))
		goto fail; /* logged */
	if (!loop_add_tick(&e->loop, auth_tick, e->auth)) {
		log_error("cannot add the authorization's tick hook");
		goto fail;
	}
	e->tick_added = true;

	/* Step 4: the built-in block types (03). */
	if (block_types_register_builtins() != BLOCK_TYPES_OK) {
		log_error("cannot register the built-in block types");
		block_types_reset();
		goto fail;
	}
	e->types_registered = true;

	/* Step 5: the IO provider (04 §0-§1): not implemented yet (plan task
	 * 22). */

	/* Step 6: the composite block types of <datadir>/types (03 §5.1), in
	 * dependency order; a type that does not load is logged and skipped
	 * (05 §1.4), it never refuses startup. No client exists yet, so
	 * nothing is broadcast. */
	size_t types = 0;
	if (type_files_load_all(&e->gf, &types) != GRAPH_FILES_OK)
		log_warn("not every composite type in %s/" GRAPH_FILES_TYPES_DIR
			 " loaded: starting with the others",
			 e->gf.datadir);
	log_info("%zu composite type%s loaded", types, types == 1 ? "" : "s");

	/* Step 7 is the host's (05 §1.4, 00-README open question 12): the
	 * engine loads and starts no process; the host does, through the
	 * local C API (hakkas.h), before it runs the loop. */

	/* Step 8: the protocol layer and the listeners. */
	if (!open_servers(e))
		goto fail;

	log_info("engine %s ready: web server on %s port %u, WebSocket on port %u",
		 e->cfg.engine_id, e->cfg.host, (unsigned)engine_web_port(e),
		 (unsigned)engine_ws_port(e));
	*out = e;
	return true;

fail:
	log_error("the engine did not start");
	engine_stop(e);
	return false;
}

void engine_run(Engine *e)
{
	loop_run(&e->loop);
}

void engine_stop(Engine *e)
{
	if (e == NULL)
		return;
	/* The servers close their listeners and connections (unless loop_run's
	 * shutdown did already), then the layer above them. */
	web_server_close(e->web);
	ws_server_close(e->ws);
	protocol_destroy(e->proto);
	if (e->tick_added)
		loop_remove_tick(&e->loop, auth_tick, e->auth);
	auth_destroy(e->auth);
	/* Before loop_deinit: the POSIX stop signals write to the PAL's
	 * wakeup, which loop_deinit closes. */
	if (e->platform_started && e->params.platform_stop != NULL)
		e->params.platform_stop(e->params.platform_ctx);
	if (e->loop_ready)
		loop_deinit(&e->loop);
	if (e->rt_ready) {
		graphs_deinit(&e->gs);
		runtime_deinit(&e->rt);
	}
	if (e->types_registered) {
		composite_types_reset(); /* no instance is left (graphs_deinit) */
		block_types_reset();
	}
	crypto_zeroize(e, sizeof(*e)); /* the server secret, the users' keys */
	free(e);
}

const Config *engine_config(const Engine *e)
{
	return &e->cfg;
}

Loop *engine_loop(Engine *e)
{
	return &e->loop;
}

Graphs *engine_graphs(Engine *e)
{
	return &e->gs;
}

GraphFiles *engine_files(Engine *e)
{
	return &e->gf;
}

uint16_t engine_web_port(const Engine *e)
{
	return e->web != NULL ? web_server_port(e->web) : 0;
}

uint16_t engine_ws_port(const Engine *e)
{
	return e->ws != NULL ? ws_server_port(e->ws) : 0;
}
