/*
 * The POSIX host (05 §1.2). See platform/posix/host.h.
 */
#include "platform/posix/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/process_list.h"
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

/* `run daemon`: every saved process; the flag decides. */
static void on_saved(void *ctx, const HakkasLoaded *l)
{
	note_loaded(ctx, l, l->started);
}

static bool load_daemon(Hakkas *h, Starts *s)
{
	enum hakkas_status st = hakkas_process_load_saved(h, on_saved, s, NULL);
	if (st != HAKKAS_OK)
		log_warn("not every process in %s/graphs loaded (%s): starting with the others",
			 hakkas_config(h)->datadir, hakkas_status_str(st));
	return true;
}

/* `run process <files>`: each must load. */
static bool load_files(Hakkas *h, const char *files, Starts *s)
{
	const char *p = files;
	for (;;) {
		const char *end = strchr(p, ':');
		size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
		enum hakkas_status st;
		HakkasLoaded l;
		if (len == 0 || len >= PAL_PATH_MAX) {
			log_error("invalid graph file name in list: \"%.*s\"",
				  (int)(len > 64 ? 64 : len), p);
			st = HAKKAS_ERR_INVALID;
		} else {
			char path[PAL_PATH_MAX];
			memcpy(path, p, len);
			path[len] = '\0';
			st = hakkas_process_load(h, path, &l, NULL);
		}
		if (st != HAKKAS_OK) {
			log_error("run process: a graph file did not load (%s): not starting",
				  hakkas_status_str(st));
			return false;
		}
		note_loaded(s, &l, l.started);
		if (end == NULL)
			return true;
		p = end + 1;
	}
}

/* A process's bindings file (04 §4). Bindings never refuse startup: a file
 * that does not apply is a warning, and the process runs without it. False
 * (logged) only when memory ran out. */
static bool apply_bindings(Hakkas *h, const char *id, const char *path)
{
	size_t applied, skipped;
	HakkasMessage msg;
	enum hakkas_status st = hakkas_bindings_apply(h, id, path, &applied, &skipped, &msg);
	if (st == HAKKAS_ERR_NO_MEMORY) {
		log_error("run processes: %s: %s: not starting", path, msg.text);
		return false;
	}
	if (st != HAKKAS_OK) {
		log_warn("run processes: %s: %s: not applied", path, msg.text);
		return true;
	}
	log_info("process %s: %zu binding%s applied from %s, %zu skipped", id, applied,
		 applied == 1 ? "" : "s", path, skipped);
	return true;
}

/* `run processes <file>`: the list; each must load. */
static bool load_list(Hakkas *h, const char *file, Starts *s)
{
	ProcessList list;
	enum graph_files_status lst = process_list_load(file, &list);
	if (lst != GRAPH_FILES_OK) {
		log_error("run processes: %s: %s: not starting", file,
			  graph_files_status_str(lst));
		return false;
	}
	bool ok = true;
	for (size_t i = 0; ok && i < list.count; i++) {
		const ProcessListEntry *e = &list.entries[i];
		HakkasLoaded l;
		enum hakkas_status st = hakkas_process_load(h, e->graph, &l, NULL);
		if (st != HAKKAS_OK) {
			log_error("run processes: %s did not load (%s): not starting", e->graph,
				  hakkas_status_str(st));
			ok = false;
			break;
		}
		/* Its bindings (04 §4), before anything starts. */
		if (e->bindings != NULL && !apply_bindings(h, l.id, e->bindings)) {
			ok = false;
			break;
		}
		bool start = e->start == PROCESS_LIST_START_FILE
				     ? l.started
				     : e->start == PROCESS_LIST_START_YES;
		note_loaded(s, &l, start);
	}
	/* The links, once every process is loaded and before any starts
	 * (05 §1.2): each must be made, like each process must load. */
	for (size_t i = 0; ok && i < list.link_count; i++) {
		const ProcessListLink *k = &list.links[i];
		HakkasMessage msg;
		if (hakkas_link_add(h, k->from_process, k->from_port, k->to_process, k->to_port,
				    &msg) != HAKKAS_OK) {
			log_error("run processes: %s: link %s.%s -> %s.%s: %s: not starting", file,
				  k->from_process, k->from_port, k->to_process, k->to_port,
				  msg.text);
			ok = false;
		}
	}
	process_list_free(&list);
	return ok;
}

int posix_host_run(const Config *cfg, enum posix_host_mode mode, const char *arg)
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
	bool ok = mode == POSIX_HOST_FILES  ? load_files(h, arg, &s)
		  : mode == POSIX_HOST_LIST ? load_list(h, arg, &s)
					    : load_daemon(h, &s);
	if (ok && s.no_memory) {
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
