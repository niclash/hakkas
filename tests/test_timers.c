/*
 * Timers (01 §3.2 item 7, 03 §2) and timing/Interval (03 §3.1), driven with
 * explicit engine time: the tests play the engine loop, calling
 * runtime_fire_timers with the time they choose and draining the queue.
 */
#include "test.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>

#include "blocktypes/block_types.h"
#include "graphs/deadlines.h"
#include "graphs/graph.h"
#include "system/log.h"

static Runtime rt;
static Graph *gr;

static char trace[4096];

static void append(const char *fmt, ...) LOG_PRINTF_FORMAT(1, 2);

static void append(const char *fmt, ...)
{
	size_t len = strlen(trace);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(trace + len, sizeof(trace) - len, fmt, ap);
	va_end(ap);
}

/* ---- Test types ------------------------------------------------------------- */

static const char reason_letter[] = {'S', 'I', 'T', 'P'}; /* START INPUT TIMER stoP */

typedef struct {
	int calls[4];
} CountState;

/* test/Tick: TIMER_PERIODIC, period 10, not computing on input. Logs
 * "<id>T@<now>;" for each REASON_TIMER call; out counts them. Returns 5,
 * which the runtime must ignore for a periodic type. */
static int64_t tick(BlockCtx *ctx)
{
	CountState *s = ctx->state;
	s->calls[ctx->reason]++;
	CHECK(ctx->now_ms == rt.now_ms);
	if (ctx->reason == REASON_TIMER)
		append("%" PRId64 "T@%" PRIu64 ";", linkable_to_i64(&ctx->inputs[0]), ctx->now_ms);
	block_ctx_set_output(ctx, 0, linkable_from_i64(s->calls[REASON_TIMER]));
	return 5;
}

/* test/Sched: TIMER_SCHEDULED, computing on input. Logs
 * "<id><reason>@<now>;" for each call and returns the "delay" input (for
 * STOP too, which the runtime must ignore). */
static int64_t sched(BlockCtx *ctx)
{
	CountState *s = ctx->state;
	s->calls[ctx->reason]++;
	CHECK(ctx->now_ms == rt.now_ms);
	append("%" PRId64 "%c@%" PRIu64 ";", linkable_to_i64(&ctx->inputs[1]),
	       reason_letter[ctx->reason], ctx->now_ms);
	block_ctx_set_output(ctx, 0, linkable_from_i64(s->calls[REASON_TIMER]));
	return linkable_to_i64(&ctx->inputs[0]);
}

static const PortDescriptor tick_in[] = {
	{.name = "id", .description = "", .type = "number", .default_value = LINKABLE_I64_INIT(0)},
};

static const PortDescriptor sched_in[] = {
	{.name = "delay", .description = "", .type = "number",
	 .default_value = LINKABLE_I64_INIT(-1)},
	{.name = "id", .description = "", .type = "number", .default_value = LINKABLE_I64_INIT(0)},
};

static const PortDescriptor count_out[] = {
	{.name = "out", .description = "", .type = "number", .default_value = LINKABLE_I64_INIT(0)},
};

static const BlockTypeDef tick_def = {.name = "test/Tick",
				      .icon = "tick",
				      .description = "",
				      .inputs = tick_in,
				      .input_count = 1,
				      .outputs = count_out,
				      .output_count = 1,
				      .compute_on_input = false,
				      .timer = TIMER_PERIODIC,
				      .period_ms = 10,
				      .compute = tick,
				      .state_size = sizeof(CountState),
				      .change_threshold = NAN};

static const BlockTypeDef sched_def = {.name = "test/Sched",
				       .icon = "sched",
				       .description = "",
				       .inputs = sched_in,
				       .input_count = 2,
				       .outputs = count_out,
				       .output_count = 1,
				       .compute_on_input = true,
				       .timer = TIMER_SCHEDULED,
				       .compute = sched,
				       .state_size = sizeof(CountState),
				       .change_threshold = NAN};

/* ---- Helpers ---------------------------------------------------------------- */

static void setup(void)
{
	runtime_init(&rt);
	gr = graph_create(&rt);
	CHECK(gr != NULL);
	trace[0] = '\0';
}

static void teardown(void)
{
	graph_destroy(gr);
	gr = NULL;
	CHECK(deadline_heap_len(&rt.timers) == 0);
	CHECK(rt.timer_blocks == 0);
	runtime_deinit(&rt);
}

static Block *add(const char *name, const char *type_name)
{
	Block *b = NULL;
	const BlockTypeDef *def = block_types_get(type_name);
	CHECK(def != NULL);
	CHECK(graph_create_block(gr, name, def, NULL, &b) == GRAPH_OK);
	return b;
}

static void set_num(const char *node, const char *port, double v)
{
	Linkable value = linkable_from_f64(v);
	CHECK(graph_add_initial(gr, node, port, &value) == GRAPH_OK);
}

static void set_value(const char *node, const char *port, Linkable v)
{
	CHECK(graph_add_initial(gr, node, port, &v) == GRAPH_OK);
	linkable_clear(&v);
}

static void drain(void)
{
	runtime_drain(&rt, SIZE_MAX);
}

/* One engine loop iteration at time now: fire, then drain. Returns the
 * number of expiries fired. */
static size_t fire(uint64_t now)
{
	size_t n = runtime_fire_timers(&rt, now);
	drain();
	return n;
}

/* The earliest deadline, or UINT64_MAX if no timer is armed. */
static uint64_t next_due(void)
{
	uint64_t due;
	return runtime_next_deadline(&rt, &due) ? due : UINT64_MAX;
}

/* A perfect engine loop: fire every deadline exactly on time up to end. */
static void run_until(uint64_t end)
{
	while (next_due() <= end)
		fire(next_due());
	runtime_set_now(&rt, end);
}

static int timer_calls(const Block *b)
{
	return ((const CountState *)b->state)->calls[REASON_TIMER];
}

/* ---- The deadline heap -------------------------------------------------- */

static void test_deadline_heap(void)
{
	DeadlineHeap h;
	Deadline d[40];
	deadline_heap_init(&h);
	for (int i = 0; i < 40; i++)
		d[i] = deadline_none();

	/* No room reserved: setting is refused. */
	CHECK(!deadline_heap_set(&h, &d[0], 1));
	CHECK(!deadline_armed(&d[0]));
	CHECK(deadline_heap_reserve(&h, 40));
	CHECK(h.cap >= 40);

	/* Each due time 0..9 four times; equal due times come out in set order. */
	for (int i = 0; i < 40; i++)
		CHECK(deadline_heap_set(&h, &d[i], (uint64_t)((i * 7) % 10)));
	CHECK(deadline_heap_len(&h) == 40);
	CHECK(deadline_heap_peek(&h)->due_ms == 0);
	CHECK(deadline_heap_pop_due(&h, 0) == &d[0]);
	CHECK(!deadline_armed(&d[0]));
	CHECK(d[0].due_ms == 0);

	/* Remove some from the middle, move some. */
	deadline_heap_remove(&h, &d[13]); /* due 1 */
	deadline_heap_remove(&h, &d[39]); /* due 3 */
	deadline_heap_remove(&h, &d[13]); /* no-op: not armed */
	CHECK(!deadline_armed(&d[13]));
	CHECK(deadline_heap_set(&h, &d[5], 100)); /* due 5 -> 100, moved later */
	CHECK(deadline_heap_set(&h, &d[38], 0));  /* due 6 -> 0, moved earlier */
	CHECK(deadline_heap_len(&h) == 37);

	CHECK(deadline_heap_pop_due(&h, 0) == &d[10]); /* set before d[38]'s move */
	CHECK(deadline_heap_pop_due(&h, 0) == &d[20]);
	CHECK(deadline_heap_pop_due(&h, 0) == &d[30]);
	CHECK(deadline_heap_pop_due(&h, 0) == &d[38]);
	CHECK(deadline_heap_pop_due(&h, 0) == NULL); /* the rest is due later */

	uint64_t last_due = 0;
	int last_i = -1, popped = 0;
	Deadline *p;
	while ((p = deadline_heap_pop_due(&h, 99)) != NULL) {
		int i = (int)(p - d);
		CHECK(p->due_ms >= last_due);
		CHECK(p->due_ms == (uint64_t)((i * 7) % 10));
		if (p->due_ms == last_due)
			CHECK(i > last_i);
		last_due = p->due_ms;
		last_i = i;
		popped++;
	}
	CHECK(popped == 32);
	CHECK(deadline_heap_len(&h) == 1);
	CHECK(deadline_heap_peek(&h) == &d[5]);
	CHECK(deadline_heap_pop_due(&h, 100) == &d[5]);
	CHECK(deadline_heap_peek(&h) == NULL);
	CHECK(deadline_heap_pop_due(&h, UINT64_MAX) == NULL);

	/* Many random-ish insertions and removals keep the order. */
	for (int i = 0; i < 40; i++)
		CHECK(deadline_heap_set(&h, &d[i], (uint64_t)((i * 7919) % 101)));
	for (int i = 0; i < 40; i += 3)
		deadline_heap_remove(&h, &d[i]);
	last_due = 0;
	while ((p = deadline_heap_pop_due(&h, UINT64_MAX)) != NULL) {
		CHECK(p->due_ms >= last_due);
		CHECK((p - d) % 3 != 0);
		last_due = p->due_ms;
	}
	deadline_heap_deinit(&h);
}

/* ---- Periodic timers ---------------------------------------------------- */

static void test_periodic(void)
{
	setup();
	Block *t = add("T", "test/Tick");
	CHECK(next_due() == UINT64_MAX); /* a stopped graph has no timers */
	CHECK(runtime_timer_timeout_ms(&rt, 0) == RUNTIME_NO_TIMEOUT);

	runtime_set_now(&rt, 100);
	graph_start(gr);
	drain();
	/* Armed one period after the start; START's return value is ignored. */
	CHECK(next_due() == 110);
	CHECK(runtime_timer_timeout_ms(&rt, 104) == 6);
	CHECK(runtime_timer_timeout_ms(&rt, 110) == 0);
	CHECK(runtime_timer_timeout_ms(&rt, 500) == 0);

	CHECK(fire(109) == 0);
	CHECK(fire(110) == 1);
	CHECK_STREQ(trace, "0T@110;");
	CHECK(next_due() == 120);

	/* Fired late: the next deadline stays on the grid (drift-free). */
	CHECK(fire(123) == 1);
	CHECK(next_due() == 130);

	/* Fell behind by more than a period: one tick, the missed ones are
	 * skipped, the grid is kept. */
	CHECK(fire(175) == 1);
	CHECK(next_due() == 180);
	/* Behind by exactly two periods: the tick due at the firing time
	 * itself is skipped too; the next deadline is after now. */
	CHECK(fire(200) == 1);
	CHECK(next_due() == 210);
	CHECK_STREQ(trace, "0T@110;0T@123;0T@175;0T@200;");
	CHECK(timer_calls(t) == 4);

	/* Two firings before a drain: both expiries are valid ticks. */
	trace[0] = '\0';
	CHECK(runtime_fire_timers(&rt, 210) == 1);
	CHECK(runtime_fire_timers(&rt, 220) == 1);
	drain();
	CHECK_STREQ(trace, "0T@220;0T@220;");
	CHECK(next_due() == 230);

	/* Input changes do not compute (compute_on_input false) and leave
	 * the timer alone. */
	set_num("T", "id", 7);
	CHECK(next_due() == 230);
	trace[0] = '\0';
	CHECK(fire(230) == 1);
	CHECK_STREQ(trace, "7T@230;");
	teardown();
}

static void test_fire_order(void)
{
	setup();
	add("A", "test/Sched");
	add("B", "test/Sched");
	add("C", "test/Tick");
	set_num("A", "id", 1);
	set_num("B", "id", 2);
	set_num("C", "id", 3);
	set_num("A", "delay", 10);
	set_num("B", "delay", 5);
	graph_start(gr); /* at 0, in creation order: A at 10, B at 5, C at 10 */
	drain();
	trace[0] = '\0';
	/* Earliest first; equal deadlines in arming order. */
	CHECK(fire(10) == 3);
	CHECK_STREQ(trace, "2T@10;1T@10;3T@10;");
	teardown();
}

static void test_set_period(void)
{
	setup();
	Block *t = add("T", "test/Tick");
	graph_start(gr);
	CHECK(next_due() == 10);

	/* Restarts from now with the new period. */
	runtime_set_now(&rt, 4);
	block_set_period(t, 25);
	CHECK(next_due() == 29);
	CHECK(fire(29) == 1);
	CHECK(next_due() == 54);

	/* 0: no timer. */
	block_set_period(t, 0);
	CHECK(next_due() == UINT64_MAX);
	CHECK(fire(1000) == 0);

	/* Set while stopped: applies at the next start. */
	graph_stop(gr);
	block_set_period(t, 30);
	CHECK(next_due() == UINT64_MAX);
	runtime_set_now(&rt, 2000);
	graph_start(gr);
	CHECK(next_due() == 2030);
	graph_stop(gr);
	block_set_period(t, 0);
	graph_start(gr);
	CHECK(next_due() == UINT64_MAX);

	/* Ignored for other timer modes. */
	Block *s = add("S", "test/Sched");
	block_set_period(s, 10);
	CHECK(s->period_ms == 0);
	CHECK(next_due() == UINT64_MAX);
	teardown();
}

/* ---- Scheduled timers --------------------------------------------------- */

static void test_scheduled(void)
{
	setup();
	Block *s = add("S", "test/Sched");
	graph_start(gr); /* delay -1: START arms nothing */
	drain();
	CHECK(next_due() == UINT64_MAX);
	CHECK(runtime_timer_timeout_ms(&rt, 0) == RUNTIME_NO_TIMEOUT);

	/* An INPUT compute arms from its return value, relative to now. */
	runtime_set_now(&rt, 5);
	set_num("S", "delay", 50);
	CHECK(next_due() == 55);
	CHECK(fire(54) == 0);
	CHECK(fire(55) == 1);
	CHECK(timer_calls(s) == 1);
	CHECK(next_due() == 105); /* re-armed by the TIMER compute */

	/* Every compute re-arms: an INPUT compute moves the deadline. */
	runtime_set_now(&rt, 70);
	set_num("S", "id", 1);
	CHECK(next_due() == 120);

	/* Delay 0: due at once, fires on the next call, once per call. */
	runtime_set_now(&rt, 80);
	set_num("S", "delay", 0);
	CHECK(runtime_timer_timeout_ms(&rt, 80) == 0);
	CHECK(fire(80) == 1);
	CHECK(fire(80) == 1);
	CHECK(timer_calls(s) == 3);

	/* Negative: cancelled. */
	set_num("S", "delay", -1);
	CHECK(next_due() == UINT64_MAX);
	CHECK(fire(10000) == 0);

	/* STOP's return value is ignored: a stopped block has no timer. */
	set_num("S", "delay", 30);
	CHECK(next_due() == 10030);
	graph_stop(gr);
	CHECK(next_due() == UINT64_MAX);

	/* START arms from its return value. */
	runtime_set_now(&rt, 20000);
	graph_start(gr);
	CHECK(next_due() == 20030);

	/* A deadline beyond the end of time saturates instead of wrapping. */
	runtime_set_now(&rt, UINT64_MAX - 10);
	set_value("S", "delay", linkable_from_i64(INT64_MAX));
	CHECK(deadline_armed(&s->timer) && s->timer.due_ms == UINT64_MAX);
	teardown();
}

/* ---- Stale expiries ----------------------------------------------------- */

static void test_stale_expiries(void)
{
	setup();
	Block *s = add("S", "test/Sched");
	set_num("S", "delay", 10);
	graph_start(gr);
	drain();
	CHECK(next_due() == 10);

	/* Re-armed after the expiry was queued: it is discarded. */
	CHECK(runtime_fire_timers(&rt, 10) == 1);
	set_num("S", "delay", 20); /* INPUT compute re-arms at 30 */
	drain();
	CHECK(timer_calls(s) == 0);
	CHECK(next_due() == 30);

	/* Cancelled after the expiry was queued. */
	CHECK(runtime_fire_timers(&rt, 30) == 1);
	set_num("S", "delay", -1);
	drain();
	CHECK(timer_calls(s) == 0);

	/* Stopped, even when started again before the drain. */
	set_num("S", "delay", 10); /* at 30: due 40 */
	CHECK(runtime_fire_timers(&rt, 40) == 1);
	graph_stop(gr);
	graph_start(gr); /* START re-arms at 50 */
	drain();
	CHECK(timer_calls(s) == 0);
	CHECK(next_due() == 50);

	/* A periodic timer re-armed by a new period. */
	Block *t = add("T", "test/Tick"); /* at 40: due 50, after S */
	CHECK(runtime_fire_timers(&rt, 50) == 2);
	block_set_period(t, 25);
	drain();
	CHECK(timer_calls(s) == 1); /* S was not touched: its expiry ran */
	CHECK(timer_calls(t) == 0);

	/* A removed block, even when its slot is reused at once. */
	CHECK(runtime_fire_timers(&rt, 75) == 2); /* S (due 60) and T */
	BlockHandle old = t->handle;
	CHECK(graph_remove_block(gr, "T") == GRAPH_OK);
	Block *t2 = add("T2", "test/Tick"); /* due 85 */
	CHECK(t2->handle.slot == old.slot && t2->handle.gen == old.gen + 1);
	drain();
	CHECK(timer_calls(t2) == 0);
	CHECK(timer_calls(s) == 2); /* S's expiry ran and re-armed at 85 */
	CHECK(next_due() == 85);
	CHECK(deadline_heap_len(&rt.timers) == 2);

	/* Destroying the graph with expiries queued and timers armed. */
	CHECK(runtime_fire_timers(&rt, 100) == 2);
	teardown(); /* checks that the heap is empty */
}

static void test_stopped_graph(void)
{
	setup();
	add("T", "test/Tick");
	add("S", "test/Sched");
	add("I", "timing/Interval");
	set_num("S", "delay", 7);
	CHECK(deadline_heap_len(&rt.timers) == 0);
	CHECK(rt.timer_blocks == 3);
	graph_start(gr);
	drain();
	CHECK(deadline_heap_len(&rt.timers) == 3);

	runtime_set_now(&rt, 5);
	graph_stop(gr);
	CHECK(deadline_heap_len(&rt.timers) == 0);
	CHECK(runtime_timer_timeout_ms(&rt, 5) == RUNTIME_NO_TIMEOUT);
	CHECK(fire(5000) == 0);

	/* Restart: everything re-armed relative to the new start. */
	graph_start(gr);
	drain();
	CHECK(deadline_heap_len(&rt.timers) == 3);
	CHECK(next_due() == 5007);
	CHECK(fire(5007) == 1);
	CHECK(fire(5010) == 1);
	CHECK(fire(5014) == 1);
	CHECK(fire(5020) == 1);
	CHECK(next_due() == 5021);

	/* A timer block added to a started graph starts at once. */
	add("T2", "test/Tick");
	CHECK(deadline_heap_len(&rt.timers) == 4);
	CHECK(graph_remove_block(gr, "T2") == GRAPH_OK);
	CHECK(deadline_heap_len(&rt.timers) == 3);
	CHECK(rt.timer_blocks == 3);
	teardown();
}

/* A timer expiry dropped on queue overflow re-arms a scheduled timer. */
static void test_dropped_expiry(void)
{
	setup();
	Block *s = add("S", "test/Sched");
	Block *t = add("T", "test/Tick");
	set_num("S", "delay", 10);
	graph_start(gr);
	drain();
	CHECK(runtime_fire_timers(&rt, 10) == 2);
	/* Flood the queue with stale entries: both expiries are dropped. */
	for (size_t i = 0; i < work_queue_capacity(); i++)
		work_queue_push_timer(&rt.queue, (BlockHandle){UINT32_MAX - 1, 0}, 0);
	CHECK(work_queue_drops(&rt.queue) == 2);
	drain();
	CHECK(timer_calls(s) == 0 && timer_calls(t) == 0);
	/* S fires again 1 ms later; T lost one tick and keeps its grid. */
	CHECK(next_due() == 11);
	CHECK(fire(11) == 1);
	CHECK(timer_calls(s) == 1);
	CHECK(next_due() == 20);
	CHECK(fire(20) == 1);
	CHECK(timer_calls(t) == 1);
	teardown();
}

/* ---- timing/Interval (03 §3.1) ------------------------------------------ */

/* A watch on the downstream input logging "<value>@<now>;": what Interval
 * propagated, and when. */
static void log_value(InputWatch *w, const Block *b, uint32_t input, const Linkable *v)
{
	(void)w;
	(void)b;
	(void)input;
	append("%d@%" PRIu64 ";", linkable_to_bool(v) ? 1 : 0, rt.now_ms);
}

static InputWatch watch = {.on_value = log_value};

/* Interval "I" wired to a downstream block whose input logs every value it
 * receives. */
static Block *interval(void)
{
	Block *i = add("I", "timing/Interval");
	Block *e = add("E", "math/Equal");
	CHECK(graph_connect(gr, "I", "out", "E", "in1") == GRAPH_OK);
	block_watch_input(e, 0, &watch);
	trace[0] = '\0';
	return i;
}

static bool out(const Block *i)
{
	return linkable_to_bool(&i->out_values[0]);
}

static void start_at(uint64_t now)
{
	runtime_set_now(&rt, now);
	graph_start(gr);
	drain();
}

static void test_interval_default(void)
{
	setup();
	Block *i = interval();
	start_at(0);
	CHECK(out(i));
	CHECK(next_due() == 500);
	run_until(3000);
	CHECK_STREQ(trace, "1@0;0@500;1@1000;0@1500;1@2000;0@2500;1@3000;");
	CHECK(next_due() == 3500);
	teardown();
}

static void test_interval_duty_extremes(void)
{
	/* 0 %: out stays false, the cycles still run (to latch inputs). */
	setup();
	Block *i = interval();
	set_num("I", "duty-cycle", 0);
	start_at(0);
	CHECK(!out(i));
	CHECK(next_due() == 1000);
	run_until(2500);
	CHECK_STREQ(trace, "0@0;");
	CHECK(next_due() == 3000);
	teardown();

	/* 100 %: out stays true. */
	setup();
	i = interval();
	set_num("I", "duty-cycle", 100);
	start_at(0);
	CHECK(out(i));
	CHECK(next_due() == 1000);
	run_until(2500);
	CHECK_STREQ(trace, "1@0;");
	CHECK(next_due() == 3000);
	teardown();

	/* Clamped beyond 0-100: 150 is 100 %, -20 is 0 %, NaN is 0 %. */
	setup();
	i = interval();
	set_num("I", "duty-cycle", 150);
	start_at(0);
	run_until(2500);
	CHECK_STREQ(trace, "1@0;");
	set_num("I", "duty-cycle", -20);
	run_until(4500);
	CHECK_STREQ(trace, "1@0;0@3000;");
	set_num("I", "duty-cycle", NAN);
	run_until(6500);
	CHECK_STREQ(trace, "1@0;0@3000;");
	set_num("I", "duty-cycle", 25);
	set_num("I", "milliseconds", 200);
	run_until(7500);
	CHECK_STREQ(trace, "1@0;0@3000;1@7000;0@7050;1@7200;0@7250;1@7400;0@7450;");
	teardown();
}

static void test_interval_latching(void)
{
	setup();
	Block *i = interval();
	start_at(0);
	/* Mid-cycle changes do not glitch the current cycle. */
	runtime_set_now(&rt, 200);
	set_num("I", "duty-cycle", 20);
	set_num("I", "milliseconds", 400);
	CHECK(out(i));
	CHECK(next_due() == 500);
	run_until(1500);
	CHECK_STREQ(trace, "1@0;0@500;1@1000;0@1080;1@1400;0@1480;");

	/* 100 % -> 0 % mid-cycle: the cycle ends at 100 %, the next is 0 %. */
	set_num("I", "duty-cycle", 100);
	run_until(1800); /* the 1400 cycle was latched at 20 % */
	runtime_set_now(&rt, 1900);
	set_num("I", "duty-cycle", 0);
	CHECK(out(i));
	run_until(2700);
	CHECK_STREQ(trace, "1@0;0@500;1@1000;0@1080;1@1400;0@1480;1@1800;0@2200;");
	CHECK(next_due() == 2600 + 400);
	teardown();
}

static void test_interval_idle(void)
{
	setup();
	Block *i = interval();
	set_num("I", "milliseconds", 0);
	start_at(0);
	CHECK(!out(i));
	CHECK(next_due() == UINT64_MAX); /* idle: no timer */
	CHECK_STREQ(trace, "0@0;");

	/* A valid period starts a fresh cycle at once. */
	runtime_set_now(&rt, 50);
	set_num("I", "milliseconds", 100);
	CHECK(out(i));
	CHECK(next_due() == 100);
	drain();

	/* An invalid period mid-cycle takes effect at the next cycle. */
	runtime_set_now(&rt, 70);
	set_num("I", "milliseconds", -5);
	CHECK(out(i));
	run_until(1000);
	CHECK_STREQ(trace, "0@0;1@50;0@100;");
	CHECK(next_due() == UINT64_MAX);
	CHECK(!out(i));

	/* Other changes keep an idle block idle. */
	set_num("I", "duty-cycle", 70);
	CHECK(next_due() == UINT64_MAX);
	teardown();
}

static void test_interval_non_number(void)
{
	setup();
	Block *i = interval();
	set_num("I", "milliseconds", NAN);
	start_at(0);
	CHECK(!out(i) && next_due() == UINT64_MAX);
	set_num("I", "milliseconds", INFINITY);
	CHECK(!out(i) && next_due() == UINT64_MAX);
	set_num("I", "milliseconds", -INFINITY);
	CHECK(!out(i) && next_due() == UINT64_MAX);
	Linkable text = {0};
	CHECK(linkable_from_string(&text, "abc", 3));
	set_value("I", "milliseconds", text);
	CHECK(!out(i) && next_due() == UINT64_MAX);
	/* Numeric text converts (01 §2.1). */
	CHECK(linkable_from_string(&text, "300", 3));
	set_value("I", "milliseconds", text);
	CHECK(out(i) && next_due() == 150);
	teardown();

	/* A period is rounded to whole ms. */
	setup();
	i = interval();
	set_num("I", "milliseconds", 2.4);
	start_at(0);
	CHECK(out(i) && next_due() == 1);
	run_until(4);
	CHECK_STREQ(trace, "1@0;0@1;1@2;0@3;1@4;");
	teardown();
}

static void test_interval_latency(void)
{
	setup();
	Block *i = interval();
	start_at(0);
	/* Fired 30 ms late: the next transition is still cycle-based. */
	CHECK(fire(530) == 1);
	CHECK(!out(i));
	CHECK(next_due() == 1000);
	CHECK(fire(1010) == 1);
	CHECK(out(i));
	CHECK(next_due() == 1500);

	/* Behind by several cycles: the output is the waveform's value now,
	 * the missed cycles are skipped, the grid is kept. */
	CHECK(fire(3700) == 1);
	CHECK(!out(i)); /* the 3000 cycle's off phase */
	CHECK(next_due() == 4000);
	CHECK(fire(4200) == 1);
	CHECK(out(i));
	CHECK(next_due() == 4500);
	CHECK_STREQ(trace, "1@0;0@530;1@1010;0@3700;1@4200;");

	/* An input change re-arms the timer for the same transition. */
	runtime_set_now(&rt, 4300);
	set_num("I", "duty-cycle", 10);
	CHECK(next_due() == 4500);
	teardown();
}

static void test_interval_stop_restart(void)
{
	setup();
	Block *i = interval();
	start_at(0);
	runtime_set_now(&rt, 200);
	graph_stop(gr);
	CHECK(next_due() == UINT64_MAX);
	CHECK(out(i)); /* kept, not propagated */
	CHECK(fire(600) == 0);

	/* Restart begins a fresh cycle at once. */
	start_at(700);
	CHECK(out(i));
	CHECK(next_due() == 1200);
	run_until(1700);
	CHECK_STREQ(trace, "1@0;1@700;0@1200;1@1700;");
	teardown();
}

int main(void)
{
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	CHECK(block_types_register(&tick_def) == BLOCK_TYPES_OK);
	CHECK(block_types_register(&sched_def) == BLOCK_TYPES_OK);
	RUN(test_deadline_heap);
	RUN(test_periodic);
	RUN(test_fire_order);
	RUN(test_set_period);
	RUN(test_scheduled);
	RUN(test_stale_expiries);
	RUN(test_stopped_graph);
	RUN(test_dropped_expiry);
	RUN(test_interval_default);
	RUN(test_interval_duty_extremes);
	RUN(test_interval_latching);
	RUN(test_interval_idle);
	RUN(test_interval_non_number);
	RUN(test_interval_latency);
	RUN(test_interval_stop_restart);
	block_types_reset();
	return test_result();
}
