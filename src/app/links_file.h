/*
 * The saved links between processes (05 §2.3; decided with the user
 * 2026-10-25, plan task 62, replacing the process list's "links" — 00-README
 * open question 13): <datadir>/links.json, written whenever a process is
 * saved (graph_files_persist) and made by hakkas_process_load_saved once
 * every saved process is loaded and before any is started.
 *
 * Naming: functions are prefixed links_file_; the file is a LinksFile.
 *
 * The form: {"links": [{"from": "<process>.<outport>",
 * "to": "<process>.<inport>"}, ...]}, a reference split on its last '.'
 * (01 §2.4) into two non-empty parts (process = graph id). "links" null or
 * absent is no links; a member the format does not know is logged as a
 * warning and ignored. Anything else is malformed: logged as a warning
 * naming the problem, and no link of the file is made (a saved file never
 * refuses startup). Whether the processes and ports exist is checked when
 * the links are made.
 *
 * Storage goes through the PAL (graph_files_read). Threading: engine-thread
 * only (the log, 05 §4).
 */
#ifndef HAKKAS_APP_LINKS_FILE_H
#define HAKKAS_APP_LINKS_FILE_H

#include <stdbool.h>
#include <stddef.h>

#include "app/graph_files.h"
#include "graphs/runtime.h"

/* The file's name in datadir. */
#define LINKS_FILE_NAME "links.json"

/* The largest links file that is read, in bytes. A port's build may define
 * it. */
#ifndef HAKKAS_LINKS_FILE_MAX
#define HAKKAS_LINKS_FILE_MAX (64 * 1024)
#endif

/* A link: from_process's outport from_port to to_process's inport to_port.
 * Each string is owned. */
typedef struct {
	char *from_process;
	char *from_port;
	char *to_process;
	char *to_port;
} LinksFileLink;

typedef struct {
	LinksFileLink *links; /* in file order */
	size_t count;
} LinksFile;

/* Parse the text (len bytes, need not be NUL-terminated); origin names it in
 * log lines. GRAPH_FILES_OK with *out filled (free with links_file_free);
 * GRAPH_FILES_ERR_MALFORMED (logged) or _NO_MEMORY with *out empty. */
enum graph_files_status links_file_parse(const char *text, size_t len, const char *origin,
					 LinksFile *out);

/* Free f's links and make it empty. */
void links_file_free(LinksFile *f);

/* rt's links in creation order in the file's form: a heap string (free with
 * free()) of *len bytes plus a NUL, or NULL if memory runs out. */
char *links_file_serialize(const Runtime *rt, size_t *len);

#endif
