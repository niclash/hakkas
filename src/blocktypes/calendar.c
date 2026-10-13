/*
 * The calendar primitives (03 §6.2): timing/MinuteRange, timing/DayOfWeek,
 * timing/DayOfMonth, timing/MonthOfYear, timing/DayOfYear and
 * timing/WeekOfYear. Each is a stateless TIMER_SCHEDULED type that computes on
 * input: out is true while the local time matches the block's setting, and
 * every compute returns the delay to the next instant at which that may
 * change, which the runtime arms as the block's one-shot timer (03 §2).
 *
 * Time (decided, plan task 24; 03 §6.2, 01 §3.4):
 *  - The wall clock is ctx.wall_ms (UTC, set by the engine loop from the
 *    PAL), converted to local time with ctx.tz (the time_zone setting,
 *    05 §1.1) in whole seconds.
 *  - The next boundary is found in local time (the next minute at which a
 *    range starts or ends, the next local midnight at which the day's match
 *    changes) and converted to UTC with tz_next_reach, so a boundary in the
 *    hour skipped in spring is reached at the jump. The block also wakes at
 *    every daylight-saving transition, where local time jumps and the value
 *    may change without a boundary being reached (in autumn the repeated
 *    hour is evaluated on local wall-clock time again, so a range in it is
 *    true twice). A wake-up that changes nothing propagates nothing.
 *  - A day-based block looks for the next day whose match differs for at
 *    most CALENDAR_HORIZON_DAYS days (week 53 may be years away); if none
 *    does, it wakes after that and looks again.
 *  - The runtime re-runs these types when the wall clock steps (the
 *    wall_clock flag, graphs/runtime.h), so a set clock or an NTP step is
 *    taken into account at once.
 *  - An invalid or empty setting makes out false with no timer armed (there
 *    is no error behavior in the dataflow, 01 §2.1); a valid one arms the
 *    timer again. A setting that is not a string is read in its text form
 *    (a number 3 is "3"). STOP changes nothing.
 */
#include "blocktypes/calendar.h"

#include <math.h>
#include <stddef.h>

#include "blocktypes/builtins.h"
#include "system/tz.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#define DAY_S 86400

/* How far ahead a day-based block looks for its next change. */
#define CALENDAR_HORIZON_DAYS 400

/* ---- Setting strings ------------------------------------------------------------ */

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/* Decimal digits into *out; a value above 9999 is invalid. Returns the
 * end, or NULL. */
static const char *parse_value(const char *p, int *out)
{
	int v = 0, n = 0;
	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (*p++ - '0');
		n++;
		if (v > 9999)
			return NULL;
	}
	if (n == 0)
		return NULL;
	*out = v;
	return p;
}

static void select_value(CalendarSelector *s, int v)
{
	s->bits[v / 64] |= (uint64_t)1 << (v % 64);
}

bool calendar_selector_parse(const char *text, int min, int max, CalendarSelector *out)
{
	CalendarSelector s = {{0}};
	const char *p = text;
	if (p == NULL || min < 1 || max > CALENDAR_SELECTOR_MAX || min > max)
		return false;
	for (;;) {
		int a, b;
		p = parse_value(skip_ws(p), &a);
		if (p == NULL || a < min || a > max)
			return false;
		b = a;
		p = skip_ws(p);
		if (*p == '-') {
			p = parse_value(skip_ws(p + 1), &b);
			if (p == NULL || b < min || b > max)
				return false;
			p = skip_ws(p);
		}
		/* a > b wraps: a..max, then min..b. */
		for (int v = a;; v = v == max ? min : v + 1) {
			select_value(&s, v);
			if (v == b)
				break;
		}
		if (*p == '\0')
			break;
		if (*p++ != ',')
			return false;
	}
	*out = s;
	return true;
}

/* "H:MM" or "HH:MM" into minutes of the day; "24:00" (1440) only if end.
 * Returns the end of the text, or NULL. */
static const char *parse_clock(const char *p, bool end, int *out)
{
	int h = 0, n = 0;
	while (*p >= '0' && *p <= '9' && n < 2) {
		h = h * 10 + (*p++ - '0');
		n++;
	}
	if (n == 0 || *p++ != ':')
		return NULL;
	if (!(p[0] >= '0' && p[0] <= '5' && p[1] >= '0' && p[1] <= '9'))
		return NULL;
	int m = (p[0] - '0') * 10 + (p[1] - '0');
	if (h > 24 || (h == 24 && (!end || m != 0)))
		return NULL;
	*out = h * 60 + m;
	return p + 2;
}

bool calendar_range_parse(const char *text, int *start, int *end)
{
	int s, e;
	const char *p = text;
	if (p == NULL || (p = parse_clock(skip_ws(p), false, &s)) == NULL)
		return false;
	p = skip_ws(p);
	if (*p != '-' || (p = parse_clock(skip_ws(p + 1), true, &e)) == NULL)
		return false;
	if (*skip_ws(p) != '\0')
		return false;
	*start = s;
	*end = e;
	return true;
}

/* ---- The current time --------------------------------------------------------- */

typedef struct {
	const TimeZone *tz;
	int64_t wall_ms;
	int64_t utc_s;	     /* wall_ms in whole seconds */
	int64_t next_change; /* the next daylight-saving transition (UTC) */
	TzLocal local;
} Now;

static void now_get(const BlockCtx *ctx, Now *n)
{
	n->tz = ctx->tz != NULL ? ctx->tz : &tz_utc;
	n->wall_ms = ctx->wall_ms;
	n->utc_s = tz_floor_div(ctx->wall_ms, 1000);
	tz_offset(n->tz, n->utc_s, NULL, &n->next_change);
	tz_local(n->tz, n->utc_s, &n->local);
}

/* The delay in ms until local time reaches local_s (a time after now), or
 * until the next daylight-saving transition if that is earlier; -1 if
 * neither ever happens. */
static int64_t wake_delay(const Now *n, int64_t local_s)
{
	int64_t at = tz_next_reach(n->tz, n->utc_s, local_s);
	if (at > n->next_change)
		at = n->next_change;
	if (at == TZ_NEVER || at > INT64_MAX / 1000)
		return -1;
	return at * 1000 - n->wall_ms; /* at > utc_s: at least 1 ms */
}

/* The text of a setting input: a string as is, another value in its text
 * form, unset as "" (invalid). */
static const char *setting_text(const Linkable *v, char *buf, size_t size)
{
	if (v->type == LINKABLE_STRING)
		return v->as.s;
	if (v->type == LINKABLE_UNSET)
		return "";
	linkable_stringify(v, buf, size);
	return buf;
}

static void set_out(BlockCtx *ctx, bool v)
{
	block_ctx_set_output(ctx, 0, linkable_from_bool(v));
}

/* ---- timing/MinuteRange ----------------------------------------------------- */

static int64_t minute_range(BlockCtx *ctx)
{
	if (ctx->reason == REASON_STOP)
		return -1;
	char buf[LINKABLE_NUMBER_TEXT_MAX];
	int s, e;
	if (!calendar_range_parse(setting_text(&ctx->inputs[0], buf, sizeof(buf)), &s, &e) ||
	    s == e) {
		set_out(ctx, false); /* invalid, or empty: never true */
		return -1;
	}
	Now n;
	now_get(ctx, &n);
	int64_t sod = (int64_t)n.local.hour * 3600 + n.local.minute * 60 + n.local.second;
	int64_t m = sod / 60;
	set_out(ctx, s < e ? m >= s && m < e : m >= s || m < e);

	/* The next start or end after now, today or tomorrow. */
	int64_t bs = (int64_t)s * 60, be = (int64_t)e * 60;
	int64_t b = INT64_MAX;
	if (bs > sod)
		b = bs;
	if (be > sod && be < b)
		b = be;
	if (b == INT64_MAX)
		b = (bs < be ? bs : be) + DAY_S;
	return wake_delay(&n, n.local.days * DAY_S + b);
}

/* ---- The day-based primitives ------------------------------------------------- */

typedef bool (*DayMatch)(const CalendarSelector *sel, int64_t days, bool flag);

static bool match_weekday(const CalendarSelector *sel, int64_t days, bool sunday_first)
{
	int wd = tz_weekday(days); /* 1 = Monday .. 7 = Sunday */
	return calendar_selector_has(sel, sunday_first ? wd % 7 + 1 : wd);
}

static bool match_day_of_month(const CalendarSelector *sel, int64_t days, bool flag)
{
	(void)flag;
	int64_t y;
	int m, d;
	tz_civil_from_days(days, &y, &m, &d);
	return calendar_selector_has(sel, d);
}

static bool match_month(const CalendarSelector *sel, int64_t days, bool flag)
{
	(void)flag;
	int64_t y;
	int m, d;
	tz_civil_from_days(days, &y, &m, &d);
	return calendar_selector_has(sel, m);
}

static bool match_day_of_year(const CalendarSelector *sel, int64_t days, bool flag)
{
	(void)flag;
	int64_t y;
	int m, d;
	tz_civil_from_days(days, &y, &m, &d);
	return calendar_selector_has(sel, (int)(days - tz_days_from_civil(y, 1, 1) + 1));
}

static bool match_iso_week(const CalendarSelector *sel, int64_t days, bool flag)
{
	(void)flag;
	int64_t y;
	int w;
	tz_iso_week(days, &y, &w);
	return calendar_selector_has(sel, w);
}

/* The common compute: input 0 the selector over 1..max, out true on the
 * days that match; wakes at the next local midnight where the match
 * changes. */
static int64_t day_block(BlockCtx *ctx, int max, DayMatch match, bool flag)
{
	if (ctx->reason == REASON_STOP)
		return -1;
	char buf[LINKABLE_NUMBER_TEXT_MAX];
	CalendarSelector sel;
	if (!calendar_selector_parse(setting_text(&ctx->inputs[0], buf, sizeof(buf)), 1, max,
				     &sel)) {
		set_out(ctx, false);
		return -1;
	}
	Now n;
	now_get(ctx, &n);
	int64_t today = n.local.days;
	bool v = match(&sel, today, flag);
	set_out(ctx, v);
	int64_t d = today + 1;
	while (d < today + CALENDAR_HORIZON_DAYS && match(&sel, d, flag) == v)
		d++;
	return wake_delay(&n, d * DAY_S);
}

static int64_t day_of_week(BlockCtx *ctx)
{
	return day_block(ctx, 7, match_weekday, linkable_to_bool(&ctx->inputs[1]));
}

static int64_t day_of_month(BlockCtx *ctx)
{
	return day_block(ctx, 31, match_day_of_month, false);
}

static int64_t month_of_year(BlockCtx *ctx)
{
	return day_block(ctx, 12, match_month, false);
}

static int64_t day_of_year(BlockCtx *ctx)
{
	return day_block(ctx, 366, match_day_of_year, false);
}

static int64_t week_of_year(BlockCtx *ctx)
{
	return day_block(ctx, 53, match_iso_week, false);
}

/* ---- Definitions -------------------------------------------------------------- */

static const PortDescriptor out_port[] = {
	{.name = "out", .description = "True while the local time matches", .type = "bool",
	 .default_value = LINKABLE_BOOL_INIT(false)},
};

static const PortDescriptor minute_range_inputs[] = {
	{.name = "range",
	 .description = "Time of day \"HH:MM-HH:MM\" in local time, e.g. \"12:30-15:45\": start "
			"included, end excluded; wraps past midnight (\"22:00-06:00\"); "
			"\"00:00-24:00\" is all day",
	 .type = "string",
	 .default_value = LINKABLE_EMPTY_STRING_INIT},
};

static const PortDescriptor day_of_week_inputs[] = {
	{.name = "days",
	 .description = "Weekdays, a selector such as \"1-5\" or \"6,7\": 1-7, day 1 Monday "
			"(or Sunday if sunday_first)",
	 .type = "string",
	 .default_value = LINKABLE_EMPTY_STRING_INIT},
	{.name = "sunday_first", .description = "Day 1 is Sunday instead of Monday",
	 .type = "bool", .default_value = LINKABLE_BOOL_INIT(false)},
};

static const PortDescriptor day_of_month_inputs[] = {
	{.name = "days",
	 .description = "Days of the month, a selector such as \"1\" or \"1-15\": 1-31 (a day a "
			"month does not have never matches)",
	 .type = "string",
	 .default_value = LINKABLE_EMPTY_STRING_INIT},
};

static const PortDescriptor month_of_year_inputs[] = {
	{.name = "months",
	 .description = "Months, a selector such as \"6-8\" or \"11-2\": 1-12, 1 January",
	 .type = "string",
	 .default_value = LINKABLE_EMPTY_STRING_INIT},
};

static const PortDescriptor day_of_year_inputs[] = {
	{.name = "days",
	 .description = "Days of the year, a selector such as \"1-31\": 1-366, 1 January 1",
	 .type = "string",
	 .default_value = LINKABLE_EMPTY_STRING_INIT},
};

static const PortDescriptor week_of_year_inputs[] = {
	{.name = "weeks",
	 .description = "ISO 8601 weeks (starting on Monday), a selector such as \"1-26\": 1-53",
	 .type = "string",
	 .default_value = LINKABLE_EMPTY_STRING_INIT},
};

#define CALENDAR_TYPE(n, ic, desc, ins, fn)                                                        \
	{.name = n,                                                                                \
	 .icon = ic,                                                                               \
	 .description = desc,                                                                      \
	 .inputs = ins,                                                                            \
	 .input_count = ARRAY_LEN(ins),                                                            \
	 .outputs = out_port,                                                                      \
	 .output_count = ARRAY_LEN(out_port),                                                      \
	 .compute_on_input = true,                                                                 \
	 .timer = TIMER_SCHEDULED,                                                                 \
	 .compute = fn,                                                                            \
	 .change_threshold = NAN,                                                                  \
	 .wall_clock = true}

const BlockTypeDef calendar_block_types[] = {
	CALENDAR_TYPE("timing/MinuteRange", "schedule", "true within a time-of-day range",
		      minute_range_inputs, minute_range),
	CALENDAR_TYPE("timing/DayOfWeek", "date_range", "true on the selected weekdays",
		      day_of_week_inputs, day_of_week),
	CALENDAR_TYPE("timing/DayOfMonth", "date_range", "true on the selected days of the month",
		      day_of_month_inputs, day_of_month),
	CALENDAR_TYPE("timing/MonthOfYear", "date_range", "true during the selected months",
		      month_of_year_inputs, month_of_year),
	CALENDAR_TYPE("timing/DayOfYear", "date_range", "true on the selected days of the year",
		      day_of_year_inputs, day_of_year),
	CALENDAR_TYPE("timing/WeekOfYear", "date_range", "true during the selected ISO weeks",
		      week_of_year_inputs, week_of_year),
};

const size_t calendar_block_types_count = ARRAY_LEN(calendar_block_types);
