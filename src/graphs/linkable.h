/*
 * Linkable: the dynamically-typed value every port holds (01 §2.1), with its
 * converters, stringification and parsing.
 *
 * Naming: functions are prefixed linkable_, the Metric category enum metric_ /
 * METRIC_. The type names Linkable and Metric follow the spec.
 *
 * Ownership: a Linkable holding a string or a Metric owns a reference to an
 * immutable, reference-counted string (the string value, or the Metric's unit).
 * Copying a value only increments the count, so passing a value along a link or
 * through the work queue never allocates; only creating a new string does.
 * Rules:
 *  - A zero-initialized Linkable is unset and owns nothing.
 *  - linkable_from_string/linkable_from_metric/linkable_parse create a value
 *    owning a new reference; linkable_copy/linkable_assign share one.
 *  - Release every value with linkable_clear. Never copy a Linkable with plain
 *    struct assignment unless the source is then abandoned (a move).
 *  - Strings are immutable; the char pointers (as.s, as.metric.unit) stay valid
 *    while any value referring to them lives. Strings are NUL-terminated.
 * The engine is single-threaded (01 §3.2), so the count is not atomic. The
 * empty string "" is a static string and never allocates.
 *
 * Number text is parsed and formatted with the C library in the "C" locale; the
 * engine never calls setlocale.
 */
#ifndef HAKKAS_GRAPHS_LINKABLE_H
#define HAKKAS_GRAPHS_LINKABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Metric categories (01 §2.1). Unit tables are not yet defined (00-README open
 * question 9), so parsed metrics are METRIC_UNCLASSIFIED. */
enum metric_category {
	METRIC_UNCLASSIFIED = 0,
	METRIC_ACCELERATION,
	METRIC_ANGLE,
	METRIC_AREA,
	METRIC_CURRENT,
	METRIC_DENSITY,
	METRIC_DISTANCE,
	METRIC_ENERGY,
	METRIC_FLOW,
	METRIC_FORCE,
	METRIC_FREQUENCY,
	METRIC_GRAVITY,
	METRIC_HUMIDITY,
	METRIC_MASS,
	METRIC_PERIODICITY,
	METRIC_POWER,
	METRIC_PRESSURE,
	METRIC_RATIO,
	METRIC_RESISTANCE,
	METRIC_TEMPERATURE,
	METRIC_VELOCITY,
	METRIC_VOLTAGE,
	METRIC_VOLUME,
};

/* A physical-unit-tagged value (01 §2.1, provisional). unit is owned as
 * described above and is never NULL in a Linkable. */
typedef struct {
	double value;
	const char *unit;
	enum metric_category category;
} Metric;

enum linkable_type {
	LINKABLE_UNSET = 0,
	LINKABLE_STRING,
	LINKABLE_I64,
	LINKABLE_F64,
	LINKABLE_BOOL,
	LINKABLE_METRIC,
};

typedef struct {
	enum linkable_type type;
	union {
		const char *s; /* LINKABLE_STRING, never NULL */
		int64_t i64;
		double f64;
		bool b;
		Metric metric;
	} as;
} Linkable;

/* Enough for any linkable_stringify of an f64, i64 or bool, including the NUL. */
#define LINKABLE_NUMBER_TEXT_MAX 32

/* Constructors. The scalar ones own nothing. */
Linkable linkable_from_i64(int64_t v);
Linkable linkable_from_f64(double v);
Linkable linkable_from_bool(bool v);

/* Copy len bytes of s into a new string value. Returns false, leaving *out
 * unset, if memory runs out. *out must not hold a value (it is overwritten). */
bool linkable_from_string(Linkable *out, const char *s, size_t len);

/* A Metric value; the unit (unit_len bytes) is copied. Same contract as
 * linkable_from_string. */
bool linkable_from_metric(Linkable *out, double value, const char *unit, size_t unit_len,
			  enum metric_category category);

/* Release v's reference, if any, and make it unset. */
void linkable_clear(Linkable *v);

/* Return a new reference to v's value. */
Linkable linkable_copy(const Linkable *v);

/* Replace *dst with a new reference to *src (self-assignment is safe). */
void linkable_assign(Linkable *dst, const Linkable *src);

/* Length in bytes of a string returned in as.s or as.metric.unit. */
size_t linkable_strlen(const char *s);

/* Converters (01 §2.1). Doubles (f64, Metric magnitude, parsed string) convert
 * to integers saturating: NaN → 0, values beyond the range → the nearest bound,
 * otherwise truncated toward zero. Strings are parsed strictly (see below);
 * anything else gives 0. */
double linkable_to_f64(const Linkable *v);
int64_t linkable_to_i64(const Linkable *v);
uint64_t linkable_to_u64(const Linkable *v);
bool linkable_to_bool(const Linkable *v);

/* default_value(typename): "bool" → false, "number" → 0.0, "string" → "",
 * anything else (including NULL) → 0.0. Never allocates. */
Linkable linkable_default_value(const char *type_name);

/* Strict parsers for a whole NUL-terminated string: no surrounding whitespace,
 * no trailing characters. On failure they return false and leave *out alone.
 *  f64: [+-]? (digits [. digits*] | . digits) [(e|E) [+-]? digits], or NaN,
 *       Infinity, +Infinity, -Infinity (exactly these spellings). Overflowing
 *       decimal text gives ±Infinity, as strtod does.
 *  i64: [+-]? digits, within INT64_MIN..INT64_MAX.
 *  u64: [+]? digits, within 0..UINT64_MAX. */
bool linkable_parse_f64(const char *s, double *out);
bool linkable_parse_i64(const char *s, int64_t *out);
bool linkable_parse_u64(const char *s, uint64_t *out);

/* Parse text according to a port's declared type (02 §3.4, initial value
 * encoding):
 *  "string": the text as-is.
 *  "bool":   to_bool, i.e. true only for exactly "true".
 *  "number" (and any other type name, like default_value): an f64 if the whole
 *            text is a number; else a Metric (METRIC_UNCLASSIFIED) if it splits
 *            on its first space into a number and a non-empty unit; else 0.0.
 * Returns false, leaving *out unset, only if memory runs out. *out must not
 * hold a value. */
bool linkable_parse(Linkable *out, const char *type_name, const char *text);

/* Write v's text form into buf with snprintf semantics: at most size bytes,
 * always NUL-terminated if size > 0; returns the full length (excluding the
 * NUL) so a caller can size a buffer. Forms:
 *  string: as-is; i64: decimal; bool: "true"/"false";
 *  f64: the shortest of printf "%.15g", "%.16g", "%.17g" that parses back to
 *       the same double, so every finite value round-trips; NaN, Infinity,
 *       -Infinity for the non-finite values;
 *  Metric: "<value as f64> <unit>";
 *  unset: "<not stringable>". */
size_t linkable_stringify(const Linkable *v, char *buf, size_t size);

#endif
