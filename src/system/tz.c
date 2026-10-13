/*
 * Local time from a POSIX TZ string. See system/tz.h.
 */
#include "system/tz.h"

#include <stddef.h>
#include <string.h>

#define DAY_S 86400

const TimeZone tz_utc = {.std_offset_s = 0, .dst_offset_s = 0, .has_dst = false};

/* ---- Calendar arithmetic ------------------------------------------------------ */

/* The algorithms of Howard Hinnant's "chrono-Compatible Low-Level Date
 * Algorithms": eras of 400 years (146097 days), years starting on March 1 so
 * the leap day is the last day of the year. */

bool tz_leap_year(int64_t year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int tz_month_days(int64_t year, int month)
{
	static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (month < 1 || month > 12)
		return 0;
	return month == 2 && tz_leap_year(year) ? 29 : days[month - 1];
}

int64_t tz_days_from_civil(int64_t year, int month, int day)
{
	int64_t y = year - (month <= 2);
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

void tz_civil_from_days(int64_t days, int64_t *year, int *month, int *day)
{
	int64_t z = days + 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	int m = (int)(mp < 10 ? mp + 3 : mp - 9);
	*day = (int)(doy - (153 * mp + 2) / 5 + 1);
	*month = m;
	*year = yoe + era * 400 + (m <= 2);
}

int tz_weekday(int64_t days)
{
	/* 1970-01-01 was a Thursday (4). */
	return (int)(days + 3 - tz_floor_div(days + 3, 7) * 7) + 1;
}

void tz_iso_week(int64_t days, int64_t *iso_year, int *week)
{
	/* The week belongs to the year of its Thursday. */
	int64_t thursday = days - tz_weekday(days) + 4;
	int64_t y;
	int m, d;
	tz_civil_from_days(thursday, &y, &m, &d);
	*iso_year = y;
	*week = (int)((thursday - tz_days_from_civil(y, 1, 1)) / 7 + 1);
}

/* ---- Parsing ------------------------------------------------------------------ */

static bool is_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

/* A zone name: letters only, or <> quoted letters, digits, '+' and '-'; 3 to
 * TZ_NAME_MAX characters. Returns the end, or NULL. */
static const char *parse_name(const char *p)
{
	const char *start;
	size_t len;
	if (*p == '<') {
		start = ++p;
		while (is_alpha(*p) || is_digit(*p) || *p == '+' || *p == '-')
			p++;
		if (*p != '>')
			return NULL;
		len = (size_t)(p - start);
		p++;
	} else {
		start = p;
		while (is_alpha(*p))
			p++;
		len = (size_t)(p - start);
	}
	return len >= 3 && len <= TZ_NAME_MAX ? p : NULL;
}

/* 1 to max_digits decimal digits into *out. Returns the end, or NULL. */
static const char *parse_num(const char *p, int max_digits, int *out)
{
	int v = 0, n = 0;
	while (is_digit(*p) && n < max_digits) {
		v = v * 10 + (*p++ - '0');
		n++;
	}
	if (n == 0 || is_digit(*p))
		return NULL;
	*out = v;
	return p;
}

/* [+-]hh[:mm[:ss]] with hh 0..max_h (1 to h_digits digits), mm and ss 0-59
 * (1 or 2 digits), into *out seconds. Returns the end, or NULL. */
static const char *parse_time(const char *p, int max_h, int h_digits, int32_t *out)
{
	int sign = 1, h, m = 0, s = 0;
	if (*p == '+' || *p == '-')
		sign = *p++ == '-' ? -1 : 1;
	p = parse_num(p, h_digits, &h);
	if (p == NULL || h > max_h)
		return NULL;
	if (*p == ':') {
		p = parse_num(p + 1, 2, &m);
		if (p == NULL || m > 59)
			return NULL;
		if (*p == ':') {
			p = parse_num(p + 1, 2, &s);
			if (p == NULL || s > 59)
				return NULL;
		}
	}
	*out = (int32_t)(sign * (h * 3600 + m * 60 + s));
	return p;
}

/* A transition rule (Jn, n or Mm.w.d) with an optional /time. Returns the
 * end, or NULL. */
static const char *parse_rule(const char *p, TzRule *r)
{
	int v, w, d;
	if (*p == 'J') {
		p = parse_num(p + 1, 3, &v);
		if (p == NULL || v < 1 || v > 365)
			return NULL;
		*r = (TzRule){.kind = TZ_RULE_JULIAN, .day = (int16_t)v};
	} else if (*p == 'M') {
		p = parse_num(p + 1, 2, &v);
		if (p == NULL || v < 1 || v > 12 || *p != '.')
			return NULL;
		p = parse_num(p + 1, 1, &w);
		if (p == NULL || w < 1 || w > 5 || *p != '.')
			return NULL;
		p = parse_num(p + 1, 1, &d);
		if (p == NULL || d > 6)
			return NULL;
		*r = (TzRule){.kind = TZ_RULE_MONTH_WEEK_DAY,
			      .day = (int16_t)d,
			      .month = (uint8_t)v,
			      .week = (uint8_t)w};
	} else {
		p = parse_num(p, 3, &v);
		if (p == NULL || v > 365)
			return NULL;
		*r = (TzRule){.kind = TZ_RULE_DAY_OF_YEAR, .day = (int16_t)v};
	}
	r->time_s = 2 * 3600;
	if (*p == '/')
		p = parse_time(p + 1, 167, 3, &r->time_s);
	return p;
}

bool tz_parse(const char *text, TimeZone *out)
{
	if (text == NULL || strlen(text) > TZ_TEXT_MAX)
		return false;
	TimeZone tz = {0};
	int32_t off;
	const char *p = parse_name(text);
	if (p == NULL || (p = parse_time(p, 24, 2, &off)) == NULL)
		return false;
	tz.std_offset_s = -off; /* the string's offset is UTC minus local */
	tz.dst_offset_s = tz.std_offset_s;
	if (*p != '\0') {
		if ((p = parse_name(p)) == NULL)
			return false;
		tz.has_dst = true;
		tz.dst_offset_s = tz.std_offset_s + 3600;
		if (*p != ',' && *p != '\0') {
			if ((p = parse_time(p, 24, 2, &off)) == NULL)
				return false;
			tz.dst_offset_s = -off;
		}
		/* The rules are required (see tz.h). */
		if (*p != ',' || (p = parse_rule(p + 1, &tz.start)) == NULL || *p != ',' ||
		    (p = parse_rule(p + 1, &tz.end)) == NULL || *p != '\0')
			return false;
	}
	*out = tz;
	return true;
}

/* ---- Offsets ------------------------------------------------------------------- */

/* The local time (seconds) of rule r's transition in year. */
static int64_t rule_local(const TzRule *r, int64_t year)
{
	int64_t jan1 = tz_days_from_civil(year, 1, 1);
	int64_t days;
	switch (r->kind) {
	case TZ_RULE_JULIAN:
		/* February 29 is not counted: from March 1 (J60) on, a leap
		 * year is one day further. */
		days = jan1 + r->day - 1 + (tz_leap_year(year) && r->day >= 60);
		break;
	case TZ_RULE_DAY_OF_YEAR:
		days = jan1 + r->day;
		break;
	default: {
		int64_t first = tz_days_from_civil(year, r->month, 1);
		int first_wd = tz_weekday(first) % 7; /* 0 = Sunday, as in the rule */
		days = first + (r->day - first_wd + 7) % 7 + (int64_t)(r->week - 1) * 7;
		/* Week 5 is the last such day of the month. */
		while (days >= first + tz_month_days(year, r->month))
			days -= 7;
		break;
	}
	}
	return days * DAY_S + r->time_s;
}

typedef struct {
	int64_t at;  /* UTC */
	bool to_dst; /* a start (to daylight-saving time), else an end */
} Transition;

/* The transitions of the years around utc_s, sorted by time (an end before a
 * start at the same instant). Returns the count. */
static int transitions(const TimeZone *tz, int64_t utc_s, Transition out[8])
{
	int64_t year;
	int m, d;
	tz_civil_from_days(tz_floor_div(utc_s + tz->std_offset_s, DAY_S), &year, &m, &d);
	int n = 0;
	for (int64_t y = year - 1; y <= year + 2; y++) {
		out[n++] = (Transition){rule_local(&tz->start, y) - tz->std_offset_s, true};
		out[n++] = (Transition){rule_local(&tz->end, y) - tz->dst_offset_s, false};
	}
	for (int i = 1; i < n; i++) {
		Transition t = out[i];
		int j = i;
		while (j > 0 && (out[j - 1].at > t.at ||
				 (out[j - 1].at == t.at && out[j - 1].to_dst && !t.to_dst))) {
			out[j] = out[j - 1];
			j--;
		}
		out[j] = t;
	}
	return n;
}

int32_t tz_offset(const TimeZone *tz, int64_t utc_s, bool *dst, int64_t *next_change)
{
	bool in_dst = false;
	int64_t next = TZ_NEVER;
	if (tz->has_dst) {
		Transition tr[8];
		int n = transitions(tz, utc_s, tr);
		/* Before the first transition, the opposite of what it sets. */
		in_dst = !tr[0].to_dst;
		for (int i = 0; i < n; i++) {
			if (tr[i].at <= utc_s) {
				in_dst = tr[i].to_dst;
			} else {
				next = tr[i].at;
				break;
			}
		}
	}
	if (dst != NULL)
		*dst = in_dst;
	if (next_change != NULL)
		*next_change = next;
	return in_dst ? tz->dst_offset_s : tz->std_offset_s;
}

void tz_local(const TimeZone *tz, int64_t utc_s, TzLocal *out)
{
	bool dst;
	int32_t off = tz_offset(tz, utc_s, &dst, NULL);
	int64_t local = utc_s + off;
	int64_t days = tz_floor_div(local, DAY_S);
	int64_t sod = local - days * DAY_S;
	tz_civil_from_days(days, &out->year, &out->month, &out->day);
	out->hour = (int)(sod / 3600);
	out->minute = (int)(sod / 60 % 60);
	out->second = (int)(sod % 60);
	out->weekday = tz_weekday(days);
	out->yday = (int)(days - tz_days_from_civil(out->year, 1, 1) + 1);
	tz_iso_week(days, &out->iso_year, &out->iso_week);
	out->dst = dst;
	out->offset_s = off;
	out->days = days;
}

int64_t tz_next_reach(const TimeZone *tz, int64_t after_s, int64_t local_s)
{
	int32_t min_off = tz->std_offset_s < tz->dst_offset_s ? tz->std_offset_s
							      : tz->dst_offset_s;
	int64_t lower = after_s + 1; /* the first instant considered */
	for (;;) {
		/* The offset is off from lower until next. */
		int64_t next;
		int32_t off = tz_offset(tz, lower, NULL, &next);
		int64_t at = local_s - off;
		if (at >= lower && at < next)
			return at;
		if (next == TZ_NEVER)
			return TZ_NEVER;
		/* Local time jumps forward over local_s at next: reached there. */
		int32_t off2 = tz_offset(tz, next, NULL, NULL);
		if (local_s >= next + off && local_s < next + off2)
			return next;
		/* From next on, local time is never below next + min_off. */
		if (local_s < next + min_off)
			return TZ_NEVER;
		lower = next;
	}
}
