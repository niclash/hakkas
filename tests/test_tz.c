/*
 * POSIX TZ strings and local time (system/tz.h; 01 §3.4, 05 §1.1): parsing,
 * the calendar arithmetic, UTC to local time compared with glibc's
 * localtime_r under the same TZ (the test may use POSIX; the core may not),
 * known 2026 transitions, and tz_next_reach across daylight-saving edges.
 */
#include "test.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "system/tz.h"

/* UTC seconds of a UTC date and time. */
static int64_t utc(int64_t y, int mo, int d, int h, int mi, int s)
{
	return tz_days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

/* Local seconds (utc + offset) of a local date and time. */
static int64_t loc(int64_t y, int mo, int d, int h, int mi)
{
	return utc(y, mo, d, h, mi, 0);
}

static TimeZone parse(const char *text)
{
	TimeZone tz = tz_utc;
	CHECK(tz_parse(text, &tz));
	return tz;
}

/* ---- Parsing ------------------------------------------------------------------ */

static void test_parse_valid(void)
{
	TimeZone tz;
	tz = parse("UTC0");
	CHECK(tz.std_offset_s == 0 && !tz.has_dst);
	tz = parse("<+0330>-3:30");
	CHECK(tz.std_offset_s == 3 * 3600 + 1800 && !tz.has_dst);
	CHECK(tz.dst_offset_s == tz.std_offset_s);
	tz = parse("<-03>3");
	CHECK(tz.std_offset_s == -3 * 3600);
	tz = parse("EST+5");
	CHECK(tz.std_offset_s == -5 * 3600);
	tz = parse("XYZ-5:45:30");
	CHECK(tz.std_offset_s == 5 * 3600 + 45 * 60 + 30);

	tz = parse("CET-1CEST,M3.5.0,M10.5.0/3");
	CHECK(tz.std_offset_s == 3600 && tz.dst_offset_s == 7200 && tz.has_dst);
	CHECK(tz.start.kind == TZ_RULE_MONTH_WEEK_DAY && tz.start.month == 3 &&
	      tz.start.week == 5 && tz.start.day == 0 && tz.start.time_s == 7200);
	CHECK(tz.end.kind == TZ_RULE_MONTH_WEEK_DAY && tz.end.month == 10 && tz.end.week == 5 &&
	      tz.end.day == 0 && tz.end.time_s == 3 * 3600);

	/* An explicit dst offset; quoted names; Jn and n rules; times out of
	 * the day (POSIX.1-2024: -167..167 hours). */
	tz = parse("CET-1CEST-3,M3.5.0,M10.5.0/3");
	CHECK(tz.dst_offset_s == 3 * 3600);
	tz = parse("<+0330>-3:30<+0430>,J79/24,J263/24");
	CHECK(tz.dst_offset_s == 4 * 3600 + 1800);
	CHECK(tz.start.kind == TZ_RULE_JULIAN && tz.start.day == 79 && tz.start.time_s == 86400);
	tz = parse("AAA3BBB,59,299/-1:30");
	CHECK(tz.start.kind == TZ_RULE_DAY_OF_YEAR && tz.start.day == 59);
	CHECK(tz.end.time_s == -5400);
	tz = parse("IST-2IDT,M3.4.4/26,M10.5.0");
	CHECK(tz.start.time_s == 26 * 3600 && tz.start.day == 4);
	tz = parse("EST5EDT,M3.2.0/2:00:00,M11.1.0/167");
	CHECK(tz.end.time_s == 167 * 3600);
	tz = parse("AAA3BBB,0,J365/25");
	CHECK(tz.start.day == 0 && tz.end.day == 365);
}

static void test_parse_invalid(void)
{
	static const char *const bad[] = {
		"",
		":Europe/Stockholm",
		"Europe/Stockholm",
		"UTC",		/* the offset is required */
		"CET-1CEST",	/* daylight-saving time without the rules */
		"CET-1CEST,M3.5.0",
		"CET-1CEST,M3.5.0,M10.5.0,",
		"CET-1CEST,M3.5.0,M10.5.0/3x",
		"CE-1",		/* a name of 2 letters */
		"C3T-1",
		"ABCDEFGHIJKLMNOPQ-1", /* 17 letters */
		"CET25",
		"CET-1:60",
		"CET-1:00:60",
		"CET-123",
		"CET-",
		"CET+",
		"CET-1 ",
		"<+03-3",
		"<+0>-3",
		"<+0 3>-3",
		"CET-1CEST,M13.1.0,M10.5.0",
		"CET-1CEST,M0.1.0,M10.5.0",
		"CET-1CEST,M3.6.0,M10.5.0",
		"CET-1CEST,M3.0.0,M10.5.0",
		"CET-1CEST,M3.5.7,M10.5.0",
		"CET-1CEST,M3.5,M10.5.0",
		"CET-1CEST,M3.5.0/168,M10.5.0",
		"CET-1CEST,M3.5.0/-168,M10.5.0",
		"CET-1CEST,J0,J365",
		"CET-1CEST,J366,J365",
		"CET-1CEST,366,0",
		"CET-1CEST,X1,J2",
		"CET-1CEST25,M3.5.0,M10.5.0",
		"CET-1CES,M3.5.0,M10.5.0x",
		"UTC0 trailing",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		TimeZone tz = tz_utc;
		tz.std_offset_s = 77;
		bool ok = tz_parse(bad[i], &tz);
		if (ok)
			fprintf(stderr, "  accepted \"%s\"\n", bad[i]);
		CHECK(!ok);
		CHECK(tz.std_offset_s == 77); /* unchanged */
	}
	CHECK(!tz_parse(NULL, &(TimeZone){0}));
	/* Too long: TZ_TEXT_MAX bytes at most. */
	char longer[TZ_TEXT_MAX + 2];
	memset(longer, 'A', sizeof(longer) - 1);
	longer[sizeof(longer) - 1] = '\0';
	memcpy(longer + 60, "-1", 2);
	CHECK(!tz_parse(longer, &(TimeZone){0}));
}

/* ---- Calendar arithmetic --------------------------------------------------------- */

static void test_civil(void)
{
	CHECK(tz_days_from_civil(1970, 1, 1) == 0);
	CHECK(tz_days_from_civil(2000, 3, 1) == 11017);
	CHECK(tz_days_from_civil(1969, 12, 31) == -1);
	CHECK(tz_weekday(0) == 4);			     /* Thursday */
	CHECK(tz_weekday(-1) == 3);			     /* Wednesday */
	CHECK(tz_weekday(tz_days_from_civil(2026, 10, 13)) == 2); /* a Tuesday */
	CHECK(tz_leap_year(2000) && tz_leap_year(2028) && !tz_leap_year(1900) &&
	      !tz_leap_year(2026));
	CHECK(tz_month_days(2028, 2) == 29 && tz_month_days(2026, 2) == 28 &&
	      tz_month_days(2026, 4) == 30 && tz_month_days(2026, 13) == 0);
	/* Round trip over a wide range, against timegm. */
	for (int64_t d = -800000; d <= 800000; d += 997) {
		int64_t y;
		int m, dd;
		tz_civil_from_days(d, &y, &m, &dd);
		CHECK(tz_days_from_civil(y, m, dd) == d);
		struct tm tm = {.tm_year = (int)(y - 1900), .tm_mon = m - 1, .tm_mday = dd};
		CHECK((int64_t)timegm(&tm) == d * 86400);
	}
	/* ISO 8601 weeks. */
	static const struct {
		int64_t y;
		int m, d;
		int64_t iso_year;
		int week;
	} weeks[] = {
		{2026, 1, 1, 2026, 1},	 {2026, 12, 31, 2026, 53}, {2027, 1, 3, 2026, 53},
		{2027, 1, 4, 2027, 1},	 {2021, 1, 3, 2020, 53},   {2024, 12, 30, 2025, 1},
		{2025, 12, 28, 2025, 52}, {2032, 12, 31, 2032, 53}, {2028, 1, 2, 2027, 52},
	};
	for (size_t i = 0; i < sizeof(weeks) / sizeof(weeks[0]); i++) {
		int64_t iy;
		int w;
		tz_iso_week(tz_days_from_civil(weeks[i].y, weeks[i].m, weeks[i].d), &iy, &w);
		CHECK(iy == weeks[i].iso_year && w == weeks[i].week);
	}
	CHECK(tz_floor_div(-1, 1000) == -1 && tz_floor_div(-1000, 1000) == -1 &&
	      tz_floor_div(999, 1000) == 0);
}

/* ---- Against glibc ------------------------------------------------------------- */

static int mismatches;

static void compare_at(const TimeZone *tz, const char *text, int64_t t)
{
	TzLocal l;
	tz_local(tz, t, &l);
	time_t tt = (time_t)t;
	struct tm tm;
	CHECK(localtime_r(&tt, &tm) != NULL);
	char iso[16];
	strftime(iso, sizeof(iso), "%G %V", &tm);
	char mine[16];
	snprintf(mine, sizeof(mine), "%04lld %02d", (long long)l.iso_year, l.iso_week);
	bool same = l.year == tm.tm_year + 1900 && l.month == tm.tm_mon + 1 &&
		    l.day == tm.tm_mday && l.hour == tm.tm_hour && l.minute == tm.tm_min &&
		    l.second == tm.tm_sec && l.weekday % 7 == tm.tm_wday &&
		    l.yday == tm.tm_yday + 1 && l.dst == (tm.tm_isdst > 0) &&
		    l.offset_s == tm.tm_gmtoff && strcmp(iso, mine) == 0;
	if (!same && mismatches++ < 10)
		fprintf(stderr,
			"  %s at %lld: mine %04lld-%02d-%02d %02d:%02d:%02d dst %d off %d week "
			"%s, glibc %04d-%02d-%02d %02d:%02d:%02d dst %d off %ld week %s\n",
			text, (long long)t, (long long)l.year, l.month, l.day, l.hour, l.minute,
			l.second, l.dst, (int)l.offset_s, mine, tm.tm_year + 1900, tm.tm_mon + 1,
			tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_isdst, tm.tm_gmtoff,
			iso);
	CHECK(same);
}

static void test_against_glibc(void)
{
	static const char *const zones[] = {
		"UTC0",
		"CET-1CEST,M3.5.0,M10.5.0/3",
		"AEST-10AEDT,M10.1.0,M4.1.0/3",
		"<+0330>-3:30",
		"EST5EDT,M3.2.0,M11.1.0",
		"NZST-12NZDT,M9.5.0,M4.1.0/3",
		"<-03>3<-02>,M3.5.0/-2,M10.5.0/-1",
		"IST-2IDT,M3.4.4/26,M10.5.0",
		"<+0330>-3:30<+0430>,J79/24,J263/24",
		"AAA3BBB,J60,J300",
		"AAA3BBB,59,299/-1:30",
		"WET0WEST,M3.5.0/1,M10.5.0",
		"<-01>1<+00>,M3.5.0/0,M10.5.0/1",
	};
	for (size_t z = 0; z < sizeof(zones) / sizeof(zones[0]); z++) {
		TimeZone tz = parse(zones[z]);
		CHECK(setenv("TZ", zones[z], 1) == 0);
		tzset();
		mismatches = 0;
		/* 2024-2030, every 7 h 13 min 17 s. */
		for (int64_t t = utc(2024, 1, 1, 0, 0, 0); t < utc(2031, 1, 1, 0, 0, 0);
		     t += 7 * 3600 + 13 * 60 + 17)
			compare_at(&tz, zones[z], t);
		/* Around every transition, to the second. */
		int64_t t = utc(2024, 1, 1, 0, 0, 0), next;
		int n = 0;
		while (tz_offset(&tz, t, NULL, &next), next < utc(2031, 1, 1, 0, 0, 0)) {
			for (int64_t d = -2; d <= 2; d++)
				compare_at(&tz, zones[z], next + d);
			t = next;
			n++;
		}
		CHECK(n == (tz.has_dst ? 14 : 0));
	}
	unsetenv("TZ");
	tzset();
}

/* ---- Known transitions --------------------------------------------------------- */

static void test_transitions_2026(void)
{
	/* Sweden: the last Sundays of March (29th, 02:00 CET -> 03:00 CEST at
	 * 01:00 UTC) and October (25th, 03:00 CEST -> 02:00 CET at 01:00 UTC). */
	TimeZone cet = parse("CET-1CEST,M3.5.0,M10.5.0/3");
	int64_t next;
	bool dst;
	CHECK(tz_offset(&cet, utc(2026, 1, 15, 0, 0, 0), &dst, &next) == 3600 && !dst);
	CHECK(next == utc(2026, 3, 29, 1, 0, 0));
	CHECK(tz_offset(&cet, utc(2026, 3, 29, 0, 59, 59), &dst, NULL) == 3600 && !dst);
	CHECK(tz_offset(&cet, utc(2026, 3, 29, 1, 0, 0), &dst, &next) == 7200 && dst);
	CHECK(next == utc(2026, 10, 25, 1, 0, 0));
	CHECK(tz_offset(&cet, utc(2026, 10, 25, 0, 59, 59), &dst, NULL) == 7200 && dst);
	CHECK(tz_offset(&cet, utc(2026, 10, 25, 1, 0, 0), &dst, &next) == 3600 && !dst);
	CHECK(next == utc(2027, 3, 28, 1, 0, 0));

	TzLocal l;
	tz_local(&cet, utc(2026, 3, 29, 1, 0, 0), &l);
	CHECK(l.hour == 3 && l.minute == 0 && l.dst && l.weekday == 7 && l.yday == 88);
	tz_local(&cet, utc(2026, 10, 25, 0, 30, 0), &l);
	CHECK(l.hour == 2 && l.minute == 30 && l.dst);
	tz_local(&cet, utc(2026, 10, 25, 1, 30, 0), &l);
	CHECK(l.hour == 2 && l.minute == 30 && !l.dst);
	/* New year in local time. */
	tz_local(&cet, utc(2026, 12, 31, 23, 0, 0), &l);
	CHECK(l.year == 2027 && l.month == 1 && l.day == 1 && l.hour == 0 && l.yday == 1 &&
	      l.iso_year == 2026 && l.iso_week == 53);

	/* Southern hemisphere: Sydney leaves daylight-saving time on the first
	 * Sunday of April (5th, 03:00 AEDT -> 02:00 AEST at 16:00 UTC on the
	 * 4th) and enters it on the first Sunday of October (4th, 02:00 AEST ->
	 * 03:00 AEDT at 16:00 UTC on the 3rd). */
	TimeZone syd = parse("AEST-10AEDT,M10.1.0,M4.1.0/3");
	CHECK(tz_offset(&syd, utc(2026, 1, 15, 0, 0, 0), &dst, &next) == 11 * 3600 && dst);
	CHECK(next == utc(2026, 4, 4, 16, 0, 0));
	CHECK(tz_offset(&syd, utc(2026, 4, 4, 16, 0, 0), &dst, &next) == 10 * 3600 && !dst);
	CHECK(next == utc(2026, 10, 3, 16, 0, 0));
	CHECK(tz_offset(&syd, utc(2026, 12, 31, 13, 0, 0), &dst, NULL) == 11 * 3600 && dst);
	tz_local(&syd, utc(2026, 12, 31, 13, 0, 0), &l);
	CHECK(l.year == 2027 && l.month == 1 && l.day == 1 && l.hour == 0);

	TimeZone u = parse("UTC0");
	CHECK(tz_offset(&u, 0, &dst, &next) == 0 && !dst && next == TZ_NEVER);
	TimeZone teh = parse("<+0330>-3:30");
	tz_local(&teh, utc(2026, 6, 1, 20, 30, 0), &l);
	CHECK(l.day == 2 && l.hour == 0 && l.minute == 0 && l.offset_s == 12600);

	/* Jn never counts February 29; n does. */
	TimeZone j = parse("AAA3BBB,J60/0,J300/0");
	TimeZone n = parse("AAA3BBB,59/0,299/0");
	CHECK(tz_offset(&j, utc(2028, 1, 1, 0, 0, 0), NULL, &next) == -3 * 3600);
	CHECK(next == utc(2028, 3, 1, 3, 0, 0)); /* J60: March 1, also in a leap year */
	CHECK(tz_offset(&n, utc(2028, 1, 1, 0, 0, 0), NULL, &next) == -3 * 3600);
	CHECK(next == utc(2028, 2, 29, 3, 0, 0)); /* 59: February 29 in a leap year */
	CHECK(tz_offset(&n, utc(2026, 1, 1, 0, 0, 0), NULL, &next) == -3 * 3600);
	CHECK(next == utc(2026, 3, 1, 3, 0, 0));
}

/* ---- tz_next_reach ------------------------------------------------------------- */

static void test_next_reach(void)
{
	TimeZone cet = parse("CET-1CEST,M3.5.0,M10.5.0/3");
	int64_t mar29 = utc(2026, 3, 29, 0, 0, 0);
	/* Before the jump, in it (reached at the jump), after it. */
	CHECK(tz_next_reach(&cet, mar29, loc(2026, 3, 29, 1, 30)) == utc(2026, 3, 29, 0, 30, 0));
	CHECK(tz_next_reach(&cet, mar29, loc(2026, 3, 29, 2, 0)) == utc(2026, 3, 29, 1, 0, 0));
	CHECK(tz_next_reach(&cet, mar29, loc(2026, 3, 29, 2, 30)) == utc(2026, 3, 29, 1, 0, 0));
	CHECK(tz_next_reach(&cet, mar29, loc(2026, 3, 29, 3, 0)) == utc(2026, 3, 29, 1, 0, 0));
	CHECK(tz_next_reach(&cet, mar29, loc(2026, 3, 29, 3, 30)) == utc(2026, 3, 29, 1, 30, 0));
	/* "After": an instant equal to after_s is not reached again. */
	CHECK(tz_next_reach(&cet, utc(2026, 3, 29, 0, 30, 0), loc(2026, 3, 29, 1, 30)) == TZ_NEVER);

	/* Autumn: 02:30 local occurs twice. */
	int64_t oct25 = utc(2026, 10, 25, 0, 0, 0);
	CHECK(tz_next_reach(&cet, oct25, loc(2026, 10, 25, 2, 30)) == utc(2026, 10, 25, 0, 30, 0));
	CHECK(tz_next_reach(&cet, utc(2026, 10, 25, 0, 30, 0), loc(2026, 10, 25, 2, 30)) ==
	      utc(2026, 10, 25, 1, 30, 0));
	CHECK(tz_next_reach(&cet, utc(2026, 10, 25, 1, 30, 0), loc(2026, 10, 25, 2, 30)) ==
	      TZ_NEVER);
	CHECK(tz_next_reach(&cet, oct25, loc(2026, 10, 25, 3, 0)) == utc(2026, 10, 25, 2, 0, 0));
	/* 02:00 CEST is oct25 itself, not after it; 02:00 CET is. */
	CHECK(tz_next_reach(&cet, oct25, loc(2026, 10, 25, 2, 0)) == utc(2026, 10, 25, 1, 0, 0));

	/* Far ahead, across transitions; in the past: never. */
	CHECK(tz_next_reach(&cet, utc(2026, 1, 1, 0, 0, 0), loc(2026, 7, 1, 12, 0)) ==
	      utc(2026, 7, 1, 10, 0, 0));
	CHECK(tz_next_reach(&cet, utc(2026, 1, 1, 0, 0, 0), loc(2027, 1, 1, 0, 0)) ==
	      utc(2026, 12, 31, 23, 0, 0));
	CHECK(tz_next_reach(&cet, utc(2026, 7, 1, 0, 0, 0), loc(2026, 1, 1, 0, 0)) == TZ_NEVER);

	TimeZone u = tz_utc;
	CHECK(tz_next_reach(&u, 100, 200) == 200);
	CHECK(tz_next_reach(&u, 200, 200) == TZ_NEVER);

	/* Southern hemisphere: Sydney's spring gap on October 4, 02:00-03:00. */
	TimeZone syd = parse("AEST-10AEDT,M10.1.0,M4.1.0/3");
	CHECK(tz_next_reach(&syd, utc(2026, 10, 3, 0, 0, 0), loc(2026, 10, 4, 2, 15)) ==
	      utc(2026, 10, 3, 16, 0, 0));
}

int main(void)
{
	RUN(test_parse_valid);
	RUN(test_parse_invalid);
	RUN(test_civil);
	RUN(test_against_glibc);
	RUN(test_transitions_2026);
	RUN(test_next_reach);
	return test_result();
}
