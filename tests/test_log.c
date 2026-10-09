#include "test.h"

#include "system/log.h"

/* A capturing sink: counts calls and keeps the last line and level. */
struct capture {
	int calls;
	enum log_level level;
	size_t len;
	char line[LOG_LINE_MAX];
};

static void capture_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	struct capture *c = ctx;
	c->calls++;
	c->level = level;
	c->len = len;
	CHECK(len < sizeof(c->line));
	CHECK(strlen(line) == len);
	memcpy(c->line, line, len + 1);
}

static struct capture cap;

static void setup(void)
{
	memset(&cap, 0, sizeof(cap));
	CHECK(log_add_sink(capture_sink, &cap));
}

static void teardown(void)
{
	CHECK(log_remove_sink(capture_sink, &cap));
	log_set_threshold(LOG_LEVEL_WARN);
}

static void test_default_threshold_is_warn(void)
{
	CHECK(log_threshold() == LOG_LEVEL_WARN);
}

static void test_level_names(void)
{
	CHECK_STREQ(log_level_name(LOG_LEVEL_FINE), "fine");
	CHECK_STREQ(log_level_name(LOG_LEVEL_INFO), "info");
	CHECK_STREQ(log_level_name(LOG_LEVEL_WARN), "warn");
	CHECK_STREQ(log_level_name(LOG_LEVEL_ERROR), "error");
	CHECK_STREQ(log_level_name((enum log_level)42), "?");
}

static void test_level_parse(void)
{
	enum log_level l = LOG_LEVEL_ERROR;
	CHECK(log_level_parse("fine", &l) && l == LOG_LEVEL_FINE);
	CHECK(log_level_parse("info", &l) && l == LOG_LEVEL_INFO);
	CHECK(log_level_parse("warn", &l) && l == LOG_LEVEL_WARN);
	CHECK(log_level_parse("error", &l) && l == LOG_LEVEL_ERROR);

	l = LOG_LEVEL_INFO;
	CHECK(!log_level_parse("WARN", &l));
	CHECK(!log_level_parse("warning", &l));
	CHECK(!log_level_parse("", &l));
	CHECK(!log_level_parse(" warn", &l));
	CHECK(!log_level_parse(NULL, &l));
	CHECK(l == LOG_LEVEL_INFO);
}

static void test_threshold_filtering(void)
{
	setup();
	log_set_threshold(LOG_LEVEL_WARN);
	log_fine("f");
	log_info("i");
	CHECK(cap.calls == 0);
	log_warn("w");
	CHECK(cap.calls == 1 && cap.level == LOG_LEVEL_WARN);
	log_error("e");
	CHECK(cap.calls == 2 && cap.level == LOG_LEVEL_ERROR);

	log_set_threshold(LOG_LEVEL_FINE);
	log_fine("f");
	CHECK(cap.calls == 3 && cap.level == LOG_LEVEL_FINE);
	log_info("i");
	CHECK(cap.calls == 4 && cap.level == LOG_LEVEL_INFO);

	log_set_threshold(LOG_LEVEL_ERROR);
	log_warn("w");
	CHECK(cap.calls == 4);
	log_error("e");
	CHECK(cap.calls == 5);
	teardown();
}

static void test_line_format(void)
{
	setup();
	int line = __LINE__ + 1;
	log_warn("value %d of %s", 7, "x");
	char expected[128];
	snprintf(expected, sizeof(expected), "warn test_log.c:%d: value 7 of x", line);
	CHECK_STREQ(cap.line, expected);
	CHECK(cap.len == strlen(expected));

	log_emit(LOG_LEVEL_ERROR, "src/graphs/graph.c", 12, "boom");
	CHECK_STREQ(cap.line, "error graph.c:12: boom");
	log_emit(LOG_LEVEL_INFO, "plain.c", 3, "%s", "");
	CHECK_STREQ(cap.line, "info plain.c:3: ");
	teardown();
}

static void test_truncation(void)
{
	setup();
	char msg[LOG_LINE_MAX * 2];
	memset(msg, 'a', sizeof(msg) - 1);
	msg[sizeof(msg) - 1] = '\0';
	log_emit(LOG_LEVEL_WARN, "t.c", 1, "%s", msg);
	CHECK(cap.calls == 1);
	CHECK(cap.len == LOG_LINE_MAX - 1);
	CHECK(strncmp(cap.line, "warn t.c:1: aaa", 15) == 0);
	CHECK_STREQ(cap.line + cap.len - 4, "a...");

	/* Exactly LOG_LINE_MAX - 1 characters fit untruncated. */
	size_t prefix = strlen("warn t.c:1: ");
	msg[LOG_LINE_MAX - 1 - prefix] = '\0';
	log_emit(LOG_LEVEL_WARN, "t.c", 1, "%s", msg);
	CHECK(cap.len == LOG_LINE_MAX - 1);
	CHECK(cap.line[cap.len - 1] == 'a');

	/* A file name longer than the buffer still yields a bounded line. */
	char file[LOG_LINE_MAX * 2];
	memset(file, 'f', sizeof(file) - 1);
	file[sizeof(file) - 1] = '\0';
	log_emit(LOG_LEVEL_WARN, file, 1, "msg");
	CHECK(cap.len == LOG_LINE_MAX - 1);
	CHECK_STREQ(cap.line + cap.len - 3, "...");
	teardown();
}

static int side_effects;

static int bump(void)
{
	return ++side_effects;
}

static void test_no_formatting_when_filtered(void)
{
	setup();
	side_effects = 0;
	log_set_threshold(LOG_LEVEL_ERROR);
	log_fine("%d", bump());
	log_info("%d", bump());
	log_warn("%d", bump());
	CHECK(side_effects == 0);
	CHECK(cap.calls == 0);
	log_error("%d", bump());
	CHECK(side_effects == 1);
	CHECK(cap.calls == 1);
	teardown();
}

static void test_multiple_sinks(void)
{
	struct capture second = {0};
	setup();
	CHECK(log_add_sink(capture_sink, &second));
	log_error("both");
	CHECK(cap.calls == 1 && second.calls == 1);
	CHECK(strcmp(cap.line, second.line) == 0);
	CHECK(second.level == LOG_LEVEL_ERROR);

	CHECK(log_remove_sink(capture_sink, &second));
	CHECK(!log_remove_sink(capture_sink, &second));
	log_error("one");
	CHECK(cap.calls == 2 && second.calls == 1);
	teardown();

	/* No sinks: nothing happens. */
	log_error("nobody");
}

static void test_sink_capacity(void)
{
	struct capture extra[LOG_MAX_SINKS + 1];
	memset(extra, 0, sizeof(extra));
	for (int i = 0; i < LOG_MAX_SINKS; i++)
		CHECK(log_add_sink(capture_sink, &extra[i]));
	CHECK(!log_add_sink(capture_sink, &extra[LOG_MAX_SINKS]));
	CHECK(!log_add_sink(NULL, NULL));
	log_error("x");
	for (int i = 0; i < LOG_MAX_SINKS; i++) {
		CHECK(extra[i].calls == 1);
		CHECK(log_remove_sink(capture_sink, &extra[i]));
	}
	CHECK(extra[LOG_MAX_SINKS].calls == 0);
}

static int reentrant_calls;

static void reentrant_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)level;
	(void)line;
	(void)len;
	reentrant_calls++;
	log_error("from inside a sink");
}

static void test_reentrant_call_dropped(void)
{
	setup();
	CHECK(log_add_sink(reentrant_sink, NULL));
	log_error("outer");
	CHECK(reentrant_calls == 1);
	CHECK(cap.calls == 1);
	CHECK(strstr(cap.line, "outer") != NULL);
	CHECK(log_remove_sink(reentrant_sink, NULL));
	teardown();
}

int main(void)
{
	RUN(test_default_threshold_is_warn);
	RUN(test_level_names);
	RUN(test_level_parse);
	RUN(test_threshold_filtering);
	RUN(test_line_format);
	RUN(test_truncation);
	RUN(test_no_formatting_when_filtered);
	RUN(test_multiple_sinks);
	RUN(test_sink_capacity);
	RUN(test_reentrant_call_dropped);
	return test_result();
}
