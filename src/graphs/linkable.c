#include "graphs/linkable.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A heap string: this header, then len chars and a NUL. Values point at the chars. */
struct lk_str {
	size_t refs;
	size_t len;
};

/* The empty string is static: retain/release ignore it. */
static const char empty_str[1] = "";

static struct lk_str *str_header(const char *s)
{
	return (struct lk_str *)(void *)(s - sizeof(struct lk_str));
}

static const char *str_new(const char *s, size_t len)
{
	if (len == 0)
		return empty_str;
	if (len > SIZE_MAX - sizeof(struct lk_str) - 1)
		return NULL;
	struct lk_str *h = malloc(sizeof(*h) + len + 1);
	if (h == NULL)
		return NULL;
	h->refs = 1;
	h->len = len;
	char *chars = (char *)(h + 1);
	memcpy(chars, s, len);
	chars[len] = '\0';
	return chars;
}

static void str_retain(const char *s)
{
	if (s != empty_str)
		str_header(s)->refs++;
}

static void str_release(const char *s)
{
	if (s == empty_str)
		return;
	struct lk_str *h = str_header(s);
	if (--h->refs == 0)
		free(h);
}

size_t linkable_strlen(const char *s)
{
	return s == empty_str ? 0 : str_header(s)->len;
}

Linkable linkable_from_i64(int64_t v)
{
	return (Linkable){.type = LINKABLE_I64, .as.i64 = v};
}

Linkable linkable_from_f64(double v)
{
	return (Linkable){.type = LINKABLE_F64, .as.f64 = v};
}

Linkable linkable_from_bool(bool v)
{
	return (Linkable){.type = LINKABLE_BOOL, .as.b = v};
}

bool linkable_from_string(Linkable *out, const char *s, size_t len)
{
	const char *str = str_new(s, len);
	if (str == NULL) {
		*out = (Linkable){0};
		return false;
	}
	*out = (Linkable){.type = LINKABLE_STRING, .as.s = str};
	return true;
}

bool linkable_from_metric(Linkable *out, double value, const char *unit, size_t unit_len,
			  enum metric_category category)
{
	const char *str = str_new(unit, unit_len);
	if (str == NULL) {
		*out = (Linkable){0};
		return false;
	}
	*out = (Linkable){.type = LINKABLE_METRIC,
			  .as.metric = {.value = value, .unit = str, .category = category}};
	return true;
}

/* The string v owns a reference to, or NULL. */
static const char *owned_str(const Linkable *v)
{
	switch (v->type) {
	case LINKABLE_STRING:
		return v->as.s;
	case LINKABLE_METRIC:
		return v->as.metric.unit;
	default:
		return NULL;
	}
}

void linkable_clear(Linkable *v)
{
	const char *s = owned_str(v);
	if (s != NULL)
		str_release(s);
	*v = (Linkable){0};
}

Linkable linkable_copy(const Linkable *v)
{
	const char *s = owned_str(v);
	if (s != NULL)
		str_retain(s);
	return *v;
}

void linkable_assign(Linkable *dst, const Linkable *src)
{
	Linkable copy = linkable_copy(src);
	linkable_clear(dst);
	*dst = copy;
}

/* ---- Parsing ---- */

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

/* Length of the run of digits at s. */
static size_t digits(const char *s)
{
	size_t n = 0;
	while (is_digit(s[n]))
		n++;
	return n;
}

/*
 * Parse the f64 grammar (see linkable.h) spanning exactly s[0..len). s[len]
 * must be a character that cannot continue a number ('\0' or ' '), so strtod
 * stops exactly there.
 */
static bool parse_f64_span(const char *s, size_t len, double *out)
{
	size_t i = 0;
	if (len == 3 && memcmp(s, "NaN", 3) == 0) {
		*out = NAN;
		return true;
	}
	if (s[i] == '+' || s[i] == '-')
		i++;
	if (len - i == 8 && memcmp(s + i, "Infinity", 8) == 0) {
		*out = s[0] == '-' ? -INFINITY : INFINITY;
		return true;
	}
	size_t int_digits = digits(s + i);
	i += int_digits;
	size_t frac_digits = 0;
	if (i < len && s[i] == '.') {
		i++;
		frac_digits = digits(s + i);
		i += frac_digits;
	}
	if (int_digits == 0 && frac_digits == 0)
		return false;
	if (i < len && (s[i] == 'e' || s[i] == 'E')) {
		i++;
		if (s[i] == '+' || s[i] == '-')
			i++;
		size_t exp_digits = digits(s + i);
		if (exp_digits == 0)
			return false;
		i += exp_digits;
	}
	if (i != len)
		return false;
	char *end;
	double v = strtod(s, &end);
	if (end != s + len)
		return false;
	*out = v;
	return true;
}

bool linkable_parse_f64(const char *s, double *out)
{
	return parse_f64_span(s, strlen(s), out);
}

/* Unsigned decimal digits making up all of s, at most max. */
static bool parse_digits(const char *s, uint64_t max, uint64_t *out)
{
	if (!is_digit(*s))
		return false;
	uint64_t v = 0;
	for (; is_digit(*s); s++) {
		unsigned d = (unsigned)(*s - '0');
		if (v > (max - d) / 10)
			return false;
		v = v * 10 + d;
	}
	if (*s != '\0')
		return false;
	*out = v;
	return true;
}

bool linkable_parse_i64(const char *s, int64_t *out)
{
	bool neg = *s == '-';
	if (*s == '+' || *s == '-')
		s++;
	uint64_t mag;
	if (!parse_digits(s, neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX, &mag))
		return false;
	/* -(INT64_MAX + 1) is not representable as a positive int64_t. */
	*out = neg ? (mag == 0 ? 0 : -(int64_t)(mag - 1) - 1) : (int64_t)mag;
	return true;
}

bool linkable_parse_u64(const char *s, uint64_t *out)
{
	if (*s == '+')
		s++;
	return parse_digits(s, UINT64_MAX, out);
}

/* ---- Converters (01 §2.1) ---- */

/* Saturating double → integer conversions (00-README open question 8, decided
 * in 01 §2.1): a plain cast of NaN or an out-of-range value is undefined. 0x1p63
 * and 0x1p64 are exact doubles; -0x1p63 is INT64_MIN, itself in range. */
static int64_t f64_to_i64(double v)
{
	if (isnan(v))
		return 0;
	if (v >= 0x1p63)
		return INT64_MAX;
	if (v < -0x1p63)
		return INT64_MIN;
	return (int64_t)v;
}

static uint64_t f64_to_u64(double v)
{
	if (isnan(v) || v <= -1.0)
		return 0;
	if (v >= 0x1p64)
		return UINT64_MAX;
	return (uint64_t)v;
}

double linkable_to_f64(const Linkable *v)
{
	double d;
	switch (v->type) {
	case LINKABLE_F64:
		return v->as.f64;
	case LINKABLE_I64:
		return (double)v->as.i64;
	case LINKABLE_METRIC:
		return v->as.metric.value;
	case LINKABLE_BOOL:
		return v->as.b ? 1.0 : 0.0;
	case LINKABLE_STRING:
		return linkable_parse_f64(v->as.s, &d) ? d : 0.0;
	case LINKABLE_UNSET:
		break;
	}
	return 0.0;
}

int64_t linkable_to_i64(const Linkable *v)
{
	int64_t i;
	switch (v->type) {
	case LINKABLE_F64:
		return f64_to_i64(v->as.f64);
	case LINKABLE_I64:
		return v->as.i64;
	case LINKABLE_METRIC:
		return f64_to_i64(v->as.metric.value);
	case LINKABLE_BOOL:
		return v->as.b ? 1 : 0;
	case LINKABLE_STRING:
		return linkable_parse_i64(v->as.s, &i) ? i : 0;
	case LINKABLE_UNSET:
		break;
	}
	return 0;
}

uint64_t linkable_to_u64(const Linkable *v)
{
	uint64_t u;
	switch (v->type) {
	case LINKABLE_F64:
		return f64_to_u64(v->as.f64);
	case LINKABLE_I64:
		return (uint64_t)v->as.i64;
	case LINKABLE_METRIC:
		return f64_to_u64(v->as.metric.value);
	case LINKABLE_BOOL:
		return v->as.b ? 1 : 0;
	case LINKABLE_STRING:
		return linkable_parse_u64(v->as.s, &u) ? u : 0;
	case LINKABLE_UNSET:
		break;
	}
	return 0;
}

static bool str_is_true(const char *s, size_t len)
{
	return len == 4 && memcmp(s, "true", 4) == 0;
}

bool linkable_to_bool(const Linkable *v)
{
	switch (v->type) {
	case LINKABLE_BOOL:
		return v->as.b;
	case LINKABLE_F64:
		return v->as.f64 != 0.0;
	case LINKABLE_METRIC:
		return v->as.metric.value != 0.0;
	case LINKABLE_I64:
		return v->as.i64 != 0;
	case LINKABLE_STRING:
		return str_is_true(v->as.s, linkable_strlen(v->as.s));
	case LINKABLE_UNSET:
		break;
	}
	return false;
}

Linkable linkable_default_value(const char *type_name)
{
	if (type_name != NULL && strcmp(type_name, "bool") == 0)
		return linkable_from_bool(false);
	if (type_name != NULL && strcmp(type_name, "string") == 0)
		return (Linkable){.type = LINKABLE_STRING, .as.s = empty_str};
	return linkable_from_f64(0.0);
}

bool linkable_parse(Linkable *out, const char *type_name, const char *text)
{
	size_t len = strlen(text);
	if (type_name != NULL && strcmp(type_name, "string") == 0)
		return linkable_from_string(out, text, len);
	if (type_name != NULL && strcmp(type_name, "bool") == 0) {
		*out = linkable_from_bool(str_is_true(text, len));
		return true;
	}

	double d;
	if (parse_f64_span(text, len, &d)) {
		*out = linkable_from_f64(d);
		return true;
	}
	const char *space = strchr(text, ' ');
	if (space != NULL && space[1] != '\0' && parse_f64_span(text, (size_t)(space - text), &d)) {
		const char *unit = space + 1;
		return linkable_from_metric(out, d, unit, len - (size_t)(unit - text),
					    METRIC_UNCLASSIFIED);
	}
	*out = linkable_from_f64(0.0);
	return true;
}

/* ---- Stringify ---- */

/* Text form of a double into buf (LINKABLE_NUMBER_TEXT_MAX bytes); returns its length. */
static size_t f64_text(double v, char *buf)
{
	if (isnan(v))
		return (size_t)snprintf(buf, LINKABLE_NUMBER_TEXT_MAX, "NaN");
	if (isinf(v))
		return (size_t)snprintf(buf, LINKABLE_NUMBER_TEXT_MAX, "%s",
					v < 0 ? "-Infinity" : "Infinity");
	int n = 0;
	for (int prec = 15; prec <= 17; prec++) {
		n = snprintf(buf, LINKABLE_NUMBER_TEXT_MAX, "%.*g", prec, v);
		if (strtod(buf, NULL) == v)
			break;
	}
	return (size_t)n;
}

/* Append len bytes of s at offset pos of buf (size bytes), snprintf-style. */
static size_t put(char *buf, size_t size, size_t pos, const char *s, size_t len)
{
	if (pos < size) {
		size_t room = size - pos - 1;
		size_t n = len < room ? len : room;
		memcpy(buf + pos, s, n);
		buf[pos + n] = '\0';
	}
	return pos + len;
}

size_t linkable_stringify(const Linkable *v, char *buf, size_t size)
{
	char num[LINKABLE_NUMBER_TEXT_MAX];
	size_t n;

	if (size > 0)
		buf[0] = '\0';
	switch (v->type) {
	case LINKABLE_STRING:
		return put(buf, size, 0, v->as.s, linkable_strlen(v->as.s));
	case LINKABLE_I64:
		n = (size_t)snprintf(num, sizeof(num), "%lld", (long long)v->as.i64);
		return put(buf, size, 0, num, n);
	case LINKABLE_F64:
		n = f64_text(v->as.f64, num);
		return put(buf, size, 0, num, n);
	case LINKABLE_BOOL:
		return v->as.b ? put(buf, size, 0, "true", 4) : put(buf, size, 0, "false", 5);
	case LINKABLE_METRIC:
		n = f64_text(v->as.metric.value, num);
		n = put(buf, size, 0, num, n);
		n = put(buf, size, n, " ", 1);
		return put(buf, size, n, v->as.metric.unit, linkable_strlen(v->as.metric.unit));
	case LINKABLE_UNSET:
		break;
	}
	return put(buf, size, 0, "<not stringable>", 16);
}
