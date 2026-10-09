/*
 * The engine loop (01 §3.2, §3.2.1): the one loop of the engine thread,
 * written once against the PAL (platform/pal.h). It owns the listening
 * sockets and connections and drives the runtime (graphs/runtime.h): timers,
 * the cross-context handoff, the work queue's drain.
 *
 * Naming: functions are prefixed loop_; the types are Loop, LoopListener and
 * LoopConn.
 *
 * One iteration (loop_run_once), in the order of 01 §3.2.1 (decided, plan
 * task 12):
 *  1. poll the sockets, at most until the next deadline: timeout 0 if work is
 *     queued or a stop is requested, otherwise the earliest of the next block
 *     timer (runtime_timer_timeout_ms), every tick hook's next deadline, and
 *     LOOP_MAX_WAIT_MS;
 *  2. read the clock (pal_uptime_ms) and fire the due timers
 *     (runtime_fire_timers, which also sets the runtime's time);
 *  3. move the handed-off entries (IO events, 04 §0) into the work queue, in
 *     the order they were pushed;
 *  4. call the tick hooks (session expiry and the like, 01 §3.2 item 7);
 *  5. dispatch socket readiness: accept on readable listeners, call
 *     on_readable for readable connections (the protocol dispatch, which may
 *     queue work), mark connections with a socket error for closing;
 *  6. drain the work queue, at most drain_budget entries (LOOP_DRAIN_BUDGET);
 *  7. write out every outgoing buffer holding data, as far as the socket
 *     takes it (so what steps 4-6 wrote goes out without another poll round;
 *     the rest waits for the socket to be writable), close the connections
 *     marked for closing, and report queue and handoff drops (rate-limited
 *     warnings).
 *
 * Connections: a listener's on_accept callback sees each new connection and
 * gives it its handler (loop_conn_set_handler): on_readable when data (or the
 * end of stream) is there to read with loop_conn_read, on_drained when the
 * outgoing buffer has been written out completely, on_closed once when it
 * goes away. Writing (loop_conn_write) only appends to the connection's
 * bounded outgoing buffer (01 §4: notification writes to a buffer, the loop
 * flushes it). Nothing closes a connection from inside a call: loop_conn_close,
 * a failed read, a full buffer only mark it, and the loop closes it at the end
 * of the iteration (calling on_closed), so callbacks may freely write to and
 * close any connection, including while graph events are being delivered.
 *
 * Full outgoing buffer (decided, plan task 12): a write that does not fit the
 * free space closes the connection (LOOP_CLOSE_OVERFLOW) and discards the
 * write and its buffered output. A client that cannot keep up must not
 * silently miss events (02 §1.1: events are the only way its view stays in
 * sync); after a reconnect it starts from a fresh snapshot.
 *
 * Memory: a connection and its outgoing buffer (the listener's out_cap bytes)
 * are allocated when it is accepted; the loop's arrays grow by doubling.
 *
 * Threading: engine thread only. Other contexts reach the loop through the
 * PAL's handoff (pal_handoff_push) and stop request (pal_request_stop).
 */
#ifndef HAKKAS_ENGINE_LOOP_H
#define HAKKAS_ENGINE_LOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphs/runtime.h"
#include "platform/pal.h"

/* Work queue entries drained per iteration (01 §3.2 drain decisions): bounds
 * the time between two socket polls when work keeps coming (a feedback
 * cycle, 01 §3.2 item 4) to a fraction of the queue (1024 entries by default).
 * A port's build may define it. */
#ifndef LOOP_DRAIN_BUDGET
#define LOOP_DRAIN_BUDGET 256
#endif

/* The longest poll wait in ms, so the iteration's housekeeping (drop
 * reports, tick hooks) runs at least this often. */
#ifndef LOOP_MAX_WAIT_MS
#define LOOP_MAX_WAIT_MS 1000
#endif

/* Listen backlog of every listener. */
#define LOOP_LISTEN_BACKLOG 8

/* Pause of a listener after accept failed for lack of resources (descriptors),
 * so a listener that stays readable does not spin the loop. */
#define LOOP_ACCEPT_RETRY_MS 100

/* The longest a loop_conn_close waits for its outgoing buffer to be written
 * out (a peer that stopped reading) before the connection is closed anyway. */
#define LOOP_CLOSE_LINGER_MS 5000

/* Most tick hooks. */
#define LOOP_TICKS_MAX 4

/* A tick hook's "no deadline". */
#define LOOP_NO_DEADLINE UINT64_MAX

typedef struct Loop Loop;
typedef struct LoopListener LoopListener;
typedef struct LoopConn LoopConn;

enum loop_close_reason {
	LOOP_CLOSE_LOCAL = 0, /* loop_conn_close or loop_conn_abort */
	LOOP_CLOSE_PEER,      /* the peer closed or reset the connection */
	LOOP_CLOSE_ERROR,     /* a socket error */
	LOOP_CLOSE_OVERFLOW,  /* a write did not fit the outgoing buffer */
	LOOP_CLOSE_REFUSED,   /* on_accept refused it, or the listener's max_conns */
	LOOP_CLOSE_SHUTDOWN,  /* its listener or the loop was closed */
};

/* A static description of reason, for log lines. */
const char *loop_close_reason_str(enum loop_close_reason reason);

/* A connection's handler; any callback may be NULL. ctx is the handler's
 * own (loop_conn_set_handler). */
typedef struct {
	/* Data or the end of stream is there: read it with loop_conn_read until
	 * it returns PAL_ERR_WOULD_BLOCK, or pause reading. Not called while
	 * reading is paused or after the connection was marked for closing.
	 * Poll is level-triggered: data left unread calls this again. Without
	 * it, the loop reads and discards the input (and so sees the end of
	 * stream). A paused connection only learns of its peer's close when it
	 * resumes or a write fails. */
	void (*on_readable)(void *ctx, LoopConn *conn);
	/* The outgoing buffer, after holding data, has been written out
	 * completely: room to stream more (e.g. a file). */
	void (*on_drained)(void *ctx, LoopConn *conn);
	/* The connection is closed; called once, from the loop's close step.
	 * conn is freed after it returns; release ctx here. */
	void (*on_closed)(void *ctx, LoopConn *conn, enum loop_close_reason reason);
} LoopConnHandler;

/* Called for each connection a listener accepts. Give it a handler
 * (loop_conn_set_handler) and return true, or return false to refuse it
 * (it is closed with LOOP_CLOSE_REFUSED; on_closed is not called). It may
 * write (e.g. a refusal message) and then loop_conn_close. */
typedef bool (*loop_accept_fn)(void *ctx, LoopConn *conn);

typedef struct {
	const char *host; /* numeric address (pal_socket_listen) */
	uint16_t port;	  /* 0: an ephemeral port (loop_listener_port) */
	/* Connections at once; one more is accepted and closed at once,
	 * unseen. 0: no limit. (A protocol limit that answers the client, such
	 * as max_connections' HTTP 503 of 02 §0, is the handler's, with
	 * loop_listener_conn_count; this is the hard bound on resources.) */
	uint32_t max_conns;
	size_t out_cap; /* outgoing buffer per connection, bytes (> 0) */
	loop_accept_fn on_accept;
	void *ctx;
} LoopListenerConfig;

/* A tick hook (01 §3.2 item 7, e.g. session expiry, 02 §2.5 item 3): called
 * once per iteration after the timers fire, with the engine time; does
 * whatever is due and returns the ms until it next needs to run
 * (LOOP_NO_DEADLINE: no deadline), which bounds the poll timeout. */
typedef uint64_t (*loop_tick_fn)(void *ctx, uint64_t now_ms);

struct LoopConn {
	Loop *loop;
	LoopListener *listener;
	pal_socket sock;
	struct pal_peer peer;
	const LoopConnHandler *handler;
	void *ctx;
	uint8_t *out; /* outgoing buffer: out_len bytes from out_start */
	size_t out_cap;
	size_t out_start;
	size_t out_len;
	bool read_paused;
	bool notify_drained; /* data was written since the last on_drained */
	bool accepted;	     /* on_accept returned true */
	enum { CONN_OPEN, CONN_CLOSE_AFTER_FLUSH, CONN_CLOSE_NOW } state;
	enum loop_close_reason reason;
	uint64_t close_by; /* CONN_CLOSE_AFTER_FLUSH: closed at this time regardless */
};

struct LoopListener {
	Loop *loop;
	pal_socket sock;
	uint16_t port;
	uint32_t max_conns;
	size_t out_cap;
	loop_accept_fn on_accept;
	void *ctx;
	uint32_t conn_count; /* its open connections */
	uint64_t paused_until;
	bool accept_failing;
};

typedef struct {
	loop_tick_fn fn;
	void *ctx;
	uint64_t due; /* when it asked to run next; LOOP_NO_DEADLINE: no deadline */
} LoopTick;

struct Loop {
	Runtime *rt;
	size_t drain_budget; /* LOOP_DRAIN_BUDGET; tests may change it */
	bool stop;
	LoopListener **listeners;
	size_t listener_count, listener_cap;
	LoopConn **conns;
	size_t conn_count, conn_cap;
	struct pal_pollfd *pollset; /* built per iteration: listeners, then conns */
	void **pollowner;	    /* the LoopListener or LoopConn of each entry */
	size_t poll_cap;
	LoopTick ticks[LOOP_TICKS_MAX];
	size_t tick_count;
	uint64_t now_ms;	  /* the current iteration's engine time */
	uint32_t last_timeout_ms; /* the last poll's timeout */
	uint64_t iterations;
	/* Handoff drop reporting (rate-limited like the work queue's). */
	uint64_t handoff_drops, handoff_drops_reported, handoff_last_report_ms;
	bool handoff_reported;
};

/* Makes loop an empty loop over rt (borrowed; it must outlive the loop) and
 * opens the PAL's wakeup and handoff (pal_loop_open). The startup calls it
 * before any IO worker starts (05 §1.4). Returns the PAL's error. */
enum pal_err loop_init(Loop *loop, Runtime *rt);

/* Closes every connection (LOOP_CLOSE_SHUTDOWN, after one non-blocking
 * attempt to write out its buffer) and listener, frees the loop's memory and
 * closes the PAL's wakeup (pal_loop_close). */
void loop_deinit(Loop *loop);

/* Opens a listener (pal_socket_listen with LOOP_LISTEN_BACKLOG). Returns the
 * PAL's error (logged with host and port), PAL_ERR_UNAVAILABLE when out of
 * memory, PAL_ERR_INVALID for a bad config. */
enum pal_err loop_listen(Loop *loop, const LoopListenerConfig *cfg, LoopListener **out);

/* Closes a listener and its connections (LOOP_CLOSE_SHUTDOWN, on_closed
 * called) at once. Not from inside a callback. */
void loop_listener_close(LoopListener *l);

static inline uint16_t loop_listener_port(const LoopListener *l)
{
	return l->port;
}

/* The listener's open connections (those marked for closing included). */
static inline uint32_t loop_listener_conn_count(const LoopListener *l)
{
	return l->conn_count;
}

/* Adds a tick hook. False if LOOP_TICKS_MAX are registered. */
bool loop_add_tick(Loop *loop, loop_tick_fn fn, void *ctx);

/* One iteration (see above). */
void loop_run_once(Loop *loop);

/* Runs iterations until loop_stop or pal_request_stop, then closes every
 * connection and listener (as loop_deinit, without closing the wakeup). */
void loop_run(Loop *loop);

/* Asks loop_run to return after the current iteration. Engine thread (from
 * a callback); other contexts use pal_request_stop. */
static inline void loop_stop(Loop *loop)
{
	loop->stop = true;
}

/* The poll timeout of the last iteration, ms. */
static inline uint32_t loop_last_timeout_ms(const Loop *loop)
{
	return loop->last_timeout_ms;
}

/* ---- Connections ---------------------------------------------------------- */

static inline void loop_conn_set_handler(LoopConn *c, const LoopConnHandler *handler, void *ctx)
{
	c->handler = handler;
	c->ctx = ctx;
}

static inline void *loop_conn_ctx(const LoopConn *c)
{
	return c->ctx;
}

static inline const struct pal_peer *loop_conn_peer(const LoopConn *c)
{
	return &c->peer;
}

static inline LoopListener *loop_conn_listener(const LoopConn *c)
{
	return c->listener;
}

/* Reads into buf (pal_socket_read). On PAL_ERR_CLOSED or another error the
 * connection is marked for closing (LOOP_CLOSE_PEER / LOOP_CLOSE_ERROR) and
 * the error returned; the handler just returns. A connection marked for
 * closing reads nothing (PAL_ERR_CLOSED). */
enum pal_err loop_conn_read(LoopConn *c, void *buf, size_t cap, size_t *len);

/* Appends len bytes to the outgoing buffer, written out by the loop. False if
 * the connection is closing or the data does not fit: then it is closed
 * (LOOP_CLOSE_OVERFLOW, see "Full outgoing buffer" above). */
bool loop_conn_write(LoopConn *c, const void *data, size_t len);

/* Free space in the outgoing buffer. */
static inline size_t loop_conn_space(const LoopConn *c)
{
	return c->out_cap - c->out_len;
}

/* Bytes waiting in the outgoing buffer. */
static inline size_t loop_conn_pending(const LoopConn *c)
{
	return c->out_len;
}

/* Stops (true) or resumes (false) on_readable, e.g. while the outgoing
 * buffer is too full to answer more requests. */
static inline void loop_conn_pause_read(LoopConn *c, bool paused)
{
	c->read_paused = paused;
}

/* Closes the connection once its outgoing buffer has been written out, or
 * after LOOP_CLOSE_LINGER_MS (reason LOOP_CLOSE_LOCAL); reading stops at
 * once. */
void loop_conn_close(LoopConn *c);

/* Closes the connection at the end of the iteration, discarding its
 * outgoing buffer (reason LOOP_CLOSE_LOCAL). */
void loop_conn_abort(LoopConn *c);

#endif
