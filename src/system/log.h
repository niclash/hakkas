/*
 * Logging (05 §4): four severity levels with one threshold, printf-style
 * macros that tag each line with its source file and line, and output through
 * registered sinks.
 *
 * Naming: functions and macros are prefixed log_, the level enum LOG_LEVEL_.
 *
 * A message is emitted only if its level is at or above the threshold
 * (default LOG_LEVEL_WARN, the log.level default of 05 §1.1). The macros test
 * the level before evaluating any argument, so a filtered call does no
 * formatting and its arguments' side effects do not happen.
 *
 * Line format: "<level> <file>:<line>: <message>", where <level> is the level
 * name ("fine", "info", "warn", "error"), <file> the source file's base name,
 * and no trailing newline. There are no timestamps; a sink may add its own.
 * A line is formatted into a fixed LOG_LINE_MAX-byte buffer (no heap); a longer
 * line is cut to LOG_LINE_MAX - 1 characters, the last three replaced by "...".
 *
 * Sinks: the core never decides where text ends up. Each formatted line is
 * handed, with its level, to every registered sink in registration order: the
 * platform's console (stderr on POSIX) and later the protocol layer's remote
 * mirror, which uses the level to choose network:output or network:error
 * (02 §3.3). With no sink registered, lines are discarded.
 *
 * Threading: the engine is single-threaded (01 §3.2) and logging is engine-
 * thread only; there is no locking. IO workers and ISRs must
 * not log: they only enqueue work, and the engine-side handler logs. A log
 * call made from inside a sink (re-entrant) is dropped.
 *
 * Never log secrets, proofs, nonces, salts or passwords (02 §2.5 item 8).
 */
#ifndef HAKKAS_SYSTEM_LOG_H
#define HAKKAS_SYSTEM_LOG_H

#include <stdbool.h>
#include <stddef.h>

/* Severity levels, least to most severe (05 §4). */
enum log_level {
	LOG_LEVEL_FINE = 0,
	LOG_LEVEL_INFO,
	LOG_LEVEL_WARN,
	LOG_LEVEL_ERROR,
};

/* Size of the line buffer, including the terminating NUL. */
#define LOG_LINE_MAX 256

/* Maximum number of registered sinks. */
#define LOG_MAX_SINKS 4

/* Receives one formatted line (NUL-terminated, len characters, no newline).
 * line is only valid during the call. */
typedef void (*log_sink_fn)(void *ctx, enum log_level level, const char *line, size_t len);

/* The level's config-file name ("fine", "info", "warn", "error"), or "?" for
 * a value outside the enum. */
const char *log_level_name(enum log_level level);

/* Parses a config-file level name (exact, lower case) into *out. Returns false,
 * leaving *out unchanged, for anything else. */
bool log_level_parse(const char *name, enum log_level *out);

void log_set_threshold(enum log_level level);
enum log_level log_threshold(void);

/* Registers a sink; ctx is passed back on every call. Returns false if all
 * LOG_MAX_SINKS slots are taken. */
bool log_add_sink(log_sink_fn fn, void *ctx);

/* Unregisters the sink registered with this fn and ctx. Returns false if there
 * is none. */
bool log_remove_sink(log_sink_fn fn, void *ctx);

/* The current threshold. Read it through log_enabled, write it only through
 * log_set_threshold. */
extern enum log_level log_current_threshold;

static inline bool log_enabled(enum log_level level)
{
	return level >= log_current_threshold;
}

#if defined(__GNUC__)
#define LOG_PRINTF_FORMAT(f, a) __attribute__((format(printf, f, a)))
#else
#define LOG_PRINTF_FORMAT(f, a)
#endif

/* Formats and emits one line unconditionally (no threshold check). Use the
 * macros below instead. */
void log_emit(enum log_level level, const char *file, int line, const char *fmt, ...)
	LOG_PRINTF_FORMAT(4, 5);

#define log_at(level, ...)                                                                         \
	do {                                                                                       \
		if (log_enabled(level))                                                            \
			log_emit((level), __FILE__, __LINE__, __VA_ARGS__);                        \
	} while (0)

#define log_fine(...)  log_at(LOG_LEVEL_FINE, __VA_ARGS__)
#define log_info(...)  log_at(LOG_LEVEL_INFO, __VA_ARGS__)
#define log_warn(...)  log_at(LOG_LEVEL_WARN, __VA_ARGS__)
#define log_error(...) log_at(LOG_LEVEL_ERROR, __VA_ARGS__)

#endif
