#include "engine/ws_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol/http.h"
#include "system/log.h"

/* Bytes read per loop_conn_read. */
#define CHUNK 4096

/* The most input bytes read and dropped before a close (see "Close" in
 * ws_server.h): what is already there, not a stream that keeps coming. */
#define DISCARD_MAX (64 * 1024)

/* The first size of a message buffer. */
#define MSG_INITIAL 256

enum ws_state {
	S_HANDSHAKE, /* reading the upgrade request */
	S_OPEN,	     /* upgraded: messages flow */
	S_CLOSING,   /* our close frame is sent; waiting for the client's */
	S_CLOSED,    /* the socket closes once its buffer is written out */
};

struct WsConn {
	WsServer *srv;
	LoopConn *conn;
	struct WsConn *prev, *next;
	enum ws_state state;
	bool counted; /* in srv->open_count */
	bool opened;  /* on_open was called */
	void *user;
	uint64_t deadline; /* S_HANDSHAKE, S_CLOSING */
	uint64_t last_rx;  /* S_OPEN: when a byte last arrived */
	bool ping_out;	   /* S_OPEN: a ping is unanswered */
	uint64_t ping_at;
	uint16_t close_code; /* the close code sent or received, for the log */

	/* The handshake: the head buffer (freed after the upgrade). */
	char *head;
	HttpRequest req;

	/* The frame being read. */
	uint8_t hdr[WS_FRAME_HEADER_MAX];
	size_t hdr_len;
	bool in_payload;
	bool skip;     /* its payload is dropped (closing, or after an error) */
	bool garbled;  /* the frame stream cannot be parsed: drop everything */
	WsFrameHeader fh;
	uint64_t remaining; /* its payload bytes still to come */
	size_t mask_off;
	uint8_t ctrl[WS_CONTROL_MAX]; /* a control frame's payload */
	size_t ctrl_len;

	/* The text message being reassembled. */
	bool in_message;
	char *msg;
	size_t msg_len, msg_cap;
	WsUtf8 utf8;
};

struct WsServer {
	Loop *loop;
	LoopListener *listener; /* NULL once closed */
	WsConn *conns;
	WsServerConfig cfg;
	char **origins; /* cfg.allowed_origins' copies, one allocation */
	uint32_t open_count;
	uint8_t chunk[CHUNK];
};

static const char *peer_addr(const WsConn *w)
{
	return loop_conn_peer(w->conn)->addr;
}

static unsigned peer_port(const WsConn *w)
{
	return loop_conn_peer(w->conn)->port;
}

static uint64_t now_ms(const WsConn *w)
{
	return loop_now_ms(w->srv->loop);
}

/* Reads and drops the input already received, so that the close sends FIN,
 * not a reset that could destroy what is still being sent. */
static void discard_input(WsConn *w)
{
	size_t total = 0, n;
	while (total < DISCARD_MAX &&
	       loop_conn_read(w->conn, w->srv->chunk, sizeof(w->srv->chunk), &n) == PAL_OK)
		total += n;
}

/* Closes the socket once the outgoing buffer is written out. */
static void finish(WsConn *w)
{
	w->state = S_CLOSED;
	discard_input(w);
	loop_conn_close(w->conn);
}

static void free_message(WsConn *w)
{
	free(w->msg);
	w->msg = NULL;
	w->msg_len = 0;
	w->msg_cap = 0;
	w->in_message = false;
}

/* ---- Sending ------------------------------------------------------------------ */

/* Frames one message; false if it does not fit (the loop closes the
 * connection and logs it, LOOP_CLOSE_OVERFLOW). */
static bool send_frame(WsConn *w, enum ws_opcode op, const void *data, size_t len)
{
	uint8_t h[WS_FRAME_HEADER_MAX];
	size_t hl = ws_frame_header_write(h, op, len);
	if (loop_conn_write(w->conn, h, hl) && loop_conn_write(w->conn, data, len))
		return true;
	w->state = S_CLOSED;
	return false;
}

/* A close frame with code (none if 0) and reason (may be NULL). */
static void send_close(WsConn *w, uint16_t code, const char *reason)
{
	uint8_t p[WS_CONTROL_MAX];
	size_t len = 0;
	if (code != 0) {
		p[0] = (uint8_t)(code >> 8);
		p[1] = (uint8_t)code;
		len = 2;
		size_t rl = reason != NULL ? strlen(reason) : 0;
		if (rl > 0 && rl <= WS_CLOSE_REASON_MAX && ws_utf8_valid(reason, rl)) {
			memcpy(p + 2, reason, rl);
			len += rl;
		}
	}
	w->close_code = code;
	send_frame(w, WS_OP_CLOSE, p, len);
}

/* Starts the server's side of the close handshake. */
static void start_close(WsConn *w, uint16_t code, const char *reason)
{
	send_close(w, code, reason);
	if (w->state == S_CLOSED)
		return; /* the buffer was full */
	w->state = S_CLOSING;
	w->deadline = now_ms(w) + w->srv->cfg.close_timeout_ms;
	w->skip = true; /* the current frame's rest */
	/* Drop the message being received, but keep its buffer (freed on
	 * close): ws_close may be called from on_message, whose text it is. */
	w->in_message = false;
	w->msg_len = 0;
}

/* A protocol error (RFC 6455 §7.1.7): close with code. */
static void fail(WsConn *w, uint16_t code, const char *why)
{
	log_info("ws: %s:%u: %s, closing (%u)", peer_addr(w), peer_port(w), why, (unsigned)code);
	start_close(w, code, why);
}

bool ws_send_text(WsConn *w, const char *text, size_t len)
{
	if (w->state != S_OPEN)
		return false;
	return send_frame(w, WS_OP_TEXT, text, len);
}

size_t ws_send_space(const WsConn *w)
{
	if (w->state != S_OPEN)
		return 0;
	size_t space = loop_conn_space(w->conn);
	if (space < 2)
		return 0;
	if (space - 2 <= 125)
		return space - 2;
	if (space - 4 <= 0xFFFF)
		return space - 4 > 125 ? space - 4 : 125;
	return space - 10;
}

size_t ws_send_pending(const WsConn *w)
{
	if (w->state != S_OPEN)
		return 0;
	return loop_conn_pending(w->conn);
}

void ws_close(WsConn *w, uint16_t code, const char *reason)
{
	if (w->state != S_OPEN)
		return;
	log_info("ws: %s:%u: closing (%u)", peer_addr(w), peer_port(w), (unsigned)code);
	start_close(w, code, reason);
}

bool ws_is_open(const WsConn *w)
{
	return w->state == S_OPEN;
}

const struct pal_peer *ws_peer(const WsConn *w)
{
	return loop_conn_peer(w->conn);
}

void ws_set_user(WsConn *w, void *user)
{
	w->user = user;
}

void *ws_user(const WsConn *w)
{
	return w->user;
}

/* ---- Frames ------------------------------------------------------------------- */

/* Checks a frame header in S_OPEN (RFC 6455 §5.2-5.5); a failure closes and
 * leaves the frame to be skipped. */
static void begin_frame(WsConn *w)
{
	const WsFrameHeader *h = &w->fh;
	w->skip = w->state != S_OPEN;
	w->ctrl_len = 0;
	w->mask_off = 0;
	if (w->skip)
		return;
	if (h->rsv != 0) {
		fail(w, WS_CLOSE_PROTOCOL_ERROR, "reserved bits set");
		return;
	}
	if (!h->masked) {
		fail(w, WS_CLOSE_PROTOCOL_ERROR, "unmasked frame");
		return;
	}
	switch (h->opcode) {
	case WS_OP_CLOSE:
	case WS_OP_PING:
	case WS_OP_PONG:
		if (!h->fin || h->len > WS_CONTROL_MAX)
			fail(w, WS_CLOSE_PROTOCOL_ERROR, "invalid control frame");
		return;
	case WS_OP_CONTINUATION:
		if (!w->in_message) {
			fail(w, WS_CLOSE_PROTOCOL_ERROR, "continuation without a message");
			return;
		}
		break;
	case WS_OP_TEXT:
		if (w->in_message) {
			fail(w, WS_CLOSE_PROTOCOL_ERROR, "new message inside a fragmented one");
			return;
		}
		w->in_message = true;
		w->msg_len = 0;
		ws_utf8_init(&w->utf8);
		break;
	case WS_OP_BINARY:
		if (w->in_message)
			fail(w, WS_CLOSE_PROTOCOL_ERROR, "new message inside a fragmented one");
		else
			fail(w, WS_CLOSE_UNSUPPORTED_DATA, "binary message");
		return;
	default:
		fail(w, WS_CLOSE_PROTOCOL_ERROR, "unknown opcode");
		return;
	}
	if (h->len > w->srv->cfg.max_message_bytes - w->msg_len)
		fail(w, WS_CLOSE_TOO_BIG, "message too big");
}

/* Appends payload bytes of a text frame; false after a failure. */
static bool append_text(WsConn *w, const uint8_t *p, size_t n)
{
	size_t need = w->msg_len + n + 1; /* <= max_message_bytes + 1 (begin_frame) */
	if (need > w->msg_cap) {
		size_t cap = w->msg_cap == 0 ? MSG_INITIAL : w->msg_cap;
		while (cap < need)
			cap *= 2;
		if (cap > w->srv->cfg.max_message_bytes + 1)
			cap = w->srv->cfg.max_message_bytes + 1;
		char *m = realloc(w->msg, cap);
		if (m == NULL) {
			log_warn("ws: %s:%u: out of memory for a message", peer_addr(w),
				 peer_port(w));
			start_close(w, WS_CLOSE_INTERNAL_ERROR, NULL);
			return false;
		}
		w->msg = m;
		w->msg_cap = cap;
	}
	memcpy(w->msg + w->msg_len, p, n);
	w->msg_len += n;
	if (!ws_utf8_feed(&w->utf8, p, n)) {
		fail(w, WS_CLOSE_INVALID_DATA, "invalid UTF-8");
		return false;
	}
	return true;
}

/* n payload bytes of the current frame (masked: unmasked in place). */
static void payload(WsConn *w, uint8_t *p, size_t n)
{
	if (w->skip)
		return;
	for (size_t i = 0; i < n; i++)
		p[i] ^= w->fh.mask[(w->mask_off + i) & 3];
	w->mask_off += n;
	if (w->fh.opcode & 0x8) {
		memcpy(w->ctrl + w->ctrl_len, p, n); /* <= WS_CONTROL_MAX (begin_frame) */
		w->ctrl_len += n;
	} else {
		append_text(w, p, n);
	}
}

/* A close frame from the client (RFC 6455 §5.5.1, §7.1.5). */
static void peer_close(WsConn *w)
{
	uint16_t code = 0;
	if (w->ctrl_len == 1) {
		log_info("ws: %s:%u: invalid close frame", peer_addr(w), peer_port(w));
		code = WS_CLOSE_PROTOCOL_ERROR;
	} else if (w->ctrl_len >= 2) {
		code = (uint16_t)(w->ctrl[0] << 8 | w->ctrl[1]);
		if (!ws_close_code_valid(code)) {
			log_info("ws: %s:%u: invalid close code %u", peer_addr(w), peer_port(w),
				 (unsigned)code);
			code = WS_CLOSE_PROTOCOL_ERROR;
		} else if (!ws_utf8_valid(w->ctrl + 2, w->ctrl_len - 2)) {
			log_info("ws: %s:%u: close reason not UTF-8", peer_addr(w), peer_port(w));
			code = WS_CLOSE_INVALID_DATA;
		}
	}
	send_close(w, code, NULL);
	finish(w);
}

/* The current frame is complete. */
static void end_frame(WsConn *w)
{
	w->in_payload = false;
	if (w->state == S_CLOSING) {
		/* Only the client's close matters now (or a frame we failed on,
		 * if it was a close). */
		if (w->fh.opcode == WS_OP_CLOSE)
			finish(w);
		return;
	}
	if (w->skip || w->state != S_OPEN)
		return;
	WsServer *srv = w->srv;
	switch (w->fh.opcode) {
	case WS_OP_PING:
		send_frame(w, WS_OP_PONG, w->ctrl, w->ctrl_len);
		return;
	case WS_OP_PONG:
		return; /* liveness is any byte (last_rx) */
	case WS_OP_CLOSE:
		peer_close(w);
		return;
	default:
		break;
	}
	if (!w->fh.fin)
		return;
	if (!ws_utf8_complete(&w->utf8)) {
		fail(w, WS_CLOSE_INVALID_DATA, "invalid UTF-8");
		return;
	}
	if (w->msg == NULL && !append_text(w, (const uint8_t *)"", 0))
		return; /* an empty message: a buffer for its NUL */
	w->msg[w->msg_len] = '\0';
	w->in_message = false;
	if (srv->cfg.handler != NULL && srv->cfg.handler->on_message != NULL)
		srv->cfg.handler->on_message(srv->cfg.ctx, w, w->msg, w->msg_len);
	if (w->msg_cap > WS_MSG_KEEP)
		free_message(w);
	w->msg_len = 0;
}

/* Feeds bytes from the client to the frame parser (they are modified:
 * unmasked in place). */
static void feed(WsConn *w, uint8_t *p, size_t n)
{
	while (n > 0 && (w->state == S_OPEN || w->state == S_CLOSING) && !w->garbled) {
		if (!w->in_payload) {
			size_t need = w->hdr_len < 2 ? 2 : ws_frame_header_size(w->hdr);
			size_t take = need - w->hdr_len < n ? need - w->hdr_len : n;
			memcpy(w->hdr + w->hdr_len, p, take);
			w->hdr_len += take;
			p += take;
			n -= take;
			if (w->hdr_len < 2 || w->hdr_len < ws_frame_header_size(w->hdr))
				continue;
			int r = ws_frame_header_parse(w->hdr, w->hdr_len, &w->fh);
			w->hdr_len = 0;
			if (r < 0) {
				if (w->state == S_OPEN)
					fail(w, WS_CLOSE_PROTOCOL_ERROR, "invalid frame length");
				w->garbled = true;
				return;
			}
			w->in_payload = true;
			w->remaining = w->fh.len;
			begin_frame(w);
			if (w->remaining == 0)
				end_frame(w);
			continue;
		}
		size_t take = w->remaining < n ? (size_t)w->remaining : n;
		payload(w, p, take);
		p += take;
		n -= take;
		w->remaining -= take;
		if (w->remaining == 0)
			end_frame(w);
	}
}

/* ---- The handshake ------------------------------------------------------------ */

/* Refuses the upgrade with status and closes; extra: header lines. */
static void refuse(WsConn *w, int status, const char *why, const char *extra)
{
	log_info("ws: upgrade from %s refused: %d (%s)", peer_addr(w), status, why);
	char body[64];
	int blen = snprintf(body, sizeof(body), "%d %s\n", status, http_reason(status));
	char head[WS_HANDSHAKE_OUT_CAP - sizeof(body)];
	int hlen = snprintf(head, sizeof(head),
			    "HTTP/1.1 %d %s\r\n"
			    "Content-Type: text/plain; charset=utf-8\r\n"
			    "Content-Length: %d\r\n"
			    "Cache-Control: no-cache\r\n"
			    "%s"
			    "\r\n",
			    status, http_reason(status), blen,
			    extra != NULL ? extra : "Connection: close\r\n");
	if (loop_conn_write(w->conn, head, (size_t)hlen))
		loop_conn_write(w->conn, body, (size_t)blen);
	finish(w);
}

/* The 426 header lines (RFC 9110 §15.5.22: Upgrade, with Connection
 * listing it; RFC 6455 §4.4: the version this server speaks). */
#define UPGRADE_REQUIRED                                                                           \
	"Upgrade: websocket\r\n"                                                                   \
	"Sec-WebSocket-Version: 13\r\n"                                                            \
	"Connection: Upgrade, close\r\n"

static bool origin_allowed(const WsConn *w, const char *origin)
{
	const WsServer *srv = w->srv;
	uint16_t web_port = srv->cfg.web_port;
	if (web_port == 0 && srv->listener != NULL)
		web_port = (uint16_t)(loop_listener_port(srv->listener) - 1);
	if (ws_origin_is_own(origin, http_request_header(&w->req, "Host"), false, web_port))
		return true;
	for (size_t i = 0; i < srv->cfg.allowed_origin_count; i++)
		if (http_str_ieq(origin, srv->origins[i]))
			return true;
	return false;
}

/* Answers a complete upgrade request (see "Upgrade" in ws_server.h). */
static void upgrade(WsConn *w)
{
	WsServer *srv = w->srv;
	const HttpRequest *r = &w->req;
	if (strcmp(r->method, "GET") != 0 || r->version_minor < 1 || r->has_body ||
	    r->target[0] != '/') {
		refuse(w, 400, "not a WebSocket upgrade request", NULL);
		return;
	}
	if (!http_request_header_has_token(r, "Upgrade", "websocket")) {
		refuse(w, 426, "no Upgrade: websocket", UPGRADE_REQUIRED);
		return;
	}
	if (!http_request_header_has_token(r, "Connection", "Upgrade")) {
		refuse(w, 400, "no Connection: Upgrade", NULL);
		return;
	}
	const char *version = http_request_header(r, "Sec-WebSocket-Version");
	if (http_request_header_count(r, "Sec-WebSocket-Version") != 1 ||
	    strcmp(version, "13") != 0) {
		refuse(w, 426, "unsupported Sec-WebSocket-Version", UPGRADE_REQUIRED);
		return;
	}
	char accept[WS_ACCEPT_LEN + 1];
	if (http_request_header_count(r, "Sec-WebSocket-Key") != 1 ||
	    !ws_accept_key(http_request_header(r, "Sec-WebSocket-Key"), accept)) {
		refuse(w, 400, "invalid Sec-WebSocket-Key", NULL);
		return;
	}
	const char *origin = http_request_header(r, "Origin");
	if (http_request_header_count(r, "Origin") != 1) {
		refuse(w, 403, origin == NULL ? "no Origin" : "several Origin fields", NULL);
		return;
	}
	if (!origin_allowed(w, origin)) {
		char why[160];
		snprintf(why, sizeof(why), "Origin %.100s not allowed", origin);
		refuse(w, 403, why, NULL);
		return;
	}
	if (srv->open_count >= srv->cfg.max_connections) {
		refuse(w, 503, "connection limit reached", NULL);
		return;
	}
	if (!loop_conn_set_out_cap(w->conn, srv->cfg.out_cap)) {
		log_warn("ws: out of memory for a connection from %s", peer_addr(w));
		refuse(w, 503, "out of memory", NULL);
		return;
	}
	char resp[160];
	int len = snprintf(resp, sizeof(resp),
			   "HTTP/1.1 101 Switching Protocols\r\n"
			   "Upgrade: websocket\r\n"
			   "Connection: Upgrade\r\n"
			   "Sec-WebSocket-Accept: %s\r\n"
			   "\r\n",
			   accept);
	if (!loop_conn_write(w->conn, resp, (size_t)len)) {
		w->state = S_CLOSED;
		return;
	}
	w->state = S_OPEN;
	w->counted = true;
	srv->open_count++;
	w->last_rx = now_ms(w);
	log_info("ws: connection from %s:%u open (%u of %u)", peer_addr(w), peer_port(w),
		 (unsigned)srv->open_count, (unsigned)srv->cfg.max_connections);
	w->opened = true;
	if (srv->cfg.handler != NULL && srv->cfg.handler->on_open != NULL)
		srv->cfg.handler->on_open(srv->cfg.ctx, w);

	/* Frames sent right behind the request are in the head buffer. */
	size_t left;
	const char *rest = http_request_leftover(r, &left);
	if (left > 0)
		feed(w, (uint8_t *)w->head + (rest - w->head), left);
	free(w->head);
	w->head = NULL;
}

/* ---- Connection callbacks ----------------------------------------------------- */

static void on_readable(void *ctx, LoopConn *c)
{
	WsConn *w = ctx;
	if (w->state == S_HANDSHAKE) {
		for (;;) {
			char *dst;
			size_t cap = http_request_space(&w->req, &dst);
			size_t n;
			if (cap == 0 || loop_conn_read(c, dst, cap, &n) != PAL_OK)
				return;
			enum http_parse res = http_request_commit(&w->req, n);
			if (res == HTTP_PARSE_COMPLETE) {
				upgrade(w);
				break; /* read what follows as frames */
			}
			if (res != HTTP_PARSE_INCOMPLETE) {
				refuse(w, http_parse_status(res), "malformed request", NULL);
				return;
			}
		}
	}
	size_t total = 0, n;
	while (total < WS_READ_BUDGET && (w->state == S_OPEN || w->state == S_CLOSING)) {
		if (loop_conn_read(c, w->srv->chunk, sizeof(w->srv->chunk), &n) != PAL_OK)
			return;
		total += n;
		w->last_rx = now_ms(w);
		w->ping_out = false;
		feed(w, w->srv->chunk, n);
	}
}

static void unlink_conn(WsConn *w)
{
	if (w->prev != NULL)
		w->prev->next = w->next;
	else
		w->srv->conns = w->next;
	if (w->next != NULL)
		w->next->prev = w->prev;
}

static void on_closed(void *ctx, LoopConn *c, enum loop_close_reason reason)
{
	(void)c;
	WsConn *w = ctx;
	WsServer *srv = w->srv;
	if (w->counted)
		srv->open_count--;
	if (w->opened) {
		if (w->close_code != 0)
			log_info("ws: connection from %s:%u closed: %s (%u)", peer_addr(w),
				 peer_port(w), loop_close_reason_str(reason),
				 (unsigned)w->close_code);
		else
			log_info("ws: connection from %s:%u closed: %s", peer_addr(w),
				 peer_port(w), loop_close_reason_str(reason));
		w->state = S_CLOSED;
		if (srv->cfg.handler != NULL && srv->cfg.handler->on_close != NULL)
			srv->cfg.handler->on_close(srv->cfg.ctx, w);
	}
	unlink_conn(w);
	free(w->head);
	free(w->msg);
	free(w);
}

static void on_drained(void *ctx, LoopConn *c)
{
	(void)c;
	WsConn *w = ctx;
	const WsHandler *h = w->srv->cfg.handler;
	if (w->state == S_OPEN && h != NULL && h->on_drained != NULL)
		h->on_drained(w->srv->cfg.ctx, w);
}

static const LoopConnHandler conn_handler = {
	.on_readable = on_readable,
	.on_drained = on_drained,
	.on_closed = on_closed,
};

static bool on_accept(void *ctx, LoopConn *c)
{
	WsServer *srv = ctx;
	WsConn *w = calloc(1, sizeof(*w));
	char *head = malloc(srv->cfg.head_max);
	if (w == NULL || head == NULL) {
		log_warn("ws: out of memory for a connection from %s", loop_conn_peer(c)->addr);
		free(w);
		free(head);
		return false;
	}
	w->srv = srv;
	w->conn = c;
	w->head = head;
	w->state = S_HANDSHAKE;
	w->deadline = loop_now_ms(srv->loop) + srv->cfg.handshake_timeout_ms;
	http_request_init(&w->req, head, srv->cfg.head_max);
	w->next = srv->conns;
	if (srv->conns != NULL)
		srv->conns->prev = w;
	srv->conns = w;
	loop_conn_set_handler(c, &conn_handler, w);
	return true;
}

static void on_listener_closed(void *ctx, LoopListener *l)
{
	(void)l;
	WsServer *srv = ctx;
	srv->listener = NULL;
}

static uint64_t until(uint64_t due, uint64_t now)
{
	return due > now ? due - now : 0;
}

/* The tick hook: handshake, liveness and close deadlines (see "Liveness"). */
static uint64_t tick(void *ctx, uint64_t now)
{
	WsServer *srv = ctx;
	const WsServerConfig *cfg = &srv->cfg;
	uint64_t next = LOOP_NO_DEADLINE;
	for (WsConn *w = srv->conns; w != NULL; w = w->next) {
		uint64_t due = LOOP_NO_DEADLINE;
		switch (w->state) {
		case S_HANDSHAKE:
			if (now < w->deadline) {
				due = w->deadline;
			} else if (w->req.len > 0) {
				refuse(w, 408, "handshake timeout", NULL);
			} else {
				log_fine("ws: connection from %s: no request, closing",
					 peer_addr(w));
				w->state = S_CLOSED;
				loop_conn_abort(w->conn);
			}
			break;
		case S_OPEN:
			if (w->ping_out) {
				due = w->ping_at + cfg->pong_timeout_ms;
				if (now >= due) {
					log_info("ws: %s:%u: no answer to a ping, closing",
						 peer_addr(w), peer_port(w));
					w->state = S_CLOSED;
					loop_conn_abort(w->conn);
					due = LOOP_NO_DEADLINE;
				}
			} else if (cfg->ping_interval_ms > 0) {
				due = w->last_rx + cfg->ping_interval_ms;
				if (now >= due && send_frame(w, WS_OP_PING, "", 0)) {
					w->ping_out = true;
					w->ping_at = now;
					due = now + cfg->pong_timeout_ms;
				} else if (now >= due) {
					due = LOOP_NO_DEADLINE; /* the write failed: closing */
				}
			}
			break;
		case S_CLOSING:
			if (now >= w->deadline) {
				log_fine("ws: %s:%u: no close frame from the client",
					 peer_addr(w), peer_port(w));
				finish(w);
			} else {
				due = w->deadline;
			}
			break;
		case S_CLOSED:
			break;
		}
		if (due != LOOP_NO_DEADLINE && until(due, now) < next)
			next = until(due, now);
	}
	return next;
}

/* ---- The server ---------------------------------------------------------------- */

void ws_server_config_defaults(WsServerConfig *cfg, const char *host, uint16_t port)
{
	*cfg = (WsServerConfig){
		.host = host,
		.port = port,
		.max_connections = CONFIG_DEFAULT_MAX_CONNECTIONS,
		.max_message_bytes = CONFIG_DEFAULT_MAX_MESSAGE_BYTES,
		.out_cap = WS_OUT_CAP,
		.head_max = HTTP_HEAD_MAX,
		.handshake_timeout_ms = WS_HANDSHAKE_TIMEOUT_MS,
		.ping_interval_ms = WS_PING_INTERVAL_MS,
		.pong_timeout_ms = WS_PONG_TIMEOUT_MS,
		.close_timeout_ms = WS_CLOSE_TIMEOUT_MS,
	};
}

/* Copies the allowed origins into one allocation: the pointers, then the
 * strings. */
static bool copy_origins(WsServer *srv, const WsServerConfig *cfg)
{
	size_t n = cfg->allowed_origin_count, bytes = n * sizeof(char *);
	if (n == 0)
		return true;
	for (size_t i = 0; i < n; i++)
		bytes += strlen(cfg->allowed_origins[i]) + 1;
	char **o = malloc(bytes);
	if (o == NULL)
		return false;
	char *p = (char *)(o + n);
	for (size_t i = 0; i < n; i++) {
		size_t len = strlen(cfg->allowed_origins[i]) + 1;
		memcpy(p, cfg->allowed_origins[i], len);
		o[i] = p;
		p += len;
	}
	srv->origins = o;
	return true;
}

enum pal_err ws_server_open(Loop *loop, const WsServerConfig *cfg, WsServer **out)
{
	*out = NULL;
	if (cfg == NULL || cfg->host == NULL || cfg->max_connections == 0 ||
	    cfg->max_message_bytes == 0 || cfg->max_message_bytes >= SIZE_MAX / 2 ||
	    cfg->out_cap < WS_OUT_CAP_MIN || cfg->head_max == 0 ||
	    (cfg->allowed_origin_count > 0 && cfg->allowed_origins == NULL))
		return PAL_ERR_INVALID;
	WsServer *srv = calloc(1, sizeof(*srv));
	if (srv == NULL)
		return PAL_ERR_UNAVAILABLE;
	srv->loop = loop;
	srv->cfg = *cfg;
	if (!copy_origins(srv, cfg)) {
		free(srv);
		return PAL_ERR_UNAVAILABLE;
	}
	srv->cfg.allowed_origins = (const char *const *)srv->origins;
	if (!loop_add_tick(loop, tick, srv)) {
		log_error("ws: no tick hook left");
		free(srv->origins);
		free(srv);
		return PAL_ERR_UNAVAILABLE;
	}
	uint32_t hard = cfg->max_connections + WS_PENDING_MAX;
	LoopListenerConfig lc = {
		.host = cfg->host,
		.port = cfg->port,
		.max_conns = hard < cfg->max_connections ? cfg->max_connections : hard,
		.out_cap = WS_HANDSHAKE_OUT_CAP,
		.on_accept = on_accept,
		.on_closed = on_listener_closed,
		.ctx = srv,
	};
	enum pal_err err = loop_listen(loop, &lc, &srv->listener);
	if (err != PAL_OK) {
		loop_remove_tick(loop, tick, srv);
		free(srv->origins);
		free(srv);
		return err;
	}
	log_info("ws: FBP WebSocket on port %u", (unsigned)loop_listener_port(srv->listener));
	*out = srv;
	return PAL_OK;
}

enum pal_err ws_server_open_config(Loop *loop, const Config *cfg, const WsHandler *handler,
				   void *ctx, WsServer **out)
{
	*out = NULL;
	if (cfg->tls.enabled) {
		log_error("ws: TLS (tls.enabled) is not supported yet");
		return PAL_ERR_UNAVAILABLE;
	}
	if (cfg->port < CONFIG_PORT_MIN || cfg->port > CONFIG_PORT_MAX)
		return PAL_ERR_INVALID;
	const char *origins[HAKKAS_CONFIG_ORIGINS_MAX];
	size_t n = cfg->allowed_origin_count;
	if (n > HAKKAS_CONFIG_ORIGINS_MAX)
		return PAL_ERR_INVALID;
	for (size_t i = 0; i < n; i++)
		origins[i] = cfg->allowed_origins[i];
	WsServerConfig wc;
	ws_server_config_defaults(&wc, cfg->host, (uint16_t)(cfg->port + 1));
	wc.web_port = (uint16_t)cfg->port;
	wc.max_connections = cfg->max_connections;
	wc.max_message_bytes = cfg->max_message_bytes;
	wc.allowed_origins = origins;
	wc.allowed_origin_count = n;
	wc.handler = handler;
	wc.ctx = ctx;
	return ws_server_open(loop, &wc, out);
}

void ws_server_close(WsServer *srv)
{
	if (srv == NULL)
		return;
	if (srv->listener != NULL)
		loop_listener_close(srv->listener); /* closes its connections too */
	loop_remove_tick(srv->loop, tick, srv);
	free(srv->origins);
	free(srv);
}

uint16_t ws_server_port(const WsServer *srv)
{
	return srv->listener != NULL ? loop_listener_port(srv->listener) : 0;
}

uint32_t ws_server_conn_count(const WsServer *srv)
{
	return srv->open_count;
}
