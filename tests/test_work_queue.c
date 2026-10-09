/*
 * Work queue tests (01 §3.2). Written against HAKKAS_QUEUE_CAPACITY, so the
 * same file also runs as test_work_queue_small, built with a tiny capacity.
 * String values are heap-allocated and reference-counted, so a missed or
 * doubled release shows up as a leak or a use-after-free under the asan preset.
 */
#include "test.h"

#include "graphs/work_queue.h"
#include "system/log.h"

#define CAP ((uint32_t)HAKKAS_QUEUE_CAPACITY)

static WorkQueue q;

static Linkable str(const char *s)
{
	Linkable v = {0};
	CHECK(linkable_from_string(&v, s, strlen(s)));
	return v;
}

static BlockHandle handle(uint32_t slot, uint32_t gen)
{
	return (BlockHandle){slot, gen};
}

static void test_capacity(void)
{
	CHECK(work_queue_capacity() == HAKKAS_QUEUE_CAPACITY);
	fprintf(stderr, "capacity %u\n", (unsigned)CAP);
}

static void test_pop_empty(void)
{
	work_queue_init(&q);
	CHECK(work_queue_empty(&q));
	CHECK(work_queue_len(&q) == 0);
	WorkEntry e;
	memset(&e, 0xAA, sizeof(e));
	CHECK(!work_queue_pop(&q, &e));
	CHECK(e.type == WORK_NONE);
	CHECK(work_queue_drops(&q) == 0);
}

/* Pushes n mixed entries tagged i = first..first+n-1, cycling input, timer,
 * IO; checks them back in FIFO order. */
static void push_mixed(uint32_t first, uint32_t n)
{
	for (uint32_t i = first; i < first + n; i++) {
		switch (i % 3) {
		case 0:
			work_queue_push_input(&q, handle(i, i + 1), i % 7, i + 2,
					      linkable_from_i64((int64_t)i));
			break;
		case 1:
			work_queue_push_timer(&q, handle(i, i + 1), i + 3);
			break;
		default:
			work_queue_push_io(&q, i, linkable_from_i64((int64_t)i));
			break;
		}
	}
}

static void check_mixed(uint32_t first, uint32_t n)
{
	for (uint32_t i = first; i < first + n; i++) {
		WorkEntry e;
		CHECK(work_queue_pop(&q, &e));
		switch (i % 3) {
		case 0:
			CHECK(e.type == WORK_INPUT_UPDATE);
			CHECK(block_handle_eq(e.as.input.block, handle(i, i + 1)));
			CHECK(e.as.input.input == i % 7);
			CHECK(e.as.input.source_gen == i + 2);
			CHECK(e.as.input.value.type == LINKABLE_I64 &&
			      e.as.input.value.as.i64 == (int64_t)i);
			break;
		case 1:
			CHECK(e.type == WORK_TIMER_EXPIRY);
			CHECK(block_handle_eq(e.as.timer.block, handle(i, i + 1)));
			CHECK(e.as.timer.timer_gen == i + 3);
			break;
		default:
			CHECK(e.type == WORK_IO_EVENT);
			CHECK(e.as.io.channel == i);
			CHECK(e.as.io.value.type == LINKABLE_I64 &&
			      e.as.io.value.as.i64 == (int64_t)i);
			break;
		}
		work_queue_entry_clear(&e);
		CHECK(e.type == WORK_NONE);
	}
}

static void test_fifo_mixed_types(void)
{
	work_queue_init(&q);
	uint32_t n = CAP < 9 ? CAP : 9;
	push_mixed(0, n);
	CHECK(work_queue_len(&q) == n);
	check_mixed(0, n);
	CHECK(work_queue_empty(&q));
	CHECK(work_queue_drops(&q) == 0);
}

static void test_fill_to_capacity(void)
{
	work_queue_init(&q);
	push_mixed(0, CAP);
	CHECK(work_queue_len(&q) == CAP);
	CHECK(work_queue_drops(&q) == 0);
	check_mixed(0, CAP);
	CHECK(work_queue_empty(&q));
}

static void test_wrap_around(void)
{
	/* Move head off zero, then cycle through the ring several times with the
	 * queue partly and completely full, so both head and tail wrap. */
	work_queue_init(&q);
	uint32_t next_push = 0, next_pop = 0;
	uint32_t half = CAP / 2; /* CAP >= 2 */
	push_mixed(next_push++, 1);
	for (int round = 0; round < 5; round++) {
		push_mixed(next_push, half);
		next_push += half;
		check_mixed(next_pop, half);
		next_pop += half;
	}
	uint32_t room = CAP - work_queue_len(&q);
	push_mixed(next_push, room);
	next_push += room;
	CHECK(work_queue_len(&q) == CAP);
	check_mixed(next_pop, next_push - next_pop);
	CHECK(work_queue_empty(&q));
	CHECK(work_queue_drops(&q) == 0);
}

static void test_overflow_drops_oldest(void)
{
	work_queue_init(&q);
	/* Strings, so a dropped entry that is not released leaks. */
	char buf[32];
	for (uint32_t i = 0; i < CAP + 3; i++) {
		snprintf(buf, sizeof(buf), "v%u", (unsigned)i);
		work_queue_push_input(&q, handle(i, 0), 0, 0, str(buf));
	}
	CHECK(work_queue_len(&q) == CAP);
	CHECK(work_queue_drops(&q) == 3);

	/* The three oldest are gone; the rest are in order. */
	for (uint32_t i = 3; i < CAP + 3; i++) {
		WorkEntry e;
		CHECK(work_queue_pop(&q, &e));
		CHECK(e.type == WORK_INPUT_UPDATE && e.as.input.block.slot == i);
		snprintf(buf, sizeof(buf), "v%u", (unsigned)i);
		CHECK(e.as.input.value.type == LINKABLE_STRING);
		CHECK_STREQ(e.as.input.value.as.s, buf);
		work_queue_entry_clear(&e);
	}
	CHECK(!work_queue_pop(&q, &(WorkEntry){0}));
	CHECK(work_queue_drops(&q) == 3);
}

static void test_overflow_drops_timer_and_io(void)
{
	work_queue_init(&q);
	work_queue_push_timer(&q, handle(1, 1), 1);
	work_queue_push_io(&q, 2, str("io"));
	for (uint32_t i = 0; i < CAP; i++)
		work_queue_push_input(&q, handle(i, 0), 0, 0, linkable_from_bool(true));
	CHECK(work_queue_drops(&q) == 2);
	WorkEntry e;
	CHECK(work_queue_pop(&q, &e) && e.type == WORK_INPUT_UPDATE);
	work_queue_entry_clear(&e);
	work_queue_clear(&q);
}

/* The drop hook sees each dropped entry, value still held, oldest first. */
static int dropped;
static uint32_t dropped_gen[2];

static void on_drop(void *ctx, const WorkEntry *e)
{
	CHECK(ctx == &q);
	if (dropped == 0) {
		CHECK(e->type == WORK_TIMER_EXPIRY);
		dropped_gen[0] = e->as.timer.timer_gen;
	} else if (dropped == 1) {
		CHECK(e->type == WORK_INPUT_UPDATE);
		CHECK_STREQ(e->as.input.value.as.s, "kept until the hook ran");
		dropped_gen[1] = e->as.input.source_gen;
	}
	dropped++;
}

static void test_drop_hook(void)
{
	work_queue_init(&q);
	CHECK(q.on_drop == NULL);
	work_queue_set_drop_hook(&q, on_drop, &q);
	dropped = 0;
	work_queue_push_timer(&q, handle(1, 1), 7);
	work_queue_push_input(&q, handle(2, 0), 0, 9, str("kept until the hook ran"));
	for (uint32_t i = 0; i < CAP; i++)
		work_queue_push_io(&q, i, linkable_from_bool(true));
	CHECK(dropped == 2);
	CHECK(dropped_gen[0] == 7 && dropped_gen[1] == 9);
	CHECK(work_queue_drops(&q) == 2);
	work_queue_clear(&q); /* a clear is no drop */
	CHECK(dropped == 2);
	work_queue_init(&q);
	CHECK(q.on_drop == NULL);
}

static void test_value_ownership(void)
{
	/* A copied value stays valid after the queue's reference is released. */
	work_queue_init(&q);
	Linkable keep = str("shared");
	work_queue_push_io(&q, 7, linkable_copy(&keep));
	work_queue_push_input(&q, handle(0, 0), 0, 0, linkable_copy(&keep));
	WorkEntry e;
	CHECK(work_queue_pop(&q, &e));
	CHECK(e.as.io.value.as.s == keep.as.s);
	work_queue_entry_clear(&e);
	work_queue_clear(&q);
	CHECK_STREQ(keep.as.s, "shared");
	linkable_clear(&keep);

	/* A popped value can be moved on instead of cleared. */
	work_queue_push_input(&q, handle(0, 0), 0, 0, str("moved"));
	CHECK(work_queue_pop(&q, &e));
	Linkable owned = e.as.input.value;
	e.as.input.value = (Linkable){0};
	work_queue_entry_clear(&e);
	CHECK_STREQ(owned.as.s, "moved");
	linkable_clear(&owned);
}

static void test_clear_releases(void)
{
	work_queue_init(&q);
	uint32_t n = CAP < 6 ? CAP : 6;
	for (uint32_t i = 0; i < n; i++) {
		if (i % 2)
			work_queue_push_io(&q, i, str("io value"));
		else
			work_queue_push_input(&q, handle(i, 0), 0, 0, str("input value"));
	}
	work_queue_push_timer(&q, handle(0, 0), 0);
	work_queue_clear(&q);
	CHECK(work_queue_empty(&q));
	CHECK(!work_queue_pop(&q, &(WorkEntry){0}));

	/* Usable again after a clear. */
	push_mixed(0, CAP);
	check_mixed(0, CAP);
}

/* Captures warn lines for the report helper. */
struct capture {
	int calls;
	char line[LOG_LINE_MAX];
};

static void capture_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	struct capture *c = ctx;
	CHECK(level == LOG_LEVEL_WARN);
	c->calls++;
	memcpy(c->line, line, len + 1);
}

static void overflow(uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		work_queue_push_timer(&q, handle(0, 0), 0);
}

static void test_report_drops(void)
{
	struct capture cap = {0};
	CHECK(log_add_sink(capture_sink, &cap));
	work_queue_init(&q);
	const uint64_t iv = WORK_QUEUE_DROP_REPORT_INTERVAL_MS;

	/* Nothing dropped: nothing to report. */
	CHECK(!work_queue_report_drops(&q, 0));
	CHECK(cap.calls == 0);

	/* The first drops are reported at once, with their count. */
	overflow(CAP + 2);
	CHECK(work_queue_report_drops(&q, 5000));
	CHECK(cap.calls == 1);
	CHECK(strstr(cap.line, "dropped 2 oldest entries since last warning (2 total)"));
	CHECK(!work_queue_report_drops(&q, 5001));
	CHECK(cap.calls == 1);

	/* More drops within the interval wait for it to pass, then are reported
	 * together. */
	overflow(3);
	CHECK(!work_queue_report_drops(&q, 5000 + iv - 1));
	overflow(1);
	CHECK(!work_queue_report_drops(&q, 5000 + iv - 1));
	CHECK(cap.calls == 1);
	CHECK(work_queue_report_drops(&q, 5000 + iv));
	CHECK(cap.calls == 2);
	CHECK(strstr(cap.line, "dropped 4 oldest entries since last warning (6 total)"));

	/* An interval passing without drops logs nothing. */
	CHECK(!work_queue_report_drops(&q, 5000 + 3 * iv));
	CHECK(cap.calls == 2);

	/* A drop after a quiet period is reported immediately. */
	overflow(1);
	CHECK(work_queue_report_drops(&q, 5000 + 3 * iv));
	CHECK(cap.calls == 3);
	CHECK(strstr(cap.line, "dropped 1 oldest entries since last warning (7 total)"));
	CHECK(work_queue_drops(&q) == 7);

	work_queue_clear(&q);
	CHECK(log_remove_sink(capture_sink, &cap));
}

int main(void)
{
	RUN(test_capacity);
	RUN(test_pop_empty);
	RUN(test_fifo_mixed_types);
	RUN(test_fill_to_capacity);
	RUN(test_wrap_around);
	RUN(test_overflow_drops_oldest);
	RUN(test_overflow_drops_timer_and_io);
	RUN(test_drop_hook);
	RUN(test_value_ownership);
	RUN(test_clear_releases);
	RUN(test_report_drops);
	return test_result();
}
