/*
 * Composite block types (03 §5; plan task 21): the authoring format and
 * every registration refusal (blocktypes/composite_types.h), the canonical
 * document and its round trip through text and type files (app/type_files.h),
 * a composite inside a composite, instances in a graph on the generic
 * runtime (values in through an exposed input, through the inner blocks, out
 * through an exposed output; inner initials; an unwired exposed input
 * reverting to the inner default; stale queued values; start, stop and
 * removal reaching the inner blocks, a timing/Interval inside included;
 * watches; rename; the change threshold override; one node to the graph),
 * a graph with an instance saved and reloaded, the registry operations
 * refused while in use, loading types/ in dependency order with the
 * leftovers logged, and assembling a group from a selection (03 §5.2).
 * Files live in a fresh temporary directory, removed afterwards.
 */
#define _XOPEN_SOURCE 700

#include "test.h"

#include <ftw.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

#include "app/file_names.h"
#include "app/graph_files.h"
#include "app/type_files.h"
#include "blocktypes/block_types.h"
#include "blocktypes/composite_types.h"
#include "graphs/graph_json.h"
#include "platform/pal.h"
#include "system/log.h"

static char root[PAL_PATH_MAX];
static Runtime rt;
static Graphs gs;
static GraphFiles gf;
static size_t builtins;

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
	for (size_t i = 0; i <= n && i < sizeof(buf[0]); i++)
		b[i] = s[i] == '\'' ? '"' : s[i];
	return b;
}

static char logs[16384];

static void log_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)len;
	append(logs, sizeof(logs), "%s %s\n", log_level_name(level), line);
}

/* Node events, one short entry each. */
static char events[4096];
static GraphsSubscriber sub;

static void on_event(void *ctx, const GraphEvent *ev)
{
	(void)ctx;
	switch (ev->type) {
	case GRAPH_EVENT_BLOCK_ADDED:
		append(events, sizeof(events), "+node %s;", ev->block->name);
		break;
	case GRAPH_EVENT_BLOCK_REMOVED:
		append(events, sizeof(events), "-node %s;", ev->block->name);
		break;
	case GRAPH_EVENT_EDGE_ADDED:
	case GRAPH_EVENT_EDGE_REMOVED:
		append(events, sizeof(events), "%cedge %s.%s>%s.%s;",
		       ev->type == GRAPH_EVENT_EDGE_ADDED ? '+' : '-', ev->src_block->name,
		       block_output_port(ev->src_block, ev->src_output)->name, ev->block->name,
		       block_input_port(ev->block, ev->input)->name);
		break;
	case GRAPH_EVENT_INITIAL_ADDED:
	case GRAPH_EVENT_INITIAL_REMOVED:
		append(events, sizeof(events), "%cinit %s.%s;",
		       ev->type == GRAPH_EVENT_INITIAL_ADDED ? '+' : '-', ev->block->name,
		       block_input_port(ev->block, ev->input)->name);
		break;
	default:
		break;
	}
}

static uint32_t fixed_random_n;

static enum pal_err fixed_random(void *buf, size_t len)
{
	uint8_t *p = buf;
	for (size_t i = 0; i < len; i++)
		p[i] = (uint8_t)(fixed_random_n++ * 37u + i);
	return PAL_OK;
}

static void setup(void)
{
	runtime_init(&rt);
	graphs_init(&gs, &rt, block_types_get, fixed_random);
	graphs_subscriber_init(&sub, on_event, NULL);
	graphs_subscribe(&gs, &sub);
	events[0] = '\0';
	logs[0] = '\0';
}

static void teardown(void)
{
	graphs_deinit(&gs);
	runtime_deinit(&rt);
	composite_types_reset();
	CHECK(block_types_count() == builtins);
}

static enum composite_status reg(const char *json, CompositeMessage *m)
{
	const char *t = jq(json);
	return composite_types_register_text(t, strlen(t), m, NULL);
}

static void refused(const char *json, enum composite_status st, const char *message)
{
	size_t before = block_types_count();
	CompositeMessage m;
	enum composite_status got = reg(json, &m);
	CHECK(got == st);
	CHECK_STREQ(m.text, message);
	if (got != st)
		fprintf(stderr, "  for %s\n", json);
	CHECK(block_types_count() == before);
}

static const char *source(const char *name)
{
	static char buf[4][4096];
	static int next;
	char *b = buf[next++ % 4];
	char *s = composite_types_source(block_types_get(name), NULL);
	snprintf(b, 4096, "%s", s != NULL ? s : "(null)");
	free(s);
	return b;
}

static Block *node(const char *graph, const char *name)
{
	Graph *g = graphs_find(&gs, graph);
	return g != NULL ? graph_find_block(g, name) : NULL;
}

static const Linkable *in_value(const char *graph, const char *name, const char *port)
{
	Block *b = node(graph, name);
	int32_t i = b != NULL ? block_input_index(b, port) : -1;
	CHECK(i >= 0);
	return i >= 0 ? block_input_value(b, (uint32_t)i) : NULL;
}

static double in_f64(const char *graph, const char *name, const char *port)
{
	const Linkable *v = in_value(graph, name, port);
	return v != NULL ? linkable_to_f64(v) : NAN;
}

static void set_initial(const char *graph, const char *name, const char *port, double v)
{
	Linkable l = linkable_from_f64(v);
	CHECK(graphs_add_initial(&gs, graph, name, port, &l, NULL) == GRAPH_OK);
}

static void drain(void)
{
	runtime_drain(&rt, SIZE_MAX);
}

/* ---- Test types --------------------------------------------------------------- */

/* a + b + c: A.out feeds B.in1. Port b gives everything explicitly. */
#define SUM3                                                                                       \
	"{'name':'t/Sum3','description':'a+b+c','icon':'add','blocks':[{'name':'A','type':"       \
	"'math/Add2'},{'name':'B','type':'math/Add2'}],'connections':[{'src':'A.out','tgt':"      \
	"'B.in1'}],'inports':[{'name':'a','tgt':'A.in1'},{'name':'b','tgt':'A.in2','type':"       \
	"'number','description':'Second','addressable':true},{'name':'c','tgt':'B.in2'}],"         \
	"'outports':[{'name':'sum','src':'B.out'}]}"

#define SUM3_CANONICAL                                                                             \
	"{'name':'t/Sum3','icon':'add','description':'a+b+c','blocks':[{'name':'A','type':"       \
	"'math/Add2'},{'name':'B','type':'math/Add2'}],'connections':[{'src':'A.out','tgt':"      \
	"'B.in1'}],'initials':[],'inports':[{'name':'a','tgt':'A.in1'},{'name':'b','tgt':"        \
	"'A.in2','type':'number','description':'Second','addressable':true},{'name':'c','tgt':"   \
	"'B.in2'}],'outports':[{'name':'sum','src':'B.out'}]}"

/* A periodic pulse inside a composite (03 §3.1). */
#define BLINK                                                                                      \
	"{'name':'t/Blink','blocks':[{'name':'I','type':'timing/Interval'}],'inports':[{'name':"  \
	"'ms','tgt':'I.milliseconds'},{'name':'duty','tgt':'I.duty-cycle'}],'outports':[{'name':" \
	"'out','src':'I.out'}]}"

/* k*in+m with inner initials k = "5" (parsed for a number port) and m = 0.1. */
#define LIN                                                                                        \
	"{'name':'t/Lin','blocks':[{'name':'L','type':'process/Linear'}],'initials':[{'src':'5',"  \
	"'tgt':'L.k'},{'src':0.1,'tgt':'L.m'}],'inports':[{'name':'in','tgt':'L.in'}],"            \
	"'outports':[{'name':'out','src':'L.out'}]}"

/* ---- Registration refusals (03 §5.1) ------------------------------------------ */

static void test_refusals(void)
{
	setup();
	refused("not json", COMPOSITE_ERR_INVALID, "Invalid JSON");
	refused("{'name':'t/X','blocks':[]} x", COMPOSITE_ERR_INVALID, "Invalid JSON");
	refused("[]", COMPOSITE_ERR_INVALID, "Invalid JSON");
	refused("{}", COMPOSITE_ERR_INVALID, "Invalid field: name");
	refused("{'name':'','blocks':[]}", COMPOSITE_ERR_INVALID, "Invalid field: name");
	refused("{'name':'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',"
		"'blocks':[]}",
		COMPOSITE_ERR_INVALID, "Invalid field: name");
	refused("{'name':'t/X'}", COMPOSITE_ERR_INVALID, "Invalid field: blocks");
	refused("{'name':'t/X','icon':5,'blocks':[]}", COMPOSITE_ERR_INVALID,
		"Invalid field: icon");
	refused("{'name':'t/X','description':[],'blocks':[]}", COMPOSITE_ERR_INVALID,
		"Invalid field: description");
	refused("{'name':'t/X','blocks':[{'name':'A'}]}", COMPOSITE_ERR_INVALID,
		"Invalid field: blocks");
	refused("{'name':'t/X','blocks':[{'name':'','type':'math/Add2'}]}", COMPOSITE_ERR_INVALID,
		"Invalid field: blocks");
	refused("{'name':'t/X','blocks':[{'name':'A','type':'math/Add2'},{'name':'A','type':"
		"'math/Add2'}]}",
		COMPOSITE_ERR_INVALID, "Invalid field: blocks");
	refused("{'name':'t/X','blocks':[],'connections':{}}", COMPOSITE_ERR_INVALID,
		"Invalid field: connections");
	refused("{'name':'t/X','blocks':[],'connections':[{'src':'nodot','tgt':'A.in1'}]}",
		COMPOSITE_ERR_INVALID, "Invalid field: connections");
	refused("{'name':'t/X','blocks':[],'connections':[{'src':'A.','tgt':'A.in1'}]}",
		COMPOSITE_ERR_INVALID, "Invalid field: connections");
	/* The former name "edges" (03 §5.1): checked the same way, and never
	 * beside "connections". */
	refused("{'name':'t/X','blocks':[],'edges':{}}", COMPOSITE_ERR_INVALID,
		"Invalid field: edges");
	refused("{'name':'t/X','blocks':[],'edges':[{'src':'nodot','tgt':'A.in1'}]}",
		COMPOSITE_ERR_INVALID, "Invalid field: edges");
	refused("{'name':'t/X','blocks':[],'connections':[],'edges':[]}", COMPOSITE_ERR_INVALID,
		"Invalid field: edges");
	refused("{'name':'t/X','blocks':[],'edges':[],'connections':{}}", COMPOSITE_ERR_INVALID,
		"Invalid field: edges");
	refused("{'name':'t/X','blocks':[],'initials':[{'src':{},'tgt':'A.in1'}]}",
		COMPOSITE_ERR_INVALID, "Invalid field: initials");
	refused("{'name':'t/X','blocks':[],'initials':[{'src':null,'tgt':'A.in1'}]}",
		COMPOSITE_ERR_INVALID, "Invalid field: initials");
	refused("{'name':'t/X','blocks':[],'inports':[{'name':'a.b','tgt':'A.in1'}]}",
		COMPOSITE_ERR_INVALID, "Invalid field: inports");
	refused("{'name':'t/X','blocks':[],'inports':[{'name':'a','tgt':'A.in1'},{'name':'a',"
		"'tgt':'A.in2'}]}",
		COMPOSITE_ERR_INVALID, "Invalid field: inports");
	refused("{'name':'t/X','blocks':[],'inports':[{'name':'a','tgt':'A.in1','type':'link'}]}",
		COMPOSITE_ERR_INVALID, "Invalid field: inports");
	refused("{'name':'t/X','blocks':[],'inports':[{'name':'a','tgt':'A.in1','addressable':1}]}",
		COMPOSITE_ERR_INVALID, "Invalid field: inports");
	refused("{'name':'t/X','blocks':[],'inports':[{'name':'a','tgt':'A.in1','description':5}]}",
		COMPOSITE_ERR_INVALID, "Invalid field: inports");
	refused("{'name':'t/X','blocks':[],'outports':[{'name':'a','tgt':'A.out'}]}",
		COMPOSITE_ERR_INVALID, "Invalid field: outports");

	/* Already registered: a built-in, and a composite. */
	refused("{'name':'math/Add2','blocks':[]}", COMPOSITE_ERR_EXISTS,
		"Component exists: math/Add2");
	CompositeMessage m;
	CHECK(reg(SUM3, &m) == COMPOSITE_OK);
	CHECK_STREQ(m.text, "");
	refused(SUM3, COMPOSITE_ERR_EXISTS, "Component exists: t/Sum3");

	/* Recursion, directly; an unknown inner type. */
	refused("{'name':'t/R','blocks':[{'name':'A','type':'math/Add2'},{'name':'R','type':"
		"'t/R'}]}",
		COMPOSITE_ERR_RECURSIVE, "Recursive component: t/R");
	refused("{'name':'t/X','blocks':[{'name':'A','type':'nope/Nope'}]}",
		COMPOSITE_ERR_UNKNOWN_TYPE, "Unknown component: nope/Nope");

	/* References that do not resolve, as the wire commands report them. */
#define TWO                                                                                        \
	"{'name':'t/X','blocks':[{'name':'A','type':'math/Add2'},{'name':'B','type':"             \
	"'math/Add2'}],"
	refused(TWO "'edges':[{'src':'Q.out','tgt':'B.in1'}]}", COMPOSITE_ERR_UNKNOWN_PORT,
		"Unknown node: Q"); /* the former name, resolved alike */
	refused(TWO "'connections':[{'src':'A.zz','tgt':'B.in1'}]}", COMPOSITE_ERR_UNKNOWN_PORT,
		"Unknown port: A.zz");
	refused(TWO "'connections':[{'src':'A.in1','tgt':'B.in1'}]}", COMPOSITE_ERR_UNKNOWN_PORT,
		"Invalid edge direction");
	refused(TWO "'connections':[{'src':'A.out','tgt':'B.out'}]}", COMPOSITE_ERR_UNKNOWN_PORT,
		"Invalid edge direction");
	refused(TWO "'connections':[{'src':'A.out','tgt':'Q.in1'}]}", COMPOSITE_ERR_UNKNOWN_PORT,
		"Unknown node: Q");
	refused(TWO "'initials':[{'src':1,'tgt':'A.out'}]}", COMPOSITE_ERR_UNKNOWN_PORT,
		"Invalid edge direction");
	refused(TWO "'initials':[{'src':1,'tgt':'A.in9'}]}", COMPOSITE_ERR_UNKNOWN_PORT,
		"Unknown port: A.in9");
	refused(TWO "'inports':[{'name':'a','tgt':'A.out'}]}", COMPOSITE_ERR_UNKNOWN_PORT,
		"Unknown port: A.out");
	refused(TWO "'inports':[{'name':'a','tgt':'Z.in1'}]}", COMPOSITE_ERR_UNKNOWN_PORT,
		"Unknown node: Z");
	refused(TWO "'outports':[{'name':'a','src':'A.in1'}]}", COMPOSITE_ERR_UNKNOWN_PORT,
		"Unknown port: A.in1");
	refused(TWO "'outports':[{'name':'a','src':'A.nope'}]}", COMPOSITE_ERR_UNKNOWN_PORT,
		"Unknown port: A.nope");

	/* An inner input fed twice (no fan-in, 01 §2.2). */
	refused(TWO "'connections':[{'src':'A.out','tgt':'B.in1'},{'src':'B.out','tgt':'B.in1'}]}",
		COMPOSITE_ERR_FAN_IN, "Input already connected: B.in1");
	refused(TWO "'connections':[{'src':'A.out','tgt':'B.in1'}],'initials':[{'src':1,'tgt':"
		    "'B.in1'}]}",
		COMPOSITE_ERR_FAN_IN, "Input already connected: B.in1");
	refused(TWO "'initials':[{'src':1,'tgt':'A.in1'},{'src':2,'tgt':'A.in1'}]}",
		COMPOSITE_ERR_FAN_IN, "Input already connected: A.in1");
	refused(TWO "'initials':[{'src':1,'tgt':'A.in1'}],'inports':[{'name':'a','tgt':'A.in1'}]}",
		COMPOSITE_ERR_FAN_IN, "Input already connected: A.in1");
	refused(TWO "'connections':[{'src':'A.out','tgt':'B.in1'}],'inports':[{'name':'a','tgt':"
		    "'B.in1'}]}",
		COMPOSITE_ERR_FAN_IN, "Input already connected: B.in1");
	refused(TWO "'inports':[{'name':'a','tgt':'A.in1'},{'name':'b','tgt':'A.in1'}]}",
		COMPOSITE_ERR_FAN_IN, "Input already connected: A.in1");
	/* An output may be exposed twice, and an inner name contain dots (split
	 * on the last one, 01 §2.4). */
	CHECK(reg("{'name':'t/Dots','blocks':[{'name':'a.b','type':'math/Add2'}],'outports':["
		  "{'name':'x','src':'a.b.out'},{'name':'y','src':'a.b.out'}],'inports':[{'name':"
		  "'in','tgt':'a.b.in1'}]}",
		  &m) == COMPOSITE_OK);
	CHECK(composite_types_count() == 2);
	CHECK(block_types_count() == builtins + 2);
#undef TWO
	teardown();
}

/* ---- The document and its round trip (03 §5.1, 05 §2.3) ------------------------- */

static void test_document(void)
{
	setup();
	CompositeMessage m;
	const BlockTypeDef *def;
	const char *t = jq(SUM3);
	CHECK(composite_types_register_text(t, strlen(t), &m, &def) == COMPOSITE_OK);
	CHECK(def == block_types_get("t/Sum3") && def->subgraph && def->compute == NULL);
	CHECK(composite_types_owns(def) && !composite_types_owns(block_types_get("math/Add2")));
	CHECK(def->input_count == 3 && def->output_count == 1);
	/* Port metadata from the inner port unless given. */
	CHECK_STREQ(def->inputs[0].description, "Operand 1");
	CHECK_STREQ(def->inputs[0].type, "number");
	CHECK_STREQ(def->inputs[0].inner, "A.in1");
	CHECK(!def->inputs[0].addressable);
	CHECK_STREQ(def->inputs[1].description, "Second");
	CHECK(def->inputs[1].addressable);
	CHECK_STREQ(def->outputs[0].description, "Result");
	CHECK_STREQ(def->outputs[0].inner, "B.out");
	CHECK(def->composite->block_count == 2 && def->composite->edge_count == 1);

	/* component:component (02 §3.5): subgraph true. */
	cJSON *c = graph_json_component(def);
	char *ct = graph_json_print(c);
	CHECK_STREQ(ct, jq("{'name':'t/Sum3','description':'a+b+c','subgraph':true,'icon':'add',"
			   "'inPorts':[{'id':'a','description':'Operand 1','type':'number',"
			   "'addressable':false},{'id':'b','description':'Second','type':"
			   "'number','addressable':true},{'id':'c','description':'Operand 2',"
			   "'type':'number','addressable':false}],'outPorts':[{'id':'sum',"
			   "'description':'Result','type':'number','addressable':false}]}"));
	free(ct);
	cJSON_Delete(c);

	/* The canonical document, and the round trip through its text. */
	CHECK_STREQ(source("t/Sum3"), jq(SUM3_CANONICAL));
	CHECK(reg("{'name':'t/Min','blocks':[]}", &m) == COMPOSITE_OK);
	CHECK_STREQ(source("t/Min"),
		    jq("{'name':'t/Min','icon':'','description':'','blocks':[],'connections':[],"
		       "'initials':[],'inports':[],'outports':[]}"));
	CHECK(reg(LIN, &m) == COMPOSITE_OK);
	CHECK_STREQ(source("t/Lin"),
		    jq("{'name':'t/Lin','icon':'','description':'','blocks':[{'name':'L','type':"
		       "'process/Linear'}],'connections':[],'initials':[{'src':5,'tgt':'L.k'},"
		       "{'src':0.1,'tgt':'L.m'}],'inports':[{'name':'in','tgt':'L.in'}],"
		       "'outports':[{'name':'out','src':'L.out'}]}"));
	char saved[3][4096];
	const char *names[3] = {"t/Sum3", "t/Min", "t/Lin"};
	for (int i = 0; i < 3; i++)
		snprintf(saved[i], sizeof(saved[i]), "%s", source(names[i]));
	composite_types_reset();
	CHECK(block_types_get("t/Sum3") == NULL && composite_types_count() == 0);
	for (int i = 0; i < 3; i++) {
		CHECK(composite_types_register_text(saved[i], strlen(saved[i]), &m, NULL) ==
		      COMPOSITE_OK);
		CHECK_STREQ(source(names[i]), saved[i]);
	}

	/* The former name "edges" (03 §5.1) registers alike; the canonical
	 * form names it "connections". */
	CHECK(reg("{'name':'t/Old','blocks':[{'name':'A','type':'math/Add2'},{'name':'B','type':"
		  "'math/Add2'}],'edges':[{'src':'A.out','tgt':'B.in1'}]}",
		  &m) == COMPOSITE_OK);
	CHECK(block_types_get("t/Old")->composite->edge_count == 1);
	CHECK_STREQ(source("t/Old"),
		    jq("{'name':'t/Old','icon':'','description':'','blocks':[{'name':'A','type':"
		       "'math/Add2'},{'name':'B','type':'math/Add2'}],'connections':[{'src':"
		       "'A.out','tgt':'B.in1'}],'initials':[],'inports':[],'outports':[]}"));
	CHECK(composite_types_unregister(&gs, "t/Old", &m) == COMPOSITE_OK);

	/* Type files: saved atomically under the encoded name, loaded back. */
	for (int i = 0; i < 3; i++)
		CHECK(type_files_save(&gf, block_types_get(names[i])) == GRAPH_FILES_OK);
	CHECK(type_files_save(&gf, block_types_get("math/Add2")) == GRAPH_FILES_ERR_INVALID);
	char path[PAL_PATH_MAX];
	CHECK(graph_files_path(&gf, GRAPH_FILES_TYPES_DIR, "t/Sum3", path, sizeof(path)) ==
	      GRAPH_FILES_OK);
	CHECK(strstr(path, "/types/t%2FSum3.json") != NULL);
	char file[4096];
	size_t len = 0;
	CHECK(pal_storage_read(path, file, sizeof(file) - 1, &len) == PAL_OK);
	file[len] = '\0';
	CHECK_STREQ(file, saved[0]);
	composite_types_reset();
	size_t loaded = 0;
	CHECK(type_files_load_all(&gf, &loaded) == GRAPH_FILES_OK);
	CHECK(loaded == 3);
	for (int i = 0; i < 3; i++)
		CHECK_STREQ(source(names[i]), saved[i]);
	for (int i = 0; i < 3; i++)
		CHECK(type_files_delete(&gf, names[i]) == GRAPH_FILES_OK);
	CHECK(type_files_delete(&gf, "t/Sum3") == GRAPH_FILES_OK); /* gone already: fine */
	CHECK(pal_storage_exists(path) == false);
	teardown();
}

/* ---- Instances on the generic runtime (03 §5 "Runtime behavior") ---------------- */

/* A watch counting values and detaches, keeping the last value. */
typedef struct {
	InputWatch w; /* first: an InputWatch pointer is a Rec pointer */
	int values;
	int detached;
	double last;
} Rec;

static void rec_value(InputWatch *w, const Block *b, uint32_t input, const Linkable *v)
{
	(void)b;
	(void)input;
	Rec *r = (Rec *)w;
	r->values++;
	r->last = linkable_to_f64(v);
}

static void rec_detach(InputWatch *w)
{
	((Rec *)w)->detached++;
}

static void test_instance(void)
{
	setup();
	CompositeMessage m;
	CHECK(reg(SUM3, &m) == COMPOSITE_OK);
	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "src", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "c", "t/Sum3", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "dst", "math/Add2", NULL, NULL) == GRAPH_OK);
	/* Inner blocks are no graph nodes: no event, no name clash. */
	CHECK_STREQ(events, "+node src;+node c;+node dst;");
	Graph *g = graphs_find(&gs, "g1");
	CHECK(g->block_count == 3);
	Block *c = node("g1", "c");
	CHECK(c->inner_count == 2 && c->inner[0]->parent == c && c->parent == NULL);
	CHECK_STREQ(c->inner[0]->name, "A");
	CHECK(graph_find_block(g, "A") == NULL);
	CHECK(graphs_add_node(&gs, "g1", "A", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_remove_node(&gs, "g1", "A", NULL) == GRAPH_OK);
	events[0] = '\0';

	set_initial("g1", "src", "in1", 1);
	set_initial("g1", "src", "in2", 2);
	CHECK(graphs_add_edge(&gs, "g1", "src", "out", "c", "a", NULL, NULL) == GRAPH_OK);
	set_initial("g1", "c", "b", 4);
	set_initial("g1", "c", "c", 10);
	CHECK(graphs_add_edge(&gs, "g1", "c", "sum", "dst", "in1", NULL, NULL) == GRAPH_OK);
	CHECK_STREQ(events, "+init src.in1;+init src.in2;+edge src.out>c.a;+init c.b;+init c.c;"
			    "+edge c.sum>dst.in1;");
	/* Edges and initials on composite ports are refused like any. */
	CHECK(graphs_add_edge(&gs, "g1", "dst", "out", "c", "a", NULL, NULL) ==
	      GRAPH_ERR_INPUT_CONNECTED);
	CHECK(graphs_add_edge(&gs, "g1", "c", "a", "dst", "in2", NULL, NULL) ==
	      GRAPH_ERR_INVALID_EDGE_DIRECTION);
	CHECK(graphs_add_edge(&gs, "g1", "c", "nope", "dst", "in2", NULL, NULL) ==
	      GRAPH_ERR_UNKNOWN_PORT);
	/* Stopped: the initials are only stored, in the inner inputs. */
	CHECK(in_f64("g1", "c", "b") == 4);
	CHECK(linkable_to_f64(&c->inner[0]->in_values[1]) == 4);
	CHECK(in_f64("g1", "dst", "in1") == 0);

	/* Start reaches the inner blocks; values flow in through a, through
	 * A and B (and the inner edge), out through sum. */
	CHECK(graphs_start(&gs, "g1", NULL) == GRAPH_OK);
	CHECK(c->started && c->inner[0]->started && c->inner[1]->started);
	drain();
	CHECK(in_f64("g1", "c", "a") == 3);
	CHECK(in_f64("g1", "dst", "in1") == 17);
	CHECK(linkable_to_f64(block_output_value(c, 0)) == 17);

	/* A live-value watch on an exposed input sits on the inner input. */
	Rec rec = {.w = {.on_value = rec_value, .on_detach = rec_detach}};
	block_watch_input(c, 0, &rec.w);
	CHECK(rec.values == 1 && rec.last == 3);
	CHECK(c->inner[0]->inputs[0].watches == &rec.w);
	set_initial("g1", "src", "in1", 5);
	drain();
	CHECK(rec.values == 2 && rec.last == 7);
	CHECK(in_f64("g1", "dst", "in1") == 21);

	/* An initial removed: the exposed input reverts to the inner default,
	 * which propagates. */
	CHECK(graphs_remove_initial(&gs, "g1", "c", "c", NULL) == GRAPH_OK);
	drain();
	CHECK(in_f64("g1", "c", "c") == 0);
	CHECK(in_f64("g1", "dst", "in1") == 11);

	/* A value queued under the old edge goes stale with it (01 §3.2). */
	set_initial("g1", "src", "in1", 100);
	CHECK(graphs_remove_edge(&gs, "g1", "src", "out", "c", "a", NULL) == GRAPH_OK);
	CHECK(rec.last == 0); /* reverted at once */
	drain();
	CHECK(in_f64("g1", "c", "a") == 0);
	CHECK(in_f64("g1", "dst", "in1") == 4);
	/* Reconnected: the edge delivers last_propagated (03 §2 connect). */
	CHECK(graphs_add_edge(&gs, "g1", "src", "out", "c", "a", NULL, NULL) == GRAPH_OK);
	CHECK(in_f64("g1", "c", "a") == 102);
	drain();
	CHECK(in_f64("g1", "dst", "in1") == 106);

	/* Rename: links and the mapping follow the block. */
	CHECK(graphs_rename_node(&gs, "g1", "c", "c2", NULL) == GRAPH_OK);
	set_initial("g1", "c2", "b", 1);
	drain();
	CHECK(in_f64("g1", "dst", "in1") == 103);

	/* Stop reaches the inner blocks; a stopped graph propagates nothing. */
	CHECK(graphs_stop(&gs, "g1", NULL) == GRAPH_OK);
	CHECK(!c->started && !c->inner[0]->started && !c->inner[1]->started);
	set_initial("g1", "c2", "b", 2);
	drain();
	CHECK(in_f64("g1", "dst", "in1") == 103);
	CHECK(graphs_start(&gs, "g1", NULL) == GRAPH_OK);
	drain();
	CHECK(in_f64("g1", "dst", "in1") == 104);

	/* Removal: the edges and initials of the instance (inbound, then
	 * outbound), the node; the watch detaches, the inner slots go stale,
	 * the downstream input reverts. */
	BlockHandle inner = c->inner[1]->handle;
	events[0] = '\0';
	CHECK(graphs_remove_node(&gs, "g1", "c2", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "-edge src.out>c2.a;-init c2.b;-edge c2.sum>dst.in1;-node c2;");
	CHECK(rec.detached == 1);
	CHECK(runtime_block(&rt, inner) == NULL);
	drain();
	CHECK(in_f64("g1", "dst", "in1") == 0);
	CHECK(node("g1", "src")->outputs[0].link_count == 0);
	teardown();
}

/* A composite inside a composite, and a self-loop on an instance. */
static void test_nested(void)
{
	setup();
	CompositeMessage m;
	CHECK(reg(SUM3, &m) == COMPOSITE_OK);
	CHECK(reg("{'name':'t/Sum5','blocks':[{'name':'S','type':'t/Sum3'},{'name':'T','type':"
		  "'math/Add2'}],'connections':[{'src':'S.sum','tgt':'T.in1'}],'initials':[{'src':"
		  "100,'tgt':'S.c'}],'inports':[{'name':'a','tgt':'S.a'},{'name':'b','tgt':'S.b'},"
		  "{'name':'d','tgt':'T.in2'}],'outports':[{'name':'out','src':'T.out'},{'name':"
		  "'mid','src':'S.sum'}]}",
		  &m) == COMPOSITE_OK);
	const BlockTypeDef *s5 = block_types_get("t/Sum5");
	CHECK_STREQ(s5->inputs[0].description, "Operand 1"); /* through two levels */
	CHECK_STREQ(s5->outputs[1].inner, "S.sum");
	/* Recursion through another composite: t/Sum3 contains no t/Sum5, but
	 * a new type cannot use itself in any way. */
	refused("{'name':'t/Loop','blocks':[{'name':'X','type':'t/Sum5'},{'name':'Y','type':"
		"'t/Loop'}]}",
		COMPOSITE_ERR_RECURSIVE, "Recursive component: t/Loop");

	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "n", "t/Sum5", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "o", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "p", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_start(&gs, "g1", NULL) == GRAPH_OK); /* started before wiring */
	set_initial("g1", "n", "a", 1);
	set_initial("g1", "n", "b", 2);
	set_initial("g1", "n", "d", 3);
	CHECK(graphs_add_edge(&gs, "g1", "n", "out", "o", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "n", "mid", "p", "in1", NULL, NULL) == GRAPH_OK);
	drain();
	CHECK(in_f64("g1", "o", "in1") == 106);
	CHECK(in_f64("g1", "p", "in1") == 103);
	Block *n = node("g1", "n");
	CHECK(n->inner[0]->inner_count == 2 && n->inner[0]->inner[0]->parent == n->inner[0]);
	/* The exposed input maps through both levels. */
	CHECK(block_input_target(n, &(uint32_t){0}) == n->inner[0]->inner[0]);

	/* A self-loop: out back into d. The values keep cycling (01 §3.2
	 * item 4); removing the instance ends it. */
	CHECK(graphs_remove_initial(&gs, "g1", "n", "d", NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "n", "out", "n", "d", NULL, NULL) == GRAPH_OK);
	runtime_drain(&rt, 50);
	CHECK(in_f64("g1", "n", "d") > 103);
	events[0] = '\0';
	CHECK(graphs_remove_node(&gs, "g1", "n", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "-init n.a;-init n.b;-edge n.out>n.d;-edge n.out>o.in1;"
			    "-edge n.mid>p.in1;-node n;");
	drain();
	CHECK(in_f64("g1", "o", "in1") == 0 && in_f64("g1", "p", "in1") == 0);
	CHECK(work_queue_empty(&rt.queue));
	teardown();
}

/* timing/Interval inside a composite: start, stop and removal reach its
 * timer; an unwired exposed input holds the inner port's default (1000). */
static void test_timer(void)
{
	setup();
	CompositeMessage m;
	CHECK(reg(BLINK, &m) == COMPOSITE_OK);
	CHECK(block_types_get("t/Blink")->inputs[0].default_value.as.f64 == 1000);
	runtime_set_now(&rt, 0);
	CHECK(graphs_new_graph(&gs, "g2", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g2", "b", "t/Blink", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g2", "p", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g2", "b", "out", "p", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(in_f64("g2", "b", "ms") == 1000);
	CHECK(rt.timer_blocks == 1);
	uint64_t due;
	CHECK(!runtime_next_deadline(&rt, &due));

	CHECK(graphs_start(&gs, "g2", NULL) == GRAPH_OK);
	drain();
	CHECK(in_value("g2", "p", "in1")->type == LINKABLE_BOOL);
	CHECK(in_f64("g2", "p", "in1") == 1);
	CHECK(runtime_next_deadline(&rt, &due) && due == 500);
	runtime_fire_timers(&rt, 500);
	drain();
	CHECK(in_f64("g2", "p", "in1") == 0);
	runtime_fire_timers(&rt, 1000);
	drain();
	CHECK(in_f64("g2", "p", "in1") == 1);

	/* An initial on the exposed input, then removed: back to 1000. */
	set_initial("g2", "b", "ms", 200);
	CHECK(in_f64("g2", "b", "ms") == 200);
	CHECK(graphs_remove_initial(&gs, "g2", "b", "ms", NULL) == GRAPH_OK);
	CHECK(in_f64("g2", "b", "ms") == 1000);

	CHECK(graphs_stop(&gs, "g2", NULL) == GRAPH_OK);
	CHECK(!runtime_next_deadline(&rt, &due));
	runtime_set_now(&rt, 5000);
	CHECK(graphs_start(&gs, "g2", NULL) == GRAPH_OK);
	CHECK(runtime_next_deadline(&rt, &due) && due == 5500);
	CHECK(graphs_remove_node(&gs, "g2", "b", NULL) == GRAPH_OK);
	CHECK(!runtime_next_deadline(&rt, &due));
	CHECK(rt.timer_blocks == 0);
	drain();
	CHECK(in_f64("g2", "p", "in1") == 0);

	/* A graph destroyed with a running instance in it. */
	CHECK(graphs_add_node(&gs, "g2", "b2", "t/Blink", NULL, NULL) == GRAPH_OK);
	CHECK(rt.timer_blocks == 1 && runtime_next_deadline(&rt, &due));
	CHECK(graphs_delete_graph(&gs, "g2", GRAPHS_DEFAULT_NAME, NULL) == GRAPH_OK);
	CHECK(rt.timer_blocks == 0 && !runtime_next_deadline(&rt, &due));
	teardown();
}

/* An inner block's change threshold is its own type's default, else the
 * engine-wide one; there is no instance override (plan task 21c, 01 §3.3).
 * Inner initials (t/Lin). */
static void test_threshold(void)
{
	setup();
	CompositeMessage m;
	CHECK(reg(LIN, &m) == COMPOSITE_OK);
	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "l", "t/Lin", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "d", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "l", "out", "d", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_start(&gs, "g1", NULL) == GRAPH_OK);
	set_initial("g1", "l", "in", 2);
	drain();
	CHECK(in_f64("g1", "d", "in1") == 10.1); /* 2*5 + 0.1, from the inner initials */
	Block *l = node("g1", "l");
	CHECK(block_change_threshold(l->inner[0]) == RUNTIME_DEFAULT_CHANGE_THRESHOLD);
	/* A change_threshold metadata key on the instance is opaque: the inner
	 * block keeps its own type's default (plan task 21c). */
	BlockChange chg = {.set_extra = true, .extra = (char *)"{\"change_threshold\":5}"};
	CHECK(graphs_change_node(&gs, "g1", "l", &chg, NULL) == GRAPH_OK);
	CHECK_STREQ(l->metadata.extra, "{\"change_threshold\":5}");
	CHECK(block_change_threshold(l->inner[0]) == RUNTIME_DEFAULT_CHANGE_THRESHOLD);
	set_initial("g1", "l", "in", 2.5); /* out 12.6: a change of 2.5 */
	drain();
	CHECK(fabs(in_f64("g1", "d", "in1") - 12.6) < 1e-9);
	set_initial("g1", "l", "in", 2.52); /* out 12.7: a change of 0.1 >= 0.05 */
	drain();
	CHECK(fabs(in_f64("g1", "d", "in1") - 12.7) < 1e-9);
	set_initial("g1", "l", "in", 2.525); /* out 12.725: 0.025 < 0.05, held */
	drain();
	CHECK(fabs(in_f64("g1", "d", "in1") - 12.7) < 1e-9);
	teardown();
}

/* A graph with instances saves and reloads (05 §2.1: the outer block only). */
static void test_graph_file(void)
{
	setup();
	CompositeMessage m;
	CHECK(reg(SUM3, &m) == COMPOSITE_OK);
	CHECK(graphs_new_graph(&gs, "g1", "G", NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "c", "t/Sum3", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "d", "math/Add2", NULL, NULL) == GRAPH_OK);
	set_initial("g1", "c", "a", 1);
	set_initial("g1", "c", "b", 2);
	CHECK(graphs_add_edge(&gs, "g1", "d", "out", "c", "c", "{\"route\":1}", NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "c", "sum", "d", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_INPORT, "x", "c", "b", NULL) == GRAPH_OK);
	CHECK(graphs_start(&gs, "g1", NULL) == GRAPH_OK);
	char *text = graph_files_serialize(graphs_find(&gs, "g1"), NULL);
	CHECK_STREQ(text, jq("{'id':'g1','name':'G','description':'','icon':'','started':true,"
			     "'blocks':{'c':{'component':'t/Sum3','metadata':{'x':50,'y':50}},"
			     "'d':{'component':'math/Add2','metadata':{'x':50,'y':50}}},"
			     "'connections':[{'src':{'data':1},'tgt':{'block':'c','port':'a'}},"
			     "{'src':{'data':2},'tgt':{'block':'c','port':'b'}},"
			     "{'src':{'block':'d','port':'out'},'tgt':{'block':'c','port':'c'},"
			     "'metadata':{'route':1}},"
			     "{'src':{'block':'c','port':'sum'},'tgt':{'block':'d','port':'in1'}}],"
			     "'inports':[{'name':'x','block':'c','port':'b'}],'outports':[]}"));
	CHECK(graphs_delete_graph(&gs, "g1", "G", NULL) == GRAPH_OK);
	Graph *g;
	bool started = false;
	CHECK(graph_files_load_text(&gf, text, strlen(text), "test", &g, &started) ==
	      GRAPH_FILES_OK);
	/* Loading leaves it stopped; started as its flag says, it saves the same. */
	CHECK(started && !g->started);
	CHECK(graphs_start(&gs, "g1", NULL) == GRAPH_OK);
	char *again = graph_files_serialize(g, NULL);
	CHECK_STREQ(again, text);
	CHECK(!block_is_placeholder(graph_find_block(g, "c")));
	free(again);
	free(text);
	teardown();
}

/* Unregister, rename, change (02 §3.4.2's model operations). */
static void test_registry_ops(void)
{
	setup();
	CompositeMessage m;
	CHECK(reg(SUM3, &m) == COMPOSITE_OK);
	CHECK(reg("{'name':'t/Wrap','blocks':[{'name':'S','type':'t/Sum3'}]}", &m) ==
	      COMPOSITE_OK);
	const BlockTypeDef *sum3 = block_types_get("t/Sum3");
	CHECK(composite_types_in_use(&gs, sum3)); /* by t/Wrap */
	CHECK(composite_types_unregister(&gs, "t/Sum3", &m) == COMPOSITE_ERR_IN_USE);
	CHECK_STREQ(m.text, "Component in use: t/Sum3");
	CHECK(composite_types_rename(&gs, "t/Sum3", "t/S3", &m) == COMPOSITE_ERR_IN_USE);
	CHECK(composite_types_unregister(&gs, "math/Add2", &m) == COMPOSITE_ERR_NOT_FOUND);
	CHECK_STREQ(m.text, "Unknown component: math/Add2");
	CHECK(composite_types_unregister(&gs, "t/None", &m) == COMPOSITE_ERR_NOT_FOUND);
	CHECK(composite_types_rename(&gs, "t/Nope", "t/X", &m) == COMPOSITE_ERR_NOT_FOUND);

	/* t/Wrap in use by a graph. */
	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g1", "w", "t/Wrap", NULL, NULL) == GRAPH_OK);
	CHECK(composite_types_unregister(&gs, "t/Wrap", &m) == COMPOSITE_ERR_IN_USE);
	CHECK(composite_types_rename(&gs, "t/Wrap", "t/W", &m) == COMPOSITE_ERR_IN_USE);
	/* Description and icon may change while in use. */
	CHECK(composite_types_change("t/Wrap", "wrapped", NULL, &m) == COMPOSITE_OK);
	CHECK(composite_types_change("t/Wrap", NULL, "box", &m) == COMPOSITE_OK);
	CHECK_STREQ(block_types_get("t/Wrap")->description, "wrapped");
	CHECK_STREQ(block_types_get("t/Wrap")->icon, "box");
	CHECK(strstr(source("t/Wrap"), "\"icon\":\"box\",\"description\":\"wrapped\"") != NULL);
	CHECK(composite_types_change("math/Add2", "x", NULL, &m) == COMPOSITE_ERR_NOT_FOUND);

	/* Once unused: rename (moved to the end), unregister. */
	CHECK(graphs_remove_node(&gs, "g1", "w", NULL) == GRAPH_OK);
	CHECK(composite_types_rename(&gs, "t/Wrap", "math/Add2", &m) == COMPOSITE_ERR_EXISTS);
	CHECK_STREQ(m.text, "Component exists: math/Add2");
	CHECK(composite_types_rename(&gs, "t/Wrap", "t/Wrap", &m) == COMPOSITE_ERR_EXISTS);
	CHECK(composite_types_rename(&gs, "t/Wrap", "", &m) == COMPOSITE_ERR_INVALID);
	CHECK_STREQ(m.text, "Invalid field: to");
	CHECK(composite_types_rename(&gs, "t/Wrap", "t/W", &m) == COMPOSITE_OK);
	CHECK(block_types_get("t/Wrap") == NULL);
	CHECK(block_types_at(block_types_count() - 1) == block_types_get("t/W"));
	CHECK(strncmp(source("t/W"), "{\"name\":\"t/W\",", 14) == 0);
	CHECK(type_files_save(&gf, block_types_get("t/W")) == GRAPH_FILES_OK);
	CHECK(composite_types_unregister(&gs, "t/Sum3", &m) == COMPOSITE_ERR_IN_USE);
	CHECK(composite_types_unregister(&gs, "t/W", &m) == COMPOSITE_OK);
	CHECK(type_files_delete(&gf, "t/W") == GRAPH_FILES_OK);
	CHECK(composite_types_unregister(&gs, "t/Sum3", &m) == COMPOSITE_OK);
	CHECK(composite_types_count() == 0 && block_types_count() == builtins);
	teardown();
}

/* ---- Loading types/ at startup (05 §1.4 step 6) ---------------------------------- */

static void put(const char *name, const char *json)
{
	char dir[PAL_PATH_MAX], path[PAL_PATH_MAX + 128];
	CHECK(graph_files_dir(&gf, GRAPH_FILES_TYPES_DIR, dir, sizeof(dir)) == GRAPH_FILES_OK);
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	const char *t = jq(json);
	CHECK(pal_storage_write(path, t, strlen(t)) == PAL_OK);
}

static void test_load_all(void)
{
	setup();
	/* Byte order puts every dependent before what it needs. */
	put("t%2FA.json", "{'name':'t/A','blocks':[{'name':'B','type':'t/B'}]}");
	put("t%2FB.json", "{'name':'t/B','blocks':[{'name':'C','type':'t/C'}]}");
	put("t%2FC.json", "{'name':'t/C','blocks':[{'name':'x','type':'math/Add2'}]}");
	put("t%2FC1.json", "{'name':'t/C1','blocks':[{'name':'x','type':'t/C2'}]}");
	put("t%2FC2.json", "{'name':'t/C2','blocks':[{'name':'x','type':'t/C1'}]}");
	put("t%2FD.json", "{'name':'t/D','blocks':[{'name':'x','type':'t/C1'}]}");
	put("t%2FR.json", "{'name':'t/R','blocks':[{'name':'x','type':'t/R'}]}");
	put("t%2FU.json", "{'name':'t/U','blocks':[{'name':'x','type':'nope/X'}]}");
	put("t%2FE.json", "{'name':'t/E','blocks':[{'name':'x','type':'math/Add2'}],'edges':["
			  "{'src':'x.in1','tgt':'x.in2'}]}");
	put("t%2FF.json", "{'name':'t/F','blocks':[{'name':'x','type':'t/E'}]}");
	put("bad.json", "{'name':");
	put("wrong.json", "{'name':'t/W','blocks':[]}");
	put("t%2FA.json.tmp", "ignored");
	size_t loaded = 0;
	CHECK(type_files_load_all(&gf, &loaded) == GRAPH_FILES_ERR_MALFORMED);
	CHECK(loaded == 4);
	CHECK(composite_types_count() == 4);
	/* Registered in dependency order. */
	CHECK(block_types_at(builtins) == block_types_get("t/C"));
	CHECK(block_types_at(builtins + 1) == block_types_get("t/W"));
	CHECK(block_types_at(builtins + 2) == block_types_get("t/B"));
	CHECK(block_types_at(builtins + 3) == block_types_get("t/A"));
	const char *want[] = {
		"bad.json: composite type skipped: Invalid JSON",
		"t%2FE.json: composite type skipped: Invalid edge direction",
		"wrong.json: type t/W is saved as t%2FW.json, not here",
		"t%2FC1.json: composite type skipped: Unknown component: t/C2 (a type that",
		"(a type that did not load either: a dependency cycle, or it depends on a",
		"t%2FC2.json: composite type skipped: Unknown component: t/C1 (a type",
		"t%2FD.json: composite type skipped: Unknown component: t/C1 (a type",
		"t%2FF.json: composite type skipped: Unknown component: t/E (a type",
		"t%2FR.json: composite type skipped: Recursive component: t/R",
		"t%2FU.json: composite type skipped: Unknown component: nope/X\n",
	};
	for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		if (strstr(logs, want[i]) == NULL) {
			fprintf(stderr, "  missing log: %s\n", want[i]);
			CHECK(false);
		}
	}
	CHECK(strstr(logs, "A.json.tmp") == NULL);
	composite_types_reset();
	/* No types/ directory: nothing to load. */
	GraphFiles none;
	char dir[PAL_PATH_MAX + 16];
	snprintf(dir, sizeof(dir), "%s/nothing", root);
	CHECK(graph_files_init(&none, &gs, dir));
	CHECK(type_files_load_all(&none, &loaded) == GRAPH_FILES_ERR_NOT_FOUND && loaded == 0);
	teardown();
}

/* ---- Creating a group from a selection (03 §5.2) --------------------------------- */

static char *assembled(const Graph *g, const char *name, const char *const *nodes, size_t n)
{
	CompositeSelection sel = {name, "made", "box", nodes, n};
	cJSON *doc;
	CompositeMessage m;
	CHECK(composite_types_assemble(g, &sel, &doc, &m) == COMPOSITE_OK);
	CHECK(composite_types_register(doc, &m, NULL) == COMPOSITE_OK);
	CHECK_STREQ(m.text, "");
	char *text = graph_json_print(doc); /* numbers turn raw: after registering */
	cJSON_Delete(doc);
	return text;
}

static void test_assemble(void)
{
	setup();
	CHECK(graphs_new_graph(&gs, "g1", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	Graph *g = graphs_find(&gs, "g1");
	const char *const blocks[] = {"x", "s1", "s2", "y", "z"};
	for (int i = 0; i < 5; i++)
		CHECK(graphs_add_node(&gs, "g1", blocks[i], "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "x", "out", "s1", "in1", "{\"r\":1}", NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "x", "out", "s2", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "s1", "out", "s2", "in2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "s2", "out", "y", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "s2", "out", "z", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g1", "s1", "out", "y", "in2", NULL, NULL) == GRAPH_OK);
	set_initial("g1", "s1", "in2", 3);

	/* Default names that clash are disambiguated with the block name;
	 * blocks in creation order whatever the selection's order. */
	const char *const sel[] = {"s2", "s1", "s2"};
	char *t = assembled(g, "t/G", sel, 3);
	CHECK_STREQ(t, jq("{'name':'t/G','icon':'box','description':'made','blocks':[{'name':"
			  "'s1','type':'math/Add2'},{'name':'s2','type':'math/Add2'}],"
			  "'connections':[{'src':'s1.out','tgt':'s2.in2'}],'initials':[{'src':3,"
			  "'tgt':'s1.in2'}],'inports':[{'name':'s1_in1','tgt':'s1.in1'},{'name':"
			  "'s2_in1','tgt':'s2.in1'}],'outports':[{'name':'s2_out','src':'s2.out'},"
			  "{'name':'s1_out','src':'s1.out'}]}"));
	free(t);

	/* Graph-level ports are exposed under their public names (the first
	 * one only on an input); a default name clashing with one is
	 * disambiguated. */
	CHECK(graphs_add_port(&gs, "g1", GRAPH_INPORT, "in1", "s1", "in1", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_INPORT, "go", "s1", "in1", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_OUTPORT, "res", "s2", "out", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_OUTPORT, "res2", "s2", "out", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g1", GRAPH_OUTPORT, "far", "z", "out", NULL) == GRAPH_OK);
	t = assembled(g, "t/G2", sel, 2);
	CHECK(strstr(t, jq("'inports':[{'name':'in1','tgt':'s1.in1'},{'name':'s2_in1','tgt':"
			   "'s2.in1'}],'outports':[{'name':'res','src':'s2.out'},{'name':'res2',"
			   "'src':'s2.out'},{'name':'out','src':'s1.out'}]}")) != NULL);
	free(t);

	/* "<block>_<port>" taken as well: "<port>_<n>". */
	CHECK(graphs_new_graph(&gs, "g2", NULL, NULL, NULL, NULL, NULL) == GRAPH_OK);
	Graph *g2 = graphs_find(&gs, "g2");
	CHECK(graphs_add_node(&gs, "g2", "o", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g2", "q1", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "g2", "q2", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g2", "o", "out", "q1", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_edge(&gs, "g2", "o", "out", "q2", "in1", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "g2", GRAPH_INPORT, "q2_in1", "q1", "in2", NULL) == GRAPH_OK);
	const char *const sel2[] = {"q1", "q2"};
	t = assembled(g2, "t/G3", sel2, 2);
	CHECK(strstr(t, jq("'inports':[{'name':'q2_in1','tgt':'q1.in2'},{'name':'q1_in1','tgt':"
			   "'q1.in1'},{'name':'in1_2','tgt':'q2.in1'}],'outports':[]}")) != NULL);
	free(t);

	/* The registered type works like a hand-authored one. */
	CHECK(graphs_add_node(&gs, "g2", "inst", "t/G3", NULL, NULL) == GRAPH_OK);
	CHECK(block_input_index(node("g2", "inst"), "in1_2") == 2);

	/* Refusals. */
	cJSON *doc = NULL;
	CompositeMessage m;
	CompositeSelection none = {"t/N", NULL, NULL, sel, 0};
	CHECK(composite_types_assemble(g, &none, &doc, &m) == COMPOSITE_ERR_INVALID);
	CHECK_STREQ(m.text, "Invalid field: nodes");
	const char *const bad[] = {"s1", "nope"};
	CompositeSelection unknown = {"t/N", NULL, NULL, bad, 2};
	CHECK(composite_types_assemble(g, &unknown, &doc, &m) == COMPOSITE_ERR_UNKNOWN_PORT);
	CHECK_STREQ(m.text, "Unknown node: nope");
	CHECK(doc == NULL);
	teardown();
}

/* ---- Setup ------------------------------------------------------------------ */

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
	builtins = block_types_count();
	const char *base = getenv("TMPDIR");
	snprintf(root, sizeof(root), "%s/hakkas-composite-XXXXXX", base && *base ? base : "/tmp");
	if (mkdtemp(root) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	CHECK(graph_files_init(&gf, &gs, root));
	CHECK(graph_files_make_dirs(&gf) == GRAPH_FILES_OK);

	RUN(test_refusals);
	RUN(test_document);
	RUN(test_instance);
	RUN(test_nested);
	RUN(test_timer);
	RUN(test_threshold);
	RUN(test_graph_file);
	RUN(test_registry_ops);
	RUN(test_load_all);
	RUN(test_assemble);

	block_types_reset();
	log_remove_sink(log_sink, NULL);
	if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
	return test_result();
}
