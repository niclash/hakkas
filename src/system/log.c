#include "system/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct log_sink {
	log_sink_fn fn;
	void *ctx;
};

enum log_level log_current_threshold = LOG_LEVEL_WARN;

static struct log_sink sinks[LOG_MAX_SINKS];
static size_t sink_count;

/* Static, not on the stack: MCU thread stacks are small. Safe because logging
 * is engine-thread only and re-entrant calls are dropped. */
static char line_buf[LOG_LINE_MAX];
static bool emitting;

static const char *const level_names[] = {
	[LOG_LEVEL_FINE] = "fine",
	[LOG_LEVEL_INFO] = "info",
	[LOG_LEVEL_WARN] = "warn",
	[LOG_LEVEL_ERROR] = "error",
};

#define LEVEL_COUNT (sizeof(level_names) / sizeof(level_names[0]))

const char *log_level_name(enum log_level level)
{
	if ((unsigned)level >= LEVEL_COUNT)
		return "?";
	return level_names[level];
}

bool log_level_parse(const char *name, enum log_level *out)
{
	if (name == NULL)
		return false;
	for (size_t i = 0; i < LEVEL_COUNT; i++) {
		if (strcmp(name, level_names[i]) == 0) {
			*out = (enum log_level)i;
			return true;
		}
	}
	return false;
}

void log_set_threshold(enum log_level level)
{
	log_current_threshold = level;
}

enum log_level log_threshold(void)
{
	return log_current_threshold;
}

bool log_add_sink(log_sink_fn fn, void *ctx)
{
	if (fn == NULL || sink_count == LOG_MAX_SINKS)
		return false;
	sinks[sink_count++] = (struct log_sink){fn, ctx};
	return true;
}

bool log_remove_sink(log_sink_fn fn, void *ctx)
{
	for (size_t i = 0; i < sink_count; i++) {
		if (sinks[i].fn == fn && sinks[i].ctx == ctx) {
			memmove(&sinks[i], &sinks[i + 1], (sink_count - i - 1) * sizeof(sinks[0]));
			sink_count--;
			return true;
		}
	}
	return false;
}

/* The part of a path after its last directory separator. */
static const char *base_name(const char *path)
{
	const char *base = path;
	for (const char *p = path; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\')
			base = p + 1;
	}
	return base;
}

void log_emit(enum log_level level, const char *file, int line, const char *fmt, ...)
{
	if (emitting || sink_count == 0)
		return;
	emitting = true;

	/* snprintf/vsnprintf return the untruncated length, or < 0 on error. */
	int n = snprintf(line_buf, sizeof(line_buf), "%s %s:%d: ", log_level_name(level),
			 base_name(file), line);
	size_t len = 0;
	if (n < 0)
		line_buf[0] = '\0';
	else
		len = (size_t)n;
	if (len < sizeof(line_buf) - 1) {
		va_list ap;
		va_start(ap, fmt);
		n = vsnprintf(line_buf + len, sizeof(line_buf) - len, fmt, ap);
		va_end(ap);
		if (n < 0)
			line_buf[len] = '\0';
		else
			len += (size_t)n;
	}
	if (len > sizeof(line_buf) - 1) {
		len = sizeof(line_buf) - 1;
		memcpy(line_buf + len - 3, "...", 3);
	}

	for (size_t i = 0; i < sink_count; i++)
		sinks[i].fn(sinks[i].ctx, level, line_buf, len);

	emitting = false;
}
