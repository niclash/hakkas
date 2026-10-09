#include "engine/loop.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "system/log.h"

/* Accepts per listener and iteration, so one busy listener cannot starve the
 * rest of the iteration. */
#define ACCEPTS_PER_ITERATION 16

/* Handed-off entries moved per pal_handoff_take call. */
#define HANDOFF_BATCH 16

const char *loop_close_reason_str(enum loop_close_reason reason)
{
	switch (reason) {
	case LOOP_CLOSE_LOCAL:
		return "closed";
	case LOOP_CLOSE_PEER:
		return "closed by peer";
	case LOOP_CLOSE_ERROR:
		return "socket error";
	case LOOP_CLOSE_OVERFLOW:
		return "outgoing buffer full";
	case LOOP_CLOSE_REFUSED:
		return "refused";
	case LOOP_CLOSE_SHUTDOWN:
		return "shutdown";
	}
	return "?";
}

/* Makes room for need elements of size in the heap array arr of *cap
 * elements, doubling from initial. Returns the (possibly moved) array, with
 * *cap updated, or NULL, with arr and *cap unchanged, when out of memory. */
static void *grow(void *arr, size_t *cap, size_t need, size_t size, size_t initial)
{
	if (need <= *cap)
		return arr;
	size_t c = *cap == 0 ? initial : *cap;
	while (c < need) {
		if (c > SIZE_MAX / 2 / size)
			return NULL;
		c *= 2;
	}
	void *p = realloc(arr, c * size);
	if (p != NULL)
		*cap = c;
	return p;
}

enum pal_err loop_init(Loop *loop, Runtime *rt)
{
	memset(loop, 0, sizeof(*loop));
	loop->rt = rt;
	loop->drain_budget = LOOP_DRAIN_BUDGET;
	return pal_loop_open();
}

/* ---- Connections ---------------------------------------------------------- */

static void mark_close_now(LoopConn *c, enum loop_close_reason reason)
{
	if (c->state == CONN_CLOSE_NOW)
		return;
	c->state = CONN_CLOSE_NOW;
	c->reason = reason;
	c->out_start = 0;
	c->out_len = 0;
}

enum pal_err loop_conn_read(LoopConn *c, void *buf, size_t cap, size_t *len)
{
	*len = 0;
	if (c->state != CONN_OPEN)
		return PAL_ERR_CLOSED;
	enum pal_err err = pal_socket_read(c->sock, buf, cap, len);
	if (err == PAL_ERR_CLOSED)
		mark_close_now(c, LOOP_CLOSE_PEER);
	else if (err != PAL_OK && err != PAL_ERR_WOULD_BLOCK)
		mark_close_now(c, LOOP_CLOSE_ERROR);
	return err;
}

bool loop_conn_write(LoopConn *c, const void *data, size_t len)
{
	if (c->state != CONN_OPEN)
		return false;
	if (len == 0)
		return true;
	if (len > c->out_cap - c->out_len) {
		log_warn("connection from %s:%u: outgoing buffer full (%zu bytes), closing",
			 c->peer.addr, (unsigned)c->peer.port, c->out_cap);
		mark_close_now(c, LOOP_CLOSE_OVERFLOW);
		return false;
	}
	if (c->out_start + c->out_len + len > c->out_cap) {
		memmove(c->out, c->out + c->out_start, c->out_len);
		c->out_start = 0;
	}
	memcpy(c->out + c->out_start + c->out_len, data, len);
	c->out_len += len;
	c->notify_drained = true;
	return true;
}

void loop_conn_close(LoopConn *c)
{
	if (c->state != CONN_OPEN)
		return;
	c->state = CONN_CLOSE_AFTER_FLUSH;
	c->reason = LOOP_CLOSE_LOCAL;
	c->close_by = c->loop->now_ms + LOOP_CLOSE_LINGER_MS;
}

void loop_conn_abort(LoopConn *c)
{
	mark_close_now(c, LOOP_CLOSE_LOCAL);
}

/* Writes as much of the outgoing buffer as the socket takes; on emptying it,
 * calls on_drained. */
static void flush(LoopConn *c)
{
	while (c->out_len > 0 && c->state != CONN_CLOSE_NOW) {
		size_t n;
		enum pal_err err = pal_socket_write(c->sock, c->out + c->out_start, c->out_len, &n);
		if (err == PAL_ERR_WOULD_BLOCK)
			return;
		if (err != PAL_OK) {
			mark_close_now(c, err == PAL_ERR_CLOSED ? LOOP_CLOSE_PEER
							       : LOOP_CLOSE_ERROR);
			return;
		}
		c->out_start += n;
		c->out_len -= n;
	}
	if (c->out_len == 0)
		c->out_start = 0;
	if (c->out_len == 0 && c->notify_drained && c->state == CONN_OPEN) {
		c->notify_drained = false;
		if (c->handler != NULL && c->handler->on_drained != NULL)
			c->handler->on_drained(c->ctx, c);
	}
}

/* Closes and frees the connection at index i of loop->conns. */
static void close_conn(Loop *loop, size_t i, enum loop_close_reason reason)
{
	LoopConn *c = loop->conns[i];
	memmove(&loop->conns[i], &loop->conns[i + 1],
		(loop->conn_count - i - 1) * sizeof(LoopConn *));
	loop->conn_count--;
	c->state = CONN_CLOSE_NOW;
	if (c->accepted && c->handler != NULL && c->handler->on_closed != NULL)
		c->handler->on_closed(c->ctx, c, reason);
	pal_socket_close(c->sock);
	c->listener->conn_count--;
	free(c->out);
	free(c);
}

/* Closes the connections marked for it, last first (closing one only moves
 * those after it). on_closed may mark others: those not yet visited close
 * now, the rest in the next iteration. */
static void sweep(Loop *loop, uint64_t now)
{
	for (size_t i = loop->conn_count; i-- > 0;) {
		LoopConn *c = loop->conns[i];
		if (c->state == CONN_CLOSE_NOW ||
		    (c->state == CONN_CLOSE_AFTER_FLUSH && (c->out_len == 0 || now >= c->close_by)))
			close_conn(loop, i, c->reason);
	}
}

/* ---- Listeners ------------------------------------------------------------ */

enum pal_err loop_listen(Loop *loop, const LoopListenerConfig *cfg, LoopListener **out)
{
	*out = NULL;
	if (cfg == NULL || cfg->host == NULL || cfg->out_cap == 0 || cfg->on_accept == NULL)
		return PAL_ERR_INVALID;
	LoopListener **ls = grow(loop->listeners, &loop->listener_cap, loop->listener_count + 1,
				 sizeof(*ls), 4);
	if (ls == NULL)
		return PAL_ERR_UNAVAILABLE;
	loop->listeners = ls;
	LoopListener *l = calloc(1, sizeof(*l));
	if (l == NULL)
		return PAL_ERR_UNAVAILABLE;
	enum pal_err err = pal_socket_listen(cfg->host, cfg->port, LOOP_LISTEN_BACKLOG, &l->sock);
	if (err == PAL_OK)
		err = pal_socket_local_port(l->sock, &l->port);
	if (err != PAL_OK) {
		log_error("cannot listen on %s port %u: %s", cfg->host, (unsigned)cfg->port,
			  pal_err_str(err));
		pal_socket_close(l->sock);
		free(l);
		return err;
	}
	l->loop = loop;
	l->max_conns = cfg->max_conns;
	l->out_cap = cfg->out_cap;
	l->on_accept = cfg->on_accept;
	l->ctx = cfg->ctx;
	loop->listeners[loop->listener_count++] = l;
	log_info("listening on %s port %u", cfg->host, (unsigned)l->port);
	*out = l;
	return PAL_OK;
}

void loop_listener_close(LoopListener *l)
{
	Loop *loop = l->loop;
	for (size_t i = loop->conn_count; i-- > 0;) {
		if (loop->conns[i]->listener == l)
			close_conn(loop, i, LOOP_CLOSE_SHUTDOWN);
	}
	for (size_t i = 0; i < loop->listener_count; i++) {
		if (loop->listeners[i] == l) {
			memmove(&loop->listeners[i], &loop->listeners[i + 1],
				(loop->listener_count - i - 1) * sizeof(LoopListener *));
			loop->listener_count--;
			break;
		}
	}
	pal_socket_close(l->sock);
	free(l);
}

/* Accepts the pending connections of l. */
static void accept_conns(Loop *loop, LoopListener *l, uint64_t now)
{
	for (int k = 0; k < ACCEPTS_PER_ITERATION; k++) {
		pal_socket sock;
		struct pal_peer peer;
		enum pal_err err = pal_socket_accept(l->sock, &sock, &peer);
		if (err == PAL_ERR_WOULD_BLOCK)
			return;
		if (err != PAL_OK) {
			/* Out of descriptors: the connection stays pending and the
			 * listener readable, so pause it instead of spinning. */
			if (!l->accept_failing)
				log_warn("port %u: cannot accept a connection: %s",
					 (unsigned)l->port, pal_err_str(err));
			l->accept_failing = true;
			l->paused_until = now + LOOP_ACCEPT_RETRY_MS;
			return;
		}
		l->accept_failing = false;
		if (l->max_conns != 0 && l->conn_count >= l->max_conns) {
			log_info("port %u: connection from %s refused: %u connections open",
				 (unsigned)l->port, peer.addr, (unsigned)l->conn_count);
			pal_socket_close(sock);
			continue;
		}
		LoopConn **cs = grow(loop->conns, &loop->conn_cap, loop->conn_count + 1,
				     sizeof(*cs), 8);
		if (cs != NULL)
			loop->conns = cs;
		LoopConn *c = calloc(1, sizeof(*c));
		uint8_t *out = malloc(l->out_cap);
		if (cs == NULL || c == NULL || out == NULL) {
			log_warn("port %u: out of memory for a connection from %s",
				 (unsigned)l->port, peer.addr);
			free(c);
			free(out);
			pal_socket_close(sock);
			continue;
		}
		c->loop = loop;
		c->listener = l;
		c->sock = sock;
		c->peer = peer;
		c->out = out;
		c->out_cap = l->out_cap;
		c->state = CONN_OPEN;
		loop->conns[loop->conn_count++] = c;
		l->conn_count++;
		log_fine("port %u: connection from %s:%u", (unsigned)l->port, peer.addr,
			 (unsigned)peer.port);
		if (l->on_accept(l->ctx, c))
			c->accepted = true;
		else
			mark_close_now(c, LOOP_CLOSE_REFUSED);
	}
}

/* ---- The iteration -------------------------------------------------------- */

bool loop_add_tick(Loop *loop, loop_tick_fn fn, void *ctx)
{
	if (fn == NULL || loop->tick_count == LOOP_TICKS_MAX)
		return false;
	loop->ticks[loop->tick_count++] = (LoopTick){fn, ctx, 0};
	return true;
}

static uint64_t min_u64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

/* The ms from now until due (0 if due already). */
static uint64_t until(uint64_t due, uint64_t now)
{
	return due <= now ? 0 : due - now;
}

/* Step 1's timeout (01 §3.2 item 7, §3.2.1). */
static uint32_t poll_timeout(const Loop *loop, uint64_t now)
{
	if (loop->stop || pal_stop_requested() || !work_queue_empty(&loop->rt->queue))
		return 0;
	uint64_t t = min_u64(LOOP_MAX_WAIT_MS, runtime_timer_timeout_ms(loop->rt, now));
	for (size_t i = 0; i < loop->tick_count; i++)
		if (loop->ticks[i].due != LOOP_NO_DEADLINE)
			t = min_u64(t, until(loop->ticks[i].due, now));
	for (size_t i = 0; i < loop->listener_count; i++)
		if (loop->listeners[i]->accept_failing)
			t = min_u64(t, until(loop->listeners[i]->paused_until, now));
	for (size_t i = 0; i < loop->conn_count; i++) {
		const LoopConn *c = loop->conns[i];
		if (c->state == CONN_CLOSE_NOW)
			return 0;
		if (c->state == CONN_CLOSE_AFTER_FLUSH)
			t = min_u64(t, until(c->close_by, now));
	}
	return (uint32_t)t;
}

/* Fills the poll set: the listeners not paused, then every connection.
 * Returns its length and the number of listener entries in *listeners. */
static size_t build_pollset(Loop *loop, uint64_t now, size_t *listeners)
{
	/* Both arrays share poll_cap; on a failure to grow, what fits is
	 * polled now and the rest later. */
	size_t need = loop->listener_count + loop->conn_count;
	size_t cap = loop->poll_cap;
	struct pal_pollfd *set = grow(loop->pollset, &cap, need, sizeof(*set), 16);
	if (set != NULL) {
		loop->pollset = set;
		size_t cap2 = loop->poll_cap;
		void **owner = grow(loop->pollowner, &cap2, need, sizeof(*owner), 16);
		if (owner != NULL) {
			loop->pollowner = owner;
			loop->poll_cap = cap;
		}
	}
	size_t n = 0;
	for (size_t i = 0; i < loop->listener_count && n < loop->poll_cap; i++) {
		LoopListener *l = loop->listeners[i];
		if (l->accept_failing && now < l->paused_until)
			continue;
		loop->pollset[n] = (struct pal_pollfd){l->sock, PAL_POLL_IN, 0};
		loop->pollowner[n++] = l;
	}
	*listeners = n;
	for (size_t i = 0; i < loop->conn_count && n < loop->poll_cap; i++) {
		LoopConn *c = loop->conns[i];
		uint8_t ev = 0;
		if (c->state == CONN_OPEN && !c->read_paused)
			ev |= PAL_POLL_IN;
		if (c->out_len > 0)
			ev |= PAL_POLL_OUT;
		loop->pollset[n] = (struct pal_pollfd){c->sock, ev, 0};
		loop->pollowner[n++] = c;
	}
	return n;
}

/* Step 3: the handed-off entries into the work queue, in push order. */
static void take_handoff(Loop *loop)
{
	WorkEntry batch[HANDOFF_BATCH];
	size_t k;
	do {
		uint64_t dropped = 0;
		k = pal_handoff_take(batch, HANDOFF_BATCH, &dropped);
		loop->handoff_drops += dropped;
		for (size_t i = 0; i < k; i++)
			work_queue_push_entry(&loop->rt->queue, &batch[i]);
	} while (k == HANDOFF_BATCH);
}

static void run_ticks(Loop *loop, uint64_t now)
{
	for (size_t i = 0; i < loop->tick_count; i++) {
		LoopTick *t = &loop->ticks[i];
		uint64_t delay = t->fn(t->ctx, now);
		if (delay == LOOP_NO_DEADLINE || delay > UINT64_MAX - now)
			t->due = LOOP_NO_DEADLINE;
		else
			t->due = now + delay;
	}
}

/* A connection without on_readable: its input is read and discarded, so
 * that the end of stream is still seen. */
static void discard_input(LoopConn *c)
{
	uint8_t buf[256];
	size_t n;
	while (loop_conn_read(c, buf, sizeof(buf), &n) == PAL_OK)
		;
}

/* Step 5. Connections accepted here are not in the poll set; they are polled
 * from the next iteration. */
static void dispatch(Loop *loop, size_t n, size_t listeners, uint64_t now)
{
	for (size_t i = 0; i < n; i++) {
		uint8_t rev = loop->pollset[i].revents;
		if (rev == 0)
			continue;
		if (i < listeners) {
			accept_conns(loop, loop->pollowner[i], now);
			continue;
		}
		LoopConn *c = loop->pollowner[i];
		if ((rev & PAL_POLL_IN) && c->state == CONN_OPEN && !c->read_paused) {
			if (c->handler != NULL && c->handler->on_readable != NULL)
				c->handler->on_readable(c->ctx, c);
			else
				discard_input(c);
		} else if (rev & PAL_POLL_ERR) {
			mark_close_now(c, LOOP_CLOSE_PEER);
		}
	}
}

/* The rate-limited warning for handoff drops, like work_queue_report_drops. */
static void report_handoff_drops(Loop *loop, uint64_t now)
{
	if (loop->handoff_drops == loop->handoff_drops_reported)
		return;
	if (loop->handoff_reported &&
	    now - loop->handoff_last_report_ms < WORK_QUEUE_DROP_REPORT_INTERVAL_MS)
		return;
	log_warn("IO handoff full: dropped %" PRIu64 " oldest entries since last warning (%" PRIu64
		 " total)",
		 loop->handoff_drops - loop->handoff_drops_reported, loop->handoff_drops);
	loop->handoff_drops_reported = loop->handoff_drops;
	loop->handoff_last_report_ms = now;
	loop->handoff_reported = true;
}

void loop_run_once(Loop *loop)
{
	Runtime *rt = loop->rt;
	uint64_t now = pal_uptime_ms();
	loop->now_ms = now;

	/* 1. Poll, at most until the next deadline. */
	uint32_t timeout = poll_timeout(loop, now);
	loop->last_timeout_ms = timeout;
	size_t listeners;
	size_t n = build_pollset(loop, now, &listeners);
	size_t ready;
	enum pal_err err = pal_poll(loop->pollset, n, timeout, &ready);
	if (err != PAL_OK) {
		log_error("poll failed: %s", pal_err_str(err));
		for (size_t i = 0; i < n; i++)
			loop->pollset[i].revents = 0;
	}

	/* 2. The clock, and the due timers. */
	now = pal_uptime_ms();
	loop->now_ms = now;
	runtime_fire_timers(rt, now);

	/* 3. IO events handed over from other contexts. */
	take_handoff(loop);

	/* 4. Other deadlines. */
	run_ticks(loop, now);

	/* 5. Sockets: accept, read into protocol dispatch. */
	dispatch(loop, n, listeners, now);

	/* 6. The work queue, within the budget. */
	runtime_drain(rt, loop->drain_budget);

	/* 7. Write out, close, report. */
	for (size_t i = 0; i < loop->conn_count; i++)
		if (loop->conns[i]->out_len > 0)
			flush(loop->conns[i]);
	sweep(loop, now);
	work_queue_report_drops(&rt->queue, now);
	report_handoff_drops(loop, now);
	loop->iterations++;
}

/* Closes every connection, after one attempt to write out its buffer, and
 * every listener. */
static void close_all(Loop *loop)
{
	for (size_t i = 0; i < loop->conn_count; i++) {
		LoopConn *c = loop->conns[i];
		if (c->state != CONN_CLOSE_NOW && c->out_len > 0)
			flush(c);
	}
	while (loop->conn_count > 0) {
		size_t i = loop->conn_count - 1;
		LoopConn *c = loop->conns[i];
		close_conn(loop, i, c->state == CONN_CLOSE_NOW ? c->reason : LOOP_CLOSE_SHUTDOWN);
	}
	while (loop->listener_count > 0)
		loop_listener_close(loop->listeners[loop->listener_count - 1]);
}

void loop_run(Loop *loop)
{
	while (!loop->stop && !pal_stop_requested())
		loop_run_once(loop);
	log_info("engine loop stopping");
	close_all(loop);
	loop->stop = false;
}

void loop_deinit(Loop *loop)
{
	close_all(loop);
	free(loop->listeners);
	free(loop->conns);
	free(loop->pollset);
	free(loop->pollowner);
	pal_loop_close();
	memset(loop, 0, sizeof(*loop));
}
