/*
 * The calendar primitives (03 §6.2, blocktypes/calendar.c): selector and
 * range strings, each primitive's matching and next boundary under a
 * controlled wall clock (month ends, leap years, ISO week 53, wrap past
 * midnight, the CET daylight-saving edges of 2026), input changes re-arming,
 * wall-clock steps (graphs/runtime.h "Wall clock"), and the schedule standard
 * library (library/schedules/, registered through type_files_load_all). The
 * tests play the engine loop: the wall clock is engine time plus a fixed
 * offset, and every deadline is fired exactly on time, so nothing waits.
 */
#define _XOPEN_SOURCE 700

#include "test.h"

#include <dirent.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include "app/graph_files.h"
#include "app/type_files.h"
#include "blocktypes/block_types.h"
#include "blocktypes/calendar.h"
#include "blocktypes/composite_types.h"
#include "graphs/graph.h"
#include "graphs/graphs.h"
#include "platform/pal.h"
#include "system/log.h"
#include "system/tz.h"

#define CET "CET-1CEST,M3.5.0,M10.5.0/3"

static Runtime rt;
static Graph *gr;
static int64_t base_ms; /* the wall clock at engine time 0 */
static char trace[4096];
static char logs[4096];

static void append(char *buf, size_t size, const char *fmt, ...) LOG_PRINTF_FORMAT(3, 4);

static void append(char *buf, size_t size, const char *fmt, ...)
{
	size_t len = strlen(buf);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + len, size - len, fmt, ap);
	va_end(ap);
}

static void log_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)len;
	append(logs, sizeof(logs), "%s %s\n", log_level_name(level), line);
}

/* UTC ms of a UTC date and time. */
static int64_t ms(int64_t y, int mo, int d, int h, int mi)
{
	return (tz_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60) * 1000;
}

/* The engine time at which the wall clock is wall_ms. */
static uint64_t engine(int64_t wall_ms)
{
	return (uint64_t)(wall_ms - base_ms);
}

/* ---- Observed blocks ------------------------------------------------------- */

typedef struct {
	const Block *b;
	const char *label;
	bool last;
} Observed;

static Observed observed[4];
static int observed_count;
static int fired;

static bool out(const Block *b)
{
	const Linkable *v = &b->out_values[0];
	return v->type == LINKABLE_BOOL && v->as.b;
}

static void observe(const Block *b, const char *label)
{
	observed[observed_count++] = (Observed){b, label, out(b)};
}

/* Append "<local time> <label>=<0|1>;" for every observed block whose out
 * changed. */
static void record(void)
{
	for (int i = 0; i < observed_count; i++) {
		Observed *o = &observed[i];
		bool v = out(o->b);
		if (v == o->last)
			continue;
		o->last = v;
		TzLocal l;
		tz_local(&rt.tz, tz_floor_div(runtime_wall_ms(&rt), 1000), &l);
		append(trace, sizeof(trace), "%04lld-%02d-%02d %02d:%02d:%02d%+d %s=%d;",
		       (long long)l.year, l.month, l.day, l.hour, l.minute, l.second,
		       (int)(l.offset_s / 3600), o->label, (int)v);
	}
}

static void drain(void)
{
	runtime_drain(&rt, SIZE_MAX);
}

static uint64_t next_due(void)
{
	uint64_t due;
	return runtime_next_deadline(&rt, &due) ? due : UINT64_MAX;
}

/* A perfect engine loop up to the wall time end_wall: fire every deadline
 * exactly on time, recording the changes. */
static void run_until(int64_t end_wall)
{
	uint64_t end = engine(end_wall);
	while (next_due() <= end) {
		runtime_fire_timers(&rt, next_due());
		drain();
		fired++;
		record();
	}
	runtime_set_now(&rt, end);
}

/* The engine at wall time start (UTC ms) in time zone zone, an empty graph. */
static void setup(const char *zone, int64_t start)
{
	runtime_init(&rt);
	TimeZone tz;
	CHECK(tz_parse(zone, &tz));
	runtime_set_time_zone(&rt, &tz);
	base_ms = start;
	runtime_set_now(&rt, 0);
	runtime_set_wall_ms(&rt, start);
	CHECK(runtime_wall_ms(&rt) == start);
	gr = graph_create(&rt);
	CHECK(gr != NULL);
	trace[0] = '\0';
	observed_count = 0;
	fired = 0;
}

static void teardown(void)
{
	graph_destroy(gr);
	gr = NULL;
	runtime_deinit(&rt);
}

static Block *add(const char *name, const char *type_name)
{
	Block *b = NULL;
	const BlockTypeDef *def = block_types_get(type_name);
	CHECK(def != NULL);
	CHECK(graph_create_block(gr, name, def, NULL, &b) == GRAPH_OK);
	return b;
}

static void set_str(const char *node, const char *port, const char *text)
{
	Linkable v = {0};
	CHECK(linkable_from_string(&v, text, strlen(text)));
	CHECK(graph_add_initial(gr, node, port, &v) == GRAPH_OK);
	linkable_clear(&v);
	drain();
}

static void set_value(const char *node, const char *port, Linkable v)
{
	CHECK(graph_add_initial(gr, node, port, &v) == GRAPH_OK);
	linkable_clear(&v);
	drain();
}

static void start(void)
{
	graph_start(gr);
	drain();
}

static void check_trace(const char *want)
{
	CHECK_STREQ(trace, want);
	trace[0] = '\0';
}

/* ---- Setting strings ------------------------------------------------------- */

/* The values selected by text over min..max as "a,b,c", or "invalid". */
static const char *selected(const char *text, int min, int max)
{
	static char buf[512];
	CalendarSelector s;
	buf[0] = '\0';
	if (!calendar_selector_parse(text, min, max, &s))
		return "invalid";
	for (int v = 0; v <= CALENDAR_SELECTOR_MAX; v++) {
		if (calendar_selector_has(&s, v))
			append(buf, sizeof(buf), "%s%d", buf[0] ? "," : "", v);
	}
	return buf;
}

static void test_selectors(void)
{
	CHECK_STREQ(selected("3", 1, 7), "3");
	CHECK_STREQ(selected("1-5", 1, 7), "1,2,3,4,5");
	CHECK_STREQ(selected("6,7", 1, 7), "6,7");
	CHECK_STREQ(selected("1-3,5", 1, 7), "1,2,3,5");
	CHECK_STREQ(selected(" 1 -\t3 , 5 ", 1, 7), "1,2,3,5");
	CHECK_STREQ(selected("5-5", 1, 7), "5");
	CHECK_STREQ(selected("07,01", 1, 7), "1,7");
	CHECK_STREQ(selected("1,1,1-2", 1, 7), "1,2");
	/* A reversed range wraps around. */
	CHECK_STREQ(selected("6-1", 1, 7), "1,6,7");
	CHECK_STREQ(selected("11-2", 1, 12), "1,2,11,12");
	CHECK_STREQ(selected("2-1", 1, 7), "1,2,3,4,5,6,7");
	CHECK_STREQ(selected("366", 1, 366), "366");
	CHECK_STREQ(selected("365-2", 1, 366), "1,2,365,366");
	CHECK_STREQ(selected("53", 1, 53), "53");

	static const char *const bad[] = {
		"", "  ", "0", "8", "1,,3", "1,", ",1", "1-", "-1", "1-8", "a", "1;2", "1 2",
		"+1", "99999", "1-2-3", "3.0", "1-3,", "1,-", "1 ,", "0001-00",
		"\xef\xbc\x91", /* a fullwidth 1 */
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		const char *got = selected(bad[i], 1, 7);
		if (strcmp(got, "invalid") != 0)
			fprintf(stderr, "  \"%s\" accepted: %s\n", bad[i], got);
		CHECK_STREQ(got, "invalid");
	}
	CHECK_STREQ(selected("32", 1, 31), "invalid");
	CHECK_STREQ(selected("13", 1, 12), "invalid");
	CHECK_STREQ(selected("367", 1, 366), "invalid");
	CHECK_STREQ(selected("54", 1, 53), "invalid");
	CalendarSelector s;
	CHECK(!calendar_selector_parse(NULL, 1, 7, &s));
	CHECK(!calendar_selector_parse("1", 0, 7, &s));
	CHECK(!calendar_selector_parse("1", 1, CALENDAR_SELECTOR_MAX + 1, &s));

	int a, b;
	CHECK(calendar_range_parse("12:30-15:45", &a, &b) && a == 750 && b == 945);
	CHECK(calendar_range_parse("22:00-06:00", &a, &b) && a == 1320 && b == 360);
	CHECK(calendar_range_parse("0:00-24:00", &a, &b) && a == 0 && b == 1440);
	CHECK(calendar_range_parse(" 7:05 -\t08:00 ", &a, &b) && a == 425 && b == 480);
	CHECK(calendar_range_parse("08:00-08:00", &a, &b) && a == 480 && b == 480);
	static const char *const bad_ranges[] = {
		"",
		"8-12",
		"08:00",
		"08:00-",
		"25:00-26:00",
		"12:60-13:00",
		"12:00-24:01",
		"24:00-12:00",
		"12:00-13:00x",
		"1200-1300",
		"-1:00-2:00",
		"12:00-13:00-14:00",
		"123:00-13:00",
		"12:5-13:00",
		"12:00:00-13:00",
	};
	for (size_t i = 0; i < sizeof(bad_ranges) / sizeof(bad_ranges[0]); i++) {
		bool ok = calendar_range_parse(bad_ranges[i], &a, &b);
		if (ok)
			fprintf(stderr, "  \"%s\" accepted\n", bad_ranges[i]);
		CHECK(!ok);
	}
	CHECK(!calendar_range_parse(NULL, &a, &b));
}

/* ---- timing/MinuteRange -------------------------------------------------------- */

static void test_minute_range(void)
{
	setup("UTC0", ms(2026, 1, 5, 10, 0));
	Block *r = add("R", "timing/MinuteRange");
	set_str("R", "range", "12:30-15:45");
	start();
	CHECK(!out(r));
	CHECK(next_due() == engine(ms(2026, 1, 5, 12, 30)));
	observe(r, "R");
	run_until(ms(2026, 1, 7, 0, 0));
	check_trace("2026-01-05 12:30:00+0 R=1;2026-01-05 15:45:00+0 R=0;"
		    "2026-01-06 12:30:00+0 R=1;2026-01-06 15:45:00+0 R=0;");
	CHECK(fired == 4);

	/* An input change computes and re-arms at once: wrap past midnight. */
	set_str("R", "range", "22:00-06:00");
	record();
	check_trace("2026-01-07 00:00:00+0 R=1;");
	CHECK(next_due() == engine(ms(2026, 1, 7, 6, 0)));
	run_until(ms(2026, 1, 8, 0, 0));
	check_trace("2026-01-07 06:00:00+0 R=0;2026-01-07 22:00:00+0 R=1;");

	/* All day: true, woken at midnight (no change). */
	set_str("R", "range", "00:00-24:00");
	CHECK(out(r));
	CHECK(next_due() == engine(ms(2026, 1, 9, 0, 0)));
	run_until(ms(2026, 1, 10, 0, 0));
	record();
	check_trace("");

	/* Equal start and end: empty, never true, no timer. */
	set_str("R", "range", "08:00-08:00");
	CHECK(!out(r) && next_due() == UINT64_MAX);
	/* Invalid settings: false, no timer; a valid one arms again. */
	static const char *const bad[] = {"", "8-12", "25:00-26:00", "12:00-24:01",
					  "24:00-12:00", "noon"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		set_str("R", "range", "00:00-24:00");
		CHECK(out(r) && next_due() != UINT64_MAX);
		set_str("R", "range", bad[i]);
		CHECK(!out(r) && next_due() == UINT64_MAX);
	}
	set_str("R", "range", "00:00-24:00");
	set_value("R", "range", linkable_from_f64(5));
	CHECK(!out(r) && next_due() == UINT64_MAX);
	/* Whitespace is fine; the start is included, the end excluded. */
	run_until(ms(2026, 1, 10, 7, 5));
	set_str("R", "range", " 7:05 - 07:06 ");
	CHECK(out(r) && next_due() == engine(ms(2026, 1, 10, 7, 6)));
	run_until(ms(2026, 1, 10, 7, 6));
	CHECK(!out(r));

	/* Stop: no timer; restart: computed afresh. */
	graph_stop(gr);
	CHECK(next_due() == UINT64_MAX);
	run_until(ms(2026, 1, 11, 7, 5) + 30000);
	start();
	CHECK(out(r) && next_due() == engine(ms(2026, 1, 11, 7, 6)));
	teardown();
}

/* Spring forward in Sweden, 2026-03-29: 02:00 CET jumps to 03:00 CEST. */
static void test_minute_range_spring(void)
{
	setup(CET, ms(2026, 3, 28, 12, 0));
	Block *a = add("A", "timing/MinuteRange"), *b = add("B", "timing/MinuteRange"),
	      *c = add("C", "timing/MinuteRange");
	set_str("A", "range", "02:30-03:30"); /* starts in the skipped hour */
	set_str("B", "range", "01:00-02:30"); /* ends in it */
	set_str("C", "range", "02:00-03:00"); /* is it */
	start();
	observe(a, "A");
	observe(b, "B");
	observe(c, "C");
	run_until(ms(2026, 3, 30, 12, 0));
	check_trace("2026-03-29 01:00:00+1 B=1;"
		    "2026-03-29 03:00:00+2 A=1;2026-03-29 03:00:00+2 B=0;"
		    "2026-03-29 03:30:00+2 A=0;"
		    "2026-03-30 01:00:00+2 B=1;2026-03-30 02:00:00+2 C=1;"
		    "2026-03-30 02:30:00+2 A=1;2026-03-30 02:30:00+2 B=0;"
		    "2026-03-30 03:00:00+2 C=0;2026-03-30 03:30:00+2 A=0;");
	teardown();
}

/* Fall back in Sweden, 2026-10-25: 03:00 CEST goes back to 02:00 CET. */
static void test_minute_range_autumn(void)
{
	setup(CET, ms(2026, 10, 24, 12, 0));
	Block *a = add("A", "timing/MinuteRange"), *d = add("D", "timing/MinuteRange"),
	      *e = add("E", "timing/MinuteRange");
	set_str("A", "range", "02:30-02:45"); /* in the repeated hour: true twice */
	set_str("D", "range", "01:00-03:00"); /* covers it: three hours */
	set_str("E", "range", "03:00-04:00"); /* after it */
	start();
	observe(a, "A");
	observe(d, "D");
	observe(e, "E");
	run_until(ms(2026, 10, 25, 12, 0));
	check_trace("2026-10-25 01:00:00+2 D=1;"
		    "2026-10-25 02:30:00+2 A=1;2026-10-25 02:45:00+2 A=0;"
		    "2026-10-25 02:30:00+1 A=1;2026-10-25 02:45:00+1 A=0;"
		    "2026-10-25 03:00:00+1 D=0;2026-10-25 03:00:00+1 E=1;"
		    "2026-10-25 04:00:00+1 E=0;");
	teardown();
}

/* ---- The day-based primitives ------------------------------------------------ */

static void test_day_of_week(void)
{
	setup("UTC0", ms(2026, 1, 9, 12, 0)); /* a Friday */
	Block *w = add("W", "timing/DayOfWeek"), *s = add("S", "timing/DayOfWeek"),
	      *x = add("X", "timing/DayOfWeek");
	set_str("W", "days", "1-5");
	set_str("S", "days", "1");
	set_value("S", "sunday_first", linkable_from_bool(true)); /* day 1: Sunday */
	set_str("X", "days", "6-1");				  /* Saturday to Monday */
	start();
	CHECK(out(w) && !out(s) && !out(x));
	CHECK(next_due() == engine(ms(2026, 1, 10, 0, 0)));
	observe(w, "W");
	observe(s, "S");
	observe(x, "X");
	run_until(ms(2026, 1, 14, 0, 0));
	check_trace("2026-01-10 00:00:00+0 W=0;2026-01-10 00:00:00+0 X=1;"
		    "2026-01-11 00:00:00+0 S=1;"
		    "2026-01-12 00:00:00+0 W=1;2026-01-12 00:00:00+0 S=0;"
		    "2026-01-13 00:00:00+0 X=0;");
	/* Wednesday: Monday-first "1" is next Monday. */
	set_value("S", "sunday_first", linkable_from_bool(false));
	CHECK(!out(s));
	set_str("W", "days", "3");
	CHECK(out(w) && next_due() == engine(ms(2026, 1, 15, 0, 0)));
	/* Invalid: false, no timer. */
	set_str("S", "days", "0");
	set_str("W", "days", "8");
	set_str("X", "days", "");
	CHECK(!out(w) && !out(x) && next_due() == UINT64_MAX);
	/* A number is read as its text. */
	set_value("W", "days", linkable_from_i64(3));
	CHECK(out(w));
	teardown();
}

static void test_day_of_month(void)
{
	/* Month ends: "31" skips the months without a 31st. */
	setup("UTC0", ms(2026, 1, 30, 12, 0));
	Block *d = add("D", "timing/DayOfMonth"), *f = add("F", "timing/DayOfMonth");
	set_str("D", "days", "31");
	set_str("F", "days", "25-5"); /* wraps over the month end */
	start();
	CHECK(!out(d) && out(f));
	observe(d, "D");
	observe(f, "F");
	run_until(ms(2026, 6, 2, 0, 0));
	check_trace("2026-01-31 00:00:00+0 D=1;2026-02-01 00:00:00+0 D=0;"
		    "2026-02-06 00:00:00+0 F=0;2026-02-25 00:00:00+0 F=1;"
		    "2026-03-06 00:00:00+0 F=0;2026-03-25 00:00:00+0 F=1;"
		    "2026-03-31 00:00:00+0 D=1;2026-04-01 00:00:00+0 D=0;"
		    "2026-04-06 00:00:00+0 F=0;2026-04-25 00:00:00+0 F=1;"
		    "2026-05-06 00:00:00+0 F=0;2026-05-25 00:00:00+0 F=1;"
		    "2026-05-31 00:00:00+0 D=1;2026-06-01 00:00:00+0 D=0;");
	teardown();

	/* February 29: only in a leap year. */
	setup("UTC0", ms(2027, 2, 1, 0, 0));
	d = add("D", "timing/DayOfMonth");
	set_str("D", "days", "29");
	start();
	CHECK(next_due() == engine(ms(2027, 3, 29, 0, 0)));
	teardown();
	setup("UTC0", ms(2028, 2, 1, 0, 0));
	d = add("D", "timing/DayOfMonth");
	set_str("D", "days", "29");
	start();
	observe(d, "D");
	run_until(ms(2028, 3, 5, 0, 0));
	check_trace("2028-02-29 00:00:00+0 D=1;2028-03-01 00:00:00+0 D=0;");
	teardown();
}

static void test_month_of_year(void)
{
	/* In local time: November starts at local midnight, 23:00 UTC. */
	setup(CET, ms(2026, 10, 15, 12, 0));
	Block *m = add("M", "timing/MonthOfYear");
	set_str("M", "months", "11-2");
	start();
	CHECK(!out(m));
	observe(m, "M");
	/* The daylight-saving transition on October 25 wakes it once. */
	run_until(ms(2026, 10, 31, 22, 59));
	CHECK(fired == 1);
	CHECK(next_due() == engine(ms(2026, 10, 31, 23, 0)));
	run_until(ms(2027, 3, 15, 0, 0));
	check_trace("2026-11-01 00:00:00+1 M=1;2027-03-01 00:00:00+1 M=0;");
	set_str("M", "months", "13");
	CHECK(!out(m) && next_due() == UINT64_MAX);
	teardown();
}

static void test_day_of_year(void)
{
	setup("UTC0", ms(2026, 6, 1, 0, 0));
	Block *l = add("L", "timing/DayOfYear"), *s = add("S", "timing/DayOfYear");
	set_str("L", "days", "366"); /* December 31 of a leap year only */
	set_str("S", "days", "60");  /* March 1, or February 29 in a leap year */
	start();
	observe(l, "L");
	observe(s, "S");
	run_until(ms(2029, 1, 2, 0, 0));
	check_trace("2027-03-01 00:00:00+0 S=1;2027-03-02 00:00:00+0 S=0;"
		    "2028-02-29 00:00:00+0 S=1;2028-03-01 00:00:00+0 S=0;"
		    "2028-12-31 00:00:00+0 L=1;2029-01-01 00:00:00+0 L=0;");
	teardown();
}

static void test_week_of_year(void)
{
	/* 2026 has an ISO week 53: Monday December 28 to Sunday January 3. */
	setup("UTC0", ms(2026, 12, 1, 0, 0));
	Block *a = add("A", "timing/WeekOfYear"), *b = add("B", "timing/WeekOfYear");
	set_str("A", "weeks", "53");
	set_str("B", "weeks", "1");
	start();
	observe(a, "A");
	observe(b, "B");
	run_until(ms(2027, 1, 12, 0, 0));
	check_trace("2026-12-28 00:00:00+0 A=1;2027-01-04 00:00:00+0 A=0;"
		    "2027-01-04 00:00:00+0 B=1;2027-01-11 00:00:00+0 B=0;");
	teardown();

	/* The next week 53 is in 2032, years ahead: the block looks again
	 * after each horizon. */
	setup("UTC0", ms(2027, 1, 10, 0, 0));
	a = add("A", "timing/WeekOfYear");
	set_str("A", "weeks", "53");
	start();
	observe(a, "A");
	run_until(ms(2033, 1, 10, 0, 0));
	check_trace("2032-12-27 00:00:00+0 A=1;2033-01-03 00:00:00+0 A=0;");
	CHECK(fired < 10);
	teardown();
}

/* A day-based block across the spring transition: the day starts at local
 * midnight in either offset. */
static void test_day_spring(void)
{
	setup(CET, ms(2026, 3, 27, 12, 0));
	Block *s = add("S", "timing/DayOfWeek");
	set_str("S", "days", "7"); /* Sunday, March 29: the transition day */
	start();
	observe(s, "S");
	run_until(ms(2026, 3, 31, 0, 0));
	check_trace("2026-03-29 00:00:00+1 S=1;2026-03-30 00:00:00+2 S=0;");
	CHECK(fired == 3); /* midnight, the transition, midnight */
	teardown();
}

/* ---- Wall-clock steps (graphs/runtime.h "Wall clock") ----------------------- */

static void test_wall_clock_steps(void)
{
	setup("UTC0", ms(2026, 1, 5, 11, 0));
	Block *r = add("R", "timing/MinuteRange");
	Block *i = add("I", "timing/Interval");
	set_str("R", "range", "12:00-13:00");
	start();
	CHECK(!out(r));
	CHECK(r->timer.due_ms == engine(ms(2026, 1, 5, 12, 0)));
	uint64_t interval_due = i->timer.due_ms;

	/* Less than RUNTIME_WALL_STEP_MS: nothing happens. */
	runtime_set_now(&rt, 100);
	CHECK(!runtime_set_wall_ms(&rt, ms(2026, 1, 5, 11, 0) + 100 + 999));
	CHECK(r->timer.due_ms == engine(ms(2026, 1, 5, 12, 0)));

	/* Set forward to 12:30: the calendar block re-evaluates at once,
	 * timing/Interval (engine time only) is left alone. */
	CHECK(runtime_set_wall_ms(&rt, ms(2026, 1, 5, 12, 30)));
	CHECK(r->timer.due_ms == 100 && i->timer.due_ms == interval_due);
	CHECK(runtime_wall_ms(&rt) == ms(2026, 1, 5, 12, 30));
	runtime_fire_timers(&rt, 100);
	drain();
	CHECK(out(r));
	CHECK(r->timer.due_ms == 100 + 30 * 60 * 1000);

	/* Set back to 11:00. */
	CHECK(runtime_set_wall_ms(&rt, ms(2026, 1, 5, 11, 0)));
	runtime_fire_timers(&rt, 100);
	drain();
	CHECK(!out(r));
	CHECK(r->timer.due_ms == 100 + 60 * 60 * 1000);

	/* Drift adds up: four moves of 300 ms, the fourth re-evaluates. */
	int64_t w = ms(2026, 1, 5, 11, 0);
	CHECK(!runtime_set_wall_ms(&rt, w + 300));
	CHECK(!runtime_set_wall_ms(&rt, w + 600));
	CHECK(!runtime_set_wall_ms(&rt, w - 300 + 1)); /* back: -299 */
	CHECK(!runtime_set_wall_ms(&rt, w + 900));
	CHECK(runtime_set_wall_ms(&rt, w + 1200));
	CHECK(r->timer.due_ms == 100);
	runtime_fire_timers(&rt, 100);
	drain();
	CHECK(r->timer.due_ms == 100 + 60 * 60 * 1000 - 1200);

	/* A stopped graph has no timer to make due. */
	graph_stop(gr);
	CHECK(runtime_set_wall_ms(&rt, ms(2026, 1, 5, 12, 30)));
	CHECK(next_due() == UINT64_MAX);
	teardown();
}

/* ---- The schedule standard library (library/schedules/) --------------------- */

static char root[PAL_PATH_MAX];

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb;
	(void)flag;
	(void)ftw;
	return remove(path);
}

static enum pal_err fixed_random(void *buf, size_t len)
{
	memset(buf, 7, len);
	return PAL_OK;
}

/* Copy every .json file of the library into dir, as a deployment installs
 * them. Returns the number copied. */
static size_t install_library(const char *dir)
{
	DIR *d = opendir(HAKKAS_LIBRARY_DIR);
	CHECK(d != NULL);
	if (d == NULL)
		return 0;
	size_t n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		size_t len = strlen(e->d_name);
		if (len < 5 || strcmp(e->d_name + len - 5, ".json") != 0)
			continue;
		char from[2 * PAL_PATH_MAX], to[2 * PAL_PATH_MAX], buf[8192];
		snprintf(from, sizeof(from), "%s/%s", HAKKAS_LIBRARY_DIR, e->d_name);
		snprintf(to, sizeof(to), "%s/%s", dir, e->d_name);
		FILE *in = fopen(from, "rb"), *o = fopen(to, "wb");
		CHECK(in != NULL && o != NULL);
		if (in != NULL && o != NULL) {
			size_t got = fread(buf, 1, sizeof(buf), in);
			CHECK(got > 0 && got < sizeof(buf));
			CHECK(fwrite(buf, 1, got, o) == got);
			n++;
		}
		if (in != NULL)
			fclose(in);
		if (o != NULL)
			fclose(o);
	}
	closedir(d);
	return n;
}

/* The inner block called name of the composite instance b. */
static const Block *inner(const Block *b, const char *name)
{
	for (uint32_t k = 0; k < b->inner_count; k++) {
		if (strcmp(b->inner[k]->name, name) == 0)
			return b->inner[k];
	}
	CHECK(!"no such inner block");
	return b;
}

static void test_library(void)
{
	static Graphs gs;
	GraphFiles gf;
	setup(CET, ms(2026, 10, 23, 12, 0)); /* Friday, 14:00 CEST */
	graphs_init(&gs, &rt, block_types_get, fixed_random);
	CHECK(graph_files_init(&gf, &gs, root));
	CHECK(graph_files_make_dirs(&gf) == GRAPH_FILES_OK);
	char types[PAL_PATH_MAX + 8];
	snprintf(types, sizeof(types), "%s/types", root);
	size_t files = install_library(types);
	CHECK(files == 4);
	size_t loaded = 0;
	logs[0] = '\0';
	CHECK(type_files_load_all(&gf, &loaded) == GRAPH_FILES_OK);
	CHECK(loaded == files);
	CHECK_STREQ(logs, ""); /* every file name is its type's encoded name */

	static const char *const names[] = {"schedule/DaySchedule", "schedule/WeekSchedule",
					    "schedule/MonthSchedule", "schedule/YearSchedule"};
	for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
		const BlockTypeDef *t = block_types_get(names[k]);
		CHECK(t != NULL && t->subgraph);
		if (t == NULL)
			continue;
		CHECK_STREQ(t->icon, "schedule");
		CHECK(t->description[0] != '\0');
		CHECK(t->output_count == 1 && strcmp(t->outputs[0].name, "out") == 0 &&
		      strcmp(t->outputs[0].type, "bool") == 0);
		CHECK(t->input_count >= 1 && strcmp(t->inputs[0].name, "range") == 0);
		for (uint32_t j = 0; j < t->input_count; j++)
			CHECK(t->inputs[j].description[0] != '\0');
	}
	CHECK(block_types_get("schedule/WeekSchedule")->input_count == 3);
	CHECK_STREQ(block_types_get("schedule/WeekSchedule")->inputs[2].name, "sunday_first");
	CHECK_STREQ(block_types_get("schedule/MonthSchedule")->inputs[1].name, "days");
	CHECK_STREQ(block_types_get("schedule/YearSchedule")->inputs[1].name, "months");

	/* Office hours on weekdays, across the autumn transition. */
	Block *w = add("W", "schedule/WeekSchedule");
	Block *d = add("D", "schedule/DaySchedule");
	Block *m = add("M", "schedule/MonthSchedule");
	Block *y = add("Y", "schedule/YearSchedule");
	set_str("W", "range", "08:00-17:00");
	set_str("W", "days", "1-5");
	set_str("D", "range", "22:00-06:00");
	set_str("M", "range", "00:00-24:00");
	set_str("M", "days", "1");
	set_str("Y", "range", "12:00-13:00");
	set_str("Y", "months", "10-3");
	start();
	CHECK(out(inner(w, "Both")) && !out(inner(d, "Time")) && !out(inner(m, "Both")) &&
	      !out(inner(y, "Both")));
	observe(inner(w, "Both"), "W");
	observe(inner(d, "Time"), "D");
	observe(inner(m, "Both"), "M");
	observe(inner(y, "Both"), "Y");
	run_until(ms(2026, 10, 27, 12, 0));
	check_trace("2026-10-23 17:00:00+2 W=0;2026-10-23 22:00:00+2 D=1;"
		    "2026-10-24 06:00:00+2 D=0;2026-10-24 12:00:00+2 Y=1;"
		    "2026-10-24 13:00:00+2 Y=0;2026-10-24 22:00:00+2 D=1;"
		    "2026-10-25 06:00:00+1 D=0;2026-10-25 12:00:00+1 Y=1;"
		    "2026-10-25 13:00:00+1 Y=0;2026-10-25 22:00:00+1 D=1;"
		    "2026-10-26 06:00:00+1 D=0;2026-10-26 08:00:00+1 W=1;"
		    "2026-10-26 12:00:00+1 Y=1;2026-10-26 13:00:00+1 Y=0;"
		    "2026-10-26 17:00:00+1 W=0;2026-10-26 22:00:00+1 D=1;"
		    "2026-10-27 06:00:00+1 D=0;2026-10-27 08:00:00+1 W=1;"
		    "2026-10-27 12:00:00+1 Y=1;2026-10-27 13:00:00+1 Y=0;");
	/* November 1 and the month's first day. */
	set_str("Y", "months", "4-9");
	run_until(ms(2026, 11, 2, 12, 0));
	CHECK(strstr(trace, "2026-11-01 00:00:00+1 M=1;") != NULL);
	CHECK(strstr(trace, "2026-11-02 00:00:00+1 M=0;") != NULL);
	CHECK(strstr(trace, "Y=") == NULL); /* no longer in October..March */

	/* A wall-clock step reaches the inner blocks. */
	trace[0] = '\0';
	CHECK(runtime_set_wall_ms(&rt, ms(2026, 11, 2, 23, 0))); /* 00:00 CET, Tuesday */
	runtime_fire_timers(&rt, rt.now_ms);
	drain();
	record();
	check_trace("2026-11-03 00:00:00+1 W=0;2026-11-03 00:00:00+1 D=1;");

	graph_destroy(gr);
	gr = graph_create(&rt);
	graphs_deinit(&gs);
	composite_types_reset();
	teardown();
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(log_add_sink(log_sink, NULL));
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	const char *base = getenv("TMPDIR");
	snprintf(root, sizeof(root), "%s/hakkas-calendar-XXXXXX", base && *base ? base : "/tmp");
	if (mkdtemp(root) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	RUN(test_selectors);
	RUN(test_minute_range);
	RUN(test_minute_range_spring);
	RUN(test_minute_range_autumn);
	RUN(test_day_of_week);
	RUN(test_day_of_month);
	RUN(test_month_of_year);
	RUN(test_day_of_year);
	RUN(test_week_of_year);
	RUN(test_day_spring);
	RUN(test_wall_clock_steps);
	RUN(test_library);

	block_types_reset();
	log_remove_sink(log_sink, NULL);
	if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
	return test_result();
}
