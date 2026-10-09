#include "test.h"

#include <math.h>
#include <stdint.h>

#include "graphs/linkable.h"

static Linkable str(const char *s)
{
	Linkable v;
	CHECK(linkable_from_string(&v, s, strlen(s)));
	return v;
}

static Linkable metric(double value, const char *unit)
{
	Linkable v;
	CHECK(linkable_from_metric(&v, value, unit, strlen(unit), METRIC_VOLTAGE));
	return v;
}

/* Stringify into a static buffer for CHECK_STREQ. */
static const char *text(const Linkable *v)
{
	static char buf[128];
	size_t n = linkable_stringify(v, buf, sizeof(buf));
	CHECK(n == strlen(buf));
	return buf;
}

static void test_unset_is_zero_initialized(void)
{
	Linkable v = {0};
	CHECK(v.type == LINKABLE_UNSET);
	CHECK(linkable_to_f64(&v) == 0.0);
	CHECK(linkable_to_i64(&v) == 0);
	CHECK(linkable_to_u64(&v) == 0);
	CHECK(linkable_to_bool(&v) == false);
	linkable_clear(&v);
}

static void test_to_f64(void)
{
	Linkable v = linkable_from_f64(2.5);
	CHECK(linkable_to_f64(&v) == 2.5);
	v = linkable_from_i64(-7);
	CHECK(linkable_to_f64(&v) == -7.0);
	v = linkable_from_bool(true);
	CHECK(linkable_to_f64(&v) == 1.0);
	v = linkable_from_bool(false);
	CHECK(linkable_to_f64(&v) == 0.0);

	v = metric(3.25, "V");
	CHECK(linkable_to_f64(&v) == 3.25);
	linkable_clear(&v);

	v = str("-1.5e3");
	CHECK(linkable_to_f64(&v) == -1500.0);
	linkable_clear(&v);
	v = str("abc");
	CHECK(linkable_to_f64(&v) == 0.0);
	linkable_clear(&v);
	v = str("12abc");
	CHECK(linkable_to_f64(&v) == 0.0);
	linkable_clear(&v);
	v = str("NaN");
	CHECK(isnan(linkable_to_f64(&v)));
	linkable_clear(&v);
	v = str("-Infinity");
	CHECK(linkable_to_f64(&v) == -INFINITY);
	linkable_clear(&v);
}

static void test_to_i64(void)
{
	Linkable v = linkable_from_f64(2.9);
	CHECK(linkable_to_i64(&v) == 2);
	v = linkable_from_f64(-2.9);
	CHECK(linkable_to_i64(&v) == -2);
	v = linkable_from_i64(INT64_MIN);
	CHECK(linkable_to_i64(&v) == INT64_MIN);
	v = linkable_from_bool(true);
	CHECK(linkable_to_i64(&v) == 1);
	v = linkable_from_bool(false);
	CHECK(linkable_to_i64(&v) == 0);

	v = metric(-4.7, "V");
	CHECK(linkable_to_i64(&v) == -4);
	linkable_clear(&v);

	v = str("-9223372036854775808");
	CHECK(linkable_to_i64(&v) == INT64_MIN);
	linkable_clear(&v);
	v = str("+42");
	CHECK(linkable_to_i64(&v) == 42);
	linkable_clear(&v);
	v = str("12.5"); /* not an integer */
	CHECK(linkable_to_i64(&v) == 0);
	linkable_clear(&v);
	v = str("9223372036854775808"); /* out of range */
	CHECK(linkable_to_i64(&v) == 0);
	linkable_clear(&v);
}

static void test_to_u64(void)
{
	Linkable v = linkable_from_f64(7.9);
	CHECK(linkable_to_u64(&v) == 7);
	v = linkable_from_i64(-1); /* modular, like the C cast */
	CHECK(linkable_to_u64(&v) == UINT64_MAX);
	v = linkable_from_i64(5);
	CHECK(linkable_to_u64(&v) == 5);
	v = linkable_from_bool(true);
	CHECK(linkable_to_u64(&v) == 1);

	v = metric(9.99, "V");
	CHECK(linkable_to_u64(&v) == 9);
	linkable_clear(&v);

	v = str("18446744073709551615");
	CHECK(linkable_to_u64(&v) == UINT64_MAX);
	linkable_clear(&v);
	v = str("18446744073709551616");
	CHECK(linkable_to_u64(&v) == 0);
	linkable_clear(&v);
	v = str("-1");
	CHECK(linkable_to_u64(&v) == 0);
	linkable_clear(&v);
}

static void test_to_bool(void)
{
	Linkable v = linkable_from_bool(true);
	CHECK(linkable_to_bool(&v));
	v = linkable_from_f64(0.0);
	CHECK(!linkable_to_bool(&v));
	v = linkable_from_f64(-0.0);
	CHECK(!linkable_to_bool(&v));
	v = linkable_from_f64(0.1);
	CHECK(linkable_to_bool(&v));
	v = linkable_from_f64(NAN); /* NaN != 0.0 */
	CHECK(linkable_to_bool(&v));
	v = linkable_from_i64(0);
	CHECK(!linkable_to_bool(&v));
	v = linkable_from_i64(-3);
	CHECK(linkable_to_bool(&v));

	v = metric(0.0, "V");
	CHECK(!linkable_to_bool(&v));
	linkable_clear(&v);
	v = metric(1.0, "V");
	CHECK(linkable_to_bool(&v));
	linkable_clear(&v);

	v = str("true");
	CHECK(linkable_to_bool(&v));
	linkable_clear(&v);
	const char *falsy[] = {"True", "TRUE", "1", "yes", "true ", " true", "truex", ""};
	for (size_t i = 0; i < sizeof(falsy) / sizeof(falsy[0]); i++) {
		v = str(falsy[i]);
		CHECK(!linkable_to_bool(&v));
		linkable_clear(&v);
	}
}

/* 00-README open question 8: saturating, NaN → 0. */
static void test_double_to_integer_edge_cases(void)
{
	Linkable v = linkable_from_f64(NAN);
	CHECK(linkable_to_i64(&v) == 0);
	CHECK(linkable_to_u64(&v) == 0);
	v = linkable_from_f64(INFINITY);
	CHECK(linkable_to_i64(&v) == INT64_MAX);
	CHECK(linkable_to_u64(&v) == UINT64_MAX);
	v = linkable_from_f64(-INFINITY);
	CHECK(linkable_to_i64(&v) == INT64_MIN);
	CHECK(linkable_to_u64(&v) == 0);
	v = linkable_from_f64(1e300);
	CHECK(linkable_to_i64(&v) == INT64_MAX);
	CHECK(linkable_to_u64(&v) == UINT64_MAX);
	v = linkable_from_f64(-1e300);
	CHECK(linkable_to_i64(&v) == INT64_MIN);
	CHECK(linkable_to_u64(&v) == 0);

	/* The exact bounds. */
	v = linkable_from_f64(0x1p63);
	CHECK(linkable_to_i64(&v) == INT64_MAX);
	CHECK(linkable_to_u64(&v) == (uint64_t)1 << 63);
	v = linkable_from_f64(-0x1p63);
	CHECK(linkable_to_i64(&v) == INT64_MIN);
	v = linkable_from_f64(0x1p63 - 1024); /* largest double below 2^63 */
	CHECK(linkable_to_i64(&v) == INT64_MAX - 1023);
	v = linkable_from_f64(0x1p64);
	CHECK(linkable_to_u64(&v) == UINT64_MAX);
	v = linkable_from_f64(0x1p64 - 2048); /* largest double below 2^64 */
	CHECK(linkable_to_u64(&v) == UINT64_MAX - 2047);
	v = linkable_from_f64(-0.9);
	CHECK(linkable_to_u64(&v) == 0);
	v = linkable_from_f64(-1.0);
	CHECK(linkable_to_u64(&v) == 0);

	v = metric(NAN, "V");
	CHECK(linkable_to_i64(&v) == 0);
	linkable_clear(&v);
	v = metric(-INFINITY, "V");
	CHECK(linkable_to_i64(&v) == INT64_MIN);
	CHECK(linkable_to_u64(&v) == 0);
	linkable_clear(&v);
	v = str("1e30");
	CHECK(linkable_to_f64(&v) == 1e30);
	CHECK(linkable_to_i64(&v) == 0); /* strings parse as i64 only */
	linkable_clear(&v);
}

static void test_default_value(void)
{
	Linkable v = linkable_default_value("bool");
	CHECK(v.type == LINKABLE_BOOL && v.as.b == false);
	v = linkable_default_value("number");
	CHECK(v.type == LINKABLE_F64 && v.as.f64 == 0.0);
	v = linkable_default_value("string");
	CHECK(v.type == LINKABLE_STRING);
	CHECK_STREQ(v.as.s, "");
	CHECK(linkable_strlen(v.as.s) == 0);
	linkable_clear(&v);
	v = linkable_default_value("link");
	CHECK(v.type == LINKABLE_F64 && v.as.f64 == 0.0);
	v = linkable_default_value(NULL);
	CHECK(v.type == LINKABLE_F64 && v.as.f64 == 0.0);
}

static void test_parse_f64_grammar(void)
{
	struct {
		const char *s;
		double v;
	} ok[] = {
		{"0", 0.0},       {"-0", -0.0},     {"+5", 5.0},     {"1.", 1.0},
		{".5", 0.5},      {"-.5", -0.5},    {"1e3", 1000.0}, {"1E+3", 1000.0},
		{"2.5e-1", 0.25}, {"1e999", INFINITY}, {"Infinity", INFINITY},
		{"+Infinity", INFINITY}, {"-Infinity", -INFINITY},
	};
	for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
		double d = 42.0;
		CHECK(linkable_parse_f64(ok[i].s, &d));
		CHECK(d == ok[i].v);
	}
	double d = 0.0;
	CHECK(linkable_parse_f64("NaN", &d) && isnan(d));

	const char *bad[] = {"",     " 1",   "1 ",  "12abc", ".",     "e3",  "1e",   "1e+",
			     "+-1",  "0x10", "nan", "inf",  "-NaN",  "INF",   "1..2", "1.2.3",
			     "Infinityx", "--1"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		d = 42.0;
		CHECK(!linkable_parse_f64(bad[i], &d));
		CHECK(d == 42.0);
	}
}

static void test_parse_integers(void)
{
	int64_t i = 7;
	CHECK(linkable_parse_i64("9223372036854775807", &i) && i == INT64_MAX);
	CHECK(linkable_parse_i64("-0", &i) && i == 0);
	CHECK(linkable_parse_i64("007", &i) && i == 7);
	const char *bad_i[] = {"", "-", "+", " 1", "1 ", "12abc", "1.0", "1e3",
			       "-9223372036854775809", "99999999999999999999999"};
	for (size_t k = 0; k < sizeof(bad_i) / sizeof(bad_i[0]); k++) {
		i = 7;
		CHECK(!linkable_parse_i64(bad_i[k], &i));
		CHECK(i == 7);
	}

	uint64_t u = 7;
	CHECK(linkable_parse_u64("+0", &u) && u == 0);
	CHECK(linkable_parse_u64("18446744073709551615", &u) && u == UINT64_MAX);
	const char *bad_u[] = {"", "-0", "-1", "1x", "18446744073709551616"};
	for (size_t k = 0; k < sizeof(bad_u) / sizeof(bad_u[0]); k++) {
		u = 7;
		CHECK(!linkable_parse_u64(bad_u[k], &u));
		CHECK(u == 7);
	}
}

static void test_stringify(void)
{
	Linkable v = {0};
	CHECK_STREQ(text(&v), "<not stringable>");
	v = linkable_from_i64(INT64_MIN);
	CHECK_STREQ(text(&v), "-9223372036854775808");
	v = linkable_from_bool(true);
	CHECK_STREQ(text(&v), "true");
	v = linkable_from_bool(false);
	CHECK_STREQ(text(&v), "false");

	v = linkable_from_f64(0.1);
	CHECK_STREQ(text(&v), "0.1");
	v = linkable_from_f64(5.0);
	CHECK_STREQ(text(&v), "5");
	v = linkable_from_f64(-2.5);
	CHECK_STREQ(text(&v), "-2.5");
	v = linkable_from_f64(1e20);
	CHECK_STREQ(text(&v), "1e+20");
	v = linkable_from_f64(0.1 + 0.2); /* needs 17 digits */
	CHECK_STREQ(text(&v), "0.30000000000000004");
	v = linkable_from_f64(NAN);
	CHECK_STREQ(text(&v), "NaN");
	v = linkable_from_f64(INFINITY);
	CHECK_STREQ(text(&v), "Infinity");
	v = linkable_from_f64(-INFINITY);
	CHECK_STREQ(text(&v), "-Infinity");

	v = str("hello world");
	CHECK_STREQ(text(&v), "hello world");
	linkable_clear(&v);
	v = metric(23.5, "degC");
	CHECK_STREQ(text(&v), "23.5 degC");
	linkable_clear(&v);
}

static void test_stringify_truncates_like_snprintf(void)
{
	Linkable v = str("abcdef");
	char buf[4] = "xyz";
	CHECK(linkable_stringify(&v, buf, sizeof(buf)) == 6);
	CHECK_STREQ(buf, "abc");
	CHECK(linkable_stringify(&v, NULL, 0) == 6);
	linkable_clear(&v);

	v = metric(1.5, "mm");
	CHECK(linkable_stringify(&v, buf, sizeof(buf)) == 6);
	CHECK_STREQ(buf, "1.5");
	linkable_clear(&v);

	v = (Linkable){0};
	CHECK(linkable_stringify(&v, buf, 1) == 16);
	CHECK_STREQ(buf, "");
}

/* Every finite double survives stringify → parse unchanged. */
static void test_f64_round_trip(void)
{
	const double values[] = {0.1, 1.0 / 3.0, 2.0 / 3.0, 1e-300, 5e-324, 1.7976931348623157e308,
				 -123456.789, 0x1p63, 9007199254740993.0, 3.141592653589793};
	char buf[LINKABLE_NUMBER_TEXT_MAX];
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		Linkable v = linkable_from_f64(values[i]);
		CHECK(linkable_stringify(&v, buf, sizeof(buf)) < sizeof(buf));
		double d = 0.0;
		CHECK(linkable_parse_f64(buf, &d));
		CHECK(d == values[i]);
	}
	char big[LINKABLE_NUMBER_TEXT_MAX];
	Linkable v = linkable_from_f64(-2.2250738585072014e-308);
	CHECK(linkable_stringify(&v, big, sizeof(big)) < sizeof(big));
	v = linkable_from_i64(INT64_MIN);
	CHECK(linkable_stringify(&v, big, sizeof(big)) < sizeof(big));
}

static void test_parse_by_port_type(void)
{
	Linkable v;

	CHECK(linkable_parse(&v, "string", " 12 V "));
	CHECK(v.type == LINKABLE_STRING);
	CHECK_STREQ(v.as.s, " 12 V ");
	CHECK(linkable_strlen(v.as.s) == 6);
	linkable_clear(&v);
	CHECK(linkable_parse(&v, "string", ""));
	CHECK(v.type == LINKABLE_STRING);
	CHECK_STREQ(v.as.s, "");
	linkable_clear(&v);

	CHECK(linkable_parse(&v, "bool", "true"));
	CHECK(v.type == LINKABLE_BOOL && v.as.b);
	CHECK(linkable_parse(&v, "bool", "1"));
	CHECK(v.type == LINKABLE_BOOL && !v.as.b);
	CHECK(linkable_parse(&v, "bool", "false"));
	CHECK(v.type == LINKABLE_BOOL && !v.as.b);

	CHECK(linkable_parse(&v, "number", "42"));
	CHECK(v.type == LINKABLE_F64 && v.as.f64 == 42.0);
	CHECK(linkable_parse(&v, "number", "-0.25"));
	CHECK(v.type == LINKABLE_F64 && v.as.f64 == -0.25);
	CHECK(linkable_parse(&v, "number", "NaN"));
	CHECK(v.type == LINKABLE_F64 && isnan(v.as.f64));

	CHECK(linkable_parse(&v, "number", "3.3 V"));
	CHECK(v.type == LINKABLE_METRIC);
	CHECK(v.as.metric.value == 3.3);
	CHECK_STREQ(v.as.metric.unit, "V");
	CHECK(v.as.metric.category == METRIC_UNCLASSIFIED);
	linkable_clear(&v);
	CHECK(linkable_parse(&v, "number", "-20 m/s 2")); /* split on the first space */
	CHECK(v.type == LINKABLE_METRIC && v.as.metric.value == -20.0);
	CHECK_STREQ(v.as.metric.unit, "m/s 2");
	linkable_clear(&v);

	const char *not_numbers[] = {"", "abc", "12abc", "5 ", " 5", "abc V", "1e V", "V 5"};
	for (size_t i = 0; i < sizeof(not_numbers) / sizeof(not_numbers[0]); i++) {
		CHECK(linkable_parse(&v, "number", not_numbers[i]));
		CHECK(v.type == LINKABLE_F64 && v.as.f64 == 0.0);
	}

	/* Unknown type names parse like "number", matching default_value. */
	CHECK(linkable_parse(&v, "link", "7 mm"));
	CHECK(v.type == LINKABLE_METRIC && v.as.metric.value == 7.0);
	linkable_clear(&v);
	CHECK(linkable_parse(&v, NULL, "7"));
	CHECK(v.type == LINKABLE_F64 && v.as.f64 == 7.0);

	/* A Metric round-trips through stringify and parse. */
	CHECK(linkable_parse(&v, "number", "0.1 bar"));
	CHECK_STREQ(text(&v), "0.1 bar");
	linkable_clear(&v);
}

static void test_copy_assign_share_strings(void)
{
	Linkable a = str("shared");
	Linkable b = linkable_copy(&a);
	CHECK(b.type == LINKABLE_STRING && b.as.s == a.as.s);
	linkable_clear(&a);
	CHECK(a.type == LINKABLE_UNSET);
	CHECK_STREQ(b.as.s, "shared"); /* still alive through b */

	Linkable m = metric(1.0, "kPa");
	linkable_assign(&b, &m); /* releases "shared" */
	CHECK(b.type == LINKABLE_METRIC && b.as.metric.unit == m.as.metric.unit);
	CHECK(b.as.metric.category == METRIC_VOLTAGE);
	linkable_assign(&b, &b); /* self-assignment keeps the value */
	CHECK_STREQ(b.as.metric.unit, "kPa");
	linkable_clear(&m);
	CHECK_STREQ(b.as.metric.unit, "kPa");

	Linkable n = linkable_from_i64(3);
	linkable_assign(&b, &n);
	CHECK(b.type == LINKABLE_I64 && b.as.i64 == 3);
	linkable_clear(&b);

	Linkable e = str("");
	Linkable e2 = linkable_copy(&e);
	linkable_clear(&e);
	linkable_clear(&e2);
}

int main(void)
{
	RUN(test_unset_is_zero_initialized);
	RUN(test_to_f64);
	RUN(test_to_i64);
	RUN(test_to_u64);
	RUN(test_to_bool);
	RUN(test_double_to_integer_edge_cases);
	RUN(test_default_value);
	RUN(test_parse_f64_grammar);
	RUN(test_parse_integers);
	RUN(test_stringify);
	RUN(test_stringify_truncates_like_snprintf);
	RUN(test_f64_round_trip);
	RUN(test_parse_by_port_type);
	RUN(test_copy_assign_share_strings);
	return test_result();
}
