/*
 * Local time from a POSIX TZ string (01 §3.4, 05 §1.1 time_zone, 03 §6.2):
 * parsing the string, converting UTC to local broken-down time, and finding
 * the UTC instant at which local time reaches a given local wall time. No
 * time-zone database and no C library time functions (localtime, mktime):
 * the string encodes the offsets and the daylight-saving rules, and the
 * calendar arithmetic is done here, so every port gives the same result.
 *
 * Naming: functions are prefixed tz_; TimeZone holds a parsed string, TzLocal
 * a broken-down local time.
 *
 * Times: UTC instants are int64_t seconds since the Unix epoch (1970-01-01
 * 00:00:00 UTC, leap seconds not counted, as POSIX time). A "local time" in
 * seconds is the same count on the local wall clock: utc + offset, where the
 * offset is local minus UTC (east of Greenwich positive, the opposite sign of
 * the TZ string's). Days are counted from 1970-01-01 in the proleptic
 * Gregorian calendar; the range is that of the int64_t arithmetic, far more
 * than any wall clock needs.
 *
 * The syntax (decided, plan task 24; 05 §1.1), POSIX.1-2024 "TZ" without the
 * implementation-defined forms:
 *
 *	std offset [dst [offset] [,start[/time],end[/time]]]
 *
 *  - std, dst: a name of at least 3 characters, either letters only
 *    ("CET") or quoted in <> with letters, digits, '+' and '-' ("<+0330>");
 *    at most TZ_NAME_MAX characters. The names are only checked, not kept.
 *  - offset: [+-]hh[:mm[:ss]], the time to add to local time to get UTC
 *    (positive west of Greenwich), hh 0-24 (one or two digits), mm and ss
 *    00-59 (one or two digits). std's offset is required. dst's defaults to
 *    one hour less than std's (one hour ahead of standard time).
 *  - start, end: when daylight-saving time starts and ends, each one of
 *      Jn     the n-th day of the year, 1-365, February 29 never counted
 *             (J60 is always March 1);
 *      n      the zero-based day of the year, 0-365, February 29 counted;
 *      Mm.w.d day d (0 = Sunday .. 6) of week w (1-5, 5 = the last such day
 *             of the month) of month m (1-12);
 *    with an optional /time, [+-]hh[:mm[:ss]] with hh 0-167 (the POSIX.1-2024
 *    extension), default 02:00:00. start's time is in standard time, end's in
 *    daylight-saving time.
 *  - A dst without the rules is invalid: POSIX leaves the rules
 *    implementation-defined (glibc applies the US rules), so the string must
 *    state them. A string starting with ':' (an implementation-defined form,
 *    e.g. a database name) is invalid, as is an empty one.
 *
 * Daylight-saving time is computed per year from the rules: in each year,
 * start's and end's instants are transitions to daylight-saving and to
 * standard time; the offset at an instant is the one set by the latest
 * transition before it (at equal instants an end comes before a start). So a
 * southern-hemisphere zone (start after end in the year) works, and the
 * transitions of neighbouring years are taken into account near new year.
 *
 * Pure: no storage, no clock, no allocation.
 */
#ifndef HAKKAS_SYSTEM_TZ_H
#define HAKKAS_SYSTEM_TZ_H

#include <stdbool.h>
#include <stdint.h>

/* The longest zone name, in characters (without the quoting <>). */
#define TZ_NAME_MAX 16

/* The longest TZ string accepted, in bytes. */
#define TZ_TEXT_MAX 64

/* No instant: no further change (tz_offset) or never reached (tz_next_reach). */
#define TZ_NEVER INT64_MAX

typedef enum {
	TZ_RULE_MONTH_WEEK_DAY = 0, /* Mm.w.d */
	TZ_RULE_JULIAN,		    /* Jn: 1-365, February 29 not counted */
	TZ_RULE_DAY_OF_YEAR,	    /* n: 0-365, February 29 counted */
} TzRuleKind;

/* One daylight-saving transition rule (start or end). */
typedef struct {
	TzRuleKind kind;
	int16_t day;	/* Jn: 1-365; n: 0-365; Mm.w.d: d, 0 (Sunday) - 6 */
	uint8_t month;	/* Mm.w.d: 1-12 */
	uint8_t week;	/* Mm.w.d: 1-5 */
	int32_t time_s; /* local time of day of the transition, -167 h .. 167 h */
} TzRule;

/* A parsed TZ string. */
typedef struct {
	int32_t std_offset_s; /* standard time minus UTC, seconds (east positive) */
	int32_t dst_offset_s; /* daylight-saving time minus UTC; = std without dst */
	bool has_dst;
	TzRule start, end; /* has_dst only */
} TimeZone;

/* Broken-down local time. */
typedef struct {
	int64_t year;
	int month;	  /* 1-12 */
	int day;	  /* 1-31 */
	int hour;	  /* 0-23 */
	int minute;	  /* 0-59 */
	int second;	  /* 0-59 */
	int weekday;	  /* ISO 8601: 1 = Monday .. 7 = Sunday */
	int yday;	  /* day of the year, 1-366 */
	int64_t iso_year; /* the ISO 8601 week-numbering year */
	int iso_week;	  /* ISO 8601 week, 1-53 */
	bool dst;	  /* daylight-saving time in effect */
	int32_t offset_s; /* local minus UTC */
	int64_t days;	  /* the local date as days since 1970-01-01 */
} TzLocal;

/* "UTC0": UTC, no daylight-saving time (the time_zone default, 05 §1.1). */
extern const TimeZone tz_utc;

/* Parse the NUL-terminated TZ string text (the syntax above) into *out.
 * False, *out unchanged, if text is NULL, longer than TZ_TEXT_MAX bytes or
 * not a valid TZ string. */
bool tz_parse(const char *text, TimeZone *out);

/* The offset (local minus UTC, seconds) in effect at the UTC instant utc_s.
 * *dst (if not NULL) tells whether it is daylight-saving time; *next_change
 * (if not NULL) is the first UTC instant after utc_s at which a transition
 * takes place (the offset may stay the same), or TZ_NEVER without
 * daylight-saving time. */
int32_t tz_offset(const TimeZone *tz, int64_t utc_s, bool *dst, int64_t *next_change);

/* The local time at the UTC instant utc_s. */
void tz_local(const TimeZone *tz, int64_t utc_s, TzLocal *out);

/*
 * The first UTC instant after after_s at which local time reaches the local
 * time local_s (seconds on the local wall clock, utc + offset): the instant
 * that is local_s in the offset in effect then. If local_s falls into a gap
 * (spring forward: the local times skipped by a transition), it is the
 * transition, where local time jumps past it (03 §6.2: a range starting in
 * the skipped hour begins at the jump). A local time repeated by a
 * transition back (autumn) is reached the first time it occurs after
 * after_s. TZ_NEVER if local time never reaches local_s after after_s.
 */
int64_t tz_next_reach(const TimeZone *tz, int64_t after_s, int64_t local_s);

/* ---- Calendar arithmetic (proleptic Gregorian) ------------------------------ */

bool tz_leap_year(int64_t year);

/* Days in month (1-12) of year. */
int tz_month_days(int64_t year, int month);

/* Days since 1970-01-01 of year-month-day (month 1-12, day 1-31; a day past
 * the month's end counts on into the next month). */
int64_t tz_days_from_civil(int64_t year, int month, int day);

/* The date of days since 1970-01-01. */
void tz_civil_from_days(int64_t days, int64_t *year, int *month, int *day);

/* The ISO 8601 weekday of days since 1970-01-01: 1 = Monday .. 7 = Sunday. */
int tz_weekday(int64_t days);

/* The ISO 8601 week (1-53) and week-numbering year of days since 1970-01-01:
 * weeks start on Monday, week 1 is the one with the year's first Thursday. */
void tz_iso_week(int64_t days, int64_t *iso_year, int *week);

/* floor(a / b) for b > 0. */
static inline int64_t tz_floor_div(int64_t a, int64_t b)
{
	int64_t q = a / b;
	return (a % b != 0 && a < 0) ? q - 1 : q;
}

#endif
