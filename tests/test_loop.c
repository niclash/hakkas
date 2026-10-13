/*
 * The engine loop (engine/loop.h, 01 §3.2.1) over the POSIX PAL's sockets,
 * poll, handoff and stop request (01 §0.2): real loopback sockets on
 * 127.0.0.1 with ephemeral ports, a second thread for the cross-context
 * enqueue. Waiting is always for a condition, with a generous deadline; the
 * socket tests add a tick hook so that no poll waits more than 10 ms.
 */
#include "test.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "engine/loop.h"
#include "graphs/graph.h"
#include "platform/posix/signals.h"

static Runtime rt;
static Loop loop;

/* Generous deadline for every wait, ms: only reached when a test fails. */
#define WAIT_MS 3000

/* ---- Helpers ---------------------------------------------------------------- */

static int count_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	if (d == NULL)
		return -1;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL)
		if (e->d_name[0] != '.')
			n++;
	closedir(d);
	return n - 1; /* the directory's own descriptor */
}

static void sleep_ms(long ms)
{
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
	while (nanosleep(&ts, &ts) != 0)
		;
}

static uint64_t short_tick(void *ctx, uint64_t now_ms)
{
	(void)ctx;
	(void)now_ms;
	return 10;
}

/* A runtime and loop; with_tick bounds every poll to 10 ms. */
static void setup(bool with_tick)
{
	runtime_init(&rt);
	CHECK(loop_init(&loop, &rt) == PAL_OK);
	if (with_tick)
		CHECK(loop_add_tick(&loop, short_tick, NULL));
}

static void teardown(void)
{
	loop_deinit(&loop);
	runtime_deinit(&rt);
}

typedef bool (*pred_fn)(void *arg);

/* Runs loop iterations until pred holds or WAIT_MS passed. */
static bool run_until(pred_fn pred, void *arg)
{
	uint64_t end = pal_uptime_ms() + WAIT_MS;
	while (!pred(arg)) {
		if (pal_uptime_ms() > end)
			return false;
		loop_run_once(&loop);
	}
	return true;
}

/* A blocking connect to 127.0.0.1:port; the client socket is then made
 * non-blocking. The kernel completes the handshake before accept. */
static int client_connect(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	CHECK(fd >= 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
	CHECK(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
	return fd;
}

/* Client receive buffer. */
typedef struct {
	int fd;
	char buf[8192];
	size_t len;
	bool eof;
} Client;

/* Reads what is there; sets eof at the end of stream or a reset. */
static void client_poll(Client *cl)
{
	for (;;) {
		char tmp[4096];
		ssize_t n = recv(cl->fd, tmp, sizeof(tmp), 0);
		if (n > 0) {
			size_t k = (size_t)n;
			if (k > sizeof(cl->buf) - cl->len)
				k = sizeof(cl->buf) - cl->len;
			memcpy(cl->buf + cl->len, tmp, k);
			cl->len += k;
			continue;
		}
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			cl->eof = true;
		return;
	}
}

typedef struct {
	Client *cl;
	size_t want;
} ClientWait;

static bool client_has(void *arg)
{
	ClientWait *w = arg;
	client_poll(w->cl);
	return w->cl->len >= w->want;
}

static bool client_eof(void *arg)
{
	Client *cl = arg;
	client_poll(cl);
	return cl->eof;
}

/* ---- The test server: echo, or a stream of bytes ------------------------------- */

typedef struct {
	int accepted;
	int closed;
	enum loop_close_reason reason;
	LoopConn *conn;
	struct pal_peer peer;
	/* Streaming: total bytes to send and bytes queued so far. */
	size_t stream_total;
	size_t stream_sent;
	size_t chunk;
	int drained;
	/* Refuse every connection in on_accept. */
	bool refuse;
} Server;

static Server srv;

static void echo_readable(void *ctx, LoopConn *c)
{
	(void)ctx;
	char buf[512];
	size_t n;
	while (loop_conn_read(c, buf, sizeof(buf), &n) == PAL_OK)
		CHECK(loop_conn_write(c, buf, n));
}

static void on_closed(void *ctx, LoopConn *c, enum loop_close_reason reason)
{
	Server *s = ctx;
	CHECK(s->conn == c);
	s->closed++;
	s->reason = reason;
	s->conn = NULL;
}

static const LoopConnHandler echo_handler = {
	.on_readable = echo_readable,
	.on_closed = on_closed,
};

static bool echo_accept(void *ctx, LoopConn *c)
{
	Server *s = ctx;
	s->accepted++;
	s->peer = *loop_conn_peer(c);
	if (s->refuse)
		return false;
	s->conn = c;
	loop_conn_set_handler(c, &echo_handler, s);
	return true;
}

static uint8_t pattern(size_t i)
{
	return (uint8_t)(i % 251);
}

/* Queues chunks of the stream while they fit the outgoing buffer. */
static void stream_more(Server *s, LoopConn *c)
{
	uint8_t buf[4096];
	while (s->stream_sent < s->stream_total && loop_conn_space(c) >= s->chunk) {
		size_t k = s->stream_total - s->stream_sent;
		if (k > s->chunk)
			k = s->chunk;
		for (size_t i = 0; i < k; i++)
			buf[i] = pattern(s->stream_sent + i);
		CHECK(loop_conn_write(c, buf, k));
		s->stream_sent += k;
	}
}

static void stream_drained(void *ctx, LoopConn *c)
{
	Server *s = ctx;
	s->drained++;
	stream_more(s, c);
}

static const LoopConnHandler stream_handler = {
	.on_drained = stream_drained,
	.on_closed = on_closed,
};

static bool stream_accept(void *ctx, LoopConn *c)
{
	Server *s = ctx;
	s->accepted++;
	s->conn = c;
	loop_conn_set_handler(c, &stream_handler, s);
	stream_more(s, c);
	return true;
}

static LoopListener *listen_local(loop_accept_fn on_accept, size_t out_cap, uint32_t max_conns)
{
	LoopListener *l = NULL;
	LoopListenerConfig cfg = {.host = "127.0.0.1",
				  .port = 0,
				  .max_conns = max_conns,
				  .out_cap = out_cap,
				  .on_accept = on_accept,
				  .ctx = &srv};
	CHECK(loop_listen(&loop, &cfg, &l) == PAL_OK);
	CHECK(l != NULL && loop_listener_port(l) != 0);
	return l;
}

static bool srv_accepted(void *arg)
{
	return srv.accepted >= *(int *)arg;
}

static bool srv_closed(void *arg)
{
	(void)arg;
	return srv.closed > 0;
}

/* ---- PAL sockets ------------------------------------------------------------ */

static void test_listen_errors(void)
{
	setup(false);
	pal_socket s;
	CHECK(pal_socket_listen("localhost", 0, 4, &s) == PAL_ERR_INVALID);
	CHECK(s == PAL_SOCKET_NONE);
	CHECK(pal_socket_listen("1.2.3", 0, 4, &s) == PAL_ERR_INVALID);
	CHECK(pal_socket_listen("127.0.0.1", 0, 4, &s) == PAL_OK);
	uint16_t port = 0;
	CHECK(pal_socket_local_port(s, &port) == PAL_OK && port != 0);
	pal_socket s2;
	CHECK(pal_socket_listen("127.0.0.1", port, 4, &s2) == PAL_ERR_ADDR_IN_USE);
	pal_socket c;
	CHECK(pal_socket_accept(s, &c, NULL) == PAL_ERR_WOULD_BLOCK);
	pal_socket_close(s);

	/* Through the loop: logged and refused. */
	LoopListener *l = (LoopListener *)&l;
	LoopListenerConfig cfg = {.host = "nonsense", .out_cap = 16, .on_accept = echo_accept};
	CHECK(loop_listen(&loop, &cfg, &l) == PAL_ERR_INVALID);
	CHECK(l == NULL);
	teardown();
}

/* ---- Round trip, peer close ------------------------------------------------- */

static void test_echo_round_trip(void)
{
	memset(&srv, 0, sizeof(srv));
	setup(true);
	LoopListener *l = listen_local(echo_accept, 4096, 0);
	Client cl = {.fd = client_connect(loop_listener_port(l))};
	int one = 1;
	CHECK(run_until(srv_accepted, &one));
	CHECK(loop_listener_conn_count(l) == 1);
	CHECK_STREQ(srv.peer.addr, "127.0.0.1");
	CHECK(srv.peer.port != 0);

	CHECK(send(cl.fd, "hello", 5, 0) == 5);
	ClientWait w = {&cl, 5};
	CHECK(run_until(client_has, &w));
	CHECK(cl.len == 5 && memcmp(cl.buf, "hello", 5) == 0);

	/* Again, a larger message. */
	char big[3000];
	for (size_t i = 0; i < sizeof(big); i++)
		big[i] = (char)pattern(i);
	CHECK(send(cl.fd, big, sizeof(big), 0) == (ssize_t)sizeof(big));
	w.want = 5 + sizeof(big);
	CHECK(run_until(client_has, &w));
	CHECK(cl.len == w.want && memcmp(cl.buf + 5, big, sizeof(big)) == 0);

	/* The peer closes: detected, on_closed once, the count back to 0. */
	close(cl.fd);
	CHECK(run_until(srv_closed, NULL));
	CHECK(srv.closed == 1 && srv.reason == LOOP_CLOSE_PEER);
	CHECK(loop_listener_conn_count(l) == 0);
	teardown();
}

static void test_close_after_flush(void)
{
	memset(&srv, 0, sizeof(srv));
	setup(true);
	LoopListener *l = listen_local(echo_accept, 4096, 0);
	Client cl = {.fd = client_connect(loop_listener_port(l))};
	int one = 1;
	CHECK(run_until(srv_accepted, &one));
	CHECK(loop_conn_write(srv.conn, "bye", 3));
	loop_conn_close(srv.conn);
	CHECK(!loop_conn_write(srv.conn, "x", 1)); /* closing: refused */
	CHECK(run_until(client_eof, &cl));
	CHECK(cl.len == 3 && memcmp(cl.buf, "bye", 3) == 0);
	CHECK(srv.closed == 1 && srv.reason == LOOP_CLOSE_LOCAL);
	close(cl.fd);
	teardown();
}

static void test_refused_and_max_conns(void)
{
	memset(&srv, 0, sizeof(srv));
	setup(true);
	LoopListener *l = listen_local(echo_accept, 4096, 1);
	Client a = {.fd = client_connect(loop_listener_port(l))};
	int one = 1;
	CHECK(run_until(srv_accepted, &one));
	/* The second exceeds max_conns: closed unseen. */
	Client b = {.fd = client_connect(loop_listener_port(l))};
	CHECK(run_until(client_eof, &b));
	CHECK(srv.accepted == 1);
	CHECK(loop_listener_conn_count(l) == 1);
	close(b.fd);
	close(a.fd);
	CHECK(run_until(srv_closed, NULL));

	/* on_accept refuses: closed, on_closed not called. */
	srv.refuse = true;
	srv.closed = 0;
	Client c = {.fd = client_connect(loop_listener_port(l))};
	CHECK(run_until(client_eof, &c));
	CHECK(srv.accepted == 2 && srv.closed == 0);
	CHECK(loop_listener_conn_count(l) == 0);
	close(c.fd);
	teardown();
}

static void test_ipv6(void)
{
	memset(&srv, 0, sizeof(srv));
	setup(true);
	pal_socket s;
	enum pal_err err = pal_socket_listen("::1", 0, 4, &s);
	if (err == PAL_ERR_UNAVAILABLE) {
		fprintf(stderr, "(IPv6 loopback unavailable, skipped)\n");
		teardown();
		return;
	}
	CHECK(err == PAL_OK);
	pal_socket_close(s);
	LoopListener *l = NULL;
	LoopListenerConfig cfg = {
		.host = "::1", .out_cap = 64, .on_accept = echo_accept, .ctx = &srv};
	CHECK(loop_listen(&loop, &cfg, &l) == PAL_OK);
	int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
	CHECK(fd >= 0);
	struct sockaddr_in6 a = {.sin6_family = AF_INET6};
	a.sin6_port = htons(loop_listener_port(l));
	a.sin6_addr = in6addr_loopback;
	CHECK(connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
	int one = 1;
	CHECK(run_until(srv_accepted, &one));
	CHECK_STREQ(srv.peer.addr, "::1");
	close(fd);
	CHECK(run_until(srv_closed, NULL));
	teardown();
}

/* ---- Partial writes, a slow reader, overflow -------------------------------- */

typedef struct {
	Client *cl;
	size_t got;
	bool ok;
} StreamCheck;

/* Reads and checks the stream's bytes; true once all arrived. */
static bool stream_received(void *arg)
{
	StreamCheck *sc = arg;
	for (;;) {
		uint8_t tmp[8192];
		ssize_t n = recv(sc->cl->fd, tmp, sizeof(tmp), 0);
		if (n <= 0)
			break;
		for (ssize_t i = 0; i < n; i++)
			if (tmp[i] != pattern(sc->got + (size_t)i))
				sc->ok = false;
		sc->got += (size_t)n;
	}
	return sc->got >= srv.stream_total;
}

static bool stream_stalled(void *arg)
{
	(void)arg;
	return srv.conn != NULL && loop_conn_pending(srv.conn) > 0 &&
	       srv.stream_sent < srv.stream_total;
}

static void test_slow_reader(void)
{
	memset(&srv, 0, sizeof(srv));
	srv.stream_total = 8u << 20; /* well beyond any loopback socket buffering */
	srv.chunk = 4000;
	setup(true);
	LoopListener *l = listen_local(stream_accept, 16384, 0);
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int small = 4096;
	CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(loop_listener_port(l))};
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
	CHECK(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
	Client cl = {.fd = fd};

	/* The client does not read: the socket fills up, the rest stays in
	 * the outgoing buffer, and the stream waits for on_drained. */
	CHECK(run_until(stream_stalled, NULL));
	for (int i = 0; i < 20; i++)
		loop_run_once(&loop);
	CHECK(srv.stream_sent < srv.stream_total);
	CHECK(loop_conn_pending(srv.conn) > 0);

	/* Now it reads: every byte arrives, in order, through partial writes. */
	StreamCheck sc = {&cl, 0, true};
	CHECK(run_until(stream_received, &sc));
	CHECK(sc.ok);
	CHECK(sc.got == srv.stream_total);
	CHECK(srv.drained > 1);
	CHECK(srv.closed == 0);
	close(fd);
	CHECK(run_until(srv_closed, NULL));
	teardown();
}

static void test_overflow_closes(void)
{
	memset(&srv, 0, sizeof(srv));
	setup(true);
	LoopListener *l = listen_local(echo_accept, 1024, 0);
	Client cl = {.fd = client_connect(loop_listener_port(l))};
	int one = 1;
	CHECK(run_until(srv_accepted, &one));
	char buf[1024] = {0};
	CHECK(loop_conn_space(srv.conn) == 1024);
	CHECK(loop_conn_write(srv.conn, buf, 1000));
	CHECK(!loop_conn_write(srv.conn, buf, 25)); /* 1025 > 1024 */
	CHECK(!loop_conn_write(srv.conn, buf, 1));  /* closing */
	CHECK(run_until(client_eof, &cl));
	CHECK(srv.closed == 1 && srv.reason == LOOP_CLOSE_OVERFLOW);
	CHECK(cl.len == 0); /* the buffered output was discarded */
	close(cl.fd);
	teardown();
}

/* ---- Timers and the poll timeout -------------------------------------------- */

typedef struct {
	uint64_t fired_at; /* pal_uptime_ms at the TIMER compute, 0 before */
	uint64_t engine_now;
} SchedState;

/* test/Once: TIMER_SCHEDULED; arms 50 ms at start, nothing after. */
static int64_t once(BlockCtx *ctx)
{
	SchedState *s = ctx->state;
	if (ctx->reason == REASON_TIMER) {
		s->fired_at = pal_uptime_ms();
		s->engine_now = ctx->now_ms;
	}
	return ctx->reason == REASON_START ? 50 : -1;
}

static const PortDescriptor once_out[] = {
	{.name = "out", .description = "", .type = "number", .default_value = LINKABLE_I64_INIT(0)},
};

static const BlockTypeDef once_def = {.name = "test/Once",
				      .icon = "once",
				      .description = "",
				      .outputs = once_out,
				      .output_count = 1,
				      .compute_on_input = false,
				      .timer = TIMER_SCHEDULED,
				      .compute = once,
				      .state_size = sizeof(SchedState),
				      .change_threshold = NAN};

static bool timer_fired(void *arg)
{
	return ((SchedState *)arg)->fired_at != 0;
}

static void test_timer_deadline(void)
{
	setup(false);
	Graph *g = graph_create(&rt);
	Block *b = NULL;
	CHECK(graph_create_block(g, "once", &once_def, NULL, &b) == GRAPH_OK);
	/* Start at the engine time the loop will use. */
	uint64_t start = pal_uptime_ms();
	runtime_set_now(&rt, start);
	graph_start(g);
	SchedState *s = b->state;
	uint64_t iterations = loop.iterations;
	CHECK(run_until(timer_fired, s));
	/* Fired on time: not early, at most a little late. */
	CHECK(s->engine_now >= start + 50);
	CHECK(s->fired_at >= start + 50);
	CHECK(s->fired_at < start + 50 + 150);
	/* And by waiting in poll, not spinning: the start's queued output, the
	 * wait, maybe a wakeup a ms early. */
	CHECK(loop.iterations - iterations <= 5);
	CHECK(loop_last_timeout_ms(&loop) <= 50);
	graph_destroy(g);
	teardown();
}

static uint64_t tick_calls;

static uint64_t tick_20(void *ctx, uint64_t now_ms)
{
	(void)ctx;
	(void)now_ms;
	tick_calls++;
	return 20;
}

static void test_drain_budget(void)
{
	setup(false);
	CHECK(loop.drain_budget == LOOP_DRAIN_BUDGET);
	size_t total = 2 * LOOP_DRAIN_BUDGET + 10;
	CHECK(total <= work_queue_capacity());
	for (size_t i = 0; i < total; i++)
		work_queue_push_io(&rt.queue, (uint32_t)i, 0, linkable_from_i64(0));

	uint64_t t0 = pal_uptime_ms();
	loop_run_once(&loop);
	CHECK(work_queue_len(&rt.queue) == total - LOOP_DRAIN_BUDGET);
	loop_run_once(&loop);
	CHECK(loop_last_timeout_ms(&loop) == 0); /* work remained */
	CHECK(work_queue_len(&rt.queue) == 10);
	loop_run_once(&loop);
	CHECK(loop_last_timeout_ms(&loop) == 0);
	CHECK(work_queue_empty(&rt.queue));
	CHECK(pal_uptime_ms() - t0 < 500);

	/* Idle: the wait is bounded by a tick hook's deadline (and by
	 * LOOP_MAX_WAIT_MS without one). */
	CHECK(loop_add_tick(&loop, tick_20, NULL));
	loop_run_once(&loop); /* first call: due at once */
	CHECK(tick_calls == 1);
	loop_run_once(&loop);
	CHECK(tick_calls == 2);
	CHECK(loop_last_timeout_ms(&loop) > 0 && loop_last_timeout_ms(&loop) <= 20);
	teardown();
}

/* ---- Cross-context enqueue -------------------------------------------------- */

#define THREAD_EVENTS 40

static void *producer(void *arg)
{
	(void)arg;
	sleep_ms(30); /* let the engine thread block in poll first */
	for (int i = 0; i < THREAD_EVENTS; i++) {
		WorkEntry e = {.type = WORK_IO_EVENT};
		e.as.io.port = (uint32_t)i;
		e.as.io.value = linkable_from_i64(1000 + i);
		CHECK(pal_handoff_push(&e) == PAL_OK);
		CHECK(e.type == WORK_NONE);
	}
	return NULL;
}

static bool queue_has(void *arg)
{
	return work_queue_len(&rt.queue) >= *(size_t *)arg;
}

static void test_handoff_from_thread(void)
{
	setup(false);
	loop.drain_budget = 0; /* keep the entries in the queue to look at */
	pthread_t t;
	uint64_t t0 = pal_uptime_ms();
	CHECK(pthread_create(&t, NULL, producer, NULL) == 0);
	size_t want = THREAD_EVENTS;
	CHECK(run_until(queue_has, &want));
	/* Woken by the push, not by LOOP_MAX_WAIT_MS. */
	CHECK(pal_uptime_ms() - t0 < LOOP_MAX_WAIT_MS / 2);
	pthread_join(t, NULL);
	CHECK(work_queue_len(&rt.queue) == THREAD_EVENTS);
	WorkEntry e;
	for (int i = 0; i < THREAD_EVENTS; i++) {
		CHECK(work_queue_pop(&rt.queue, &e));
		CHECK(e.type == WORK_IO_EVENT && e.as.io.port == (uint32_t)i);
		CHECK(linkable_to_i64(&e.as.io.value) == 1000 + i);
		work_queue_entry_clear(&e);
	}
	CHECK(pal_handoff_drops() == 0);
	teardown();
}

static void test_handoff_overflow(void)
{
	setup(false);
	loop.drain_budget = 0;
	size_t extra = 10;
	for (size_t i = 0; i < PAL_HANDOFF_CAPACITY + extra; i++) {
		WorkEntry e = {.type = WORK_IO_EVENT};
		e.as.io.port = (uint32_t)i;
		/* Strings: a dropped entry's value must be released (asan). */
		CHECK(linkable_from_string(&e.as.io.value, "value", 5));
		CHECK(pal_handoff_push(&e) == PAL_OK);
	}
	CHECK(pal_handoff_drops() == extra);
	WorkEntry none = {.type = WORK_NONE};
	CHECK(pal_handoff_push(&none) == PAL_ERR_INVALID);

	loop_run_once(&loop);
	CHECK(loop.handoff_drops == extra);
	CHECK(work_queue_len(&rt.queue) == PAL_HANDOFF_CAPACITY);
	WorkEntry e;
	CHECK(work_queue_pop(&rt.queue, &e));
	CHECK(e.as.io.port == extra); /* the oldest were dropped */
	work_queue_entry_clear(&e);

	/* Entries still in the handoff at close are released. */
	WorkEntry left = {.type = WORK_IO_EVENT};
	CHECK(linkable_from_string(&left.as.io.value, "left", 4));
	CHECK(pal_handoff_push(&left) == PAL_OK);
	teardown();

	/* Closed: refused, and the value released. */
	WorkEntry late = {.type = WORK_IO_EVENT};
	CHECK(linkable_from_string(&late.as.io.value, "late", 4));
	CHECK(pal_handoff_push(&late) == PAL_ERR_UNAVAILABLE);
	CHECK(late.type == WORK_NONE);
}

/* ---- Stop ------------------------------------------------------------------- */

static void *stopper(void *arg)
{
	(void)arg;
	sleep_ms(30);
	pal_request_stop();
	return NULL;
}

static void test_stop_from_thread(void)
{
	int fds_before = count_fds();
	memset(&srv, 0, sizeof(srv));
	setup(false);
	LoopListener *l = listen_local(echo_accept, 4096, 0);
	Client cl = {.fd = client_connect(loop_listener_port(l))};
	int one = 1;
	CHECK(run_until(srv_accepted, &one));
	CHECK(loop_conn_write(srv.conn, "last", 4));

	pthread_t t;
	uint64_t t0 = pal_uptime_ms();
	CHECK(pthread_create(&t, NULL, stopper, NULL) == 0);
	loop_run(&loop); /* blocks in poll until the stop request */
	pthread_join(t, NULL);
	CHECK(pal_uptime_ms() - t0 < LOOP_MAX_WAIT_MS / 2);
	CHECK(pal_stop_requested());
	CHECK(srv.closed == 1 && srv.reason == LOOP_CLOSE_SHUTDOWN);
	CHECK(loop.listener_count == 0 && loop.conn_count == 0);
	/* What was buffered went out before the close. */
	CHECK(run_until(client_eof, &cl) || cl.eof);
	CHECK(cl.len == 4 && memcmp(cl.buf, "last", 4) == 0);
	close(cl.fd);
	teardown();
	CHECK(!pal_stop_requested());
	CHECK(count_fds() == fds_before);
}

static void *interrupter(void *arg)
{
	(void)arg;
	sleep_ms(30);
	kill(getpid(), SIGINT); /* any thread may take it: the handler only requests */
	return NULL;
}

static void test_stop_signal(void)
{
	int fds_before = count_fds();
	memset(&srv, 0, sizeof(srv));
	CHECK(posix_stop_signals_install() == PAL_OK);
	/* A stop requested before the loop opens still stops it. */
	CHECK(raise(SIGTERM) == 0);
	CHECK(pal_stop_requested());
	setup(false);
	LoopListener *l = listen_local(echo_accept, 4096, 0);
	(void)l;
	loop_run(&loop);
	CHECK(loop.listener_count == 0);
	teardown();
	CHECK(!pal_stop_requested());

	/* SIGINT while the loop waits in poll. */
	setup(false);
	listen_local(echo_accept, 4096, 0);
	pthread_t t;
	uint64_t t0 = pal_uptime_ms();
	CHECK(pthread_create(&t, NULL, interrupter, NULL) == 0);
	loop_run(&loop);
	pthread_join(t, NULL);
	CHECK(pal_uptime_ms() - t0 < LOOP_MAX_WAIT_MS / 2);
	CHECK(loop.listener_count == 0);
	teardown();
	posix_stop_signals_restore();
	CHECK(count_fds() == fds_before);
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	int fds_start = count_fds();
	CHECK(fds_start > 0);
	RUN(test_listen_errors);
	RUN(test_echo_round_trip);
	RUN(test_close_after_flush);
	RUN(test_refused_and_max_conns);
	RUN(test_ipv6);
	RUN(test_slow_reader);
	RUN(test_overflow_closes);
	RUN(test_timer_deadline);
	RUN(test_drain_budget);
	RUN(test_handoff_from_thread);
	RUN(test_handoff_overflow);
	RUN(test_stop_from_thread);
	RUN(test_stop_signal);
	/* Nothing leaked across all tests. */
	CHECK(count_fds() == fds_start);
	return test_result();
}
