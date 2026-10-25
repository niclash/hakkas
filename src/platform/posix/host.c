/*
 * The POSIX host (05 §1.2). See platform/posix/host.h.
 */
#include "platform/posix/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hakkas.h"
#include "platform/pal.h"
#include "platform/posix/cli.h"
#include "platform/posix/env_link.h"
#include "platform/posix/signals.h"
#include "system/log.h"

/* The processes loaded that are to be started, in load order. */
typedef struct {
	char (*ids)[HAKKAS_ID_MAX + 1];
	size_t count, cap;
	size_t loaded;
	bool no_memory;
} Starts;

static void note_loaded(Starts *s, const HakkasLoaded *l, bool start)
{
	s->loaded++;
	if (!start)
		return;
	if (s->count == s->cap) {
		size_t cap = s->cap == 0 ? 8 : s->cap * 2;
		void *ids = realloc(s->ids, cap * sizeof(*s->ids));
		if (ids == NULL) {
			s->no_memory = true;
			return;
		}
		s->ids = ids;
		s->cap = cap;
	}
	memcpy(s->ids[s->count++], l->id, sizeof(l->id));
}

/* The platform hooks (hakkas.h HakkasParams): the SIGINT/SIGTERM stop
 * handlers while the loop exists (01 §0.2, 05 §1.4 step 2). */
static bool signals_start(void *ctx)
{
	(void)ctx;
	enum pal_err err = posix_stop_signals_install();
	if (err != PAL_OK)
		log_error("cannot install the stop signal handlers: %s", pal_err_str(err));
	return err == PAL_OK;
}

static void signals_stop(void *ctx)
{
	(void)ctx;
	posix_stop_signals_restore();
}

/* Every process's lifecycle and every link between processes, logged at
 * info. */
static void log_lifecycle(void *ctx, const HakkasEvent *ev)
{
	(void)ctx;
	if (ev->type == HAKKAS_EVENT_PROCESS_STARTED)
		log_info("process %s started", ev->process);
	else if (ev->type == HAKKAS_EVENT_PROCESS_STOPPED)
		log_info("process %s stopped", ev->process);
	else if (ev->type == HAKKAS_EVENT_PROCESS_DELETED)
		log_info("process %s deleted", ev->process);
	else if (ev->type == HAKKAS_EVENT_LINK_ADDED || ev->type == HAKKAS_EVENT_LINK_REMOVED)
		log_info("link %s.%s -> %s.%s %s", ev->process, ev->name, ev->to_process,
			 ev->to_port, ev->type == HAKKAS_EVENT_LINK_ADDED ? "made" : "removed");
}

/* Every saved process; the flag decides. */
static void on_saved(void *ctx, const HakkasLoaded *l)
{
	note_loaded(ctx, l, l->started);
}

static void load_saved(Hakkas *h, Starts *s)
{
	enum hakkas_status st = hakkas_process_load_saved(h, on_saved, s, NULL);
	if (st != HAKKAS_OK)
		log_warn("not every process in %s/graphs loaded (%s): starting with the others",
			 hakkas_config(h)->datadir, hakkas_status_str(st));
}

int posix_host_run(const Config *cfg)
{
	HakkasParams params = {.platform_start = signals_start, .platform_stop = signals_stop};
	/* The external environment program (04 §0.1), if configured: startup
	 * waits for its description in hakkas_create (step 5). */
	PosixEnvLink *link = NULL;
	if (cfg->environment.socket[0] != '\0') {
		if (!posix_env_link_create(cfg->environment.socket, POSIX_ENV_LINK_RETRY_MS,
					   &link)) {
			log_error("the engine did not start");
			return CLI_EXIT_FAILURE;
		}
		params.environment = posix_env_link_environment(link);
	}
	Hakkas *h;
	if (!hakkas_create(cfg, &params, &h)) {
		/* A stop request while waiting for the environment is a clean
		 * stop. */
		bool interrupted = link != NULL && posix_env_link_interrupted(link);
		posix_env_link_destroy(link);
		if (interrupted)
			log_info("engine stopped");
		return interrupted ? CLI_EXIT_OK : CLI_EXIT_FAILURE; /* logged */
	}
	if (link != NULL && !hakkas_add_tick(h, posix_env_link_tick, link)) {
		log_error("cannot add the environment link's tick hook: not starting");
		hakkas_destroy(h);
		posix_env_link_destroy(link);
		return CLI_EXIT_FAILURE;
	}
	hakkas_subscribe(h, log_lifecycle, NULL);

	Starts s = {0};
	load_saved(h, &s);
	bool ok = true;
	if (s.no_memory) {
		log_error("out of memory: not starting");
		ok = false;
	}
	size_t started = 0;
	for (size_t i = 0; ok && i < s.count; i++) {
		HakkasMessage msg;
		if (hakkas_process_start(h, s.ids[i], &msg) == HAKKAS_OK)
			started++;
		else
			log_error("cannot start process %s: %s", s.ids[i], msg.text);
	}
	free(s.ids);
	if (!ok) {
		hakkas_destroy(h);
		posix_env_link_destroy(link);
		return CLI_EXIT_FAILURE;
	}
	log_info("%zu process%s loaded, %zu started", s.loaded, s.loaded == 1 ? "" : "es",
		 started);
	hakkas_run(h);
	hakkas_destroy(h);
	posix_env_link_destroy(link); /* stopped by hakkas_destroy */
	log_info("engine stopped");
	return CLI_EXIT_OK;
}
