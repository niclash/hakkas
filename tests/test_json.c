/*
 * The vendored cJSON (third_party/cjson/, 01 §6): a usage check of the pinned
 * release as built into the core — a graph-file-like round trip (05 §2.1),
 * how it prints numbers, and the nesting limit.
 */
#include "test.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include "cJSON.h"
#include "graphs/linkable.h"

/* A graph file in the shape of 05 §2.1, minified as cJSON prints it. */
static const char graph_file[] =
	"{\"id\":\"g1\",\"name\":\"Boiler\",\"description\":\"Heating loop\",\"icon\":\"fire\","
	"\"started\":true,"
	"\"blocks\":{"
	"\"gain\":{\"component\":\"math/Multiply\",\"metadata\":{\"x\":120,\"y\":-40,"
	"\"change_threshold\":0.1,\"label\":\"G\\u00e4in \\\"1\\\"\"}},"
	"\"tick\":{\"component\":\"timing/Interval\",\"metadata\":{\"x\":50,\"y\":50}}},"
	"\"connections\":["
	"{\"src\":{\"node\":\"tick\",\"port\":\"out\"},\"tgt\":{\"node\":\"gain\",\"port\":\"a\"},"
	"\"metadata\":{\"route\":3}},"
	"{\"src\":{\"data\":2.5},\"tgt\":{\"node\":\"gain\",\"port\":\"b\"}},"
	"{\"src\":{\"data\":\"12 V\"},\"tgt\":{\"node\":\"gain\",\"port\":\"c\"}},"
	"{\"src\":{\"data\":false},\"tgt\":{\"node\":\"tick\",\"port\":\"enable\"}}],"
	"\"inports\":[{\"name\":\"in\",\"node\":\"gain\",\"port\":\"a\"}],"
	"\"outports\":[{\"name\":\"out\",\"node\":\"gain\",\"port\":\"out\"}]}";

static void test_version(void)
{
	CHECK_STREQ(cJSON_Version(), "1.7.19");
}

static void test_graph_file_round_trip(void)
{
	cJSON *doc = cJSON_Parse(graph_file);
	CHECK(doc != NULL);
	if (doc == NULL)
		return;

	/* Reading it the way a loader would. */
	CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "started")));
	const cJSON *blocks = cJSON_GetObjectItemCaseSensitive(doc, "blocks");
	CHECK(cJSON_IsObject(blocks));
	CHECK(cJSON_GetArraySize(blocks) == 2);
	const cJSON *gain = cJSON_GetObjectItemCaseSensitive(blocks, "gain");
	CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(gain, "component")),
		    "math/Multiply");
	const cJSON *meta = cJSON_GetObjectItemCaseSensitive(gain, "metadata");
	CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(meta, "y")) == -40.0);
	CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(meta, "change_threshold")) ==
	      0.1);
	/* \u escapes decode to UTF-8. */
	CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(meta, "label")),
		    "G\xc3\xa4in \"1\"");
	const cJSON *conns = cJSON_GetObjectItemCaseSensitive(doc, "connections");
	CHECK(cJSON_GetArraySize(conns) == 4);
	const cJSON *src = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(conns, 1), "src");
	CHECK(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(src, "data")) == 2.5);

	/* Printing keeps member order, so the minified form comes back byte for
	 * byte, except that cJSON prints non-ASCII as UTF-8, not as \u escapes. */
	char *out = cJSON_PrintUnformatted(doc);
	CHECK(out != NULL);
	char *expected = malloc(sizeof(graph_file));
	CHECK(expected != NULL);
	if (out != NULL && expected != NULL) {
		/* Replace the 6-byte "ä" with its 2-byte UTF-8 form. */
		const char *esc = strstr(graph_file, "\\u00e4");
		CHECK(esc != NULL);
		size_t head = (size_t)(esc - graph_file);
		memcpy(expected, graph_file, head);
		memcpy(expected + head, "\xc3\xa4", 2);
		strcpy(expected + head + 2, esc + 6);
		CHECK_STREQ(out, expected);
	}
	free(expected);

	/* The formatted (pretty) form parses back to an equal document. */
	char *pretty = cJSON_Print(doc);
	CHECK(pretty != NULL);
	cJSON *again = cJSON_Parse(pretty);
	CHECK(again != NULL);
	CHECK(cJSON_Compare(doc, again, true));

	cJSON_Delete(again);
	cJSON_free(pretty);
	cJSON_free(out);
	cJSON_Delete(doc);
}

static void test_parse_with_length(void)
{
	/* A WebSocket frame is not NUL-terminated: parsing stops at the length. */
	const char frame[] = "{\"protocol\":\"runtime\",\"command\":\"getruntime\"}GARBAGE";
	size_t len = sizeof(frame) - 1 - strlen("GARBAGE");
	cJSON *doc = cJSON_ParseWithLength(frame, len);
	CHECK(doc != NULL);
	CHECK_STREQ(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(doc, "command")),
		    "getruntime");
	cJSON_Delete(doc);

	/* Trailing data after the value is an error when it must be the whole text. */
	const char *end = NULL;
	CHECK(cJSON_ParseWithOpts("{} x", &end, true) == NULL);
	CHECK(cJSON_Parse("{\"a\":1,}") == NULL);
	CHECK(cJSON_Parse("") == NULL);
}

/* cJSON's text for the number v (static buffer). */
static const char *number_text(double v)
{
	static char buf[64];
	cJSON *n = cJSON_CreateNumber(v);
	char *s = n != NULL ? cJSON_PrintUnformatted(n) : NULL;
	CHECK(s != NULL);
	snprintf(buf, sizeof(buf), "%s", s != NULL ? s : "");
	cJSON_free(s);
	cJSON_Delete(n);
	return buf;
}

static void test_number_printing(void)
{
	/* Integral values in int range print as integers. */
	CHECK_STREQ(number_text(0.0), "0");
	CHECK_STREQ(number_text(42.0), "42");
	CHECK_STREQ(number_text(-7.0), "-7");
	CHECK_STREQ(number_text(2147483647.0), "2147483647");
	/* Beyond int, "%.15g": integers up to 15 digits stay plain, larger ones
	 * switch to exponent form. */
	CHECK_STREQ(number_text(3000000000.0), "3000000000");
	CHECK_STREQ(number_text(-123456789012345.0), "-123456789012345");
	CHECK_STREQ(number_text(1e15), "1e+15");
	CHECK_STREQ(number_text(1e20), "1e+20");
	/* Fractions. */
	CHECK_STREQ(number_text(0.1), "0.1");
	CHECK_STREQ(number_text(2.5), "2.5");
	CHECK_STREQ(number_text(1e-300), "1e-300");

	/* Lossy: cJSON keeps the "%.15g" form if it reads back within a relative
	 * DBL_EPSILON, not only if it reads back exactly ("%.17g" otherwise), so a
	 * nearby double comes back. linkable_stringify round-trips exactly. */
	static const struct {
		double v;
		const char *cjson, *exact;
	} lossy[] = {
		{0.1 + 0.2, "0.3", "0.30000000000000004"},
		{0.1 + 0.7, "0.8", "0.7999999999999999"},
		{9007199254740992.0, "9.00719925474099e+15", "9007199254740992"}, /* 2^53 */
	};
	for (size_t i = 0; i < sizeof(lossy) / sizeof(lossy[0]); i++) {
		CHECK_STREQ(number_text(lossy[i].v), lossy[i].cjson);
		CHECK(strtod(lossy[i].cjson, NULL) != lossy[i].v);
		Linkable l = linkable_from_f64(lossy[i].v);
		char buf[LINKABLE_NUMBER_TEXT_MAX];
		linkable_stringify(&l, buf, sizeof(buf));
		CHECK_STREQ(buf, lossy[i].exact);
		CHECK(strtod(buf, NULL) == lossy[i].v);
		/* That text can go into a document as a raw value. */
		cJSON *raw = cJSON_CreateRaw(buf);
		char *s = raw != NULL ? cJSON_PrintUnformatted(raw) : NULL;
		CHECK(s != NULL);
		CHECK_STREQ(s, lossy[i].exact);
		cJSON_free(s);
		cJSON_Delete(raw);
	}

	/* Negative zero loses its sign; non-finite values print as null. */
	CHECK_STREQ(number_text(-0.0), "0");
	CHECK_STREQ(number_text(NAN), "null");
	CHECK_STREQ(number_text(INFINITY), "null");
	CHECK_STREQ(number_text(-INFINITY), "null");
}

static void test_number_parsing(void)
{
	/* Every number parses to a double: integers beyond 2^53 lose precision,
	 * and valueint saturates at the int range. */
	cJSON *n = cJSON_Parse("9007199254740993");
	CHECK(cJSON_IsNumber(n));
	CHECK(cJSON_GetNumberValue(n) == 9007199254740992.0);
	CHECK(n != NULL && n->valueint == INT_MAX);
	cJSON_Delete(n);

	n = cJSON_Parse("-1e400");
	CHECK(cJSON_IsNumber(n));
	CHECK(isinf(cJSON_GetNumberValue(n)));
	cJSON_Delete(n);

	/* JSON has no NaN or Infinity literals. */
	CHECK(cJSON_Parse("NaN") == NULL);
	CHECK(cJSON_Parse("Infinity") == NULL);
}

/* depth nested arrays ([...]) or objects ({"k":...}) around a 0, written into buf. */
static const char *nested(char *buf, int depth, char open, char close)
{
	char *p = buf;
	for (int i = 0; i < depth; i++) {
		if (open == '{') {
			memcpy(p, "{\"k\":", 5);
			p += 5;
		} else {
			*p++ = open;
		}
	}
	*p++ = '0';
	for (int i = 0; i < depth; i++)
		*p++ = close;
	*p = '\0';
	return buf;
}

static void test_nesting_limit(void)
{
	static char buf[8 * (CJSON_NESTING_LIMIT + 2)];

	CHECK(CJSON_NESTING_LIMIT == 32);

	cJSON *doc = cJSON_Parse(nested(buf, CJSON_NESTING_LIMIT, '[', ']'));
	CHECK(doc != NULL);
	cJSON_Delete(doc);
	CHECK(cJSON_Parse(nested(buf, CJSON_NESTING_LIMIT + 1, '[', ']')) == NULL);

	doc = cJSON_Parse(nested(buf, CJSON_NESTING_LIMIT, '{', '}'));
	CHECK(doc != NULL);
	cJSON_Delete(doc);
	CHECK(cJSON_Parse(nested(buf, CJSON_NESTING_LIMIT + 1, '{', '}')) == NULL);
}

int main(void)
{
	RUN(test_version);
	RUN(test_graph_file_round_trip);
	RUN(test_parse_with_length);
	RUN(test_number_printing);
	RUN(test_number_parsing);
	RUN(test_nesting_limit);
	return test_result();
}
