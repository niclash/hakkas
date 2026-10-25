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
 *     1-6 and 8 (05 §1.4); a failure exits CLI_EXIT_FAILURE. With the
 *     configuration key environment.socket, the environment is the external
 *     environment program behind that Unix socket (platform/posix/env_link.h,
 *     04 §0.1): step 5 waits for it (retrying every second; a stop request
 *     meanwhile exits CLI_EXIT_OK), and the link's tick hook logs what its
 *     thread reports;
 *  2. subscribes to the events and logs every process's lifecycle at info:
 *     "process <id> started", "... stopped", "... deleted" (whoever caused
 *     it: the host, a client), and every link between processes: "link
 *     <process>.<outport> -> <process>.<inport> made" / "... removed";
 *  3. step 7: every saved process with its saved bindings, then the saved
 *     links (hakkas_process_load_saved, 05 §1.2, §2.3; plan task 62, which
 *     removed `run process` and `run processes`); what does not load or
 *     apply is logged and skipped, the rest runs. Then every loaded process
 *     whose file's started flag is set (05 §2.1) is started, in load order,
 *     and the info line "<n> process(es) loaded, <m> started";
 *  4. hakkas_run until the stop request, then hakkas_destroy (nothing is
 *     saved) and the info line "engine stopped": CLI_EXIT_OK.
 * Processes are loaded before the loop runs, so no client sees the loading
 * (connections are accepted by the loop only).
 */
#ifndef HAKKAS_PLATFORM_POSIX_HOST_H
#define HAKKAS_PLATFORM_POSIX_HOST_H

#include "system/config.h"

/* Run the engine with every saved process until a stop request. Returns the
 * exit code (CLI_EXIT_OK, CLI_EXIT_FAILURE). */
int posix_host_run(const Config *cfg);

#endif
