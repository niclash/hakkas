/*
 * The POSIX host (05 §1.2, §1.4; decided, plan task 21e): `run` embeds the
 * runtime through the local C API (hakkas.h) and manages process lifetime
 * itself — the engine loads and starts no process (00-README open question
 * 12).
 *
 * Naming: functions are prefixed posix_host_.
 *
 * posix_host_run:
 *  1. hakkas_create with the configuration, SIGINT/SIGTERM as the stop
 *     request (platform/posix/signals.h, the platform hooks), startup steps
 *     1-6 and 8 (05 §1.4); a failure exits CLI_EXIT_FAILURE;
 *  2. subscribes to the events and logs every process's lifecycle at info:
 *     "process <id> started", "... stopped", "... deleted" (whoever caused
 *     it: the host, a client), and every link between processes: "link
 *     <process>.<outport> -> <process>.<inport> made" / "... removed";
 *  3. step 7, the processes of the mode (05 §1.2):
 *     - POSIX_HOST_FILES (`run process <file>[:<file>...]`): the files in
 *       order; any file that does not load refuses startup (logged, then
 *       "run process: a graph file did not load (<cause>): not starting");
 *     - POSIX_HOST_LIST (`run processes <file>`): the process list
 *       (app/process_list.h) in order; a list that cannot be read or is
 *       malformed, or any process that does not load, refuses startup
 *       ("run processes: ... : not starting"). A "bindings" member is
 *       logged at info and ignored (environment bindings: plan task 22).
 *       Once every process is loaded, and before any is started, the
 *       list's "links" are made in list order (hakkas_link_add); a link
 *       that is refused refuses startup like a process that does not load
 *       ("run processes: <file>: link <from> -> <to>: <message>: not
 *       starting"; plan task 21f);
 *     - POSIX_HOST_DAEMON (`run daemon`): every saved process
 *       (hakkas_process_load_saved); a file that does not load is logged
 *       and skipped, the others run;
 *     then every loaded process to be started is started, in load order:
 *     a list entry's "start" if given, else the graph file's started flag
 *     (05 §2.1); in the other modes the flag. Then the info line "<n>
 *     process(es) loaded, <m> started";
 *  4. hakkas_run until the stop request, then hakkas_destroy (nothing is
 *     saved) and the info line "engine stopped": CLI_EXIT_OK.
 * Processes are loaded before the loop runs, so no client sees the loading
 * (connections are accepted by the loop only).
 */
#ifndef HAKKAS_PLATFORM_POSIX_HOST_H
#define HAKKAS_PLATFORM_POSIX_HOST_H

#include "system/config.h"

enum posix_host_mode {
	POSIX_HOST_DAEMON, /* every graph in <datadir>/graphs */
	POSIX_HOST_FILES,  /* a colon-separated list of graph files */
	POSIX_HOST_LIST,   /* a process list file (processes.json) */
};

/* Run the engine with the processes of mode (arg: the file list or the
 * process list file; unused for POSIX_HOST_DAEMON) until a stop request.
 * Returns the exit code (CLI_EXIT_OK, CLI_EXIT_FAILURE). */
int posix_host_run(const Config *cfg, enum posix_host_mode mode, const char *arg);

#endif
