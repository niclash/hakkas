/*
 * The WebSocket layer: the pure pieces of protocol/websocket.h, and the
 * server (engine/ws_server.h, 02 §0) over the engine loop and the POSIX PAL
 * with real loopback connections from a small test client that masks its
 * frames. Waiting is always for a condition, with a generous deadline; a
 * tick hook bounds every poll to 10 ms.
 */
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "engine/ws_server.h"
#include "protocol/websocket.h"

#define WAIT_MS 3000
#define WEB_PORT 8080 /* the Origin's port the tests' servers expect */
#define RFC_KEY "dGhlIHNhbXBsZSBub25jZQ=="
#define RFC_ACCEPT "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="

static Runtime rt;
static Loop loop;
static WsServer *server;

/* ---- Unit: protocol/websocket.h ------------------------------------------------ */

static void test_accept_key(void)
{
	char out[WS_ACCEPT_LEN + 1];
	CHECK(ws_accept_key(RFC_KEY, out)); /* RFC 6455 §1.3 */
	CHECK_STREQ(out, RFC_ACCEPT);
	CHECK(!ws_accept_key("", out));
	CHECK(!ws_accept_key("dGhlIHNhbXBsZSBub25jZQ=", out));	 /* 23 characters */
	CHECK(!ws_accept_key("dGhlIHNhbXBsZSBub25jZQ===", out)); /* 25 */
	CHECK(!ws_accept_key("dGhlIHNhbXBsZSBub25jZS4=", out)); /* 17 bytes */
	CHECK(!ws_accept_key("dGhlIHNhbXBsZSBub25jZ!==", out)); /* not base64 */
	CHECK(!ws_accept_key("dGhlIHNhbXBsZSBub25jZQ== ", out));
}

static void test_close_codes(void)
{
	CHECK(ws_close_code_valid(1000) && ws_close_code_valid(1003));
	CHECK(ws_close_code_valid(1007) && ws_close_code_valid(1014));
	CHECK(ws_close_code_valid(3000) && ws_close_code_valid(4999));
	CHECK(!ws_close_code_valid(0) && !ws_close_code_valid(999));
	CHECK(!ws_close_code_valid(1004) && !ws_close_code_valid(1005));
	CHECK(!ws_close_code_valid(1006) && !ws_close_code_valid(1015));
	CHECK(!ws_close_code_valid(2999) && !ws_close_code_valid(5000));
}

static void test_frame_header(void)
{
	uint8_t h[WS_FRAME_HEADER_MAX];
	WsFrameHeader fh;
	CHECK(ws_frame_header_write(h, WS_OP_TEXT, 5) == 2);
	CHECK(h[0] == 0x81 && h[1] == 5);
	CHECK(ws_frame_header_write(h, WS_OP_TEXT, 125) == 2);
	CHECK(ws_frame_header_write(h, WS_OP_PONG, 126) == 4);
	CHECK(h[0] == 0x8A && h[1] == 126 && h[2] == 0 && h[3] == 126);
	CHECK(ws_frame_header_write(h, WS_OP_TEXT, 65535) == 4);
	CHECK(ws_frame_header_write(h, WS_OP_TEXT, 65536) == 10);
	CHECK(h[1] == 127 && h[7] == 1 && h[8] == 0 && h[9] == 0);
	CHECK(ws_frame_header_parse(h, 9, &fh) == 0);
	CHECK(ws_frame_header_parse(h, 10, &fh) == 10);
	CHECK(fh.fin && fh.opcode == WS_OP_TEXT && !fh.masked && fh.len == 65536 && fh.rsv == 0);

	const uint8_t masked[] = {0x01, 0xFE, 0x01, 0x00, 1, 2, 3, 4};
	CHECK(ws_frame_header_size(masked) == 8);
	CHECK(ws_frame_header_parse(masked, 7, &fh) == 0);
	CHECK(ws_frame_header_parse(masked, 8, &fh) == 8);
	CHECK(!fh.fin && fh.opcode == 1 && fh.masked && fh.len == 256 && fh.mask[3] == 4);
	const uint8_t rsv[] = {0xF2, 0x00};
	CHECK(ws_frame_header_parse(rsv, 2, &fh) == 2);
	CHECK(fh.rsv == 7 && fh.opcode == 2 && fh.len == 0);
	const uint8_t huge[] = {0x81, 0x7F, 0x80, 0, 0, 0, 0, 0, 0, 0};
	CHECK(ws_frame_header_parse(huge, sizeof(huge), &fh) == -1);
}

static bool utf8(const char *s)
{
	return ws_utf8_valid(s, strlen(s));
}

static void test_utf8(void)
{
	CHECK(utf8(""));
	CHECK(utf8("plain ASCII {\"a\":1}"));
	CHECK(utf8("\xC2\x80 \xDF\xBF \xE0\xA0\x80 \xED\x9F\xBF \xEE\x80\x80 \xEF\xBF\xBF"));
	CHECK(utf8("\xF0\x90\x80\x80 \xF4\x8F\xBF\xBF \xE2\x82\xAC \xC3\xA9"));
	CHECK(ws_utf8_valid("a\0b", 3)); /* U+0000 */
	/* Overlong forms. */
	CHECK(!utf8("\xC0\x80"));
	CHECK(!utf8("\xC1\xBF"));
	CHECK(!utf8("\xE0\x80\xAF"));
	CHECK(!utf8("\xE0\x9F\xBF"));
	CHECK(!utf8("\xF0\x80\x80\xAF"));
	CHECK(!utf8("\xF0\x8F\xBF\xBF"));
	/* Surrogates. */
	CHECK(!utf8("\xED\xA0\x80"));
	CHECK(!utf8("\xED\xBF\xBF"));
	/* Beyond U+10FFFF, invalid bytes. */
	CHECK(!utf8("\xF4\x90\x80\x80"));
	CHECK(!utf8("\xF5\x80\x80\x80"));
	CHECK(!utf8("\xFE"));
	CHECK(!utf8("\xFF"));
	/* Stray or missing continuation bytes. */
	CHECK(!utf8("\x80"));
	CHECK(!utf8("a\xBF"));
	CHECK(!utf8("\xC3"));
	CHECK(!utf8("\xE2\x82"));
	CHECK(!utf8("\xE2\x82x"));
	CHECK(!utf8("\xF0\x90\x80"));

	/* Any split, down to single bytes. */
	const char *s = "x\xF0\x9F\x98\x80y\xE2\x82\xAC";
	WsUtf8 u;
	ws_utf8_init(&u);
	for (size_t i = 0; s[i] != '\0'; i++) {
		CHECK(ws_utf8_feed(&u, (const uint8_t *)s + i, 1));
		bool boundary = s[i + 1] == '\0' || s[i + 1] == 'y';
		if (i == 0 || boundary)
			CHECK(ws_utf8_complete(&u));
		else if (i < 4)
			CHECK(!ws_utf8_complete(&u));
	}
	ws_utf8_init(&u);
	CHECK(ws_utf8_feed(&u, (const uint8_t *)"\xED", 1));
	CHECK(!ws_utf8_feed(&u, (const uint8_t *)"\xA0", 1));
	CHECK(!ws_utf8_feed(&u, (const uint8_t *)"a", 1)); /* stays invalid */
	CHECK(!ws_utf8_complete(&u));
}

static void test_origin(void)
{
	CHECK(ws_origin_is_own("http://127.0.0.1:8080", "127.0.0.1:8081", false, 8080));
	CHECK(ws_origin_is_own("HTTP://Device.LAN:8080", "device.lan:8081", false, 8080));
	CHECK(ws_origin_is_own("http://device:8080", "device", false, 8080));
	CHECK(ws_origin_is_own("http://[::1]:8080", "[::1]:8081", false, 8080));
	CHECK(ws_origin_is_own("http://[FE80::1]:8080", "[fe80::1]:8081", false, 8080));
	CHECK(ws_origin_is_own("http://device", "device:81", false, 80)); /* default port */
	CHECK(ws_origin_is_own("http://device:80", "device:81", false, 80));
	CHECK(ws_origin_is_own("https://device", "device:444", true, 443));
	CHECK(ws_origin_is_own("https://device:8443", "device:8444", true, 8443));
	/* Mismatches. */
	CHECK(!ws_origin_is_own("http://evil:8080", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://device:8081", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://device", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("https://device:8080", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://device:8080", "device:8081", true, 8080));
	CHECK(!ws_origin_is_own("http://device.evil:8080", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://devic:8080", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://[::1]:8080", "[::2]:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://::1:8080", "[::1]:8081", false, 8080));
	/* Malformed. */
	CHECK(!ws_origin_is_own("null", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("device:8080", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://device:8080/", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://device:8080/x", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://u@device:8080", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://device:", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://device:08080x", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://device:99999", "device:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://:8080", ":8081", false, 8080));
	CHECK(!ws_origin_is_own("http://[::1:8080", "[::1:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://[::1]x:8080", "[::1]:8081", false, 8080));
	CHECK(!ws_origin_is_own("http://device:8080", "", false, 8080));
	CHECK(!ws_origin_is_own(NULL, "device", false, 8080));
	CHECK(!ws_origin_is_own("http://device:8080", NULL, false, 8080));
}

/* ---- The handler ---------------------------------------------------------------- */

static struct {
	int opened, closed, messages, drained;
	WsConn *conn; /* the last opened */
	bool bad_user, bad_nul;
	char last[256];
} ev;

static int user_tag;

static void h_open(void *ctx, WsConn *conn)
{
	CHECK(ctx == &ev);
	CHECK(ws_is_open(conn));
	CHECK(ws_user(conn) == NULL);
	ws_set_user(conn, &user_tag);
	ev.opened++;
	ev.conn = conn;
}

/* Echoes every message, except "close-me" (closes with 1000 "done") and
 * "quiet" (no answer). */
static void h_message(void *ctx, WsConn *conn, const char *text, size_t len)
{
	(void)ctx;
	ev.messages++;
	if (ws_user(conn) != &user_tag)
		ev.bad_user = true;
	if (text[len] != '\0')
		ev.bad_nul = true;
	snprintf(ev.last, sizeof(ev.last), "%.*s", (int)(len < 255 ? len : 255), text);
	if (len == 8 && memcmp(text, "close-me", 8) == 0) {
		ws_close(conn, WS_CLOSE_NORMAL, "done");
		CHECK(memcmp(text, "close-me", 8) == 0); /* text stays valid */
		CHECK(!ws_is_open(conn));
		CHECK(!ws_send_text(conn, "x", 1));
		return;
	}
	if (len == 5 && memcmp(text, "quiet", 5) == 0)
		return;
	CHECK(ws_send_text(conn, text, len));
}

static void h_close(void *ctx, WsConn *conn)
{
	(void)ctx;
	if (ws_user(conn) != &user_tag)
		ev.bad_user = true;
	CHECK(!ws_is_open(conn));
	ev.closed++;
	if (ev.conn == conn)
		ev.conn = NULL;
}

static void h_drained(void *ctx, WsConn *conn)
{
	(void)ctx;
	CHECK(ws_is_open(conn));
	ev.drained++;
}

static const WsHandler handler = {h_open, h_message, h_close, h_drained};

/* ---- Loop ------------------------------------------------------------------- */

static uint64_t short_tick(void *ctx, uint64_t now_ms)
{
	(void)ctx;
	(void)now_ms;
	return 10;
}

static void setup_with(void (*cfg_fn)(WsServerConfig *))
{
	memset(&ev, 0, sizeof(ev));
	runtime_init(&rt);
	CHECK(loop_init(&loop, &rt) == PAL_OK);
	CHECK(loop_add_tick(&loop, short_tick, NULL));
	WsServerConfig cfg;
	ws_server_config_defaults(&cfg, "127.0.0.1", 0);
	cfg.web_port = WEB_PORT;
	cfg.handler = &handler;
	cfg.ctx = &ev;
	if (cfg_fn != NULL)
		cfg_fn(&cfg);
	CHECK(ws_server_open(&loop, &cfg, &server) == PAL_OK);
	CHECK(server != NULL && ws_server_port(server) != 0);
}

static void setup(void)
{
	setup_with(NULL);
}

static void teardown(void)
{
	ws_server_close(server);
	server = NULL;
	loop_deinit(&loop);
	runtime_deinit(&rt);
	CHECK(!ev.bad_user && !ev.bad_nul);
	CHECK(ev.opened == ev.closed);
}

typedef bool (*pred_fn)(void *arg);

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

static bool closed_is(void *arg)
{
	return ev.closed == *(int *)arg;
}

static bool conns_are(void *arg)
{
	return loop.conn_count == *(size_t *)arg;
}

static void wait_closed(int n)
{
	CHECK(run_until(closed_is, &n));
}

/* ---- Client ----------------------------------------------------------------- */

typedef struct {
	int fd;
	uint8_t *buf;
	size_t len, cap;
	bool eof;
	uint8_t mask_seed;
} Client;

static Client client_connect_port(uint16_t port)
{
	Client cl = {0};
	cl.fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	CHECK(cl.fd >= 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(connect(cl.fd, (struct sockaddr *)&a, sizeof(a)) == 0);
	CHECK(fcntl(cl.fd, F_SETFL, O_NONBLOCK) == 0);
	return cl;
}

static Client client_connect(void)
{
	return client_connect_port(ws_server_port(server));
}

static void client_close(Client *cl)
{
	close(cl->fd);
	free(cl->buf);
	memset(cl, 0, sizeof(*cl));
}

static void client_send_raw(Client *cl, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len > 0) {
		ssize_t n = send(cl->fd, p, len, MSG_NOSIGNAL);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			loop_run_once(&loop); /* let the server read */
			continue;
		}
		if (n < 0 && (errno == EPIPE || errno == ECONNRESET))
			return; /* the server closed already (e.g. after its close) */
		CHECK(n > 0);
		if (n <= 0)
			return;
		p += n;
		len -= (size_t)n;
	}
}

static void client_poll(Client *cl)
{
	for (;;) {
		if (cl->cap - cl->len < 65536) {
			cl->cap = cl->cap * 2 + 65536;
			cl->buf = realloc(cl->buf, cl->cap);
			CHECK(cl->buf != NULL);
		}
		ssize_t n = recv(cl->fd, cl->buf + cl->len, cl->cap - cl->len, 0);
		if (n > 0) {
			cl->len += (size_t)n;
			continue;
		}
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			cl->eof = true;
		return;
	}
}

static bool client_eof(void *arg)
{
	Client *cl = arg;
	client_poll(cl);
	return cl->eof;
}

static void consume(Client *cl, size_t n)
{
	memmove(cl->buf, cl->buf + n, cl->len - n);
	cl->len -= n;
}

/* Encodes a client frame (masked unless !masked) into out; returns its size.
 * b0 is the first byte as sent (FIN, RSV, opcode). */
static size_t encode(uint8_t *out, uint8_t b0, const void *data, size_t len, bool masked,
		     uint8_t seed)
{
	size_t p = 0;
	out[p++] = b0;
	uint8_t mbit = masked ? 0x80 : 0;
	if (len < 126) {
		out[p++] = (uint8_t)(mbit | len);
	} else if (len <= 0xFFFF) {
		out[p++] = mbit | 126;
		out[p++] = (uint8_t)(len >> 8);
		out[p++] = (uint8_t)len;
	} else {
		out[p++] = mbit | 127;
		for (int i = 0; i < 8; i++)
			out[p++] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
	}
	uint8_t key[4] = {(uint8_t)(0x37 + seed), (uint8_t)(0xA5 ^ seed), 0x00,
			  (uint8_t)(seed * 3)};
	if (masked) {
		memcpy(out + p, key, 4);
		p += 4;
	}
	const uint8_t *d = data;
	for (size_t i = 0; i < len; i++)
		out[p + i] = masked ? (uint8_t)(d[i] ^ key[i & 3]) : d[i];
	return p + len;
}

static void send_frame_b0(Client *cl, uint8_t b0, const void *data, size_t len, bool masked)
{
	uint8_t *f = malloc(len + WS_FRAME_HEADER_MAX);
	CHECK(f != NULL);
	size_t n = encode(f, b0, data, len, masked, cl->mask_seed++);
	client_send_raw(cl, f, n);
	free(f);
}

static void send_frame(Client *cl, bool fin, int op, const void *data, size_t len)
{
	send_frame_b0(cl, (uint8_t)((fin ? 0x80 : 0) | op), data, len, true);
}

static void send_text(Client *cl, const char *text)
{
	send_frame(cl, true, WS_OP_TEXT, text, strlen(text));
}

static void send_close(Client *cl, int code, const char *reason)
{
	uint8_t p[WS_CONTROL_MAX];
	size_t len = 0;
	if (code >= 0) {
		p[0] = (uint8_t)(code >> 8);
		p[1] = (uint8_t)code;
		len = 2 + strlen(reason);
		memcpy(p + 2, reason, strlen(reason));
	}
	send_frame(cl, true, WS_OP_CLOSE, p, len);
}

/* ---- Responses and frames --------------------------------------------------- */

typedef struct {
	int status;
	char head[2048];
} Resp;

typedef struct {
	Client *cl;
	Resp *r;
} WantResp;

static bool take_head(void *arg)
{
	WantResp *w = arg;
	Client *cl = w->cl;
	client_poll(cl);
	for (size_t i = 0; i + 4 <= cl->len; i++) {
		if (memcmp(cl->buf + i, "\r\n\r\n", 4) == 0) {
			size_t hl = i + 4;
			CHECK(hl < sizeof(w->r->head));
			if (hl >= sizeof(w->r->head))
				return true;
			memcpy(w->r->head, cl->buf, hl);
			w->r->head[hl] = '\0';
			w->r->status = 0;
			sscanf(w->r->head, "HTTP/1.1 %d", &w->r->status);
			consume(cl, hl);
			return true;
		}
	}
	return false;
}

static Resp get_head(Client *cl)
{
	Resp r = {0};
	WantResp w = {cl, &r};
	CHECK(run_until(take_head, &w));
	return r;
}

static const char *header(const Resp *r, const char *name)
{
	static char value[256];
	char pat[128];
	snprintf(pat, sizeof(pat), "\r\n%s: ", name);
	const char *p = strstr(r->head, pat);
	if (p == NULL)
		return NULL;
	p += strlen(pat);
	size_t n = strcspn(p, "\r");
	if (n >= sizeof(value))
		n = sizeof(value) - 1;
	memcpy(value, p, n);
	value[n] = '\0';
	return value;
}

typedef struct {
	int op;
	bool fin;
	uint8_t *data; /* malloc'd, NUL-terminated */
	size_t len;
} Frame;

typedef struct {
	Client *cl;
	Frame *f;
} WantFrame;

static bool take_frame(void *arg)
{
	WantFrame *w = arg;
	Client *cl = w->cl;
	client_poll(cl);
	if (cl->len < 2)
		return false;
	WsFrameHeader h;
	int hl = ws_frame_header_parse(cl->buf, cl->len, &h);
	CHECK(hl >= 0);
	if (hl <= 0 || cl->len - (size_t)hl < h.len)
		return false;
	CHECK(!h.masked && h.rsv == 0); /* server frames are unmasked */
	w->f->op = h.opcode;
	w->f->fin = h.fin;
	w->f->len = (size_t)h.len;
	w->f->data = malloc(h.len + 1);
	memcpy(w->f->data, cl->buf + hl, h.len);
	w->f->data[h.len] = '\0';
	consume(cl, (size_t)hl + (size_t)h.len);
	return true;
}

static Frame get_frame(Client *cl)
{
	Frame f = {-1, false, NULL, 0};
	WantFrame w = {cl, &f};
	CHECK(run_until(take_frame, &w));
	return f;
}

/* The next frame is a text message equal to text. */
static void expect_text(Client *cl, const char *text, size_t len)
{
	Frame f = get_frame(cl);
	CHECK(f.op == WS_OP_TEXT && f.fin);
	CHECK(f.len == len && f.data != NULL && memcmp(f.data, text, len) == 0);
	free(f.data);
}

/* The next frame is a close with code (-1: no payload). */
static void expect_close_frame(Client *cl, int code)
{
	Frame f = get_frame(cl);
	CHECK(f.op == WS_OP_CLOSE);
	if (code < 0) {
		CHECK(f.len == 0);
	} else {
		CHECK(f.len >= 2);
		if (f.len >= 2) {
			int got = f.data[0] << 8 | f.data[1];
			if (got != code)
				fprintf(stderr, "close code %d, expected %d\n", got, code);
			CHECK(got == code);
		}
	}
	free(f.data);
}

/* The next frame is a close with code (-1: no payload); then the client
 * answers it (unless !answer) and the server closes the socket. */
static void expect_close_answer(Client *cl, int code, bool answer)
{
	expect_close_frame(cl, code);
	if (answer)
		send_close(cl, code < 0 ? WS_CLOSE_NORMAL : code, "");
	CHECK(run_until(client_eof, cl));
}

static void expect_close(Client *cl, int code)
{
	expect_close_answer(cl, code, true);
}

/* ---- Handshake -------------------------------------------------------------- */

/* Whether line's field name is in omit, a comma-separated list. */
static bool omitted(const char *omit, const char *line)
{
	size_t nl = strcspn(line, ":");
	for (const char *p = omit; p != NULL && *p != '\0';) {
		size_t il = strcspn(p, ",");
		if (il == nl && strncmp(p, line, nl) == 0)
			return true;
		p += il;
		if (*p == ',')
			p++;
	}
	return false;
}

/* Writes an upgrade request: the standard fields minus omit (field names,
 * comma-separated), plus extra lines; first is the request line (NULL:
 * "GET / HTTP/1.1"). */
static void format_upgrade(char *out, size_t cap, const char *first, const char *omit,
			   const char *extra)
{
	char lines[6][128];
	snprintf(lines[0], sizeof(lines[0]), "Host: 127.0.0.1:%u", ws_server_port(server));
	snprintf(lines[1], sizeof(lines[1]), "Upgrade: websocket");
	snprintf(lines[2], sizeof(lines[2]), "Connection: Upgrade");
	snprintf(lines[3], sizeof(lines[3]), "Sec-WebSocket-Key: " RFC_KEY);
	snprintf(lines[4], sizeof(lines[4]), "Sec-WebSocket-Version: 13");
	snprintf(lines[5], sizeof(lines[5]), "Origin: http://127.0.0.1:%u", WEB_PORT);
	size_t n = (size_t)snprintf(out, cap, "%s\r\n", first != NULL ? first : "GET / HTTP/1.1");
	for (int i = 0; i < 6; i++) {
		if (omitted(omit, lines[i]))
			continue;
		n += (size_t)snprintf(out + n, cap - n, "%s\r\n", lines[i]);
	}
	snprintf(out + n, cap - n, "%s\r\n", extra != NULL ? extra : "");
}

/* An upgraded connection. */
static Client ws_connect(void)
{
	Client cl = client_connect();
	char req[1024];
	format_upgrade(req, sizeof(req), NULL, NULL, NULL);
	client_send_raw(&cl, req, strlen(req));
	Resp r = get_head(&cl);
	CHECK(r.status == 101);
	return cl;
}

/* An upgrade attempt that must be refused with status; returns the head. */
static Resp refused(const char *first, const char *omit, const char *extra, int status)
{
	int opened = ev.opened;
	Client cl = client_connect();
	char req[1024];
	format_upgrade(req, sizeof(req), first, omit, extra);
	client_send_raw(&cl, req, strlen(req));
	Resp r = get_head(&cl);
	if (r.status != status)
		fprintf(stderr, "status %d, expected %d (omit %s, extra %s)\n", r.status, status,
			omit ? omit : "-", extra ? extra : "-");
	CHECK(r.status == status);
	CHECK(run_until(client_eof, &cl)); /* and closed */
	CHECK(ev.opened == opened);
	client_close(&cl);
	return r;
}

static void test_handshake(void)
{
	setup();
	Client cl = client_connect();
	char req[1024];
	format_upgrade(req, sizeof(req), "GET /any/path?x=1 HTTP/1.1", NULL,
		       "Sec-WebSocket-Protocol: noflo\r\nSec-WebSocket-Extensions: "
		       "permessage-deflate\r\n");
	client_send_raw(&cl, req, strlen(req));
	Resp r = get_head(&cl);
	CHECK(r.status == 101);
	CHECK_STREQ(header(&r, "Upgrade"), "websocket");
	CHECK_STREQ(header(&r, "Connection"), "Upgrade");
	CHECK_STREQ(header(&r, "Sec-WebSocket-Accept"), RFC_ACCEPT);
	CHECK(header(&r, "Sec-WebSocket-Protocol") == NULL);
	CHECK(header(&r, "Sec-WebSocket-Extensions") == NULL);
	CHECK(ev.opened == 1 && ev.conn != NULL);
	CHECK(ws_server_conn_count(server) == 1);
	CHECK_STREQ(ws_peer(ev.conn)->addr, "127.0.0.1");
	/* The out buffer grew on the upgrade. */
	CHECK(ws_send_space(ev.conn) == WS_OUT_CAP - 4);
	send_text(&cl, "hello");
	expect_text(&cl, "hello", 5);

	/* The peer vanishes: on_close. */
	client_close(&cl);
	wait_closed(1);
	CHECK(ws_server_conn_count(server) == 0);

	/* Frames right behind the request (in the head buffer). */
	cl = client_connect();
	format_upgrade(req, sizeof(req), NULL, NULL, NULL);
	uint8_t buf[1200];
	size_t n = strlen(req);
	memcpy(buf, req, n);
	n += encode(buf + n, 0x81, "first", 5, true, 1);
	n += encode(buf + n, 0x81, "second", 6, true, 2);
	client_send_raw(&cl, buf, n);
	r = get_head(&cl);
	CHECK(r.status == 101);
	expect_text(&cl, "first", 5);
	expect_text(&cl, "second", 6);
	client_close(&cl);
	wait_closed(2);
	teardown();
}

static void allow_dev_origin(WsServerConfig *cfg)
{
	static const char *const origins[] = {"http://Dev.Example:3000", "https://other"};
	cfg->allowed_origins = origins;
	cfg->allowed_origin_count = 2;
}

static void test_refusals(void)
{
	setup_with(allow_dev_origin);
	/* Version. */
	Resp r = refused(NULL, "Sec-WebSocket-Version", "Sec-WebSocket-Version: 8\r\n", 426);
	CHECK_STREQ(header(&r, "Sec-WebSocket-Version"), "13");
	CHECK_STREQ(header(&r, "Upgrade"), "websocket");
	refused(NULL, "Sec-WebSocket-Version", NULL, 426);
	refused(NULL, NULL, "Sec-WebSocket-Version: 13\r\n", 426); /* twice */
	/* Key. */
	refused(NULL, "Sec-WebSocket-Key", NULL, 400);
	refused(NULL, "Sec-WebSocket-Key", "Sec-WebSocket-Key: abc\r\n", 400);
	refused(NULL, "Sec-WebSocket-Key", "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZS4=\r\n",
		400);
	refused(NULL, NULL, "Sec-WebSocket-Key: " RFC_KEY "\r\n", 400); /* twice */
	/* Upgrade headers. */
	refused(NULL, "Upgrade", NULL, 426);
	refused(NULL, "Upgrade", "Upgrade: h2c\r\n", 426);
	refused(NULL, "Connection", NULL, 400);
	refused(NULL, "Connection", "Connection: keep-alive\r\n", 400);
	/* Case and lists in the token fields are fine. */
	Client cl = client_connect();
	char req[1024];
	format_upgrade(req, sizeof(req), NULL, "Connection,Upgrade",
		       "Connection: keep-alive, UPGRADE\r\nUpgrade: WebSocket\r\n");
	client_send_raw(&cl, req, strlen(req));
	r = get_head(&cl);
	CHECK(r.status == 101);
	client_close(&cl);
	wait_closed(1);
	/* Not an upgrade at all. */
	cl = client_connect();
	client_send_raw(&cl, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 27);
	r = get_head(&cl);
	CHECK(r.status == 426);
	CHECK(run_until(client_eof, &cl));
	client_close(&cl);
	refused("POST / HTTP/1.1", NULL, NULL, 400);
	refused("GET / HTTP/1.0", NULL, NULL, 400);
	refused("GET * HTTP/1.1", NULL, NULL, 400);
	refused(NULL, NULL, "Content-Length: 3\r\n", 400);
	refused("GET  / HTTP/1.1", NULL, NULL, 400); /* malformed */

	/* Origin. */
	refused(NULL, "Origin", NULL, 403);
	refused(NULL, "Origin", "Origin: http://evil.example:8080\r\n", 403);
	refused(NULL, "Origin", "Origin: http://127.0.0.1:9999\r\n", 403);
	refused(NULL, "Origin", "Origin: https://127.0.0.1:8080\r\n", 403);
	refused(NULL, "Origin", "Origin: null\r\n", 403);
	refused(NULL, NULL, "Origin: http://127.0.0.1:8080\r\n", 403); /* twice */
	refused(NULL, "Origin", "Origin: http://dev.example:3001\r\n", 403);
	refused(NULL, "Origin", "Origin: http://dev.example:3000/\r\n", 403);
	/* allowed_origins, case-insensitively. */
	const char *ok[] = {"Origin: http://dev.example:3000\r\n", "Origin: https://other\r\n"};
	for (int i = 0; i < 2; i++) {
		cl = client_connect();
		format_upgrade(req, sizeof(req), NULL, "Origin", ok[i]);
		client_send_raw(&cl, req, strlen(req));
		r = get_head(&cl);
		CHECK(r.status == 101);
		client_close(&cl);
		wait_closed(2 + i);
	}
	/* The own origin is the Host field's host. */
	cl = client_connect();
	format_upgrade(req, sizeof(req), NULL, "Host,Origin",
		       "Host: LocalHost:1\r\nOrigin: http://localhost:8080\r\n");
	client_send_raw(&cl, req, strlen(req));
	r = get_head(&cl);
	CHECK(r.status == 101);
	client_close(&cl);
	wait_closed(4);
	teardown();
}

static void two_connections(WsServerConfig *cfg)
{
	cfg->max_connections = 2;
}

static void test_connection_limit(void)
{
	setup_with(two_connections);
	Client a = ws_connect();
	Client b = ws_connect();
	CHECK(ws_server_conn_count(server) == 2);
	Resp r = refused(NULL, NULL, NULL, 503);
	CHECK_STREQ(header(&r, "Connection"), "close");
	/* Pending handshakes do not count: a TCP connection still waits. */
	Client p = client_connect();
	client_close(&a);
	wait_closed(1);
	char req[1024];
	format_upgrade(req, sizeof(req), NULL, NULL, NULL);
	client_send_raw(&p, req, strlen(req));
	r = get_head(&p);
	CHECK(r.status == 101);
	CHECK(ws_server_conn_count(server) == 2);
	send_text(&b, "still here");
	expect_text(&b, "still here", 10);
	client_close(&b);
	client_close(&p);
	wait_closed(3);
	teardown();
}

/* ---- Messages ----------------------------------------------------------------- */

static void small_messages(WsServerConfig *cfg)
{
	cfg->max_message_bytes = 1024;
}

static void test_echo(void)
{
	setup();
	Client cl = ws_connect();
	send_text(&cl, "{\"protocol\":\"runtime\"}");
	expect_text(&cl, "{\"protocol\":\"runtime\"}", 22);
	CHECK_STREQ(ev.last, "{\"protocol\":\"runtime\"}");
	send_text(&cl, ""); /* empty */
	expect_text(&cl, "", 0);
	send_text(&cl, "h\xC3\xA9llo \xF0\x9F\x98\x80");
	expect_text(&cl, "h\xC3\xA9llo \xF0\x9F\x98\x80", 11);

	/* 16-bit and exactly max_message_bytes (16384) lengths. */
	static char big[CONFIG_DEFAULT_MAX_MESSAGE_BYTES];
	for (size_t i = 0; i < sizeof(big); i++)
		big[i] = (char)('a' + i % 26);
	send_frame(&cl, true, WS_OP_TEXT, big, 300);
	expect_text(&cl, big, 300);
	send_frame(&cl, true, WS_OP_TEXT, big, sizeof(big));
	expect_text(&cl, big, sizeof(big));

	/* Several frames in one write, and one frame byte by byte. */
	uint8_t buf[64];
	size_t n = encode(buf, 0x81, "one", 3, true, 9);
	n += encode(buf + n, 0x81, "two", 3, true, 10);
	client_send_raw(&cl, buf, n);
	expect_text(&cl, "one", 3);
	expect_text(&cl, "two", 3);
	n = encode(buf, 0x81, "split", 5, true, 11);
	for (size_t i = 0; i < n; i++) {
		client_send_raw(&cl, buf + i, 1);
		loop_run_once(&loop);
	}
	expect_text(&cl, "split", 5);
	CHECK(ev.messages == 8);
	client_close(&cl);
	wait_closed(1);
	teardown();
}

static void test_fragments(void)
{
	setup_with(small_messages);
	Client cl = ws_connect();
	/* Fragments with control frames between them; a character split
	 * across fragments. */
	send_frame(&cl, false, WS_OP_TEXT, "Hel", 3);
	send_frame(&cl, true, WS_OP_PING, "p1", 2);
	send_frame(&cl, false, WS_OP_CONTINUATION, "lo \xE2\x82", 5);
	send_frame(&cl, true, WS_OP_PONG, "", 0);
	send_frame(&cl, false, WS_OP_CONTINUATION, "", 0);
	send_frame(&cl, true, WS_OP_CONTINUATION, "\xAC!", 2);
	Frame f = get_frame(&cl);
	CHECK(f.op == WS_OP_PONG && f.len == 2 && memcmp(f.data, "p1", 2) == 0);
	free(f.data);
	expect_text(&cl, "Hello \xE2\x82\xAC!", 10);
	CHECK(ev.messages == 1);

	/* Exactly max_message_bytes in fragments. */
	static char m[1025];
	memset(m, 'x', sizeof(m));
	send_frame(&cl, false, WS_OP_TEXT, m, 1000);
	send_frame(&cl, true, WS_OP_CONTINUATION, m + 1000, 24);
	expect_text(&cl, m, 1024);
	/* One byte more: 1009, before the last fragment's payload is kept. */
	send_frame(&cl, false, WS_OP_TEXT, m, 1000);
	send_frame(&cl, true, WS_OP_CONTINUATION, m + 1000, 25);
	expect_close(&cl, WS_CLOSE_TOO_BIG);
	CHECK(ev.messages == 2);
	client_close(&cl);
	wait_closed(1);

	/* A single frame exactly at, then one byte over the limit. */
	cl = ws_connect();
	send_frame(&cl, true, WS_OP_TEXT, m, 1024);
	expect_text(&cl, m, 1024);
	send_frame(&cl, true, WS_OP_TEXT, m, 1025);
	expect_close(&cl, WS_CLOSE_TOO_BIG);
	client_close(&cl);
	wait_closed(2);

	/* An announced huge frame: refused from its header alone. */
	cl = ws_connect();
	uint8_t hdr[] = {0x81, 0xFF, 0, 0, 0, 1, 0, 0, 0, 0, 1, 2, 3, 4};
	client_send_raw(&cl, hdr, sizeof(hdr));
	expect_close_frame(&cl, WS_CLOSE_TOO_BIG); /* a close now would be payload */
	client_close(&cl);
	wait_closed(3);
	teardown();
}

/* A fresh connection gets bytes (frames) and must close with code; answer:
 * the client answers the close. */
static void violation_answer(const void *frames, size_t len, int code, bool answer)
{
	int closed = ev.closed;
	int messages = ev.messages;
	Client cl = ws_connect();
	client_send_raw(&cl, frames, len);
	if (answer)
		expect_close(&cl, code);
	else
		expect_close_frame(&cl, code);
	client_close(&cl);
	wait_closed(closed + 1);
	CHECK(ev.messages == messages);
}

static void violation(const void *frames, size_t len, int code)
{
	violation_answer(frames, len, code, true);
}

static void violation_frame(uint8_t b0, const void *data, size_t len, bool masked, int code)
{
	uint8_t buf[300];
	size_t n = encode(buf, b0, data, len, masked, 5);
	violation(buf, n, code);
}

static void test_violations(void)
{
	setup();
	violation_frame(0x81, "hi", 2, false, WS_CLOSE_PROTOCOL_ERROR); /* unmasked */
	violation_frame(0x82, "\x01\x02", 2, true, WS_CLOSE_UNSUPPORTED_DATA); /* binary */
	violation_frame(0x02, "\x01", 1, true, WS_CLOSE_UNSUPPORTED_DATA); /* fragmented */
	violation_frame(0xC1, "hi", 2, true, WS_CLOSE_PROTOCOL_ERROR); /* RSV1 */
	violation_frame(0x91, "hi", 2, true, WS_CLOSE_PROTOCOL_ERROR); /* RSV3 */
	violation_frame(0x83, "hi", 2, true, WS_CLOSE_PROTOCOL_ERROR); /* opcode 3 */
	violation_frame(0x8B, "", 0, true, WS_CLOSE_PROTOCOL_ERROR);	/* opcode 0xB */
	violation_frame(0x80, "x", 1, true, WS_CLOSE_PROTOCOL_ERROR);	/* stray continuation */
	violation_frame(0x09, "", 0, true, WS_CLOSE_PROTOCOL_ERROR);	/* fragmented ping */
	static char long_ping[126];
	violation_frame(0x89, long_ping, 126, true, WS_CLOSE_PROTOCOL_ERROR);
	violation_frame(0x88, "\x03", 1, true, WS_CLOSE_PROTOCOL_ERROR); /* 1-byte close */
	violation_frame(0x88, "\x03\xED", 2, true, WS_CLOSE_PROTOCOL_ERROR); /* code 1005 */
	violation_frame(0x88, "\x03\xEE", 2, true, WS_CLOSE_PROTOCOL_ERROR); /* code 1006 */
	violation_frame(0x88, "\x03\xE8\xC0\xAF", 4, true, WS_CLOSE_INVALID_DATA); /* reason */
	/* A new text message inside a fragmented one. */
	uint8_t buf[64];
	size_t n = encode(buf, 0x01, "a", 1, true, 1);
	n += encode(buf + n, 0x81, "b", 1, true, 2);
	violation(buf, n, WS_CLOSE_PROTOCOL_ERROR);
	n = encode(buf, 0x01, "a", 1, true, 1);
	n += encode(buf + n, 0x82, "b", 1, true, 2);
	violation(buf, n, WS_CLOSE_PROTOCOL_ERROR);
	/* A 64-bit length with the top bit set: the stream cannot be parsed
	 * any more (an answer would not be seen). */
	uint8_t huge[] = {0x81, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4};
	violation_answer(huge, sizeof(huge), WS_CLOSE_PROTOCOL_ERROR, false);

	/* Invalid UTF-8: 1007. */
	const char *bad[] = {
		"\xC0\xAF",	    /* overlong '/' */
		"ab\xE0\x80\xAF",   /* overlong, 3 bytes */
		"\xF0\x80\x80\xAF", /* overlong, 4 bytes */
		"\xED\xA0\x80",	    /* surrogate U+D800 */
		"x\xED\xBF\xBFy",   /* surrogate U+DFFF */
		"\xF4\x90\x80\x80", /* above U+10FFFF */
		"\xFF",
		"abc\xE2\x82", /* cut at the end */
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		violation_frame(0x81, bad[i], strlen(bad[i]), true, WS_CLOSE_INVALID_DATA);
	/* Cut at the end of a fragmented message; invalid across fragments. */
	n = encode(buf, 0x01, "ok", 2, true, 1);
	n += encode(buf + n, 0x80, "\xF0\x9F", 2, true, 2);
	violation(buf, n, WS_CLOSE_INVALID_DATA);
	n = encode(buf, 0x01, "\xED", 1, true, 1);
	n += encode(buf + n, 0x80, "\xA0\x80", 2, true, 2);
	violation(buf, n, WS_CLOSE_INVALID_DATA);
	teardown();
}

static void test_ping_close(void)
{
	setup();
	Client cl = ws_connect();
	send_frame(&cl, true, WS_OP_PING, "abc", 3);
	Frame f = get_frame(&cl);
	CHECK(f.op == WS_OP_PONG && f.fin && f.len == 3 && memcmp(f.data, "abc", 3) == 0);
	free(f.data);
	static char p125[125];
	memset(p125, 'p', sizeof(p125));
	send_frame(&cl, true, WS_OP_PING, p125, 125);
	f = get_frame(&cl);
	CHECK(f.op == WS_OP_PONG && f.len == 125);
	free(f.data);
	send_frame(&cl, true, WS_OP_PONG, "unsolicited", 11); /* ignored */
	send_text(&cl, "after");
	expect_text(&cl, "after", 5);

	/* The client closes: its code is echoed and the socket closed. */
	send_close(&cl, 1000, "bye");
	expect_close_answer(&cl, 1000, false);
	CHECK(cl.len == 0);
	client_close(&cl);
	wait_closed(1);
	/* A close without a code: an empty close. */
	cl = ws_connect();
	send_close(&cl, -1, "");
	expect_close_answer(&cl, -1, false);
	client_close(&cl);
	wait_closed(2);
	/* A registered code (4000), and data after the close is dropped. */
	cl = ws_connect();
	uint8_t buf[64];
	size_t n = encode(buf, 0x88, "\x0F\xA0"
				     "app",
			  5, true, 1);
	n += encode(buf + n, 0x81, "ignored", 7, true, 2);
	client_send_raw(&cl, buf, n); /* in one write: read together */
	expect_close_answer(&cl, 4000, false);
	client_close(&cl);
	wait_closed(3);
	CHECK(ev.messages == 1);

	/* The server closes (ws_close): its frame, then data is dropped until
	 * the client's close; then the socket closes. */
	cl = ws_connect();
	send_text(&cl, "close-me");
	f = get_frame(&cl);
	CHECK(f.op == WS_OP_CLOSE && f.len == 6 && memcmp(f.data, "\x03\xE8"
							  "done",
							  6) == 0);
	free(f.data);
	CHECK(ws_server_conn_count(server) == 1); /* closing still counts */
	send_text(&cl, "dropped");
	send_frame(&cl, true, WS_OP_PING, "x", 1);
	send_close(&cl, 1000, "");
	CHECK(run_until(client_eof, &cl));
	CHECK(cl.len == 0); /* no echo, no pong */
	client_close(&cl);
	wait_closed(4);
	CHECK(ev.messages == 2);
	teardown();
}

static void short_close(WsServerConfig *cfg)
{
	cfg->close_timeout_ms = 150;
}

static void test_close_timeout(void)
{
	setup_with(short_close);
	Client cl = ws_connect();
	send_text(&cl, "close-me");
	uint64_t t0 = pal_uptime_ms();
	expect_close_answer(&cl, 1000, false); /* no answer: closed after the timeout */
	CHECK(pal_uptime_ms() - t0 >= 150);
	client_close(&cl);
	wait_closed(1);

	/* A protocol error's close waits for the client too: it answers. */
	cl = ws_connect();
	uint8_t buf[32];
	size_t n = encode(buf, 0x82, "b", 1, true, 1);
	client_send_raw(&cl, buf, n);
	expect_close(&cl, WS_CLOSE_UNSUPPORTED_DATA);
	client_close(&cl);
	wait_closed(2);

	/* The peer vanishes while the server waits. */
	cl = ws_connect();
	send_text(&cl, "close-me");
	Frame f = get_frame(&cl);
	CHECK(f.op == WS_OP_CLOSE);
	free(f.data);
	client_close(&cl);
	wait_closed(3);

	/* ... or in the middle of a message. */
	cl = ws_connect();
	send_frame(&cl, false, WS_OP_TEXT, "part", 4);
	uint8_t half[] = {0x81, 0x85, 1, 2, 3, 4, 'a'};
	client_send_raw(&cl, half, sizeof(half));
	client_close(&cl);
	wait_closed(4);
	CHECK(ev.messages == 2);
	teardown();
}

/* ---- Buffers and liveness ------------------------------------------------------- */

static void small_out(WsServerConfig *cfg)
{
	cfg->out_cap = 2048;
}

static void test_overflow(void)
{
	setup_with(small_out);
	Client cl = ws_connect();
	WsConn *c = ev.conn;
	CHECK(ws_send_space(c) == 2048 - 4);
	CHECK(ws_send_pending(c) == 0);
	static char m[2048]; /* messages of 1000 bytes, and up to out_cap */
	memset(m, 'm', sizeof(m));
	/* Without running the loop nothing is written out: the third message
	 * does not fit, and the connection closes (no close frame). */
	CHECK(ws_send_text(c, m, 1000));
	CHECK(ws_send_space(c) == 2048 - 1004 - 4);
	CHECK(ws_send_pending(c) == 1004);
	CHECK(ws_send_text(c, m, 1000));
	CHECK(ws_send_space(c) == 2048 - 2008 - 2);
	CHECK(!ws_send_text(c, m, 1000));
	CHECK(!ws_is_open(c));
	CHECK(ws_send_space(c) == 0);
	CHECK(ws_send_pending(c) == 0); /* not open */
	CHECK(!ws_send_text(c, "x", 1));
	ws_close(c, WS_CLOSE_NORMAL, NULL); /* ignored */
	wait_closed(1);
	CHECK(run_until(client_eof, &cl));
	CHECK(cl.len == 0);
	client_close(&cl);

	/* What fits goes out. */
	cl = ws_connect();
	c = ev.conn;
	size_t space = ws_send_space(c);
	CHECK(ws_send_text(c, m, 900));
	CHECK(ws_send_text(c, m, ws_send_space(c)));
	CHECK(ws_send_space(c) == 0);
	int drained = ev.drained;
	expect_text(&cl, m, 900);
	Frame f = get_frame(&cl);
	CHECK(f.op == WS_OP_TEXT && f.len == space - 904);
	free(f.data);
	/* Written out: on_drained, and all the space is free again. */
	CHECK(ev.drained > drained);
	CHECK(ws_send_space(c) == space);
	CHECK(ws_send_pending(c) == 0);
	client_close(&cl);
	wait_closed(2);
	teardown();
}

static void short_liveness(WsServerConfig *cfg)
{
	cfg->ping_interval_ms = 100;
	cfg->pong_timeout_ms = 500;
	cfg->handshake_timeout_ms = 100;
}

static void test_liveness(void)
{
	setup_with(short_liveness);
	/* A client that answers pings stays. */
	Client cl = ws_connect();
	uint64_t t0 = pal_uptime_ms();
	for (int i = 0; i < 3; i++) {
		Frame f = get_frame(&cl);
		CHECK(f.op == WS_OP_PING && f.len == 0);
		free(f.data);
		send_frame(&cl, true, WS_OP_PONG, "", 0);
	}
	CHECK(pal_uptime_ms() - t0 >= 200);
	CHECK(ev.closed == 0);
	send_text(&cl, "alive");
	expect_text(&cl, "alive", 5);
	/* One that does not is aborted. */
	Frame f = get_frame(&cl);
	CHECK(f.op == WS_OP_PING);
	free(f.data);
	CHECK(run_until(client_eof, &cl));
	CHECK(cl.len == 0);
	client_close(&cl);
	wait_closed(1);

	/* The handshake timeout: silent, or 408 after a partial request. */
	cl = client_connect();
	CHECK(run_until(client_eof, &cl));
	CHECK(cl.len == 0);
	client_close(&cl);
	cl = client_connect();
	client_send_raw(&cl, "GET / HTTP/1.1\r\n", 16);
	Resp r = get_head(&cl);
	CHECK(r.status == 408);
	CHECK(run_until(client_eof, &cl));
	client_close(&cl);
	CHECK(ev.opened == 1);
	teardown();
}

/* ---- The server ----------------------------------------------------------------- */

static void test_server_close(void)
{
	setup();
	Client a = ws_connect();
	Client b = ws_connect();
	Client p = client_connect(); /* pending */
	CHECK(run_until(conns_are, &(size_t){3}));
	ws_server_close(server);
	server = NULL;
	CHECK(ev.closed == 2);
	CHECK(loop.conn_count == 0 && loop.listener_count == 0 && loop.tick_count == 1);
	CHECK(run_until(client_eof, &a));
	CHECK(run_until(client_eof, &b));
	client_close(&a);
	client_close(&b);
	client_close(&p);
	loop_deinit(&loop);
	runtime_deinit(&rt);

	/* The loop's shutdown closes the listener and the connections. */
	setup();
	a = ws_connect();
	pal_request_stop();
	loop_run(&loop);
	CHECK(ev.closed == 1);
	CHECK(ws_server_port(server) == 0);
	client_close(&a);
	teardown();
}

static void test_open_config(void)
{
	memset(&ev, 0, sizeof(ev));
	runtime_init(&rt);
	CHECK(loop_init(&loop, &rt) == PAL_OK);
	Config cfg;
	ConfigDefaults d = {.confdir = "/tmp", .webdir = "/tmp", .datadir = "/tmp"};
	CHECK(config_init(&cfg, &d) == CONFIG_OK);
	snprintf(cfg.host, sizeof(cfg.host), "127.0.0.1");
	snprintf(cfg.allowed_origins[0], sizeof(cfg.allowed_origins[0]), "http://ui.example");
	cfg.allowed_origin_count = 1;
	cfg.max_connections = 1;
	/* A free port for the WebSocket: bound once, then released. */
	pal_socket s;
	uint16_t port = 0;
	CHECK(pal_socket_listen("127.0.0.1", 0, 1, &s) == PAL_OK);
	CHECK(pal_socket_local_port(s, &port) == PAL_OK);
	pal_socket_close(s);
	cfg.port = port - 1u;
	cfg.tls.enabled = true;
	WsServer *x = (WsServer *)&x;
	CHECK(ws_server_open_config(&loop, &cfg, &handler, &ev, &x) == PAL_ERR_UNAVAILABLE);
	CHECK(x == NULL && loop.listener_count == 0 && loop.tick_count == 0);
	cfg.tls.enabled = false;
	CHECK(ws_server_open_config(&loop, &cfg, &handler, &ev, &server) == PAL_OK);
	CHECK(ws_server_port(server) == port);

	/* The web port is port - 1: the own origin. */
	Client cl = client_connect();
	char req[512];
	snprintf(req, sizeof(req),
		 "GET / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nUpgrade: websocket\r\n"
		 "Connection: Upgrade\r\nSec-WebSocket-Key: " RFC_KEY "\r\n"
		 "Sec-WebSocket-Version: 13\r\nOrigin: http://127.0.0.1:%u\r\n\r\n",
		 port, port - 1u);
	client_send_raw(&cl, req, strlen(req));
	Resp r = get_head(&cl);
	CHECK(r.status == 101);
	/* max_connections 1: the next is 503, also from an allowed origin. */
	Client c2 = client_connect();
	snprintf(req, sizeof(req),
		 "GET / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nUpgrade: websocket\r\n"
		 "Connection: Upgrade\r\nSec-WebSocket-Key: " RFC_KEY "\r\n"
		 "Sec-WebSocket-Version: 13\r\nOrigin: http://UI.example\r\n\r\n",
		 port);
	client_send_raw(&c2, req, strlen(req));
	r = get_head(&c2);
	CHECK(r.status == 503);
	client_close(&c2);
	client_close(&cl);
	wait_closed(1);

	/* The same port again: refused. */
	CHECK(ws_server_open_config(&loop, &cfg, &handler, &ev, &x) == PAL_ERR_ADDR_IN_USE);
	CHECK(x == NULL);
	cfg.port = 0;
	CHECK(ws_server_open_config(&loop, &cfg, &handler, &ev, &x) == PAL_ERR_INVALID);
	ws_server_close(server);
	server = NULL;

	WsServerConfig wc;
	ws_server_config_defaults(&wc, "127.0.0.1", 0);
	wc.out_cap = 100;
	CHECK(ws_server_open(&loop, &wc, &x) == PAL_ERR_INVALID);
	ws_server_config_defaults(&wc, "127.0.0.1", 0);
	wc.max_connections = 0;
	CHECK(ws_server_open(&loop, &wc, &x) == PAL_ERR_INVALID);
	CHECK(loop.tick_count == 0);
	loop_deinit(&loop);
	runtime_deinit(&rt);
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	RUN(test_accept_key);
	RUN(test_close_codes);
	RUN(test_frame_header);
	RUN(test_utf8);
	RUN(test_origin);
	RUN(test_handshake);
	RUN(test_refusals);
	RUN(test_connection_limit);
	RUN(test_echo);
	RUN(test_fragments);
	RUN(test_violations);
	RUN(test_ping_close);
	RUN(test_close_timeout);
	RUN(test_overflow);
	RUN(test_liveness);
	RUN(test_server_close);
	RUN(test_open_config);
	return test_result();
}
