/*
 * The POSIX console log sink (05 §4): writes each log line to stderr.
 */
#ifndef HAKKAS_PLATFORM_POSIX_LOG_STDERR_H
#define HAKKAS_PLATFORM_POSIX_LOG_STDERR_H

#include <stddef.h>

#include "system/log.h"

/* A log_sink_fn; ctx is unused (pass NULL). Appends a newline to each line. */
void posix_log_stderr(void *ctx, enum log_level level, const char *line, size_t len);

#endif
