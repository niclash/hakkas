/*
 * The JSON forms of the graph model (graphs/graph_json.h; 05 §2.1, 02 §3.4):
 * exact number printing, node metadata from and to JSON with opaque keys, the
 * changenode merge (change_threshold null included), edge metadata, and
 * initial values of every type, non-finite, Metric and i64 included.
 */
#include "test.h"

#include <math.h>
#include <stdlib.h>

#include "graphs/graph_json.h"

/* Print item (consumed) with graph_json_print and compare. */
static void check_print(cJSON *item, const char *want)
{
	char *text = graph_json_print(item);
	CHECK_STREQ(text, want);
	free(text);
	cJSON_Delete(item);
}

static void test_exact_numbers(void)
{
	/* cJSON alone prints 0.1 + 0.2 as 0.3 (third_party/cjson/README.md). */
	cJSON *arr = cJSON_CreateArray();
	cJSON_AddItemToArray(arr, cJSON_CreateNumber(0.1 + 0.2));
	cJSON_AddItemToArray(arr, cJSON_CreateNumber(9007199254740992.0)); /* 2^53 */
	cJSON_AddItemToArray(arr, cJSON_CreateNumber(-0.0));
	cJSON_AddItemToArray(arr, cJSON_CreateNumber(5));
	cJSON_AddItemToArray(arr, cJSON_CreateNumber(1e300 * 1e300));
	cJSON_AddItemToArray(arr, cJSON_CreateNumber(-1e300 * 1e300));
	cJSON *obj = cJSON_CreateObject();
	cJSON_AddItemToObject(obj, "k", cJSON_CreateNumber(0.1));
	cJSON_AddItemToArray(arr, obj);
	check_print(arr, "[0.30000000000000004,9007199254740992,-0,5,1e999,-1e999,{\"k\":0.1}]");

	/* A parsed tree prints back exactly; 1e999 reads back as Infinity. */
	cJSON *parsed = cJSON_Parse("{\"a\":[0.30000000000000004,1e999,{\"b\":-0}],\"c\":\"s\"}");
	check_print(parsed, "{\"a\":[0.30000000000000004,1e999,{\"b\":-0}],\"c\":\"s\"}");
	/* A lone number. */
	check_print(cJSON_CreateNumber(0.1 + 0.7), "0.7999999999999999");

	cJSON *n = graph_json_number(NAN);
	CHECK(cJSON_IsNull(n));
	cJSON_Delete(n);
	check_print(graph_json_number(1.0 / 3), "0.3333333333333333");
}

static void test_metadata(void)
{
	BlockMetadata md;
	CHECK(graph_json_to_metadata(NULL, &md) == GRAPH_OK);
	CHECK(md.x == 50 && md.y == 50 && isnan(md.change_threshold) && md.extra == NULL);

	cJSON *obj = cJSON_Parse("{\"route\":3,\"x\":10.5,\"label\":{\"t\":\"a\"},\"y\":-2,"
				 "\"change_threshold\":0.1,\"z\":null}");
	CHECK(graph_json_to_metadata(obj, &md) == GRAPH_OK);
	cJSON_Delete(obj);
	CHECK(md.x == 10.5 && md.y == -2 && md.change_threshold == 0.1);
	/* Opaque keys in their order, null values kept as they are. */
	CHECK_STREQ(md.extra, "{\"route\":3,\"label\":{\"t\":\"a\"},\"z\":null}");
	/* Back to JSON: x, y, change_threshold first, then the opaque keys. */
	check_print(graph_json_from_metadata(&md),
		    "{\"x\":10.5,\"y\":-2,\"change_threshold\":0.1,\"route\":3,"
		    "\"label\":{\"t\":\"a\"},\"z\":null}");
	block_metadata_release(&md);
	CHECK(md.extra == NULL);

	/* No opaque keys, null position and threshold: the defaults. */
	obj = cJSON_Parse("{\"x\":null,\"change_threshold\":null}");
	CHECK(graph_json_to_metadata(obj, &md) == GRAPH_OK);
	cJSON_Delete(obj);
	CHECK(md.x == 50 && isnan(md.change_threshold) && md.extra == NULL);
	check_print(graph_json_from_metadata(&md), "{\"x\":50,\"y\":50}");

	/* An infinite threshold is valid (01 §3.3) and round-trips. */
	md.change_threshold = INFINITY;
	check_print(graph_json_from_metadata(&md),
		    "{\"x\":50,\"y\":50,\"change_threshold\":1e999}");
	obj = cJSON_Parse("{\"change_threshold\":1e999}");
	CHECK(graph_json_to_metadata(obj, &md) == GRAPH_OK && isinf(md.change_threshold));
	cJSON_Delete(obj);

	/* Refused, with the default left in *md. */
	const char *bad[] = {"{\"x\":\"1\"}", "{\"y\":true}", "{\"x\":1e999}",
			     "{\"change_threshold\":-1}", "{\"change_threshold\":\"0.1\"}", "[]",
			     "3"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		obj = cJSON_Parse(bad[i]);
		CHECK(obj != NULL);
		CHECK(graph_json_to_metadata(obj, &md) == GRAPH_ERR_INVALID_ARGUMENT);
		CHECK(md.x == 50 && md.extra == NULL);
		cJSON_Delete(obj);
	}
}

/* Apply the changenode patch to md as graph_change_block would. */
static void apply(BlockMetadata *md, const char *patch, enum graph_status want)
{
	cJSON *p = cJSON_Parse(patch);
	CHECK(p != NULL);
	BlockChange chg;
	CHECK(graph_json_to_change(md, p, &chg) == want);
	cJSON_Delete(p);
	if (want != GRAPH_OK) {
		CHECK(!chg.set_x && !chg.set_y && !chg.set_extra && chg.extra == NULL);
		return;
	}
	if (chg.set_x)
		md->x = chg.x;
	if (chg.set_y)
		md->y = chg.y;
	if (chg.threshold == THRESHOLD_SET)
		md->change_threshold = chg.change_threshold;
	else if (chg.threshold == THRESHOLD_CLEAR)
		md->change_threshold = NAN;
	if (chg.set_extra) {
		free(md->extra);
		md->extra = chg.extra; /* a move */
		chg.extra = NULL;
	}
	block_change_release(&chg);
}

static void check_metadata(const BlockMetadata *md, const char *want)
{
	check_print(graph_json_from_metadata(md), want);
}

static void test_change_merge(void)
{
	BlockMetadata md = block_metadata_default();
	/* Only the given keys change. */
	apply(&md, "{\"x\":7}", GRAPH_OK);
	check_metadata(&md, "{\"x\":7,\"y\":50}");
	apply(&md, "{\"change_threshold\":0.25,\"a\":1,\"b\":[1,2]}", GRAPH_OK);
	check_metadata(&md, "{\"x\":7,\"y\":50,\"change_threshold\":0.25,\"a\":1,\"b\":[1,2]}");
	/* An opaque key is replaced in place (a nested object as a whole), a new
	 * one goes to the end, null removes one; untouched keys stay. */
	apply(&md, "{\"c\":{\"d\":1},\"a\":\"one\",\"y\":3}", GRAPH_OK);
	check_metadata(&md, "{\"x\":7,\"y\":3,\"change_threshold\":0.25,\"a\":\"one\","
			    "\"b\":[1,2],\"c\":{\"d\":1}}");
	apply(&md, "{\"c\":{\"e\":2}}", GRAPH_OK);
	check_metadata(&md, "{\"x\":7,\"y\":3,\"change_threshold\":0.25,\"a\":\"one\","
			    "\"b\":[1,2],\"c\":{\"e\":2}}");
	apply(&md, "{\"b\":null,\"nope\":null}", GRAPH_OK);
	check_metadata(&md, "{\"x\":7,\"y\":3,\"change_threshold\":0.25,\"a\":\"one\","
			    "\"c\":{\"e\":2}}");
	/* change_threshold null removes the override (02 §3.4). */
	apply(&md, "{\"change_threshold\":null}", GRAPH_OK);
	CHECK(isnan(md.change_threshold));
	check_metadata(&md, "{\"x\":7,\"y\":3,\"a\":\"one\",\"c\":{\"e\":2}}");
	/* x / y null reset the default position. */
	apply(&md, "{\"x\":null}", GRAPH_OK);
	check_metadata(&md, "{\"x\":50,\"y\":3,\"a\":\"one\",\"c\":{\"e\":2}}");
	/* Removing the last opaque keys leaves none. */
	apply(&md, "{\"a\":null,\"c\":null}", GRAPH_OK);
	CHECK(md.extra == NULL);
	/* An empty patch changes nothing. */
	apply(&md, "{}", GRAPH_OK);
	check_metadata(&md, "{\"x\":50,\"y\":3}");
	/* Refused as a whole: nothing applied. */
	apply(&md, "{\"x\":1,\"y\":\"2\",\"q\":1}", GRAPH_ERR_INVALID_ARGUMENT);
	apply(&md, "{\"change_threshold\":-0.5}", GRAPH_ERR_INVALID_ARGUMENT);
	apply(&md, "[1]", GRAPH_ERR_INVALID_ARGUMENT);
	check_metadata(&md, "{\"x\":50,\"y\":3}");
	block_metadata_release(&md);
}

static void test_edge_metadata(void)
{
	char *text = (char *)"stale";
	cJSON *obj = cJSON_Parse("{\"route\":3,\"w\":0.30000000000000004}");
	CHECK(graph_json_to_edge_metadata(obj, &text) == GRAPH_OK);
	CHECK_STREQ(text, "{\"route\":3,\"w\":0.30000000000000004}");
	cJSON_Delete(obj);
	check_print(graph_json_from_edge_metadata(text), "{\"route\":3,\"w\":0.30000000000000004}");
	free(text);

	CHECK(graph_json_to_edge_metadata(NULL, &text) == GRAPH_OK && text == NULL);
	obj = cJSON_CreateNull();
	CHECK(graph_json_to_edge_metadata(obj, &text) == GRAPH_OK && text == NULL);
	cJSON_Delete(obj);
	obj = cJSON_CreateString("x");
	CHECK(graph_json_to_edge_metadata(obj, &text) == GRAPH_ERR_INVALID_ARGUMENT);
	CHECK(text == NULL);
	cJSON_Delete(obj);
	/* An empty object is metadata too. */
	obj = cJSON_CreateObject();
	CHECK(graph_json_to_edge_metadata(obj, &text) == GRAPH_OK);
	CHECK_STREQ(text, "{}");
	free(text);
	cJSON_Delete(obj);
	CHECK(graph_json_from_edge_metadata(NULL) == NULL);
	CHECK(graph_json_from_edge_metadata("[1]") == NULL);
}

/* Store v as a graph file does, print it, and read it back for port type. */
static void round_trip(Linkable v, const char *port_type, const char *want_json,
		       const char *want_back)
{
	cJSON *item = graph_json_from_initial(&v);
	CHECK(item != NULL);
	char *text = graph_json_print(item);
	cJSON_Delete(item);
	CHECK_STREQ(text, want_json);
	cJSON *parsed = cJSON_Parse(text);
	free(text);
	Linkable back;
	CHECK(graph_json_to_initial(parsed, port_type, &back) == GRAPH_OK);
	cJSON_Delete(parsed);
	char buf[64];
	linkable_stringify(&back, buf, sizeof(buf));
	CHECK_STREQ(buf, want_back);
	linkable_clear(&back);
	linkable_clear(&v);
}

static void test_initials(void)
{
	Linkable v;
	/* Strings and booleans natively. */
	CHECK(linkable_from_string(&v, "hi \"you\"", 8));
	round_trip(v, "string", "\"hi \\\"you\\\"\"", "hi \"you\"");
	round_trip(linkable_from_bool(true), "bool", "true", "true");
	round_trip(linkable_from_bool(false), "number", "false", "false");
	/* Finite f64 exactly. */
	round_trip(linkable_from_f64(0.1 + 0.2), "number", "0.30000000000000004",
		   "0.30000000000000004");
	round_trip(linkable_from_f64(-0.0), "number", "-0", "-0");
	round_trip(linkable_from_f64(1e300), "number", "1e+300", "1e+300");
	/* Non-finite as their strings, parsed back by the number port. */
	round_trip(linkable_from_f64(NAN), "number", "\"NaN\"", "NaN");
	round_trip(linkable_from_f64(INFINITY), "number", "\"Infinity\"", "Infinity");
	round_trip(linkable_from_f64(-INFINITY), "number", "\"-Infinity\"", "-Infinity");
	/* A Metric as "<value> <unit>". */
	CHECK(linkable_from_metric(&v, 21.5, "degC", 4, METRIC_UNCLASSIFIED));
	round_trip(v, "number", "\"21.5 degC\"", "21.5 degC");
	/* An i64 as exact integer text, read back as an f64 (02 §3.4). */
	round_trip(linkable_from_i64(INT64_MIN), "number", "-9223372036854775808",
		   "-9.223372036854776e+18");
	round_trip(linkable_from_i64(42), "number", "42", "42");

	/* The types after reading. */
	cJSON *j = cJSON_Parse("\"21.5 degC\"");
	CHECK(graph_json_to_initial(j, "number", &v) == GRAPH_OK && v.type == LINKABLE_METRIC);
	linkable_clear(&v);
	CHECK(graph_json_to_initial(j, "string", &v) == GRAPH_OK && v.type == LINKABLE_STRING);
	linkable_clear(&v);
	cJSON_Delete(j);
	j = cJSON_Parse("42");
	CHECK(graph_json_to_initial(j, "number", &v) == GRAPH_OK && v.type == LINKABLE_F64);
	cJSON_Delete(j);
	j = cJSON_Parse("\"true\"");
	CHECK(graph_json_to_initial(j, "bool", &v) == GRAPH_OK && v.type == LINKABLE_BOOL &&
	      v.as.b);
	cJSON_Delete(j);

	/* No initial: null, arrays, objects; nothing to store for unset. */
	const char *bad[] = {"null", "[1]", "{}"};
	for (size_t i = 0; i < 3; i++) {
		j = cJSON_Parse(bad[i]);
		CHECK(graph_json_to_initial(j, "number", &v) == GRAPH_ERR_INVALID_ARGUMENT);
		CHECK(v.type == LINKABLE_UNSET);
		cJSON_Delete(j);
	}
	CHECK(graph_json_to_initial(NULL, "number", &v) == GRAPH_ERR_INVALID_ARGUMENT);
	Linkable unset = {0};
	CHECK(graph_json_from_initial(&unset) == NULL);
}

static void test_component(void)
{
	/* NULL texts are "", subgraph and addressable are reported, ports in
	 * order (02 §3.5). */
	static const PortDescriptor ins[] = {
		{.name = "a", .type = "string", .addressable = true},
		{.name = "b", .description = "B", .type = "bool"},
	};
	static const BlockTypeDef def = {.name = "my/Group", .inputs = ins, .input_count = 2,
					 .subgraph = true};
	cJSON *j = graph_json_component(&def);
	char *text = graph_json_print(j);
	CHECK_STREQ(text, "{\"name\":\"my/Group\",\"description\":\"\",\"subgraph\":true,"
			  "\"icon\":\"\",\"inPorts\":["
			  "{\"id\":\"a\",\"description\":\"\",\"type\":\"string\","
			  "\"addressable\":true},"
			  "{\"id\":\"b\",\"description\":\"B\",\"type\":\"bool\","
			  "\"addressable\":false}],\"outPorts\":[]}");
	free(text);
	cJSON_Delete(j);
}

int main(void)
{
	RUN(test_exact_numbers);
	RUN(test_metadata);
	RUN(test_change_merge);
	RUN(test_edge_metadata);
	RUN(test_initials);
	RUN(test_component);
	return test_result();
}
