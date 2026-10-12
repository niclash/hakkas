/*
 * The process list (`processes.json`, 05 §1.2; decided, plan task 21e): the
 * processes a deployment loads and runs, read by the host (00-README open
 * question 12) — the POSIX command line's `run processes <file>`
 * (platform/posix/host.c); a Zephyr application may use it too.
 *
 * Naming: functions are prefixed process_list_; the list is a ProcessList.
 *
 * Two forms are accepted:
 *  - the sister project's plain array of graph file paths,
 *	["./docs/example.json", ...];
 *  - an object,
 *	{"processes": [{"graph": "<path>", "bindings"?: "<path>",
 *			"start"?: true|false}, ...],
 *	 "links": [{"from": "<process>.<outport>",
 *		    "to": "<process>.<inport>"}, ...]}
 *    where "processes" is required and "links" optional.
 * Validation (decided, plan task 21e): a path is a non-empty string; a
 * "bindings" or "start" member that is null is absent; a link is an object
 * with string "from" and "to" (only checked for its shape: links are plan
 * task 21f). Anything else — text that is not one JSON array or object, a
 * wrong type, a missing "processes" or "graph" — refuses the whole file
 * (logged as an error naming the problem, PROCESS_LIST_ERR_MALFORMED). A
 * member the format does not know, at the top level or in an entry, is
 * logged as a warning and ignored (forward compatibility, like the
 * configuration file, 05 §1.1).
 *
 * Paths (decided): a relative path ("graph", "bindings") is relative to the
 * directory of the process list file, as written in the list; an absolute
 * path is used as it is. A resolved path must fit PAL_PATH_MAX (else the
 * file is malformed).
 *
 * Storage goes through the PAL (graph_files_read). Threading: any, but the
 * log is engine-thread only (05 §4), so read it there.
 */
#ifndef HAKKAS_APP_PROCESS_LIST_H
#define HAKKAS_APP_PROCESS_LIST_H

#include <stdbool.h>
#include <stddef.h>

#include "app/graph_files.h"

/* The largest process list file that is read, in bytes. A port's build may
 * define it. */
#ifndef HAKKAS_PROCESS_LIST_MAX
#define HAKKAS_PROCESS_LIST_MAX (64 * 1024)
#endif

/* Whether a process is started after loading (05 §1.2 "Process list"). */
enum process_list_start {
	PROCESS_LIST_START_FILE = 0, /* no "start" member: the graph file's started flag */
	PROCESS_LIST_START_YES,	     /* "start": true */
	PROCESS_LIST_START_NO,	     /* "start": false */
};

typedef struct {
	char *graph;	/* the graph file's path, resolved; owned */
	char *bindings; /* the bindings file's path, resolved, or NULL; owned */
	enum process_list_start start;
} ProcessListEntry;

typedef struct {
	ProcessListEntry *entries; /* in list order */
	size_t count;
	size_t link_count; /* the "links" (not implemented yet: plan task 21f) */
} ProcessList;

/* Parse the process list text (len bytes, need not be NUL-terminated); path
 * is the file it came from: relative paths are resolved against its
 * directory, and it names the file in log lines. GRAPH_FILES_OK with *out
 * filled (free with process_list_free); GRAPH_FILES_ERR_MALFORMED (logged)
 * or _NO_MEMORY with *out empty. */
enum graph_files_status process_list_parse(const char *text, size_t len, const char *path,
					   ProcessList *out);

/* Read and parse the process list file at path (at most
 * HAKKAS_PROCESS_LIST_MAX bytes): as process_list_parse, and
 * GRAPH_FILES_ERR_NOT_FOUND, _TOO_BIG, _STORAGE (logged). */
enum graph_files_status process_list_load(const char *path, ProcessList *out);

/* Free l's entries and make it empty. */
void process_list_free(ProcessList *l);

#endif
