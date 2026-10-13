#include "engine/web_server.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol/http.h"
#include "system/log.h"

/* File bytes read per pal_file_read while streaming. */
#define CHUNK 4096

/* The most input bytes read and discarded before a close (see "Connections"
 * in web_server.h): what is already there, not a stream that keeps coming. */
#define DISCARD_MAX (64 * 1024)

/* A deadline that never expires (a connection about to close). */
#define NO_DEADLINE UINT64_MAX

enum web_state {
	W_HEAD,	   /* reading a request head */
	W_SENDING, /* streaming a file body */
	W_FLUSH,   /* the response is queued; waiting for it to be written out */
};

typedef struct WebConn {
	WebServer *srv;
	LoopConn *conn;
	struct WebConn *prev, *next;
	enum web_state state;
	bool started;	 /* W_HEAD: a byte of the request arrived */
	bool keep_alive; /* the connection persists after this response */
	uint64_t deadline;
	pal_file file; /* W_SENDING: the file being sent */
	uint64_t remaining;
	HttpRequest req;
	char head[]; /* the request head buffer, head_max bytes */
} WebConn;

struct WebServer {
	Loop *loop;
	LoopListener *listener; /* NULL once closed */
	WebConn *conns;
	WebServerConfig cfg;
	char webdir[PAL_PATH_MAX]; /* without a trailing '/' */
	uint8_t chunk[CHUNK];
};

/* ---- Paths and content types ---------------------------------------------------- */

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* The extension of path's last segment (after its last '.'), or NULL. */
static const char *extension(const char *path)
{
	const char *slash = strrchr(path, '/');
	const char *dot = strrchr(slash != NULL ? slash : path, '.');
	return dot != NULL ? dot + 1 : NULL;
}

enum web_path web_path_from_target(const char *target, char *out, size_t cap, bool *route)
{
	static const char entry[] = WEB_ENTRY_DOCUMENT;
	*route = false;
	if (cap == 0)
		return WEB_PATH_NOT_FOUND;
	if (target[0] != '/')
		return WEB_PATH_BAD; /* origin-form only (RFC 9112 §3.2.1) */
	size_t o = 0, seg = 0;
	bool hidden = false;
	const char *p = target + 1;
	for (;;) {
		char c = *p;
		if (c == '\0' || c == '?' || c == '#' || c == '/') {
			/* The segment [seg, o) ends. */
			size_t n = o - seg;
			if ((n == 1 && out[seg] == '.') ||
			    (n == 2 && out[seg] == '.' && out[seg + 1] == '.'))
				return WEB_PATH_BAD;
			if (n > 0 && out[seg] == '.')
				hidden = true;
			if (c != '/')
				break;
			if (n > 0) { /* an empty segment collapses */
				if (o + 1 >= cap)
					return WEB_PATH_NOT_FOUND;
				out[o++] = '/';
				seg = o;
			}
			p++;
			continue;
		}
		unsigned char b;
		if (c == '%') {
			int hi = hex_value(p[1]);
			int lo = hi < 0 ? -1 : hex_value(p[2]);
			if (lo < 0)
				return WEB_PATH_BAD;
			b = (unsigned char)(hi * 16 + lo);
			p += 3;
			if (b == '\0' || b == '/' || b == '\\')
				return WEB_PATH_BAD;
		} else {
			b = (unsigned char)c;
			p++;
			if (b == '\\')
				return WEB_PATH_BAD;
		}
		if (b < 0x20 || b == 0x7f)
			return WEB_PATH_BAD;
		if (o + 1 >= cap)
			return WEB_PATH_NOT_FOUND;
		out[o++] = (char)b;
	}
	out[o] = '\0';
	if (hidden)
		return WEB_PATH_NOT_FOUND;
	if (o == 0 || out[o - 1] == '/') {
		*route = true; /* a directory: no extension */
		if (o + sizeof(entry) > cap)
			return WEB_PATH_NOT_FOUND;
		memcpy(out + o, entry, sizeof(entry));
	} else {
		*route = extension(out) == NULL;
	}
	return WEB_PATH_OK;
}

const char *web_content_type(const char *path)
{
	static const struct {
		const char *ext;
		const char *type;
	} types[] = {
		{"html", "text/html; charset=utf-8"},
		{"htm", "text/html; charset=utf-8"},
		{"js", "text/javascript; charset=utf-8"},
		{"mjs", "text/javascript; charset=utf-8"},
		{"css", "text/css; charset=utf-8"},
		{"json", "application/json"},
		{"map", "application/json"},
		{"txt", "text/plain; charset=utf-8"},
		{"svg", "image/svg+xml"},
		{"png", "image/png"},
		{"jpg", "image/jpeg"},
		{"jpeg", "image/jpeg"},
		{"gif", "image/gif"},
		{"webp", "image/webp"},
		{"ico", "image/x-icon"},
		{"woff", "font/woff"},
		{"woff2", "font/woff2"},
		{"wasm", "application/wasm"},
	};
	const char *ext = extension(path);
	if (ext != NULL)
		for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
			if (http_str_ieq(ext, types[i].ext))
				return types[i].type;
	return "application/octet-stream";
}

/* ---- Responses ---------------------------------------------------------------- */

static uint64_t now_ms(const WebConn *w)
{
	return loop_now_ms(w->srv->loop);
}

static void log_response(const WebConn *w, int status, uint64_t bytes)
{
	const HttpRequest *r = &w->req;
	bool parsed = r->result == HTTP_PARSE_COMPLETE;
	const char *target = parsed ? r->target : "-";
	size_t len = strcspn(target, "?#"); /* never the query */
	log_info("%s %s %.*s %d %" PRIu64, loop_conn_peer(w->conn)->addr,
		 parsed ? r->method : "-", (int)len, target, status, bytes);
}

/* The response is queued: wait for it to be written out (on_drained). */
static void start_flush(WebConn *w)
{
	w->state = W_FLUSH;
	w->deadline = now_ms(w) + w->srv->cfg.send_timeout_ms;
	loop_conn_pause_read(w->conn, true);
}

/* A small fixed error response; keep: the connection may persist. */
static void respond_error(WebConn *w, int status, bool keep)
{
	w->keep_alive = w->keep_alive && keep;
	char body[64];
	int blen = snprintf(body, sizeof(body), "%d %s\n", status, http_reason(status));
	char head[384];
	int hlen = snprintf(head, sizeof(head),
			    "HTTP/1.1 %d %s\r\n"
			    "Content-Type: text/plain; charset=utf-8\r\n"
			    "Content-Length: %d\r\n"
			    "Cache-Control: no-cache\r\n"
			    "X-Content-Type-Options: nosniff\r\n"
			    "%s%s\r\n",
			    status, http_reason(status), blen,
			    status == 405 ? "Allow: GET, HEAD\r\n" : "",
			    w->keep_alive ? "" : "Connection: close\r\n");
	bool head_only = w->req.result == HTTP_PARSE_COMPLETE && strcmp(w->req.method, "HEAD") == 0;
	if (loop_conn_write(w->conn, head, (size_t)hlen) && !head_only)
		loop_conn_write(w->conn, body, (size_t)blen);
	log_response(w, status, (uint64_t)blen);
	start_flush(w);
}

static bool not_found(enum pal_err err)
{
	return err == PAL_ERR_NOT_FOUND || err == PAL_ERR_INVALID || err == PAL_ERR_NAME_TOO_LONG;
}

/* Opens rel under webdir, or its .gz variant when the client takes gzip. */
static enum pal_err open_asset(WebConn *w, const char *rel, bool *gzip, uint64_t *size)
{
	char path[PAL_PATH_MAX];
	*gzip = false;
	int n = snprintf(path, sizeof(path), "%s/%s.gz", w->srv->webdir, rel);
	if (n < 0 || (size_t)n >= sizeof(path))
		return PAL_ERR_NAME_TOO_LONG;
	if (http_request_accepts_encoding(&w->req, "gzip")) {
		enum pal_err err = pal_file_open(path, &w->file, size);
		if (err == PAL_OK) {
			*gzip = true;
			return PAL_OK;
		}
		if (!not_found(err))
			return err;
	}
	path[n - 3] = '\0'; /* without ".gz" */
	return pal_file_open(path, &w->file, size);
}

static void close_file(WebConn *w)
{
	pal_file_close(w->file);
	w->file = PAL_FILE_NONE;
}

/* Fills the outgoing buffer with the file's next bytes. */
static void stream(WebConn *w)
{
	LoopConn *c = w->conn;
	while (w->remaining > 0 && loop_conn_space(c) > 0) {
		size_t n = loop_conn_space(c);
		if (n > CHUNK)
			n = CHUNK;
		if (n > w->remaining)
			n = (size_t)w->remaining;
		size_t got = 0;
		enum pal_err err = pal_file_read(w->file, w->srv->chunk, n, &got);
		if (err != PAL_OK || got == 0) {
			/* The headers announced more: the client must not take
			 * a short body for the whole file. */
			log_warn("web: sending a file to %s failed: %s", loop_conn_peer(c)->addr,
				 err != PAL_OK ? pal_err_str(err) : "the file shrank");
			close_file(w);
			w->deadline = NO_DEADLINE;
			loop_conn_abort(c);
			return;
		}
		if (!loop_conn_write(c, w->srv->chunk, got)) {
			close_file(w);
			w->deadline = NO_DEADLINE;
			return;
		}
		w->remaining -= got;
	}
	if (w->remaining == 0) {
		close_file(w);
		start_flush(w);
	}
}

/* Answers a complete, valid request head. */
static void serve(WebConn *w)
{
	const HttpRequest *r = &w->req;
	w->keep_alive = http_request_keep_alive(r) && !r->has_body;
	bool head_only = strcmp(r->method, "HEAD") == 0;
	if (!head_only && strcmp(r->method, "GET") != 0) {
		respond_error(w, 405, true);
		return;
	}
	char rel[PAL_PATH_MAX];
	bool route;
	switch (web_path_from_target(r->target, rel, sizeof(rel), &route)) {
	case WEB_PATH_OK:
		break;
	case WEB_PATH_BAD:
		respond_error(w, 400, true);
		return;
	case WEB_PATH_NOT_FOUND:
		respond_error(w, 404, true);
		return;
	}

	bool gzip;
	uint64_t size = 0;
	const char *served = rel;
	enum pal_err err = open_asset(w, served, &gzip, &size);
	if (not_found(err) && route) {
		served = WEB_ENTRY_DOCUMENT; /* a client-side route (SPA fallback) */
		err = open_asset(w, served, &gzip, &size);
	}
	if (not_found(err)) {
		respond_error(w, 404, true);
		return;
	}
	if (err != PAL_OK) {
		log_warn("web: cannot open %s/%s: %s", w->srv->webdir, served, pal_err_str(err));
		respond_error(w, err == PAL_ERR_UNAVAILABLE ? 503 : 500, false);
		return;
	}

	char head[512];
	int hlen = snprintf(head, sizeof(head),
			    "HTTP/1.1 200 OK\r\n"
			    "Content-Type: %s\r\n"
			    "Content-Length: %" PRIu64 "\r\n"
			    "%s"
			    "Vary: Accept-Encoding\r\n"
			    "Cache-Control: no-cache\r\n"
			    "X-Content-Type-Options: nosniff\r\n"
			    "%s\r\n",
			    web_content_type(served), size, gzip ? "Content-Encoding: gzip\r\n" : "",
			    w->keep_alive ? "" : "Connection: close\r\n");
	log_response(w, 200, head_only ? 0 : size);
	if (!loop_conn_write(w->conn, head, (size_t)hlen) || head_only || size == 0) {
		close_file(w);
		start_flush(w);
		return;
	}
	w->state = W_SENDING;
	w->remaining = size;
	w->deadline = now_ms(w) + w->srv->cfg.send_timeout_ms;
	loop_conn_pause_read(w->conn, true);
	stream(w);
}

static void handle(WebConn *w, enum http_parse res)
{
	if (res == HTTP_PARSE_COMPLETE) {
		serve(w);
	} else {
		w->keep_alive = false; /* the stream cannot be trusted any more */
		respond_error(w, http_parse_status(res), false);
	}
}

/* Waits for the next request head; its bytes may be in the buffer already. */
static void begin_request(WebConn *w)
{
	w->state = W_HEAD;
	w->started = w->req.len > 0;
	w->deadline = now_ms(w) +
		      (w->started ? w->srv->cfg.head_timeout_ms : w->srv->cfg.idle_timeout_ms);
	loop_conn_pause_read(w->conn, false);
}

/* Reads and drops the input already received, so that the close sends FIN,
 * not a reset that could destroy the response in flight. */
static void discard_input(WebConn *w)
{
	size_t total = 0, n;
	while (total < DISCARD_MAX &&
	       loop_conn_read(w->conn, w->srv->chunk, sizeof(w->srv->chunk), &n) == PAL_OK)
		total += n;
}

/* The response has been written out. */
static void response_done(WebConn *w)
{
	if (!w->keep_alive) {
		discard_input(w);
		w->deadline = NO_DEADLINE;
		loop_conn_close(w->conn);
		return;
	}
	http_request_next(&w->req);
	begin_request(w);
	enum http_parse res = http_request_commit(&w->req, 0); /* a pipelined request */
	if (res != HTTP_PARSE_INCOMPLETE)
		handle(w, res);
}

/* ---- Connection callbacks ----------------------------------------------------- */

static void on_readable(void *ctx, LoopConn *c)
{
	WebConn *w = ctx;
	if (w->state != W_HEAD) {
		loop_conn_pause_read(c, true);
		return;
	}
	for (;;) {
		char *dst;
		size_t cap = http_request_space(&w->req, &dst);
		size_t n;
		if (cap == 0 || loop_conn_read(c, dst, cap, &n) != PAL_OK)
			return;
		if (!w->started) {
			w->started = true;
			w->deadline = now_ms(w) + w->srv->cfg.head_timeout_ms;
		}
		enum http_parse res = http_request_commit(&w->req, n);
		if (res != HTTP_PARSE_INCOMPLETE) {
			handle(w, res);
			return;
		}
	}
}

static void on_drained(void *ctx, LoopConn *c)
{
	(void)c;
	WebConn *w = ctx;
	if (w->deadline == NO_DEADLINE)
		return;
	if (w->state == W_SENDING) {
		w->deadline = now_ms(w) + w->srv->cfg.send_timeout_ms;
		stream(w);
	} else if (w->state == W_FLUSH) {
		response_done(w);
	}
}

static void on_closed(void *ctx, LoopConn *c, enum loop_close_reason reason)
{
	WebConn *w = ctx;
	if (reason != LOOP_CLOSE_LOCAL && reason != LOOP_CLOSE_PEER)
		log_fine("web: connection from %s: %s", loop_conn_peer(c)->addr,
			 loop_close_reason_str(reason));
	close_file(w);
	if (w->prev != NULL)
		w->prev->next = w->next;
	else
		w->srv->conns = w->next;
	if (w->next != NULL)
		w->next->prev = w->prev;
	free(w);
}

static const LoopConnHandler handler = {
	.on_readable = on_readable,
	.on_drained = on_drained,
	.on_closed = on_closed,
};

static bool on_accept(void *ctx, LoopConn *c)
{
	WebServer *srv = ctx;
	WebConn *w = calloc(1, sizeof(*w) + srv->cfg.head_max);
	if (w == NULL) {
		log_warn("web: out of memory for a connection from %s", loop_conn_peer(c)->addr);
		return false;
	}
	w->srv = srv;
	w->conn = c;
	w->file = PAL_FILE_NONE;
	http_request_init(&w->req, w->head, srv->cfg.head_max);
	w->next = srv->conns;
	if (srv->conns != NULL)
		srv->conns->prev = w;
	srv->conns = w;
	loop_conn_set_handler(c, &handler, w);
	begin_request(w);
	return true;
}

static void on_listener_closed(void *ctx, LoopListener *l)
{
	(void)l;
	WebServer *srv = ctx;
	srv->listener = NULL;
}

/* The tick hook: expired deadlines; returns the time to the next one. */
static uint64_t tick(void *ctx, uint64_t now)
{
	WebServer *srv = ctx;
	uint64_t next = LOOP_NO_DEADLINE;
	for (WebConn *w = srv->conns; w != NULL; w = w->next) {
		if (w->deadline != NO_DEADLINE && now >= w->deadline) {
			if (w->state == W_HEAD && w->started) {
				respond_error(w, 408, false); /* a new deadline: sending */
			} else {
				log_fine("web: connection from %s timed out",
					 loop_conn_peer(w->conn)->addr);
				close_file(w);
				w->deadline = NO_DEADLINE;
				loop_conn_abort(w->conn);
			}
		}
		if (w->deadline != NO_DEADLINE && w->deadline - now < next)
			next = w->deadline - now;
	}
	return next;
}

/* ---- The server ---------------------------------------------------------------- */

void web_server_config_defaults(WebServerConfig *cfg, const char *host, uint16_t port,
				const char *webdir)
{
	*cfg = (WebServerConfig){
		.host = host,
		.port = port,
		.webdir = webdir,
		.max_conns = WEB_MAX_CONNS,
		.out_cap = WEB_OUT_CAP,
		.head_max = HTTP_HEAD_MAX,
		.idle_timeout_ms = WEB_IDLE_TIMEOUT_MS,
		.head_timeout_ms = WEB_HEAD_TIMEOUT_MS,
		.send_timeout_ms = WEB_SEND_TIMEOUT_MS,
	};
}

enum pal_err web_server_open(Loop *loop, const WebServerConfig *cfg, WebServer **out)
{
	*out = NULL;
	if (cfg == NULL || cfg->host == NULL || cfg->webdir == NULL || cfg->webdir[0] == '\0' ||
	    strlen(cfg->webdir) >= PAL_PATH_MAX || cfg->out_cap < WEB_OUT_CAP_MIN ||
	    cfg->head_max == 0)
		return PAL_ERR_INVALID;
	WebServer *srv = calloc(1, sizeof(*srv));
	if (srv == NULL)
		return PAL_ERR_UNAVAILABLE;
	srv->loop = loop;
	srv->cfg = *cfg;
	strcpy(srv->webdir, cfg->webdir);
	size_t n = strlen(srv->webdir);
	while (n > 0 && srv->webdir[n - 1] == '/')
		srv->webdir[--n] = '\0'; /* "/" becomes "", and paths "/<rel>" */
	srv->cfg.webdir = srv->webdir;
	if (!loop_add_tick(loop, tick, srv)) {
		log_error("web: no tick hook left");
		free(srv);
		return PAL_ERR_UNAVAILABLE;
	}
	LoopListenerConfig lc = {
		.host = cfg->host,
		.port = cfg->port,
		.max_conns = cfg->max_conns,
		.out_cap = cfg->out_cap,
		.on_accept = on_accept,
		.on_closed = on_listener_closed,
		.ctx = srv,
	};
	enum pal_err err = loop_listen(loop, &lc, &srv->listener);
	if (err != PAL_OK) {
		loop_remove_tick(loop, tick, srv);
		free(srv);
		return err;
	}
	log_info("web: serving %s on port %u", cfg->webdir, (unsigned)loop_listener_port(srv->listener));
	char index[PAL_PATH_MAX];
	int len = snprintf(index, sizeof(index), "%s/index.html", cfg->webdir);
	if (len > 0 && (size_t)len + 3 < sizeof(index) && !pal_storage_exists(index) &&
	    !pal_storage_exists(strcat(index, ".gz")))
		log_warn("web: no frontend in %s (no index.html): build it with `npm run build` in "
			 "frontend/, or set webdir (06 §1)",
			 cfg->webdir);
	*out = srv;
	return PAL_OK;
}

enum pal_err web_server_open_config(Loop *loop, const Config *cfg, WebServer **out)
{
	if (cfg->port < CONFIG_PORT_MIN || cfg->port > CONFIG_PORT_MAX) {
		*out = NULL;
		return PAL_ERR_INVALID;
	}
	return web_server_open_config_at(loop, cfg, (uint16_t)cfg->port, out);
}

enum pal_err web_server_open_config_at(Loop *loop, const Config *cfg, uint16_t port,
				       WebServer **out)
{
	WebServerConfig wc;
	web_server_config_defaults(&wc, cfg->host, port, cfg->webdir);
	return web_server_open(loop, &wc, out);
}

void web_server_close(WebServer *srv)
{
	if (srv == NULL)
		return;
	if (srv->listener != NULL)
		loop_listener_close(srv->listener); /* closes its connections too */
	loop_remove_tick(srv->loop, tick, srv);
	free(srv);
}

uint16_t web_server_port(const WebServer *srv)
{
	return srv->listener != NULL ? loop_listener_port(srv->listener) : 0;
}
