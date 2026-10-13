/*
 * The calendar primitives' setting strings (03 §6.2): selector strings
 * ("1-5", "6,7", "1-3,5") and time-of-day ranges ("12:30-15:45"). The block
 * types themselves are blocktypes/calendar.c, registered with the built-ins.
 *
 * Naming: functions are prefixed calendar_.
 *
 * Selector strings (decided, plan task 24; 03 §6.2): a comma-separated list
 * of items, each a value ("3") or a range of values ("1-5"), values in
 * decimal digits only (no sign; leading zeros allowed). Spaces and tabs are
 * allowed around values, '-' and ','. A range whose first value is greater
 * than its last wraps around: "6-1" over 1-7 is 6, 7, 1; "11-2" over 1-12 is
 * November to February. Repeated or overlapping items are fine. The
 * selector is invalid — and the block's output false — if it is empty (or
 * only whitespace), has an empty item ("1,,3", "1,"), a value outside the
 * block's range (0 or 8 for weekdays), or any other character.
 *
 * Time-of-day ranges (decided, plan task 24; 03 §6.2): "HH:MM-HH:MM", the
 * hour one or two digits (0-23), the minute two digits (00-59), spaces and
 * tabs allowed around the times and the '-'. The start is included and the
 * end excluded; a range whose end is before its start wraps past midnight.
 * The end may be "24:00", the end of the day ("00:00-24:00" is all day). An
 * equal start and end is an empty range: never true.
 *
 * Pure: no allocation.
 */
#ifndef HAKKAS_BLOCKTYPES_CALENDAR_H
#define HAKKAS_BLOCKTYPES_CALENDAR_H

#include <stdbool.h>
#include <stdint.h>

/* The largest selector value of any primitive (timing/DayOfYear: 366). */
#define CALENDAR_SELECTOR_MAX 366

/* A parsed selector: bit v is set when value v is selected. */
typedef struct {
	uint64_t bits[(CALENDAR_SELECTOR_MAX + 64) / 64];
} CalendarSelector;

/* Parse the selector text for values min..max (1 <= min <= max <=
 * CALENDAR_SELECTOR_MAX) into *out. False if text is invalid (see above). */
bool calendar_selector_parse(const char *text, int min, int max, CalendarSelector *out);

/* Whether value v is selected. */
static inline bool calendar_selector_has(const CalendarSelector *s, int v)
{
	return v >= 0 && v <= CALENDAR_SELECTOR_MAX &&
	       (s->bits[v / 64] >> (v % 64) & 1u) != 0;
}

/* Parse the time-of-day range text into *start and *end, minutes of the day
 * (start 0-1439, end 0-1440). False if text is invalid (see above). */
bool calendar_range_parse(const char *text, int *start, int *end);

#endif
