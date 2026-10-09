/*
 * The generic block runtime and the graph model (01 §2.2–§3.3, 03 §2,
 * 02 §3.4.1), driven with the real built-in math types plus a few test types.
 * String values are heap-allocated and reference-counted, so a missed release
 * shows up as a leak under the asan preset.
 */
#include "test.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>

#include "blocktypes/block_types.h"
#include "graphs/graph.h"
#include "system/log.h"

static Runtime rt;
static Graph *gr;

/* ---- Logs: observer events and live-value watches ------------------------ */

static char events[4096];
static char trace[4096];

static void append(char *buf, size_t size, const char *fmt, ...) LOG_PRINTF_FORMAT(3, 4);

static void append(char *buf, size_t size, const char *fmt, ...)
{
	size_t len = strlen(buf);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + len, size - len, fmt, ap);
	va_end(ap);
}

static const char *text(const Linkable *v)
{
	static char buf[8][64];
	static int next;
	char *b = buf[next++ % 8];
	linkable_stringify(v, b, 64);
	return b;
}

static void on_event(void *ctx, const GraphEvent *ev)
{
	(void)ctx;
	CHECK(ev->graph == gr);
	const Block *b = ev->block;
	switch (ev->type) {
	case GRAPH_EVENT_BLOCK_ADDED:
		append(events, sizeof(events), "+node %s;", b->name);
		break;
	case GRAPH_EVENT_BLOCK_REMOVED:
		append(events, sizeof(events), "-node %s;", b->name);
		break;
	case GRAPH_EVENT_BLOCK_RENAMED:
		append(events, sizeof(events), "rename %s>%s;", ev->old_name, b->name);
		break;
	case GRAPH_EVENT_BLOCK_CHANGED:
		append(events, sizeof(events), "change %s;", b->name);
		break;
	case GRAPH_EVENT_EDGE_ADDED:
	case GRAPH_EVENT_EDGE_REMOVED:
		append(events, sizeof(events), "%cedge %s.%s>%s.%s;",
		       ev->type == GRAPH_EVENT_EDGE_ADDED ? '+' : '-', ev->src_block->name,
		       block_output_port(ev->src_block, ev->src_output)->name, b->name,
		       block_input_port(b, ev->input)->name);
		break;
	case GRAPH_EVENT_INITIAL_ADDED:
	case GRAPH_EVENT_INITIAL_REMOVED:
		append(events, sizeof(events), "%cinit %s>%s.%s;",
		       ev->type == GRAPH_EVENT_INITIAL_ADDED ? '+' : '-', text(ev->value), b->name,
		       block_input_port(b, ev->input)->name);
		break;
	default: /* graph ports and graph-level events: tests/test_graphs.c */
		append(events, sizeof(events), "other;");
		break;
	}
}

/* A watch logging "<block>.<input>=<value>;" into trace. */
typedef struct {
	InputWatch w; /* first, so an InputWatch pointer is a Rec pointer */
	int values;
	int detached;
} Rec;

static void rec_value(InputWatch *w, const Block *b, uint32_t input, const Linkable *v)
{
	Rec *r = (Rec *)w;
	r->values++;
	append(trace, sizeof(trace), "%s.%s=%s;", b->name, block_input_port(b, input)->name,
	       text(v));
}

static void rec_detach(InputWatch *w)
{
	((Rec *)w)->detached++;
}

static Rec rec_new(void)
{
	return (Rec){.w = {.on_value = rec_value, .on_detach = rec_detach}};
}

/* ---- Test types --------------------------------------------------------------- */

/* test/Probe: out = in; counts its compute calls by reason, globally and in
 * its state. "in" defaults to 42, "aux" has no default (so 0.0, the number
 * default_value), "out" defaults to -1. */
typedef struct {
	int calls[4];
	int last_changed;
} ProbeState;

static int probe_calls[4];

static int64_t probe(BlockCtx *ctx)
{
	ProbeState *s = ctx->state;
	s->calls[ctx->reason]++;
	s->last_changed = ctx->changed_input;
	probe_calls[ctx->reason]++;
	CHECK(ctx->error != NULL);
	CHECK(ctx->now_ms == rt.now_ms);
	block_ctx_set_output(ctx, 0, linkable_copy(&ctx->inputs[0]));
	return 0;
}

static const PortDescriptor probe_in[] = {
	{.name = "in", .description = "", .type = "number", .default_value = LINKABLE_F64_INIT(42)},
	{.name = "aux", .description = "", .type = "number"},
};

static const PortDescriptor probe_out[] = {
	{.name = "out", .description = "", .type = "number",
	 .default_value = LINKABLE_F64_INIT(-1)},
};

#define PROBE_DEF(type_name, on_input, threshold)                                                  \
	{.name = (type_name),                                                                      \
	 .icon = "probe",                                                                          \
	 .description = "",                                                                        \
	 .inputs = probe_in,                                                                       \
	 .input_count = 2,                                                                         \
	 .outputs = probe_out,                                                                     \
	 .output_count = 1,                                                                        \
	 .compute_on_input = (on_input),                                                           \
	 .timer = TIMER_NONE,                                                                      \
	 .compute = probe,                                                                         \
	 .state_size = sizeof(ProbeState),                                                         \
	 .change_threshold = (threshold)}

static const BlockTypeDef probe_def = PROBE_DEF("test/Probe", true, NAN);
static const BlockTypeDef hold_def = PROBE_DEF("test/Hold", false, NAN);
static const BlockTypeDef coarse_def = PROBE_DEF("test/Coarse", true, 1.0);

/* ---- Helpers ---------------------------------------------------------------- */

static void setup(void)
{
	runtime_init(&rt);
	runtime_set_observer(&rt, on_event, NULL);
	gr = graph_create(&rt);
	CHECK(gr != NULL);
	events[0] = '\0';
	trace[0] = '\0';
	memset(probe_calls, 0, sizeof(probe_calls));
}

static void teardown(void)
{
	graph_destroy(gr);
	gr = NULL;
	runtime_deinit(&rt);
}

static void clear_logs(void)
{
	events[0] = '\0';
	trace[0] = '\0';
}

static Block *add(const char *name, const char *type_name)
{
	Block *b = NULL;
	const BlockTypeDef *def = block_types_get(type_name);
	CHECK(def != NULL);
	CHECK(graph_create_block(gr, name, def, NULL, &b) == GRAPH_OK);
	return b;
}

static void wire(const char *src, const char *src_port, const char *tgt, const char *tgt_port)
{
	CHECK(graph_connect(gr, src, src_port, tgt, tgt_port, NULL) == GRAPH_OK);
}

static void initial(const char *node, const char *port, double v)
{
	Linkable value = linkable_from_f64(v);
	CHECK(graph_add_initial(gr, node, port, &value) == GRAPH_OK);
}

static double in_f64(const Block *b, const char *port)
{
	int32_t i = block_input_index(b, port);
	CHECK(i >= 0);
	return i < 0 ? NAN : linkable_to_f64(&b->in_values[i]);
}

static double out_f64(const Block *b)
{
	return linkable_to_f64(&b->out_values[0]);
}

static double last_f64(const Block *b)
{
	return linkable_to_f64(&b->outputs[0].last_propagated);
}

static ProbeState *probe_state(const Block *b)
{
	return b->state;
}

static void drain(void)
{
	runtime_drain(&rt, SIZE_MAX);
}

static size_t queued(void)
{
	return work_queue_len(&rt.queue);
}

static void watch(Block *b, const char *port, Rec *r)
{
	int32_t i = block_input_index(b, port);
	CHECK(i >= 0);
	block_watch_input(b, (uint32_t)i, &r->w);
}

/* ---- Propagation ---------------------------------------------------------- */

static void test_chain(void)
{
	setup();
	Block *a = add("A", "math/Add2");
	Block *b = add("B", "math/Add2");
	Block *c = add("C", "math/Add2");
	wire("A", "out", "B", "in1");
	wire("B", "out", "C", "in1");
	initial("A", "in1", 1);
	initial("A", "in2", 2);
	initial("B", "in2", 10);
	initial("C", "in2", 100);
	/* Stopped: inputs store, nothing computes or is queued. */
	CHECK(in_f64(a, "in1") == 1);
	CHECK(out_f64(a) == 0);
	CHECK(queued() == 0);

	graph_start(gr);
	CHECK(out_f64(a) == 3);
	CHECK(out_f64(b) == 10); /* computed at start, before A's value arrived */
	CHECK(out_f64(c) == 100);
	CHECK(queued() == 2); /* A → B and B → C; C has no links */
	drain();
	CHECK(in_f64(b, "in1") == 3);
	CHECK(out_f64(b) == 13);
	CHECK(in_f64(c, "in1") == 13);
	CHECK(out_f64(c) == 113);
	CHECK(queued() == 0);

	/* An initial on A computes A at once; B and C follow through the queue. */
	initial("A", "in1", 5);
	CHECK(out_f64(a) == 7);
	CHECK(out_f64(b) == 13);
	CHECK(runtime_drain(&rt, 1) == 1);
	CHECK(out_f64(b) == 17);
	CHECK(out_f64(c) == 113);
	drain();
	CHECK(out_f64(c) == 117);
	teardown();
}

static void test_breadth_first_fifo(void)
{
	setup();
	add("A", "math/Add2");
	Block *b = add("B", "math/Add2");
	Block *c = add("C", "math/Add2");
	Block *d = add("D", "math/Add2");
	Block *e = add("E", "math/Add2");
	wire("A", "out", "B", "in1");
	wire("A", "out", "C", "in1");
	wire("B", "out", "D", "in1");
	wire("C", "out", "E", "in1");
	graph_start(gr);
	drain();
	Rec rb = rec_new(), rc = rec_new(), rd = rec_new(), re = rec_new();
	watch(b, "in1", &rb);
	watch(c, "in1", &rc);
	watch(d, "in1", &rd);
	watch(e, "in1", &re);
	clear_logs();

	/* Breadth-first: both of A's links before anything downstream of them. */
	initial("A", "in1", 1);
	drain();
	CHECK_STREQ(trace, "B.in1=1;C.in1=1;D.in1=1;E.in1=1;");

	/* FIFO per block: two values queued for B and C arrive in order. */
	clear_logs();
	initial("A", "in1", 2);
	initial("A", "in1", 3);
	drain();
	CHECK_STREQ(trace, "B.in1=2;C.in1=2;B.in1=3;C.in1=3;D.in1=2;E.in1=2;D.in1=3;E.in1=3;");
	teardown();
}

static void test_value_changed(void)
{
	Linkable f0 = linkable_from_f64(0), f1 = linkable_from_f64(1), u = {0};

	/* Numbers: |new − last| >= threshold, and equal never changes. */
	Linkable below = linkable_from_f64(0.25), at = linkable_from_f64(0.5),
		 above = linkable_from_f64(0.75);
	CHECK(!block_value_changed(&f0, &below, 0.5));
	CHECK(block_value_changed(&f0, &at, 0.5));
	CHECK(block_value_changed(&f0, &above, 0.5));
	CHECK(!block_value_changed(&f1, &f1, 0));
	Linkable next_up = linkable_from_f64(nextafter(1.0, 2.0));
	CHECK(block_value_changed(&f1, &next_up, 0));
	Linkable neg0 = linkable_from_f64(-0.0);
	CHECK(!block_value_changed(&f0, &neg0, 0));

	/* NaN: transitions count, NaN → NaN does not. */
	Linkable nan = linkable_from_f64(NAN), nan2 = linkable_from_f64(-NAN);
	CHECK(!block_value_changed(&nan, &nan2, 0.05));
	CHECK(block_value_changed(&nan, &f1, 1e9));
	CHECK(block_value_changed(&f1, &nan, 1e9));

	/* Infinities. */
	Linkable inf = linkable_from_f64(INFINITY), ninf = linkable_from_f64(-INFINITY),
		 big = linkable_from_f64(1e300);
	CHECK(!block_value_changed(&inf, &inf, 0));
	CHECK(block_value_changed(&inf, &ninf, 1e300));
	CHECK(block_value_changed(&big, &inf, INFINITY));
	CHECK(!block_value_changed(&f0, &f1, INFINITY));

	/* i64, without overflow at the extremes. */
	Linkable imin = linkable_from_i64(INT64_MIN), imax = linkable_from_i64(INT64_MAX);
	Linkable i5 = linkable_from_i64(5), i6 = linkable_from_i64(6);
	CHECK(block_value_changed(&imin, &imax, 1e19));
	CHECK(!block_value_changed(&imin, &imax, 1e20));
	CHECK(!block_value_changed(&i5, &i5, 0));
	CHECK(block_value_changed(&i5, &i6, 0));
	CHECK(block_value_changed(&i5, &i6, 1));
	CHECK(!block_value_changed(&i5, &i6, 2));

	/* Any change of value type, even between equal numbers. */
	Linkable i1 = linkable_from_i64(1);
	CHECK(block_value_changed(&f1, &i1, 100));
	CHECK(block_value_changed(&u, &f0, 100));
	CHECK(block_value_changed(&f0, &u, 100));
	CHECK(!block_value_changed(&u, &u, 0));

	/* Bool and string: any difference. */
	Linkable t = linkable_from_bool(true), fa = linkable_from_bool(false);
	CHECK(!block_value_changed(&t, &t, 100));
	CHECK(block_value_changed(&t, &fa, 100));
	Linkable s1 = {0}, s2 = {0}, s3 = {0}, empty = LINKABLE_EMPTY_STRING_INIT;
	CHECK(linkable_from_string(&s1, "abc", 3));
	CHECK(linkable_from_string(&s2, "abc", 3)); /* equal text, another allocation */
	CHECK(linkable_from_string(&s3, "abd", 3));
	CHECK(!block_value_changed(&s1, &s2, 100));
	CHECK(block_value_changed(&s1, &s3, 100));
	CHECK(!block_value_changed(&empty, &empty, 100));
	CHECK(block_value_changed(&empty, &s1, 100));
	CHECK(block_value_changed(&s1, &f1, 100));

	/* Metric: magnitude by threshold; unit or category change always. */
	Linkable m1 = {0}, m2 = {0}, m3 = {0}, m4 = {0}, mnan = {0};
	CHECK(linkable_from_metric(&m1, 1.0, "V", 1, METRIC_VOLTAGE));
	CHECK(linkable_from_metric(&m2, 1.25, "V", 1, METRIC_VOLTAGE));
	CHECK(linkable_from_metric(&m3, 1.0, "mV", 2, METRIC_VOLTAGE));
	CHECK(linkable_from_metric(&m4, 1.0, "V", 1, METRIC_UNCLASSIFIED));
	CHECK(linkable_from_metric(&mnan, NAN, "V", 1, METRIC_VOLTAGE));
	CHECK(!block_value_changed(&m1, &m2, 0.5));
	CHECK(block_value_changed(&m1, &m2, 0.25));
	CHECK(block_value_changed(&m1, &m3, 100));
	CHECK(block_value_changed(&m1, &m4, 100));
	CHECK(block_value_changed(&m1, &mnan, 100));
	CHECK(!block_value_changed(&mnan, &mnan, 0));
	CHECK(block_value_changed(&m1, &f1, 100));

	linkable_clear(&s1);
	linkable_clear(&s2);
	linkable_clear(&s3);
	linkable_clear(&m1);
	linkable_clear(&m2);
	linkable_clear(&m3);
	linkable_clear(&m4);
	linkable_clear(&mnan);
}

static void test_threshold_drift(void)
{
	setup();
	CHECK(runtime_set_change_threshold(&rt, 0.5));
	Block *s = add("S", "math/Add2");
	Block *t = add("T", "test/Probe");
	wire("S", "out", "T", "in");
	graph_start(gr);
	drain();
	Rec r = rec_new();
	watch(t, "in", &r);
	clear_logs();

	initial("S", "in1", 0.25); /* below */
	drain();
	CHECK_STREQ(trace, "");
	CHECK(out_f64(s) == 0.25);
	CHECK(last_f64(s) == 0);
	initial("S", "in1", 0.5); /* at the threshold */
	drain();
	CHECK_STREQ(trace, "T.in=0.5;");
	initial("S", "in1", 0.75); /* drift, each step below the threshold ... */
	initial("S", "in1", 0.875);
	drain();
	CHECK_STREQ(trace, "T.in=0.5;");
	initial("S", "in1", 1.0); /* ... until it accumulates to it */
	drain();
	CHECK_STREQ(trace, "T.in=0.5;T.in=1;");
	initial("S", "in1", 2.5); /* above */
	drain();
	CHECK_STREQ(trace, "T.in=0.5;T.in=1;T.in=2.5;");
	teardown();
}

static void test_threshold_zero(void)
{
	setup();
	CHECK(runtime_set_change_threshold(&rt, 0));
	add("S", "math/Add2");
	Block *t = add("T", "test/Probe");
	wire("S", "out", "T", "in");
	graph_start(gr);
	drain();
	Rec r = rec_new();
	watch(t, "in", &r);
	clear_logs();

	initial("S", "in1", 1e-12); /* every distinct value */
	drain();
	initial("S", "in1", 1e-12); /* but not the same one again */
	drain();
	initial("S", "in1", 2e-12);
	drain();
	CHECK_STREQ(trace, "T.in=1e-12;T.in=2e-12;");
	teardown();
}

static void test_threshold_nan_transitions(void)
{
	setup();
	add("D", "math/Divide");
	Block *p = add("P", "test/Probe");
	wire("D", "out", "P", "in");
	initial("D", "in1", 1);
	initial("D", "in2", 1);
	graph_start(gr);
	drain();
	Rec r = rec_new();
	watch(p, "in", &r);
	clear_logs();

	initial("D", "in1", 0); /* 1 → 0 */
	drain();
	initial("D", "in2", 0); /* 0 → NaN */
	drain();
	initial("D", "in1", 0); /* NaN → NaN: nothing */
	drain();
	initial("D", "in2", 1); /* NaN → 0 */
	drain();
	CHECK_STREQ(trace, "P.in=0;P.in=NaN;P.in=0;");
	teardown();
}

static void test_threshold_precedence(void)
{
	setup();
	CHECK(block_types_register(&coarse_def) == BLOCK_TYPES_OK);
	Block *p = add("P", "test/Probe");
	Block *c = add("C", "test/Coarse");

	/* Engine default, unless the type has one. */
	CHECK(block_change_threshold(p) == RUNTIME_DEFAULT_CHANGE_THRESHOLD);
	CHECK(runtime_change_threshold(&rt) == 0.05);
	CHECK(runtime_set_change_threshold(&rt, 0.25));
	CHECK(block_change_threshold(p) == 0.25);
	CHECK(block_change_threshold(c) == 1.0);
	CHECK(!runtime_set_change_threshold(&rt, -1));
	CHECK(!runtime_set_change_threshold(&rt, NAN));
	CHECK(runtime_change_threshold(&rt) == 0.25);

	/* The instance override beats both; clearing it falls back. */
	BlockChange chg = {.threshold = THRESHOLD_SET, .change_threshold = 2.0};
	CHECK(graph_change_block(gr, "C", &chg) == GRAPH_OK);
	CHECK(block_change_threshold(c) == 2.0);
	CHECK(graph_change_block(gr, "P", &chg) == GRAPH_OK);
	CHECK(block_change_threshold(p) == 2.0);
	chg = (BlockChange){.threshold = THRESHOLD_CLEAR};
	CHECK(graph_change_block(gr, "C", &chg) == GRAPH_OK);
	CHECK(block_change_threshold(c) == 1.0);
	CHECK(isnan(c->metadata.change_threshold));
	CHECK_STREQ(events, "+node P;+node C;change C;change P;change C;");

	/* At creation, from the metadata; 0 is a valid override. */
	BlockMetadata md = block_metadata_default();
	md.change_threshold = 0;
	Block *z = NULL;
	CHECK(graph_create_block(gr, "Z", &coarse_def, &md, &z) == GRAPH_OK);
	CHECK(block_change_threshold(z) == 0);
	CHECK(z->metadata.x == 50 && z->metadata.y == 50);

	/* Behavior: C (type default 1.0) feeding Q. */
	Block *q = add("Q", "test/Probe");
	wire("C", "out", "Q", "in");
	initial("C", "in", 0);
	graph_start(gr);
	drain();
	Rec r = rec_new();
	watch(q, "in", &r);
	clear_logs();
	initial("C", "in", 0.5);
	drain();
	CHECK_STREQ(trace, "");
	initial("C", "in", 1.0);
	drain();
	CHECK_STREQ(trace, "Q.in=1;");
	chg = (BlockChange){.threshold = THRESHOLD_SET, .change_threshold = 0};
	CHECK(graph_change_block(gr, "C", &chg) == GRAPH_OK);
	initial("C", "in", 1.25);
	drain();
	CHECK_STREQ(trace, "Q.in=1;Q.in=1.25;");
	teardown();
	CHECK(block_types_unregister("test/Coarse"));
}

static void test_change_block_position(void)
{
	setup();
	Block *p = add("P", "test/Probe");
	BlockChange chg = {.set_x = true, .x = 10};
	CHECK(graph_change_block(gr, "P", &chg) == GRAPH_OK);
	CHECK(p->metadata.x == 10 && p->metadata.y == 50 && isnan(p->metadata.change_threshold));
	chg = (BlockChange){.set_y = true, .y = -3, .threshold = THRESHOLD_SET,
			    .change_threshold = 0.5};
	CHECK(graph_change_block(gr, "P", &chg) == GRAPH_OK);
	CHECK(p->metadata.x == 10 && p->metadata.y == -3 && p->metadata.change_threshold == 0.5);
	teardown();
}

/* ---- Start / stop / destroy (03 §2) ------------------------------------------ */

static void test_start_pushes_all(void)
{
	setup();
	Block *a = add("A", "math/Add2");
	Block *p = add("P", "test/Probe");
	CHECK(probe_state(p)->calls[REASON_START] == 0); /* state zeroed */
	CHECK(a->state == NULL);
	wire("A", "out", "P", "in");
	CHECK(in_f64(p, "in") == 42); /* stopped: connect delivers nothing */
	Rec r = rec_new();
	watch(p, "in", &r);
	clear_logs();

	/* A's output (0) equals its last_propagated (the default 0), but start
	 * pushes it anyway. */
	graph_start(gr);
	CHECK(probe_state(p)->calls[REASON_START] == 1);
	CHECK(probe_state(p)->last_changed == -1);
	CHECK(out_f64(p) == 42 && last_f64(p) == 42); /* P pushed its own output */
	drain();
	CHECK_STREQ(trace, "P.in=0;");
	CHECK(probe_state(p)->calls[REASON_INPUT] == 1);
	CHECK(probe_state(p)->last_changed == 0);

	graph_start(gr); /* already started: nothing */
	CHECK(probe_state(p)->calls[REASON_START] == 1);
	CHECK(queued() == 0);

	/* Stop computes once with STOP; a restart pushes again. */
	graph_stop(gr);
	CHECK(!p->started && !gr->started);
	CHECK(probe_state(p)->calls[REASON_STOP] == 1);
	graph_stop(gr);
	CHECK(probe_state(p)->calls[REASON_STOP] == 1);
	graph_start(gr);
	drain();
	CHECK_STREQ(trace, "P.in=0;P.in=0;");
	CHECK(probe_state(p)->calls[REASON_START] == 2);
	teardown();
}

static void test_stopped_graph(void)
{
	setup();
	Block *a = add("A", "math/Add2");
	Block *b = add("B", "math/Add2");
	wire("A", "out", "B", "in1");
	initial("A", "in1", 5);
	CHECK(in_f64(a, "in1") == 5);
	CHECK(out_f64(a) == 0);
	CHECK(queued() == 0);

	graph_start(gr);
	drain();
	CHECK(out_f64(b) == 5);

	graph_stop(gr);
	initial("A", "in1", 7);
	CHECK(in_f64(a, "in1") == 7);
	CHECK(out_f64(a) == 5);
	CHECK(queued() == 0);
	Linkable v = linkable_from_f64(100);
	block_output_set(a, 0, v); /* output_set on a stopped block only stores */
	CHECK(out_f64(a) == 100 && last_f64(a) == 5);
	CHECK(queued() == 0);

	/* Entries queued before a stop are delivered (stored), not computed. */
	graph_start(gr); /* A computes 7 and queues it for B */
	CHECK(queued() == 1);
	graph_stop(gr);
	drain();
	CHECK(in_f64(b, "in1") == 7);
	CHECK(out_f64(b) == 5);
	teardown();
}

static void test_hold_type(void)
{
	setup();
	CHECK(block_types_register(&hold_def) == BLOCK_TYPES_OK);
	Block *h = add("H", "test/Hold");
	graph_start(gr);
	initial("H", "in", 3);
	CHECK(in_f64(h, "in") == 3);
	CHECK(probe_state(h)->calls[REASON_INPUT] == 0); /* not compute_on_input */
	CHECK(out_f64(h) == 42);			   /* from the START compute */
	teardown();
	CHECK(block_types_unregister("test/Hold"));
}

static void test_create_in_started_graph(void)
{
	setup();
	graph_start(gr);
	Block *p = add("P", "test/Probe");
	CHECK(p->started);
	CHECK(probe_state(p)->calls[REASON_START] == 1);
	CHECK(out_f64(p) == 42 && last_f64(p) == 42);
	teardown();
}

static void test_destroy_stops(void)
{
	setup();
	add("P", "test/Probe");
	add("Q", "test/Probe");
	CHECK(graph_remove_block(gr, "P") == GRAPH_OK); /* stopped: no STOP compute */
	CHECK(probe_calls[REASON_STOP] == 0);
	graph_start(gr);
	CHECK(graph_remove_block(gr, "Q") == GRAPH_OK); /* started: STOP once */
	CHECK(probe_calls[REASON_STOP] == 1);
	add("R", "test/Probe");
	add("S", "test/Probe");
	CHECK(probe_calls[REASON_START] == 3);
	teardown(); /* destroying a started graph stops every block */
	CHECK(probe_calls[REASON_STOP] == 3);
}

static void test_output_set(void)
{
	setup();
	Block *p = add("P", "test/Probe");
	Block *q = add("Q", "test/Probe");
	wire("P", "out", "Q", "in");
	graph_start(gr);
	drain();
	CHECK(last_f64(p) == 42);
	block_output_set(p, 0, linkable_from_f64(42.01)); /* below 0.05 */
	CHECK(out_f64(p) == 42.01 && last_f64(p) == 42);
	CHECK(queued() == 0);
	block_output_set(p, 0, linkable_from_f64(43));
	CHECK(last_f64(p) == 43);
	CHECK(queued() == 1);
	drain();
	CHECK(in_f64(q, "in") == 43);
	teardown();
}

/* ---- Structure ------------------------------------------------------------------ */

static void test_connect_delivers(void)
{
	setup();
	Block *a = add("A", "math/Add2");
	Block *p = add("P", "test/Probe");
	initial("A", "in1", 3);
	graph_start(gr);
	drain();
	clear_logs();

	wire("A", "out", "P", "in");
	CHECK(queued() == 0);
	CHECK(in_f64(p, "in") == 3); /* at once, no drain needed */
	CHECK(probe_state(p)->calls[REASON_INPUT] == 1);
	CHECK(out_f64(p) == 3);
	CHECK_STREQ(events, "+edge A.out>P.in;");
	CHECK(a->outputs[0].link_count == 1);
	CHECK(p->inputs[0].source == INPUT_SOURCE_EDGE && p->inputs[0].src_block == a);

	/* last_propagated, not a newer value held back by the threshold. */
	CHECK(graph_disconnect(gr, "A", "out", "P", "in") == GRAPH_OK);
	initial("A", "in2", 0.01);
	CHECK(out_f64(a) == 3.01 && last_f64(a) == 3);
	wire("A", "out", "P", "in");
	CHECK(in_f64(p, "in") == 3);

	/* Stopped: the input keeps its value until the graph starts. */
	graph_stop(gr);
	CHECK(graph_disconnect(gr, "A", "out", "P", "in") == GRAPH_OK);
	CHECK(in_f64(p, "in") == 42);
	wire("A", "out", "P", "in");
	CHECK(in_f64(p, "in") == 42);
	graph_start(gr);
	drain();
	CHECK(in_f64(p, "in") == 3.01);
	teardown();
}

static void test_fan_in_refused(void)
{
	setup();
	Block *a = add("A", "math/Add2");
	add("B", "math/Add2");
	Block *c = add("C", "math/Add2");
	wire("A", "out", "C", "in1");
	initial("C", "in2", 1);
	clear_logs();

	CHECK(graph_connect(gr, "B", "out", "C", "in1", NULL) == GRAPH_ERR_INPUT_CONNECTED);
	CHECK(graph_connect(gr, "A", "out", "C", "in1", NULL) == GRAPH_ERR_INPUT_CONNECTED);
	CHECK(graph_connect(gr, "B", "out", "C", "in2", NULL) == GRAPH_ERR_INPUT_CONNECTED);
	Linkable v = linkable_from_f64(9);
	CHECK(graph_add_initial(gr, "C", "in1", &v) == GRAPH_ERR_INPUT_CONNECTED);
	CHECK(c->inputs[0].src_block == a && c->inputs[1].source == INPUT_SOURCE_INITIAL);
	CHECK(in_f64(c, "in2") == 1);
	CHECK_STREQ(events, "");

	/* Fan-out is fine. */
	wire("A", "out", "B", "in1");
	CHECK(a->outputs[0].link_count == 2);
	teardown();
}

static void test_initial_replace(void)
{
	setup();
	Block *p = add("P", "test/Probe");
	graph_start(gr);
	uint32_t gen = p->inputs[0].source_gen;
	initial("P", "in", 1);
	CHECK(in_f64(p, "in") == 1 && out_f64(p) == 1);
	CHECK(p->inputs[0].source == INPUT_SOURCE_INITIAL);
	CHECK(p->inputs[0].source_gen == gen + 1);
	initial("P", "in", 2);
	CHECK(in_f64(p, "in") == 2 && out_f64(p) == 2);
	CHECK(linkable_to_f64(&p->inputs[0].initial) == 2);
	CHECK(p->inputs[0].source_gen == gen + 2);
	CHECK(graph_remove_initial(gr, "P", "in") == GRAPH_OK);
	CHECK(in_f64(p, "in") == 42 && out_f64(p) == 42);
	CHECK(p->inputs[0].source == INPUT_SOURCE_NONE);
	CHECK(p->inputs[0].source_gen == gen + 3);
	CHECK(graph_remove_initial(gr, "P", "in") == GRAPH_ERR_NO_SUCH_CONNECTION);
	CHECK_STREQ(events, "+node P;+init 1>P.in;+init 2>P.in;-init 2>P.in;");
	teardown();
}

static void test_severing(void)
{
	setup();
	Block *a = add("A", "math/Add2");
	Block *p = add("P", "test/Probe");
	Block *q = add("Q", "test/Probe");
	initial("A", "in1", 3);
	wire("A", "out", "P", "in");
	wire("P", "out", "Q", "in");
	initial("P", "aux", 5);
	graph_start(gr);
	drain();
	CHECK(in_f64(q, "in") == 3);

	/* Removing an edge: the input reverts at once and that propagates. */
	CHECK(graph_disconnect(gr, "A", "out", "P", "in") == GRAPH_OK);
	CHECK(in_f64(p, "in") == 42 && out_f64(p) == 42);
	CHECK(in_f64(q, "in") == 3);
	drain();
	CHECK(in_f64(q, "in") == 42);

	/* An input with no stated default reverts to default_value(type). */
	CHECK(graph_remove_initial(gr, "P", "aux") == GRAPH_OK);
	CHECK(p->in_values[1].type == LINKABLE_F64 && p->in_values[1].as.f64 == 0);

	/* Removing the upstream block. */
	wire("A", "out", "P", "in");
	drain();
	CHECK(in_f64(q, "in") == 3);
	CHECK(graph_remove_block(gr, "A") == GRAPH_OK);
	CHECK(in_f64(p, "in") == 42);
	CHECK(p->inputs[0].source == INPUT_SOURCE_NONE);
	drain();
	CHECK(in_f64(q, "in") == 42);
	(void)a;
	teardown();
}

static void test_stale_entries(void)
{
	setup();
	add("A", "math/Add2");
	Block *b = add("B", "math/Add2");
	Block *p = add("P", "test/Probe");
	initial("B", "in1", 9);
	wire("A", "out", "P", "in");
	graph_start(gr);
	drain();

	/* A removed edge: its queued value is dropped. */
	initial("A", "in1", 1);
	CHECK(queued() == 1);
	CHECK(graph_disconnect(gr, "A", "out", "P", "in") == GRAPH_OK);
	CHECK(runtime_drain(&rt, SIZE_MAX) == 1);
	CHECK(in_f64(p, "in") == 42);

	/* A replaced source: values from the old one are dropped. */
	wire("A", "out", "P", "in");
	initial("A", "in1", 2);
	CHECK(queued() == 1);
	CHECK(graph_disconnect(gr, "A", "out", "P", "in") == GRAPH_OK);
	wire("B", "out", "P", "in");
	CHECK(in_f64(p, "in") == 9);
	drain();
	CHECK(in_f64(p, "in") == 9);
	CHECK(graph_disconnect(gr, "B", "out", "P", "in") == GRAPH_OK);

	/* A removed block: entries addressed to it go stale, even when its
	 * slot is reused at once. */
	wire("A", "out", "P", "in");
	initial("A", "in1", 3);
	CHECK(queued() == 1);
	BlockHandle old = p->handle;
	CHECK(runtime_block(&rt, old) == p);
	CHECK(graph_remove_block(gr, "P") == GRAPH_OK);
	CHECK(runtime_block(&rt, old) == NULL);
	Block *p2 = add("P2", "test/Probe");
	CHECK(p2->handle.slot == old.slot && p2->handle.gen == old.gen + 1);
	CHECK(runtime_block(&rt, old) == NULL);
	drain();
	CHECK(in_f64(p2, "in") == 42);
	CHECK(probe_state(p2)->calls[REASON_INPUT] == 0);

	/* A removed upstream block: its queued values are dropped. */
	wire("A", "out", "P2", "in");
	CHECK(in_f64(p2, "in") == 3);
	initial("A", "in1", 4);
	CHECK(queued() == 1);
	CHECK(graph_remove_block(gr, "A") == GRAPH_OK);
	CHECK(in_f64(p2, "in") == 42);
	drain();
	CHECK(in_f64(p2, "in") == 42);
	(void)b;
	teardown();
}

static void test_rename_keeps_links(void)
{
	setup();
	add("A", "math/Add2");
	Block *b = add("B", "math/Add2");
	add("C", "math/Add2");
	wire("A", "out", "B", "in1");
	wire("B", "out", "C", "in1");
	graph_start(gr);
	drain();
	clear_logs();
	CHECK(graph_rename_block(gr, "B", "my.block.B") == GRAPH_OK);
	CHECK_STREQ(events, "rename B>my.block.B;");
	CHECK(graph_find_block(gr, "B") == NULL);
	CHECK(graph_find_block(gr, "my.block.B") == b);
	CHECK_STREQ(b->name, "my.block.B");
	initial("A", "in1", 4);
	drain();
	CHECK(in_f64(b, "in1") == 4);
	CHECK(in_f64(graph_find_block(gr, "C"), "in1") == 4);
	CHECK(graph_disconnect(gr, "my.block.B", "out", "C", "in1") == GRAPH_OK);
	teardown();
}

static void test_cycle(void)
{
	setup();
	Block *a = add("A", "math/Add2");
	wire("A", "out", "A", "in1");
	initial("A", "in2", 1);
	graph_start(gr); /* out = 0 + 1, pushed to itself */
	CHECK(runtime_drain(&rt, 100) == 100);
	CHECK(out_f64(a) == 101);
	CHECK(queued() == 1); /* still going */
	CHECK(runtime_drain(&rt, 0) == 0);

	/* Cutting the loop ends it; the pending entry is stale. */
	CHECK(graph_disconnect(gr, "A", "out", "A", "in1") == GRAPH_OK);
	CHECK(out_f64(a) == 1);
	CHECK(runtime_drain(&rt, SIZE_MAX) == 1);
	CHECK(queued() == 0);

	/* Removing a block in a cycle. */
	wire("A", "out", "A", "in1");
	CHECK(runtime_drain(&rt, 10) == 10);
	clear_logs();
	CHECK(graph_remove_block(gr, "A") == GRAPH_OK);
	CHECK_STREQ(events, "-edge A.out>A.in1;-init 1>A.in2;-node A;");
	CHECK(runtime_drain(&rt, SIZE_MAX) == 1);
	teardown();
}

static void test_remove_block_events(void)
{
	setup();
	add("A", "math/Add2");
	add("B", "math/Add2");
	Block *c = add("C", "math/Add2");
	Block *d = add("D", "math/Add2");
	add("E", "math/Add2");
	wire("A", "out", "B", "in1");
	initial("B", "in2", 7);
	wire("B", "out", "C", "in1");
	wire("B", "out", "D", "in1");
	wire("D", "out", "E", "in1");
	graph_start(gr);
	drain();
	CHECK(in_f64(c, "in1") == 7 && in_f64(d, "in1") == 7);
	clear_logs();

	CHECK(graph_remove_block(gr, "B") == GRAPH_OK);
	CHECK_STREQ(events, "-edge A.out>B.in1;-init 7>B.in2;-edge B.out>C.in1;"
			    "-edge B.out>D.in1;-node B;");
	CHECK(gr->block_count == 4);
	CHECK(graph_find_block(gr, "B") == NULL);
	CHECK(graph_find_block(gr, "A")->outputs[0].link_count == 0);
	CHECK(in_f64(c, "in1") == 0 && out_f64(c) == 0);
	CHECK(in_f64(d, "in1") == 0 && out_f64(d) == 0);
	drain();
	CHECK(in_f64(graph_find_block(gr, "E"), "in1") == 0);
	/* Creation order is kept. */
	CHECK_STREQ(gr->blocks[0]->name, "A");
	CHECK_STREQ(gr->blocks[1]->name, "C");
	CHECK_STREQ(gr->blocks[3]->name, "E");
	teardown();
}

static void test_live_value_watch(void)
{
	setup();
	Block *p = add("P", "test/Probe");
	Rec r1 = rec_new(), r2 = rec_new();
	watch(p, "in", &r1); /* the current value at once */
	CHECK_STREQ(trace, "P.in=42;");
	watch(p, "in", &r2);
	initial("P", "in", 5); /* stopped graph: still an update */
	CHECK_STREQ(trace, "P.in=42;P.in=42;P.in=5;P.in=5;");
	CHECK(r1.values == 2 && r2.values == 2);
	block_unwatch_input(p, 0, &r1.w);
	CHECK(graph_remove_initial(gr, "P", "in") == GRAPH_OK);
	CHECK(r1.values == 2 && r2.values == 3);
	CHECK(r1.detached == 0);

	/* The block goes: remaining watches are detached. */
	CHECK(graph_remove_block(gr, "P") == GRAPH_OK);
	CHECK(r2.detached == 1 && r1.detached == 0);

	/* So does destroying the graph. */
	Block *q = add("Q", "test/Probe");
	Rec r3 = rec_new();
	watch(q, "aux", &r3);
	teardown();
	CHECK(r3.detached == 1);
}

static void test_port_ref_split(void)
{
	size_t len = 0;
	const char *port = NULL;
	CHECK(port_ref_split("A.out", &len, &port));
	CHECK(len == 1 && strcmp(port, "out") == 0);
	const char *ref = "my.block.in1";
	CHECK(port_ref_split(ref, &len, &port));
	CHECK(len == 8 && strncmp(ref, "my.block", len) == 0);
	CHECK_STREQ(port, "in1");
	CHECK(port_ref_split("a..b", &len, &port));
	CHECK(len == 2 && strcmp(port, "b") == 0);
	CHECK(!port_ref_split("nodot", &len, &port));
	CHECK(!port_ref_split(".port", &len, &port));
	CHECK(!port_ref_split("block.", &len, &port));
	CHECK(!port_ref_split("", &len, &port));

	/* With the length-based lookup. */
	setup();
	Block *b = add("my.block", "math/Add2");
	CHECK(port_ref_split(ref, &len, &port));
	CHECK(graph_find_block_n(gr, ref, len) == b);
	CHECK(block_input_index(b, port) == 0);
	CHECK(graph_find_block_n(gr, ref, 2) == NULL);
	teardown();
}

/* ---- Refusals (02 §3.4.1) --------------------------------------------------------- */

/* Everything observable about the graph, as text. */
static void fingerprint(char *buf, size_t size)
{
	buf[0] = '\0';
	append(buf, size, "started=%d q=%zu;", gr->started, queued());
	for (uint32_t k = 0; k < gr->block_count; k++) {
		const Block *b = gr->blocks[k];
		append(buf, size, "%s[%s x=%g y=%g t=%g %s h=%u/%u]", b->name, block_component(b),
		       b->metadata.x, b->metadata.y, b->metadata.change_threshold,
		       b->metadata.extra ? b->metadata.extra : "-", (unsigned)b->handle.slot,
		       (unsigned)b->handle.gen);
		for (uint32_t i = 0; i < b->input_count; i++) {
			const Input *in = &b->inputs[i];
			append(buf, size, " in%u=%s src=%d:%s.%u init=%s gen=%u", (unsigned)i,
			       text(&b->in_values[i]), (int)in->source,
			       in->src_block ? in->src_block->name : "-", (unsigned)in->src_output,
			       text(&in->initial), (unsigned)in->source_gen);
			if (in->edge_metadata != NULL)
				append(buf, size, " md=%s", in->edge_metadata);
		}
		for (uint32_t o = 0; o < b->output_count; o++) {
			const Output *out = &b->outputs[o];
			append(buf, size, " out%u=%s last=%s links:", (unsigned)o,
			       text(&b->out_values[o]), text(&out->last_propagated));
			for (uint32_t l = 0; l < out->link_count; l++)
				append(buf, size, "%s.%u,", out->links[l].block->name,
				       (unsigned)out->links[l].input);
		}
		append(buf, size, ";");
	}
}

static char before[8192], after[8192];

#define REFUSED(expr, status)                                                                      \
	do {                                                                                       \
		fingerprint(before, sizeof(before));                                               \
		clear_logs();                                                                      \
		CHECK((expr) == (status));                                                         \
		fingerprint(after, sizeof(after));                                                 \
		CHECK_STREQ(after, before);                                                        \
		CHECK_STREQ(events, "");                                                           \
	} while (0)

static void test_refusals(void)
{
	setup();
	add("A", "math/Add2");
	add("B", "math/Add2");
	add("C", "math/Add2");
	wire("A", "out", "B", "in1");
	initial("B", "in2", 3);
	graph_start(gr);
	drain();

	char long_id[GRAPH_ID_MAX + 2];
	memset(long_id, 'x', sizeof(long_id) - 1);
	long_id[GRAPH_ID_MAX + 1] = '\0'; /* 65 bytes */
	Linkable v = linkable_from_f64(1);
	const BlockTypeDef *add2 = block_types_get("math/Add2");
	BlockMetadata bad_md = block_metadata_default();
	bad_md.change_threshold = -1;
	BlockChange bad_chg = {.threshold = THRESHOLD_SET, .change_threshold = -0.5};
	BlockChange nan_chg = {.threshold = THRESHOLD_SET, .change_threshold = NAN};
	BlockChange ok_chg = {.set_x = true, .x = 1};

	/* Ids. */
	CHECK(!graph_id_valid(NULL) && !graph_id_valid("") && !graph_id_valid(long_id));
	REFUSED(graph_create_block(gr, "", add2, NULL, NULL), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_create_block(gr, NULL, add2, NULL, NULL), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_create_block(gr, long_id, add2, NULL, NULL), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_remove_block(gr, ""), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_rename_block(gr, "A", ""), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_rename_block(gr, "A", long_id), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_rename_block(gr, long_id, "Z"), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_change_block(gr, "", &ok_chg), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_connect(gr, "", "out", "C", "in1", NULL), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_connect(gr, "A", "out", long_id, "in1", NULL), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_disconnect(gr, "A", "out", "", "in1"), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_add_initial(gr, "", "in1", &v), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_remove_initial(gr, long_id, "in2"), GRAPH_ERR_INVALID_ID);
	long_id[GRAPH_ID_MAX] = '\0'; /* 64 bytes is valid */
	CHECK(graph_id_valid(long_id));
	CHECK(graph_create_block(gr, long_id, add2, NULL, NULL) == GRAPH_OK);
	CHECK(graph_remove_block(gr, long_id) == GRAPH_OK);

	/* Node exists. */
	REFUSED(graph_create_block(gr, "A", add2, NULL, NULL), GRAPH_ERR_NODE_EXISTS);
	REFUSED(graph_rename_block(gr, "A", "B"), GRAPH_ERR_NODE_EXISTS);
	REFUSED(graph_rename_block(gr, "A", "A"), GRAPH_ERR_NODE_EXISTS);

	/* Unknown node. */
	REFUSED(graph_remove_block(gr, "X"), GRAPH_ERR_UNKNOWN_NODE);
	REFUSED(graph_rename_block(gr, "X", "Y"), GRAPH_ERR_UNKNOWN_NODE);
	REFUSED(graph_change_block(gr, "X", &ok_chg), GRAPH_ERR_UNKNOWN_NODE);
	REFUSED(graph_connect(gr, "X", "out", "C", "in1", NULL), GRAPH_ERR_UNKNOWN_NODE);
	REFUSED(graph_connect(gr, "A", "out", "X", "in1", NULL), GRAPH_ERR_UNKNOWN_NODE);
	REFUSED(graph_disconnect(gr, "X", "out", "B", "in1"), GRAPH_ERR_UNKNOWN_NODE);
	REFUSED(graph_disconnect(gr, "A", "out", "X", "in1"), GRAPH_ERR_UNKNOWN_NODE);
	REFUSED(graph_add_initial(gr, "X", "in1", &v), GRAPH_ERR_UNKNOWN_NODE);
	REFUSED(graph_remove_initial(gr, "X", "in2"), GRAPH_ERR_UNKNOWN_NODE);
	/* The source is checked before the target. */
	REFUSED(graph_connect(gr, "X", "nope", "Y", "in1", NULL), GRAPH_ERR_UNKNOWN_NODE);
	REFUSED(graph_connect(gr, "A", "nope", "Y", "in1", NULL), GRAPH_ERR_UNKNOWN_PORT);

	/* Unknown port. */
	REFUSED(graph_connect(gr, "A", "nope", "C", "in1", NULL), GRAPH_ERR_UNKNOWN_PORT);
	REFUSED(graph_connect(gr, "A", "out", "C", "nope", NULL), GRAPH_ERR_UNKNOWN_PORT);
	REFUSED(graph_disconnect(gr, "A", "nope", "B", "in1"), GRAPH_ERR_UNKNOWN_PORT);
	REFUSED(graph_disconnect(gr, "A", "out", "B", "nope"), GRAPH_ERR_UNKNOWN_PORT);
	REFUSED(graph_add_initial(gr, "C", "nope", &v), GRAPH_ERR_UNKNOWN_PORT);
	REFUSED(graph_remove_initial(gr, "B", "nope"), GRAPH_ERR_UNKNOWN_PORT);

	/* Invalid edge direction. */
	REFUSED(graph_connect(gr, "A", "in1", "C", "in1", NULL), GRAPH_ERR_INVALID_EDGE_DIRECTION);
	REFUSED(graph_connect(gr, "A", "out", "C", "out", NULL), GRAPH_ERR_INVALID_EDGE_DIRECTION);
	REFUSED(graph_add_initial(gr, "C", "out", &v), GRAPH_ERR_INVALID_EDGE_DIRECTION);

	/* Input already connected (no fan-in). */
	REFUSED(graph_connect(gr, "C", "out", "B", "in1", NULL), GRAPH_ERR_INPUT_CONNECTED);
	REFUSED(graph_connect(gr, "C", "out", "B", "in2", NULL), GRAPH_ERR_INPUT_CONNECTED);
	REFUSED(graph_add_initial(gr, "B", "in1", &v), GRAPH_ERR_INPUT_CONNECTED);

	/* No such connection. */
	REFUSED(graph_disconnect(gr, "C", "out", "B", "in1"), GRAPH_ERR_NO_SUCH_CONNECTION);
	REFUSED(graph_disconnect(gr, "A", "out", "C", "in1"), GRAPH_ERR_NO_SUCH_CONNECTION);
	REFUSED(graph_disconnect(gr, "A", "out", "B", "in2"), GRAPH_ERR_NO_SUCH_CONNECTION);
	REFUSED(graph_disconnect(gr, "A", "in1", "B", "in1"), GRAPH_ERR_NO_SUCH_CONNECTION);
	REFUSED(graph_disconnect(gr, "A", "out", "B", "out"), GRAPH_ERR_NO_SUCH_CONNECTION);
	REFUSED(graph_remove_initial(gr, "C", "in1"), GRAPH_ERR_NO_SUCH_CONNECTION);
	REFUSED(graph_remove_initial(gr, "B", "in1"), GRAPH_ERR_NO_SUCH_CONNECTION);
	REFUSED(graph_remove_initial(gr, "B", "out"), GRAPH_ERR_NO_SUCH_CONNECTION);

	/* Invalid arguments. */
	REFUSED(graph_create_block(gr, "N", NULL, NULL, NULL), GRAPH_ERR_INVALID_ARGUMENT);
	REFUSED(graph_create_block(gr, "N", add2, &bad_md, NULL), GRAPH_ERR_INVALID_ARGUMENT);
	REFUSED(graph_change_block(gr, "A", &bad_chg), GRAPH_ERR_INVALID_ARGUMENT);
	REFUSED(graph_change_block(gr, "A", &nan_chg), GRAPH_ERR_INVALID_ARGUMENT);
	REFUSED(graph_change_block(gr, "A", NULL), GRAPH_ERR_INVALID_ARGUMENT);
	REFUSED(graph_add_initial(gr, "C", "in1", NULL), GRAPH_ERR_INVALID_ARGUMENT);
	static Block dummy;
	Block *out = &dummy;
	CHECK(graph_create_block(gr, "A", add2, NULL, &out) == GRAPH_ERR_NODE_EXISTS);
	CHECK(out == NULL);
	teardown();
}

/* ---- Strings through the queue (asan: no leaks) -------------------------- */

static void test_string_values(void)
{
	setup();
	add("P", "test/Probe");
	Block *q = add("Q", "test/Probe");
	add("R", "test/Probe");
	wire("P", "out", "Q", "in");
	wire("Q", "out", "R", "in");
	Linkable s = {0};
	CHECK(linkable_from_string(&s, "hello world", 11));
	CHECK(graph_add_initial(gr, "P", "in", &s) == GRAPH_OK);
	graph_start(gr);
	drain();
	CHECK(q->in_values[0].type == LINKABLE_STRING);
	CHECK_STREQ(q->in_values[0].as.s, "hello world");
	Linkable s2 = {0};
	CHECK(linkable_from_string(&s2, "bye", 3));
	CHECK(graph_add_initial(gr, "P", "in", &s2) == GRAPH_OK);
	CHECK(queued() == 1); /* left queued: destroyed with the runtime */
	linkable_clear(&s);
	linkable_clear(&s2);
	CHECK(graph_remove_initial(gr, "P", "in") == GRAPH_OK);
	teardown();
}

/* Placeholder blocks (05 §2.2) and opaque metadata (01 §2.2): a placeholder
 * has no ports and takes part in no dataflow; metadata text is copied. */
static void test_placeholder_and_metadata(void)
{
	setup();
	char extra[] = "{\"label\":\"a\"}";
	BlockMetadata md = block_metadata_default();
	md.x = 3;
	md.extra = extra; /* only read: the block takes a copy */
	Block *p = NULL;
	CHECK(graph_create_placeholder(gr, "P", "vendor/Gizmo", &md, &p) == GRAPH_OK);
	CHECK_STREQ(events, "+node P;");
	CHECK(p != NULL && block_is_placeholder(p) && p->input_count == 0 && p->output_count == 0);
	CHECK_STREQ(block_component(p), "vendor/Gizmo");
	CHECK(p->metadata.x == 3 && p->metadata.extra != extra);
	CHECK_STREQ(p->metadata.extra, extra);
	Block *a = add("A", "math/Add2");
	CHECK_STREQ(block_component(a), "math/Add2");
	CHECK(a->metadata.extra == NULL);

	/* No ports: every edge or initial is refused like an unknown port. */
	Linkable v = linkable_from_f64(1);
	CHECK(graph_connect(gr, "A", "out", "P", "x", NULL) == GRAPH_ERR_UNKNOWN_PORT);
	CHECK(graph_connect(gr, "P", "y", "A", "in1", NULL) == GRAPH_ERR_UNKNOWN_PORT);
	CHECK(graph_add_initial(gr, "P", "x", &v) == GRAPH_ERR_UNKNOWN_PORT);
	/* Start, stop and changing metadata work as for any block. */
	graph_start(gr);
	runtime_drain(&rt, 100);
	CHECK(p->started);
	BlockChange chg = {.set_extra = true, .extra = (char *)"{\"b\":2}"};
	CHECK(graph_change_block(gr, "P", &chg) == GRAPH_OK);
	CHECK_STREQ(p->metadata.extra, "{\"b\":2}");
	chg = (BlockChange){.set_extra = true, .extra = NULL};
	CHECK(graph_change_block(gr, "P", &chg) == GRAPH_OK);
	CHECK(p->metadata.extra == NULL);
	graph_stop(gr);

	/* Refusals as for graph_create_block. */
	REFUSED(graph_create_placeholder(gr, "P", "x/Y", NULL, NULL), GRAPH_ERR_NODE_EXISTS);
	REFUSED(graph_create_placeholder(gr, "", "x/Y", NULL, NULL), GRAPH_ERR_INVALID_ID);
	REFUSED(graph_create_placeholder(gr, "Q", NULL, NULL, NULL), GRAPH_ERR_INVALID_ARGUMENT);

	/* Edge metadata lives on the destination input, and goes with the
	 * edge. */
	Block *b = add("B", "math/Add2");
	CHECK(graph_connect(gr, "A", "out", "B", "in1", "{\"route\":1}") == GRAPH_OK);
	CHECK_STREQ(b->inputs[0].edge_metadata, "{\"route\":1}");
	clear_logs();
	CHECK(graph_change_edge(gr, "A", "out", "B", "in1", "{\"route\":2}") == GRAPH_OK);
	CHECK_STREQ(events, "other;"); /* EDGE_CHANGED */
	CHECK_STREQ(b->inputs[0].edge_metadata, "{\"route\":2}");
	REFUSED(graph_change_edge(gr, "A", "out", "B", "in2", "{}"), GRAPH_ERR_NO_SUCH_CONNECTION);
	CHECK(graph_disconnect(gr, "A", "out", "B", "in1") == GRAPH_OK);
	CHECK(b->inputs[0].edge_metadata == NULL);
	CHECK(graph_connect(gr, "A", "out", "B", "in1", "{\"route\":3}") == GRAPH_OK);
	/* Freed with the block (asan checks). */
	CHECK(graph_remove_block(gr, "P") == GRAPH_OK);
	teardown();
}

int main(void)
{
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	CHECK(block_types_register(&probe_def) == BLOCK_TYPES_OK);
	RUN(test_chain);
	RUN(test_breadth_first_fifo);
	RUN(test_value_changed);
	RUN(test_threshold_drift);
	RUN(test_threshold_zero);
	RUN(test_threshold_nan_transitions);
	RUN(test_threshold_precedence);
	RUN(test_change_block_position);
	RUN(test_start_pushes_all);
	RUN(test_stopped_graph);
	RUN(test_hold_type);
	RUN(test_create_in_started_graph);
	RUN(test_destroy_stops);
	RUN(test_output_set);
	RUN(test_connect_delivers);
	RUN(test_fan_in_refused);
	RUN(test_initial_replace);
	RUN(test_severing);
	RUN(test_stale_entries);
	RUN(test_rename_keeps_links);
	RUN(test_cycle);
	RUN(test_remove_block_events);
	RUN(test_live_value_watch);
	RUN(test_port_ref_split);
	RUN(test_refusals);
	RUN(test_string_values);
	RUN(test_placeholder_and_metadata);
	block_types_reset();
	return test_result();
}
