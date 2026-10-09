/*
 * Graph files (app/graph_files.h, app/file_names.h; 05 §2, 02 §3.2–§3.4):
 * file names, the canonical format and its round trip, every initial type,
 * opaque metadata, placeholder blocks and their retained connections,
 * invalid entries and malformed files, the started flag, persist and its
 * failure event, delete, and loading a directory or a list of files. Files
 * live in a fresh temporary directory that is removed afterwards.
 */
#define _XOPEN_SOURCE 700

#include "test.h"

#include <ftw.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

#include "app/file_names.h"
#include "app/graph_files.h"
#include "blocktypes/block_types.h"
#include "graphs/graph_json.h"
#include "platform/pal.h"
#include "system/log.h"

static char root[PAL_PATH_MAX];
static Runtime rt;
static Graphs gs;
static GraphFiles gf;

/* ---- Helpers ------------------------------------------------------------------ */

static void append(char *buf, size_t size, const char *fmt, ...) LOG_PRINTF_FORMAT(3, 4);

static void append(char *buf, size_t size, const char *fmt, ...)
{
	size_t len = strlen(buf);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + len, size - len, fmt, ap);
	va_end(ap);
}

/* JSON written with ' for ": a copy with every ' replaced. */
static const char *jq(const char *s)
{
	static char buf[4][8192];
	static int next;
	char *b = buf[next++ % 4];
	size_t n = strlen(s);
	CHECK(n < sizeof(buf[0]));
	for (size_t i = 0; i <= n; i++)
		b[i] = s[i] == '\'' ? '"' : s[i];
	return b;
}

/* Log lines, without the "<level> <file>:<line>: " prefix, one per line. */
static char logs[8192];

static void log_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)len;
	const char *msg = strstr(line, ": ");
	append(logs, sizeof(logs), "%s %s\n", log_level_name(level), msg != NULL ? msg + 2 : line);
}

/* Events, one short entry per event. */
static char events[4096];
static GraphsSubscriber sub;

static void on_event(void *ctx, const GraphEvent *ev)
{
	(void)ctx;
	static const char *const names[] = {
		[GRAPH_EVENT_BLOCK_ADDED] = "+node",
		[GRAPH_EVENT_BLOCK_REMOVED] = "-node",
		[GRAPH_EVENT_BLOCK_RENAMED] = "renamenode",
		[GRAPH_EVENT_BLOCK_CHANGED] = "changenode",
		[GRAPH_EVENT_EDGE_ADDED] = "+edge",
		[GRAPH_EVENT_EDGE_REMOVED] = "-edge",
		[GRAPH_EVENT_EDGE_CHANGED] = "changeedge",
		[GRAPH_EVENT_INITIAL_ADDED] = "+init",
		[GRAPH_EVENT_INITIAL_REMOVED] = "-init",
		[GRAPH_EVENT_INPORT_ADDED] = "+inport",
		[GRAPH_EVENT_INPORT_REMOVED] = "-inport",
		[GRAPH_EVENT_INPORT_RENAMED] = "renameinport",
		[GRAPH_EVENT_OUTPORT_ADDED] = "+outport",
		[GRAPH_EVENT_OUTPORT_REMOVED] = "-outport",
		[GRAPH_EVENT_OUTPORT_RENAMED] = "renameoutport",
		[GRAPH_EVENT_GRAPH_CREATED] = "+graph",
		[GRAPH_EVENT_GRAPH_DELETED] = "-graph",
		[GRAPH_EVENT_GRAPH_RENAMED] = "rename",
		[GRAPH_EVENT_GRAPH_STARTED] = "started",
		[GRAPH_EVENT_GRAPH_STOPPED] = "stopped",
		[GRAPH_EVENT_GRAPH_STATUS] = "status",
		[GRAPH_EVENT_ERROR] = "error",
	};
	append(events, sizeof(events), "%s", names[ev->type]);
	if (ev->type == GRAPH_EVENT_BLOCK_ADDED)
		append(events, sizeof(events), " %s", ev->block->name);
	if (ev->type == GRAPH_EVENT_ERROR)
		append(events, sizeof(events), " %s: %s", ev->graph_id ? ev->graph_id : "-",
		       ev->message);
	append(events, sizeof(events), ";");
}

/* root/name, made a directory. */
static const char *subdir(const char *name)
{
	static char buf[4][PAL_PATH_MAX];
	static int next;
	char *p = buf[next++ % 4];
	CHECK(snprintf(p, PAL_PATH_MAX, "%s/%s", root, name) < PAL_PATH_MAX);
	CHECK(pal_storage_mkdir(p) == PAL_OK);
	return p;
}

/* A registry with its graph files in the fresh datadir root/name. */
static void setup(const char *name)
{
	runtime_init(&rt);
	graphs_init(&gs, &rt, block_types_get, pal_random_bytes);
	graphs_subscriber_init(&sub, on_event, NULL);
	graphs_subscribe(&gs, &sub);
	CHECK(graph_files_init(&gf, &gs, subdir(name)));
	logs[0] = events[0] = '\0';
}

static void teardown(void)
{
	graphs_deinit(&gs);
	runtime_deinit(&rt);
}

/* The whole file at path as a heap string, or NULL. */
static char *slurp(const char *path)
{
	struct pal_stat st;
	if (pal_storage_stat(path, &st) != PAL_OK)
		return NULL;
	char *buf = malloc((size_t)st.size + 1);
	size_t len = 0;
	if (buf == NULL || pal_storage_read(path, buf, (size_t)st.size, &len) != PAL_OK) {
		free(buf);
		return NULL;
	}
	buf[len] = '\0';
	return buf;
}

static void put(const char *path, const char *text)
{
	CHECK(pal_storage_write(path, text, strlen(text)) == PAL_OK);
}

/* dir + "/" + name, in one of a few static buffers. */
static const char *join(const char *dir, const char *name)
{
	static char buf[4][PAL_PATH_MAX];
	static int next;
	char *p = buf[next++ % 4];
	int n = snprintf(p, PAL_PATH_MAX, "%s/%s", dir, name);
	CHECK(n > 0 && n < PAL_PATH_MAX);
	return p;
}

/* The path of graph id's file. */
static const char *graph_path(const char *id)
{
	static char buf[2][PAL_PATH_MAX];
	static int next;
	char *p = buf[next++ % 2];
	CHECK(graph_files_path(&gf, GRAPH_FILES_GRAPHS_DIR, id, p, PAL_PATH_MAX) ==
	      GRAPH_FILES_OK);
	return p;
}

/* Expect g's saved text to be want. */
static void check_saved(const Graph *g, const char *want)
{
	size_t len = 0;
	char *text = graph_files_serialize(g, &len);
	CHECK_STREQ(text, want);
	CHECK(text == NULL || len == strlen(text));
	free(text);
}

/* Load text (JSON with ' for "), expecting st. */
static Graph *load(const char *text, enum graph_files_status st)
{
	const char *t = jq(text);
	Graph *g = (Graph *)&gs;
	CHECK(graph_files_load_text(&gf, t, strlen(t), "v", &g) == st);
	CHECK((st == GRAPH_FILES_OK) == (g != NULL));
	return g;
}

/* The value of node.port's input. */
static const char *input_value(const Graph *g, const char *node, const char *port)
{
	static char buf[64];
	const Block *b = graph_find_block(g, node);
	CHECK(b != NULL);
	if (b == NULL)
		return "";
	int32_t i = block_input_index(b, port);
	CHECK(i >= 0);
	linkable_stringify(&b->in_values[i], buf, sizeof(buf));
	return buf;
}

/* A test type with a port of each type (no compute). */
static const PortDescriptor sink_inputs[] = {
	{.name = "n", .description = "", .type = "number", .default_value = LINKABLE_F64_INIT(0)},
	{.name = "s", .description = "", .type = "string",
	 .default_value = LINKABLE_EMPTY_STRING_INIT},
	{.name = "b", .description = "", .type = "bool",
	 .default_value = LINKABLE_BOOL_INIT(false)},
};

static const BlockTypeDef sink_type = {
	.name = "test/Sink",
	.icon = "",
	.description = "",
	.inputs = sink_inputs,
	.input_count = 3,
	.change_threshold = NAN,
};

/* ---- File names (05 §2.3) --------------------------------------------------- */

static void check_name(const char *name, const char *file)
{
	char enc[FILE_NAME_ENCODED_MAX + 1], dec[FILE_NAME_ID_MAX + 1];
	CHECK(file_name_encode(name, enc, sizeof(enc)));
	CHECK_STREQ(enc, file);
	CHECK(file_name_decode(file, dec, sizeof(dec)));
	CHECK_STREQ(dec, name);
}

static void test_file_names(void)
{
	check_name("math/MyBlock", "math%2FMyBlock.json");
	check_name("0b5c4f0e-7a1d-4c4e-9d2b-3f6a8e1c2d4b",
		   "0b5c4f0e-7a1d-4c4e-9d2b-3f6a8e1c2d4b.json"); /* a UUID is unchanged */
	check_name("A_z-9", "A_z-9.json");
	check_name("a b.c%", "a%20b%2Ec%25.json");
	check_name("\xc3\xa9", "%C3%A9.json"); /* every byte of UTF-8 */
	check_name("..", "%2E%2E.json");

	/* The 64-byte limit (02 §3.4.1); the longest encoding fits 197 bytes. */
	char name[FILE_NAME_ID_MAX + 2];
	char enc[FILE_NAME_ENCODED_MAX + 1];
	memset(name, '/', sizeof(name) - 1);
	name[FILE_NAME_ID_MAX] = '\0';
	CHECK(file_name_encode(name, enc, sizeof(enc)));
	CHECK(strlen(enc) == FILE_NAME_ENCODED_MAX && FILE_NAME_ENCODED_MAX == 197);
	char dec[FILE_NAME_ID_MAX + 1];
	CHECK(file_name_decode(enc, dec, sizeof(dec)) && strcmp(dec, name) == 0);
	name[FILE_NAME_ID_MAX] = '/';
	name[FILE_NAME_ID_MAX + 1] = '\0';
	CHECK(!file_name_encode(name, enc, sizeof(enc)) && enc[0] == '\0');
	CHECK(!file_name_encode("", enc, sizeof(enc)));
	CHECK(!file_name_encode(NULL, enc, sizeof(enc)));
	CHECK(!file_name_encode("abc", enc, 8)); /* "abc.json" needs 9 bytes with the NUL */
	CHECK(file_name_encode("abc", enc, 9));
	CHECK(!file_name_encode("/", enc, 8)); /* "%2F.json" needs 9 too */
	CHECK(file_name_encode("/", enc, 9));

	/* Decoding accepts only the canonical form. */
	const char *bad[] = {"abc",	"abc.JSON", ".json",    "%41.json", "%2f.json", "%2.json",
			     "%.json",	"a.b.json", "a b.json", "%00.json", "%G0.json", NULL};
	for (int i = 0; bad[i] != NULL; i++)
		CHECK(!file_name_decode(bad[i], dec, sizeof(dec)) && dec[0] == '\0');
	char long_file[80 + 6];
	memset(long_file, 'x', 65);
	strcpy(long_file + 65, ".json");
	CHECK(!file_name_decode(long_file, dec, sizeof(dec)));
	CHECK(!file_name_decode("abc.json", dec, 3)); /* does not fit */
}

static void test_paths(void)
{
	char path[PAL_PATH_MAX];
	GraphFiles f;
	CHECK(graph_files_init(&f, &gs, "/data/"));
	CHECK_STREQ(f.datadir, "/data");
	CHECK(graph_files_path(&f, GRAPH_FILES_GRAPHS_DIR, "math/MyBlock", path, sizeof(path)) ==
	      GRAPH_FILES_OK);
	CHECK_STREQ(path, "/data/graphs/math%2FMyBlock.json");
	CHECK(graph_files_path(&f, GRAPH_FILES_TYPES_DIR, "t", path, sizeof(path)) ==
	      GRAPH_FILES_OK);
	CHECK_STREQ(path, "/data/types/t.json");
	CHECK(graph_files_dir(&f, GRAPH_FILES_GRAPHS_DIR, path, sizeof(path)) == GRAPH_FILES_OK);
	CHECK_STREQ(path, "/data/graphs");
	CHECK(graph_files_path(&f, GRAPH_FILES_GRAPHS_DIR, "", path, sizeof(path)) ==
	      GRAPH_FILES_ERR_INVALID);
	CHECK(graph_files_path(&f, GRAPH_FILES_GRAPHS_DIR, "x", path, 10) ==
	      GRAPH_FILES_ERR_INVALID);
	CHECK(graph_files_init(&f, &gs, "/"));
	CHECK(graph_files_path(&f, GRAPH_FILES_GRAPHS_DIR, "x", path, sizeof(path)) ==
	      GRAPH_FILES_OK);
	CHECK_STREQ(path, "/graphs/x.json");
	CHECK(!graph_files_init(&f, &gs, ""));
	CHECK(!graph_files_init(&f, &gs, NULL));

	/* A datadir so long that no path fits is refused; one where only long
	 * names do not fit is accepted, and those names are refused. */
	char dir[PAL_PATH_MAX + 1];
	memset(dir, 'd', sizeof(dir) - 1);
	dir[0] = '/';
	size_t room = PAL_PATH_MAX - sizeof("/graphs/x.json.tmp");
	dir[room + 1] = '\0';
	CHECK(!graph_files_init(&f, &gs, dir));
	dir[room] = '\0';
	CHECK(graph_files_init(&f, &gs, dir));
	CHECK(graph_files_path(&f, GRAPH_FILES_GRAPHS_DIR, "x", path, sizeof(path)) ==
	      GRAPH_FILES_OK);
	CHECK(strlen(path) + sizeof(PAL_TMP_SUFFIX) == PAL_PATH_MAX);
	CHECK(graph_files_path(&f, GRAPH_FILES_GRAPHS_DIR, "xy", path, sizeof(path)) ==
	      GRAPH_FILES_ERR_INVALID);

	/* make_dirs creates graphs/ and types/, and is idempotent. */
	setup("paths");
	CHECK(graph_files_make_dirs(&gf) == GRAPH_FILES_OK);
	CHECK(graph_files_make_dirs(&gf) == GRAPH_FILES_OK);
	struct pal_stat st;
	CHECK(graph_files_dir(&gf, GRAPH_FILES_GRAPHS_DIR, path, sizeof(path)) == GRAPH_FILES_OK);
	CHECK(pal_storage_stat(path, &st) == PAL_OK && st.is_dir);
	CHECK(graph_files_dir(&gf, GRAPH_FILES_TYPES_DIR, path, sizeof(path)) == GRAPH_FILES_OK);
	CHECK(pal_storage_stat(path, &st) == PAL_OK && st.is_dir);
	teardown();
}

/* ---- Saving and the round trip (05 §2.1) ------------------------------------ */

static const char canonical[] =
	"{'id':'g1','name':'Test','description':'d','icon':'cog','started':false,"
	"'blocks':{"
	"'A':{'component':'math/Add2','metadata':{'x':10,'y':20.5,'change_threshold':0.5,"
	"'label':'a','color':[1,2,0.30000000000000004]}},"
	"'B':{'component':'math/Add2','metadata':{'x':50,'y':50}},"
	"'S':{'component':'test/Sink','metadata':{'x':50,'y':50}}},"
	"'connections':["
	"{'src':{'data':0.30000000000000004},'tgt':{'node':'A','port':'in1'}},"
	"{'src':{'data':'NaN'},'tgt':{'node':'A','port':'in2'}},"
	"{'src':{'node':'A','port':'out'},'tgt':{'node':'B','port':'in1'},"
	"'metadata':{'route':3,'secure':false}},"
	"{'src':{'data':'21.5 degC'},'tgt':{'node':'B','port':'in2'}},"
	"{'src':{'data':'-Infinity'},'tgt':{'node':'S','port':'n'}},"
	"{'src':{'data':'hi \\\\ \\'you\\''},'tgt':{'node':'S','port':'s'}},"
	"{'src':{'data':true},'tgt':{'node':'S','port':'b'}}],"
	"'inports':[{'name':'x','node':'A','port':'in1'}],"
	"'outports':[{'name':'y','node':'B','port':'out'}]}";

/* Build the graph of canonical through the commands. */
static Graph *build_canonical(void)
{
	Graph *g;
	CHECK(graphs_new_graph(&gs, "g1", "Test", "d", "cog", &g, NULL) == GRAPH_OK);
	cJSON *md_json = cJSON_Parse(jq("{'label':'a','x':10,'y':20.5,'change_threshold':0.5,"
					"'color':[1,2,0.30000000000000004]}"));
	BlockMetadata md;
	CHECK(graph_json_to_metadata(md_json, &md) == GRAPH_OK);
	cJSON_Delete(md_json);
	CHECK(graphs_add_node(&gs, "g1", "A", "math/Add2", &md, NULL) == GRAPH_OK);
	block_metadata_release(&md);
	CHECK(graphs_add_node(&gs, "g1", "B", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "S", "test/Sink", NULL, NULL) == GRAPH_OK);
	/* Ports and connections in another order than they are saved in. */
	CHECK(graphs_add_port(&gs, "g1", GRAPH_OUTPORT, "y", "B", "out", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_INPORT, "x", "A", "in1", NULL) == GRAPH_OK);
	Linkable v;
	CHECK(linkable_from_string(&v, "hi \\ \"you\"", 10));
	CHECK(graphs_add_initial(&gs, "g1", "S", "s", &v, NULL) == GRAPH_OK);
	linkable_clear(&v);
	v = linkable_from_bool(true);
	CHECK(graphs_add_initial(&gs, "g1", "S", "b", &v, NULL) == GRAPH_OK);
	v = linkable_from_f64(-INFINITY);
	CHECK(graphs_add_initial(&gs, "g1", "S", "n", &v, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "A", "out", "B", "in1", "{\"route\":3,\"secure\":false}",
			      NULL) == GRAPH_OK);
	CHECK(linkable_from_metric(&v, 21.5, "degC", 4, METRIC_UNCLASSIFIED));
	CHECK(graphs_add_initial(&gs, "g1", "B", "in2", &v, NULL) == GRAPH_OK);
	linkable_clear(&v);
	v = linkable_from_f64(NAN);
	CHECK(graphs_add_initial(&gs, "g1", "A", "in2", &v, NULL) == GRAPH_OK);
	v = linkable_from_f64(0.1 + 0.2);
	CHECK(graphs_add_initial(&gs, "g1", "A", "in1", &v, NULL) == GRAPH_OK);
	return g;
}

static void test_save_canonical(void)
{
	setup("canonical");
	Graph *g = build_canonical();
	check_saved(g, jq(canonical));

	/* An empty graph has every key. */
	Graph *e;
	CHECK(graphs_new_graph(&gs, "e", NULL, NULL, NULL, &e, NULL) == GRAPH_OK);
	check_saved(e, jq("{'id':'e','name':'<<new graph>>','description':'','icon':'',"
			  "'started':false,'blocks':{},'connections':[],'inports':[],"
			  "'outports':[]}"));
	teardown();
}

/* Save → load → save is byte-identical, and the model comes back. */
static void test_round_trip(void)
{
	setup("round-trip");
	Graph *g = build_canonical();
	CHECK(graph_files_persist(&gf, "g1", NULL) == GRAPH_FILES_OK);
	char *first = slurp(graph_path("g1"));
	CHECK_STREQ(first, jq(canonical));
	CHECK(graphs_delete_graph(&gs, "g1", "Test", NULL) == GRAPH_OK);
	(void)g;

	events[0] = '\0';
	CHECK(graph_files_load(&gf, graph_path("g1"), &g) == GRAPH_FILES_OK);
	CHECK_STREQ(logs, "");
	/* Loading reports what the equivalent commands report (05 §2.2). */
	CHECK_STREQ(events, "+graph;+node A;+node B;+node S;+init;+init;+edge;+init;+init;+init;"
			    "+init;+inport;+outport;");
	CHECK(g != NULL && !g->started);
	CHECK_STREQ(g->name, "Test");
	CHECK_STREQ(g->description, "d");
	CHECK_STREQ(g->icon, "cog");
	CHECK_STREQ(input_value(g, "A", "in1"), "0.30000000000000004");
	CHECK_STREQ(input_value(g, "A", "in2"), "NaN");
	CHECK_STREQ(input_value(g, "S", "n"), "-Infinity");
	CHECK_STREQ(input_value(g, "S", "s"), "hi \\ \"you\"");
	const Block *b = graph_find_block(g, "B");
	CHECK(b->inputs[1].initial.type == LINKABLE_METRIC);
	CHECK_STREQ(b->inputs[1].initial.as.metric.unit, "degC");
	CHECK(b->inputs[1].initial.as.metric.value == 21.5);
	CHECK_STREQ(b->inputs[0].edge_metadata, "{\"route\":3,\"secure\":false}");
	const Block *s = graph_find_block(g, "S");
	CHECK(s->inputs[2].initial.type == LINKABLE_BOOL && s->inputs[2].initial.as.b);
	CHECK(s->inputs[1].initial.type == LINKABLE_STRING);
	const Block *a = graph_find_block(g, "A");
	CHECK(a->metadata.x == 10 && a->metadata.y == 20.5 && a->metadata.change_threshold == 0.5);
	CHECK_STREQ(a->metadata.extra, "{\"label\":\"a\",\"color\":[1,2,0.30000000000000004]}");

	CHECK(graph_files_persist(&gf, "g1", NULL) == GRAPH_FILES_OK);
	char *second = slurp(graph_path("g1"));
	CHECK_STREQ(second, first);
	free(first);
	free(second);
	teardown();
}

/* A hand-written file loads into the same graph; its first save is the
 * canonical form, and saving is a fixed point from then on. */
static void test_hand_written(void)
{
	setup("hand");
	const char *file =
		"{ 'outports': [], 'blocks': { 'B': { 'metadata': { 'route': 1e2, 'y': 3, "
		"'x': 4.0 }, 'component': 'math/Add2' }, 'A': { 'component': 'math/Add2' } },\n"
		"  'connections': [ { 'tgt': { 'port': 'in2', 'node': 'B' },"
		" 'src': { 'data': '5' } },"
		" { 'metadata': null, 'tgt': { 'node': 'B', 'port': 'in1' },"
		" 'src': { 'port': 'out', 'node': 'A' } } ],"
		"  'id': 'h', 'name': 'Hand' }";
	Graph *g = load(file, GRAPH_FILES_OK);
	const char *saved =
		"{'id':'h','name':'Hand','description':'','icon':'','started':false,"
		"'blocks':{'B':{'component':'math/Add2','metadata':{'x':4,'y':3,'route':100}},"
		"'A':{'component':'math/Add2','metadata':{'x':50,'y':50}}},"
		"'connections':[{'src':{'node':'A','port':'out'},'tgt':{'node':'B','port':'in1'}},"
		"{'src':{'data':5},'tgt':{'node':'B','port':'in2'}}],"
		"'inports':[],'outports':[]}";
	check_saved(g, jq(saved));
	CHECK(graphs_delete_graph(&gs, "h", "Hand", NULL) == GRAPH_OK);
	g = load(saved, GRAPH_FILES_OK);
	check_saved(g, jq(saved));

	/* An i64 initial (only the C API makes one) is stored as its exact
	 * integer, read back as an f64 (02 §3.4), and is canonical from then. */
	Linkable v = linkable_from_i64(INT64_C(1152921504606846977)); /* 2^60 + 1 */
	CHECK(graphs_add_initial(&gs, "h", "A", "in1", &v, NULL) == GRAPH_OK);
	size_t len;
	char *text = graph_files_serialize(g, &len);
	CHECK(text != NULL && strstr(text, "{\"data\":1152921504606846977}") != NULL);
	CHECK(graphs_delete_graph(&gs, "h", "Hand", NULL) == GRAPH_OK);
	CHECK(graph_files_load_text(&gf, text, len, "h", &g) == GRAPH_FILES_OK);
	free(text);
	CHECK(graph_find_block(g, "A")->inputs[0].initial.type == LINKABLE_F64);
	text = graph_files_serialize(g, &len);
	CHECK(text != NULL && strstr(text, "{\"data\":1.152921504606847e+18}") != NULL);
	CHECK(graphs_delete_graph(&gs, "h", "Hand", NULL) == GRAPH_OK);
	CHECK(graph_files_load_text(&gf, text, len, "h", &g) == GRAPH_FILES_OK);
	check_saved(g, text);
	free(text);
	CHECK_STREQ(logs, "");
	teardown();
}

/* ---- Started flag (05 §2.1, §2.2) ------------------------------------------- */

static void test_started(void)
{
	setup("started");
	const char *file = "{'id':'s','started':true,'blocks':{'A':{'component':'math/Add2'},"
			   "'B':{'component':'math/Add2'}},'connections':["
			   "{'src':{'data':2},'tgt':{'node':'A','port':'in1'}},"
			   "{'src':{'node':'A','port':'out'},'tgt':{'node':'B','port':'in1'}},"
			   "{'src':{'data':3},'tgt':{'node':'A','port':'in2'}}]}";
	Graph *g = load(file, GRAPH_FILES_OK);
	CHECK(g->started);
	/* Started after everything else is loaded. */
	CHECK_STREQ(events, "+graph;+node A;+node B;+init;+edge;+init;started;");
	runtime_drain(&rt, 100);
	CHECK_STREQ(input_value(g, "B", "in1"), "5");
	/* The current state is what persist writes. */
	char *text = graph_files_serialize(g, NULL);
	CHECK(text != NULL && strstr(text, "\"started\":true") != NULL);
	free(text);
	CHECK(graphs_stop(&gs, "s", NULL) == GRAPH_OK);
	text = graph_files_serialize(g, NULL);
	CHECK(text != NULL && strstr(text, "\"started\":false") != NULL);
	free(text);

	/* Absent or false: stopped, values only stored. */
	g = load("{'id':'t','blocks':{'A':{'component':'math/Add2'},'B':{'component':"
		 "'math/Add2'}},'connections':[{'src':{'data':2},'tgt':{'node':'A','port':'in1'}},"
		 "{'src':{'node':'A','port':'out'},'tgt':{'node':'B','port':'in1'}}]}",
		 GRAPH_FILES_OK);
	CHECK(!g->started);
	runtime_drain(&rt, 100);
	CHECK_STREQ(input_value(g, "B", "in1"), "0");
	CHECK_STREQ(input_value(g, "A", "in1"), "2");
	teardown();
}

/* ---- Placeholder blocks (05 §2.2) -------------------------------------------- */

static const char placeholder_file[] =
	"{'id':'p','name':'P','description':'','icon':'','started':true,"
	"'blocks':{'A':{'component':'math/Add2','metadata':{'x':50,'y':50}},"
	"'P':{'component':'vendor/Gizmo','metadata':{'x':1,'y':2,'change_threshold':0.25,"
	"'knob':{'v':0.30000000000000004}}},"
	"'B':{'component':'math/Add2','metadata':{'x':50,'y':50}}},"
	"'connections':[{'src':{'data':1},'tgt':{'node':'A','port':'in1'}},"
	"{'src':{'node':'A','port':'out'},'tgt':{'node':'P','port':'x'},'metadata':{'route':1}},"
	"{'src':{'node':'P','port':'y'},'tgt':{'node':'B','port':'in1'}},"
	"{'src':{'data':'abc'},'tgt':{'node':'P','port':'z'}},"
	"{'src':{'data':2.5},'tgt':{'node':'P','port':'w'}},"
	"{'src':{'node':'P','port':'q'},'tgt':{'node':'P','port':'r'}}],"
	"'inports':[{'name':'i','node':'P','port':'x'}],"
	"'outports':[{'name':'o','node':'P','port':'y'}]}";

static void test_placeholders(void)
{
	setup("placeholders");
	Graph *g = load(placeholder_file, GRAPH_FILES_OK);
	CHECK_STREQ(logs, "warn v: block P: unknown component vendor/Gizmo, kept as a "
			  "placeholder\n");
	CHECK_STREQ(events, "+graph;+node A;+node P;+node B;+init;started;");
	const Block *p = graph_find_block(g, "P");
	CHECK(p != NULL && block_is_placeholder(p) && p->input_count == 0 &&
	      p->output_count == 0);
	CHECK_STREQ(block_component(p), "vendor/Gizmo");
	CHECK(p->metadata.change_threshold == 0.25);
	CHECK(g->retained_count == 5);
	CHECK(g->retained_port_count[GRAPH_INPORT] == 1 &&
	      g->retained_port_count[GRAPH_OUTPORT] == 1);
	CHECK(g->started);
	runtime_drain(&rt, 100);
	CHECK_STREQ(input_value(g, "B", "in1"), "0"); /* no dataflow from P */
	/* Written back unchanged. */
	check_saved(g, jq(placeholder_file));

	/* Retained entries follow a rename, without events. */
	events[0] = '\0';
	CHECK(graphs_rename_node(&gs, "p", "P", "Q", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "renamenode;");
	char *text = graph_files_serialize(g, NULL);
	CHECK(text != NULL && strstr(text, "\"node\":\"P\"") == NULL &&
	      strstr(text, "\"node\":\"Q\"") != NULL);
	free(text);

	/* A live edge into B.in1 and a live inport called i replace the
	 * retained ones (the saved file never has fan-in). */
	CHECK(graphs_add_edge(&gs, "p", "A", "out", "B", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "p", GRAPH_INPORT, "i", "A", "in2", NULL) == GRAPH_OK);
	/* Renaming a live outport to a retained one's name replaces it too. */
	CHECK(graphs_add_port(&gs, "p", GRAPH_OUTPORT, "o2", "A", "out", NULL) == GRAPH_OK);
	CHECK(graphs_rename_port(&gs, "p", GRAPH_OUTPORT, "o2", "o", NULL) == GRAPH_OK);
	check_saved(g,
		    jq("{'id':'p','name':'P','description':'','icon':'','started':true,"
		       "'blocks':{'A':{'component':'math/Add2','metadata':{'x':50,'y':50}},"
		       "'Q':{'component':'vendor/Gizmo','metadata':{'x':1,'y':2,"
		       "'change_threshold':0.25,'knob':{'v':0.30000000000000004}}},"
		       "'B':{'component':'math/Add2','metadata':{'x':50,'y':50}}},"
		       "'connections':[{'src':{'data':1},'tgt':{'node':'A','port':'in1'}},"
		       "{'src':{'node':'A','port':'out'},'tgt':{'node':'B','port':'in1'}},"
		       "{'src':{'node':'A','port':'out'},'tgt':{'node':'Q','port':'x'},"
		       "'metadata':{'route':1}},"
		       "{'src':{'data':'abc'},'tgt':{'node':'Q','port':'z'}},"
		       "{'src':{'data':2.5},'tgt':{'node':'Q','port':'w'}},"
		       "{'src':{'node':'Q','port':'q'},'tgt':{'node':'Q','port':'r'}}],"
		       "'inports':[{'name':'i','node':'A','port':'in2'}],"
		       "'outports':[{'name':'o','node':'A','port':'out'}]}"));

	/* A live initial replaces a retained connection too. */
	CHECK(graph_retain_connection(g, &(RetainedConnection){.src = graph_find_block(g, "Q"),
							       .src_port = (char *)"y",
							       .tgt = graph_find_block(g, "B"),
							       .tgt_port = (char *)"in2"}) ==
	      GRAPH_OK);
	CHECK(graph_input_retained(g, graph_find_block(g, "B"), "in2"));
	Linkable seven = linkable_from_f64(7);
	CHECK(graphs_add_initial(&gs, "p", "B", "in2", &seven, NULL) == GRAPH_OK);
	CHECK(!graph_input_retained(g, graph_find_block(g, "B"), "in2"));
	CHECK(graphs_remove_initial(&gs, "p", "B", "in2", NULL) == GRAPH_OK);

	/* Changing a placeholder's metadata works like any block's. */
	BlockChange chg = {.set_x = true, .x = 9};
	CHECK(graphs_change_node(&gs, "p", "Q", &chg, NULL) == GRAPH_OK);
	CHECK(graph_find_block(g, "Q")->metadata.x == 9);
	/* Removing it drops its retained entries, silently: only the node. */
	events[0] = '\0';
	CHECK(graphs_remove_node(&gs, "p", "Q", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "-node;");
	CHECK(g->retained_count == 0 && g->retained_port_count[GRAPH_INPORT] == 0);
	text = graph_files_serialize(g, NULL);
	CHECK(text != NULL && strstr(text, "Gizmo") == NULL);
	free(text);

	/* A placeholder that is the target of a live-port-name clash. */
	CHECK(graph_retain_port(g, GRAPH_INPORT, "i", graph_find_block(g, "A"), "in1") ==
	      GRAPH_ERR_INVALID_ARGUMENT); /* A is no placeholder */
	teardown();
}

/* ---- Invalid entries and malformed files (05 §2.2) ----------------------------- */

static void test_invalid_entries(void)
{
	setup("invalid");
	char file[4096] = "{'id':'v','blocks':{'A':{'component':'math/Add2'},"
			  "'B':{'component':'math/Add2','metadata':{'x':'bad'}},"
			  "'C':{'metadata':{}},'D':'x','";
	char long_id[GRAPH_ID_MAX + 2];
	memset(long_id, 'L', sizeof(long_id) - 1);
	long_id[GRAPH_ID_MAX + 1] = '\0';
	append(file, sizeof(file), "%s", long_id);
	append(file, sizeof(file), "%s",
	       "':{'component':'math/Add2'},'E':{'component':'math/Add2'},"
	       "'A':{'component':'math/Mul2'},'P':{'component':'nope/Type'}},"
	       "'connections':["
	       "{'src':{'node':'A','port':'out'},'tgt':{'node':'E','port':'in1'}},"
	       "{'src':{'node':'A','port':'out'},'tgt':{'node':'E','port':'in1'}},"
	       "{'src':{'data':3},'tgt':{'node':'E','port':'in1'}},"
	       "{'src':{'node':'X','port':'out'},'tgt':{'node':'E','port':'in2'}},"
	       "{'src':{'node':'A','port':'nope'},'tgt':{'node':'E','port':'in2'}},"
	       "{'src':{'node':'A','port':'in1'},'tgt':{'node':'E','port':'in2'}},"
	       "{'src':{'data':4},'tgt':{'node':'E','port':'out'}},"
	       "{'src':{'data':[1]},'tgt':{'node':'E','port':'in2'}},"
	       "{'src':{'node':'A','port':'out'}},"
	       "{'src':{'node':'A','port':'out'},'tgt':{'node':'E','port':'in2'},'metadata':5},"
	       "{'src':{'node':'P','port':'y'},'tgt':{'node':'A','port':'in1'}},"
	       "{'src':{'data':1},'tgt':{'node':'A','port':'in1'}},"
	       "{'src':{'node':'P','port':'y'},'tgt':{'node':'A','port':'nope'}},"
	       "{'src':{'node':'P','port':'y'},'tgt':{'node':'A','port':'out'}},"
	       "{'src':{'node':'A','port':'in2'},'tgt':{'node':'P','port':'x'}},"
	       "{'src':{'node':'A','port':'out'},'tgt':{'node':'P','port':'x'}},"
	       "{'src':{'data':1},'tgt':{'node':'P','port':'x'}},"
	       "{'src':{'node':'P','port':'y'},'tgt':{'node':'E','port':'in1'}},"
	       "{'src':{'data':2},'tgt':{'node':'Z','port':'in1'}},"
	       "{'src':{'node':'','port':'out'},'tgt':{'node':'E','port':'in2'}},"
	       "7],"
	       "'inports':[{'name':'i','node':'A','port':'in2'},"
	       "{'name':'i','node':'E','port':'in2'},{'name':'j','node':'A','port':'out'},"
	       "{'name':'k','node':'P','port':'x'},{'name':'k','node':'E','port':'in2'},"
	       "{'name':'i','node':'P','port':'x'},{'name':5},{'name':'','node':'A','port':'in1'}],"
	       "'outports':[{'name':'o','node':'A','port':'in1'}]}");
	Graph *g = load(file, GRAPH_FILES_OK);
	char want[4096];
	snprintf(want, sizeof(want),
		 "warn v: block B skipped: invalid metadata\n"
		 "warn v: block C skipped: no component\n"
		 "warn v: block D skipped: no component\n"
		 "warn v: block %s skipped: Invalid id\n"
		 "warn v: block A skipped: Node exists: A\n"
		 "warn v: block P: unknown component nope/Type, kept as a placeholder\n"
		 "warn v: connection skipped: Input already connected: E.in1\n"
		 "warn v: initial skipped: Input already connected: E.in1\n"
		 "warn v: connection skipped: Unknown node: X\n"
		 "warn v: connection skipped: Unknown port: A.nope\n"
		 "warn v: connection skipped: Invalid edge direction\n"
		 "warn v: initial skipped: Invalid edge direction\n"
		 "warn v: connection skipped: invalid entry\n"
		 "warn v: connection skipped: invalid entry\n"
		 "warn v: connection skipped: invalid metadata\n"
		 "warn v: initial skipped: Input already connected: A.in1\n"
		 "warn v: connection skipped: Unknown port: A.nope\n"
		 "warn v: connection skipped: Invalid edge direction\n"
		 "warn v: connection skipped: Invalid edge direction\n"
		 "warn v: connection skipped: Input already connected: P.x\n"
		 "warn v: connection skipped: Input already connected: E.in1\n"
		 "warn v: initial skipped: Unknown node: Z\n"
		 "warn v: connection skipped: Invalid id\n"
		 "warn v: connection skipped: invalid entry\n"
		 "warn v: inport skipped: Port exists: i\n"
		 "warn v: inport skipped: Unknown port: A.out\n"
		 "warn v: inport skipped: Port exists: k\n"
		 "warn v: inport skipped: Port exists: i\n"
		 "warn v: inport skipped: invalid entry\n"
		 "warn v: inport skipped: Invalid id\n"
		 "warn v: outport skipped: Unknown port: A.in1\n",
		 long_id);
	CHECK_STREQ(logs, want);
	/* What remains is valid, and saves as such. */
	check_saved(g, jq("{'id':'v','name':'<<new graph>>','description':'','icon':'',"
			  "'started':false,'blocks':{"
			  "'A':{'component':'math/Add2','metadata':{'x':50,'y':50}},"
			  "'E':{'component':'math/Add2','metadata':{'x':50,'y':50}},"
			  "'P':{'component':'nope/Type','metadata':{'x':50,'y':50}}},"
			  "'connections':["
			  "{'src':{'node':'A','port':'out'},'tgt':{'node':'E','port':'in1'}},"
			  "{'src':{'node':'P','port':'y'},'tgt':{'node':'A','port':'in1'}},"
			  "{'src':{'node':'A','port':'out'},'tgt':{'node':'P','port':'x'}}],"
			  "'inports':[{'name':'i','node':'A','port':'in2'},"
			  "{'name':'k','node':'P','port':'x'}],'outports':[]}"));
	teardown();
}

static void test_malformed(void)
{
	setup("malformed");
	const char *bad[] = {
		"",
		"not json",
		"{'id':'x'",
		"[]",
		"'x'",
		"{}",
		"{'id':''}",
		"{'id':7}",
		"{'id':'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'}",
		"{'id':'x','name':3}",
		"{'id':'x','description':null}",
		"{'id':'x','icon':[]}",
		"{'id':'x','started':'true'}",
		"{'id':'x','blocks':[]}",
		"{'id':'x','connections':{}}",
		"{'id':'x','inports':{}}",
		"{'id':'x','outports':'none'}",
		"{'id':'x','blocks':{'A':{'component':'math/Add2'}}} trailing",
		NULL,
	};
	for (int i = 0; bad[i] != NULL; i++) {
		logs[0] = events[0] = '\0';
		load(bad[i], GRAPH_FILES_ERR_MALFORMED);
		CHECK_STREQ(logs, "error v: not a valid graph file\n");
		CHECK_STREQ(events, "");
	}
	CHECK(graphs_count(&gs) == 0);

	/* Too deep for the parser (CJSON_NESTING_LIMIT): malformed too. */
	char deep[200] = "{'id':'x','m':";
	for (int i = 0; i < 40; i++)
		strcat(deep, "[");
	for (int i = 0; i < 40; i++)
		strcat(deep, "]");
	strcat(deep, "}");
	load(deep, GRAPH_FILES_ERR_MALFORMED);

	/* The minimum valid document. */
	logs[0] = '\0';
	Graph *g = load("{'id':'x'}", GRAPH_FILES_OK);
	CHECK_STREQ(g->name, GRAPHS_DEFAULT_NAME);
	CHECK(!g->started && g->block_count == 0);
	CHECK_STREQ(logs, "");
	/* The same id again. */
	load("{'id':'x'}", GRAPH_FILES_ERR_EXISTS);
	CHECK_STREQ(logs, "error v: graph not loaded: Graph exists: x\n");

	/* Files: missing, a directory, too big. */
	char path[PAL_PATH_MAX];
	snprintf(path, sizeof(path), "%s", join(gf.datadir, "none.json"));
	logs[0] = '\0';
	CHECK(graph_files_load(&gf, path, NULL) == GRAPH_FILES_ERR_NOT_FOUND);
	CHECK(strstr(logs, "not found") != NULL);
	CHECK(graph_files_load(&gf, gf.datadir, NULL) == GRAPH_FILES_ERR_STORAGE);
	size_t size = HAKKAS_GRAPH_FILE_MAX + 1;
	char *big = malloc(size);
	memset(big, ' ', size);
	memcpy(big, "{\"id\":\"big\"}", 12);
	snprintf(path, sizeof(path), "%s", join(gf.datadir, "big.json"));
	CHECK(pal_storage_write(path, big, size) == PAL_OK);
	CHECK(graph_files_load(&gf, path, NULL) == GRAPH_FILES_ERR_TOO_BIG);
	CHECK(pal_storage_write(path, big, size - 1) == PAL_OK); /* at the limit: fine */
	CHECK(graph_files_load(&gf, path, NULL) == GRAPH_FILES_OK);
	free(big);
	CHECK(graphs_find(&gs, "big") != NULL);
	teardown();
}

/* ---- Persist and delete (02 §3.2, §3.3) ---------------------------------------- */

static void test_persist(void)
{
	setup("persist");
	CHECK(graphs_new_graph(&gs, "math/x", "Name (with) stuff", NULL, NULL, NULL, NULL) ==
	      GRAPH_OK);
	events[0] = '\0';
	GraphsMessage m;
	/* graphs/ is created on demand. */
	CHECK(graph_files_persist(&gf, "math/x", &m) == GRAPH_FILES_OK);
	CHECK_STREQ(m.text, "");
	CHECK_STREQ(events, ""); /* no reply on success */
	char path[PAL_PATH_MAX];
	snprintf(path, sizeof(path), "%s", join(gf.datadir, "graphs/math%2Fx.json"));
	char *text = slurp(path);
	CHECK(text != NULL && strncmp(text, "{\"id\":\"math/x\"", 14) == 0);
	free(text);

	/* Refusals go to the requester only: no event. */
	CHECK(graph_files_persist(&gf, "nope", &m) == GRAPH_FILES_ERR_NOT_FOUND);
	CHECK_STREQ(m.text, "Unknown graph: nope");
	CHECK(graph_files_persist(&gf, "", &m) == GRAPH_FILES_ERR_INVALID);
	CHECK_STREQ(m.text, "Invalid id");
	CHECK_STREQ(events, "");
	teardown();

	/* A failed write: graphs/ is a file here. */
	setup("persist-fail");
	snprintf(path, sizeof(path), "%s", join(gf.datadir, "graphs"));
	put(path, "x");
	CHECK(graphs_new_graph(&gs, "g", "My graph", NULL, NULL, NULL, NULL) == GRAPH_OK);
	events[0] = logs[0] = '\0';
	CHECK(graph_files_persist(&gf, "g", &m) == GRAPH_FILES_ERR_STORAGE);
	CHECK_STREQ(m.text, "Unable to save graph: My graph (g)");
	/* Reported as a system-wide error event (02 §3.3). */
	CHECK_STREQ(events, "error g: Unable to save graph: My graph (g);");
	CHECK(strstr(logs, "error cannot create directory") != NULL);
	/* A long name is not cut in the event. */
	char name[300];
	memset(name, 'n', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	CHECK(graphs_rename_graph(&gs, "g", name, NULL) == GRAPH_OK);
	events[0] = '\0';
	CHECK(graph_files_persist(&gf, "g", NULL) == GRAPH_FILES_ERR_STORAGE);
	CHECK(strlen(events) == strlen("error g: Unable to save graph:  (g);") + 299);
	teardown();
}

static void test_delete(void)
{
	setup("delete");
	CHECK(graphs_new_graph(&gs, "g", "G", NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graph_files_persist(&gf, "g", NULL) == GRAPH_FILES_OK);
	CHECK(pal_storage_exists(graph_path("g")));
	GraphsMessage m;
	/* A refused delete keeps the graph and its file. */
	CHECK(graph_files_delete(&gf, "g", "wrong", &m) == GRAPH_ERR_NAME_MISMATCH);
	CHECK_STREQ(m.text, "Graph name mismatch: g");
	CHECK(pal_storage_exists(graph_path("g")));
	CHECK(graph_files_delete(&gf, "nope", "G", &m) == GRAPH_ERR_UNKNOWN_GRAPH);
	events[0] = '\0';
	CHECK(graph_files_delete(&gf, "g", "G", &m) == GRAPH_OK);
	CHECK_STREQ(events, "-graph;");
	CHECK(!pal_storage_exists(graph_path("g")));
	CHECK(graphs_find(&gs, "g") == NULL);
	/* A graph that was never persisted has no file: fine. */
	CHECK(graphs_new_graph(&gs, "h", "H", NULL, NULL, NULL, NULL) == GRAPH_OK);
	logs[0] = '\0';
	CHECK(graph_files_delete(&gf, "h", "H", NULL) == GRAPH_OK);
	CHECK_STREQ(logs, "");
	teardown();
}

/* ---- Loading many (05 §1.2, §1.4 step 7) -------------------------------------- */

static void test_load_all(void)
{
	setup("all");
	/* No graphs/ directory yet. */
	size_t n = 99;
	CHECK(graph_files_load_all(&gf, &n) == GRAPH_FILES_ERR_NOT_FOUND && n == 0);
	CHECK(graph_files_make_dirs(&gf) == GRAPH_FILES_OK);
	logs[0] = '\0';
	CHECK(graph_files_load_all(&gf, &n) == GRAPH_FILES_OK && n == 0);

	char dir[PAL_PATH_MAX], path[PAL_PATH_MAX];
	CHECK(graph_files_dir(&gf, GRAPH_FILES_GRAPHS_DIR, dir, sizeof(dir)) == GRAPH_FILES_OK);
	put(graph_path("b"), jq("{'id':'b','started':true}"));
	put(graph_path("a/1"), jq("{'id':'a/1'}"));
	snprintf(path, sizeof(path), "%s", join(dir, "c.json"));
	put(path, jq("{'id':'c-other'}")); /* loaded, with a warning */
	snprintf(path, sizeof(path), "%s", join(dir, "d.json"));
	put(path, "nonsense");
	snprintf(path, sizeof(path), "%s", join(dir, "e.json.tmp")); /* a leftover: not loaded */
	put(path, jq("{'id':'tmp'}"));
	snprintf(path, sizeof(path), "%s", join(dir, "notes.txt"));
	put(path, jq("{'id':'txt'}"));
	snprintf(path, sizeof(path), "%s", join(dir, "sub.json"));
	CHECK(pal_storage_mkdir(path) == PAL_OK);

	events[0] = logs[0] = '\0';
	CHECK(graph_files_load_all(&gf, &n) == GRAPH_FILES_ERR_MALFORMED);
	CHECK(n == 3);
	/* In name order: a%2F1, b, c, (d fails). */
	CHECK_STREQ(events, "+graph;+graph;started;+graph;");
	CHECK(graphs_count(&gs) == 3);
	CHECK_STREQ(graphs_at(&gs, 0)->id, "a/1");
	CHECK_STREQ(graphs_at(&gs, 1)->id, "b");
	CHECK_STREQ(graphs_at(&gs, 2)->id, "c-other");
	CHECK(graphs_at(&gs, 1)->started);
	char want[1024];
	snprintf(want, sizeof(want),
		 "warn %s/c.json: graph c-other is saved as c-other.json, not here\n"
		 "error %s/d.json: not a valid graph file\n",
		 dir, dir);
	CHECK_STREQ(logs, want);
	teardown();
}

static void test_load_list(void)
{
	setup("list");
	char a[PAL_PATH_MAX], b[PAL_PATH_MAX], list[3 * PAL_PATH_MAX];
	snprintf(a, sizeof(a), "%s", join(gf.datadir, "one.json"));
	snprintf(b, sizeof(b), "%s", join(gf.datadir, "two.json"));
	put(a, jq("{'id':'one'}"));
	put(b, jq("{'id':'two'}"));
	size_t n = 0;
	snprintf(list, sizeof(list), "%s:%s", b, a);
	CHECK(graph_files_load_list(&gf, list, &n) == GRAPH_FILES_OK && n == 2);
	CHECK_STREQ(graphs_at(&gs, 0)->id, "two"); /* in list order */
	CHECK_STREQ(graphs_at(&gs, 1)->id, "one");
	/* Errors: the first is returned, the rest still load. */
	teardown();
	setup("list2");
	snprintf(a, sizeof(a), "%s", join(gf.datadir, "one.json"));
	put(a, jq("{'id':'one'}"));
	snprintf(list, sizeof(list), "%s/missing.json::%s", gf.datadir, a);
	logs[0] = '\0';
	CHECK(graph_files_load_list(&gf, list, &n) == GRAPH_FILES_ERR_NOT_FOUND && n == 1);
	CHECK(strstr(logs, "invalid graph file name in list") != NULL);
	CHECK(graphs_find(&gs, "one") != NULL);
	CHECK(graph_files_load_list(&gf, "", &n) == GRAPH_FILES_ERR_INVALID && n == 0);
	teardown();
}

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb;
	(void)flag;
	(void)ftw;
	return remove(path);
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(log_add_sink(log_sink, NULL));
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	CHECK(block_types_register(&sink_type) == BLOCK_TYPES_OK);
	const char *base = getenv("TMPDIR");
	snprintf(root, sizeof(root), "%s/hakkas-files-XXXXXX", base && *base ? base : "/tmp");
	if (mkdtemp(root) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	RUN(test_file_names);
	RUN(test_paths);
	RUN(test_save_canonical);
	RUN(test_round_trip);
	RUN(test_hand_written);
	RUN(test_started);
	RUN(test_placeholders);
	RUN(test_invalid_entries);
	RUN(test_malformed);
	RUN(test_persist);
	RUN(test_delete);
	RUN(test_load_all);
	RUN(test_load_list);

	block_types_reset();
	log_remove_sink(log_sink, NULL);
	if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
	return test_result();
}
