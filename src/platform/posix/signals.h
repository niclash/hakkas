/*
 * POSIX stop signals (01 §0.2, plan task 12): SIGINT and SIGTERM ask the
 * engine loop to stop (pal_request_stop), which then closes its sockets and
 * returns. SIGPIPE is left alone: sockets write with MSG_NOSIGNAL.
 */
#ifndef HAKKAS_PLATFORM_POSIX_SIGNALS_H
#define HAKKAS_PLATFORM_POSIX_SIGNALS_H

#include "platform/pal.h"

/* Installs the SIGINT/SIGTERM handlers (no SA_RESTART, so a blocked call
 * returns EINTR), saving the previous ones. PAL_ERR_INVALID if already
 * installed. */
enum pal_err posix_stop_signals_install(void);

/* Restores the handlers saved by posix_stop_signals_install. */
void posix_stop_signals_restore(void);

#endif
