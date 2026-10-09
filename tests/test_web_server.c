/*
 * The web server (engine/web_server.h, 05 §3) over the engine loop and the
 * POSIX PAL: real loopback connections to a server on an ephemeral port,
 * serving a temporary webdir (mkdtemp under /tmp, removed afterwards) whose
 * parent also holds a file that must never be served. Waiting is always for
 * a condition, with a generous deadline; a tick hook bounds every poll to
 * 10 ms.
 */
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine/web_server.h"
#include "protocol/http.h"

static Runtime rt;
static Loop loop;
static WebServer *server;
static char root[PAL_PATH_MAX];	  /* the temporary directory */
static char webdir[PAL_PATH_MAX]; /* <root>/web */

#define WAIT_MS 3000

#define BIG_SIZE (8u << 20) /* far beyond the outgoing buffer and socket buffers */

static const char INDEX[] = "<!doctype html><title>entry</title>\n";
static const char SECRET[] = "SECRET outside webdir\n";

/* snprintf into an array, checked not to truncate. */
#define FMT(arr, ...) CHECK((size_t)snprintf(arr, sizeof(arr), __VA_ARGS__) < sizeof(arr))

/* ---- Files ------------------------------------------------------------------ */

static void put_file(const char *rel, const void *data, size_t len)
{
	char path[PAL_PATH_MAX];
	FMT(path, "%s/%s", root, rel);
	FILE *f = fopen(path, "wb");
	CHECK(f != NULL);
	if (f == NULL)
		return;
	CHECK(fwrite(data, 1, len, f) == len);
	fclose(f);
}

static void put_text(const char *rel, const char *text)
{
	put_file(rel, text, strlen(text));
}

static uint8_t pattern(size_t i)
{
	return (uint8_t)((i * 7 + i / 251) % 256);
}

static void make_webdir(void)
{
	FMT(root, "/tmp/hakkas-web-XXXXXX");
	CHECK(mkdtemp(root) != NULL);
	FMT(webdir, "%s/web", root);
	CHECK(mkdir(webdir, 0755) == 0);
	char sub[PAL_PATH_MAX];
	FMT(sub, "%s/sub", webdir);
	CHECK(mkdir(sub, 0755) == 0);
	put_text("secret.txt", SECRET);
	put_text("web/index.html", INDEX);
	put_text("web/sub/page.txt", "nested\n");
	put_text("web/style.css", "body{}");
	put_text("web/app.js", "console.log(1);");
	put_text("web/app.js.gz", "GZIP-OF-APP-JS");
	put_text("web/only.mjs.gz", "GZIP-ONLY");
	put_text("web/.hidden", "hidden");
	put_text("web/empty.txt", "");
	uint8_t *big = malloc(BIG_SIZE);
	CHECK(big != NULL);
	for (size_t i = 0; i < BIG_SIZE; i++)
		big[i] = pattern(i);
	put_file("web/big.bin", big, BIG_SIZE);
	free(big);
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
	(void)st;
	(void)flag;
	(void)ftw;
	return remove(path);
}

static void remove_webdir(void)
{
	CHECK(nftw(root, remove_entry, 8, FTW_DEPTH | FTW_PHYS) == 0);
}

/* ---- Loop ------------------------------------------------------------------- */

static uint64_t short_tick(void *ctx, uint64_t now_ms)
{
	(void)ctx;
	(void)now_ms;
	return 10;
}

/* A loop with a web server on webdir; cfg_fn may adjust its config. */
static void setup_with(void (*cfg_fn)(WebServerConfig *))
{
	runtime_init(&rt);
	CHECK(loop_init(&loop, &rt) == PAL_OK);
	CHECK(loop_add_tick(&loop, short_tick, NULL));
	WebServerConfig cfg;
	web_server_config_defaults(&cfg, "127.0.0.1", 0, webdir);
	if (cfg_fn != NULL)
		cfg_fn(&cfg);
	CHECK(web_server_open(&loop, &cfg, &server) == PAL_OK);
	CHECK(server != NULL && web_server_port(server) != 0);
}

static void setup(void)
{
	setup_with(NULL);
}

static void teardown(void)
{
	web_server_close(server);
	server = NULL;
	loop_deinit(&loop);
	runtime_deinit(&rt);
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

/* ---- Client ----------------------------------------------------------------- */

typedef struct {
	int fd;
	char *buf;
	size_t len, cap;
	bool eof;
	bool no_read; /* client_poll does not read (a stalled reader) */
} Client;

static Client client_connect_rcvbuf(int rcvbuf)
{
	Client cl = {0};
	cl.fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	CHECK(cl.fd >= 0);
	if (rcvbuf > 0)
		CHECK(setsockopt(cl.fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) == 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(web_server_port(server))};
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(connect(cl.fd, (struct sockaddr *)&a, sizeof(a)) == 0);
	CHECK(fcntl(cl.fd, F_SETFL, O_NONBLOCK) == 0);
	return cl;
}

static Client client_connect(void)
{
	return client_connect_rcvbuf(0);
}

static void client_close(Client *cl)
{
	close(cl->fd);
	free(cl->buf);
	memset(cl, 0, sizeof(*cl));
}

static void client_send(Client *cl, const char *text)
{
	size_t len = strlen(text);
	CHECK(send(cl->fd, text, len, MSG_NOSIGNAL) == (ssize_t)len);
}

static void client_poll(Client *cl)
{
	while (!cl->no_read) {
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

typedef struct {
	int status;
	char head[4096]; /* NUL-terminated, the final CRLF CRLF included */
	char *body;	 /* malloc'd, NUL-terminated */
	size_t body_len;
} Resp;

static void resp_free(Resp *r)
{
	free(r->body);
	r->body = NULL;
}

/* A field's value, or NULL; exact name case (as the server writes it). */
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
	Client *cl;
	bool head_only;
	Resp *r;
} Want;

/* Takes a complete response from the front of the client's buffer. */
static bool take_response(void *arg)
{
	Want *w = arg;
	Client *cl = w->cl;
	client_poll(cl);
	if (cl->len == 0)
		return false;
	char *end = NULL;
	for (size_t i = 0; i + 4 <= cl->len; i++)
		if (memcmp(cl->buf + i, "\r\n\r\n", 4) == 0) {
			end = cl->buf + i + 4;
			break;
		}
	if (end == NULL)
		return false;
	size_t hl = (size_t)(end - cl->buf);
	if (hl >= sizeof(w->r->head))
		return false;
	memcpy(w->r->head, cl->buf, hl);
	w->r->head[hl] = '\0';
	const char *clen = header(w->r, "Content-Length");
	size_t body = clen != NULL ? (size_t)strtoull(clen, NULL, 10) : 0;
	if (w->head_only)
		body = 0;
	if (cl->len < hl + body)
		return false;
	w->r->status = 0;
	sscanf(w->r->head, "HTTP/1.1 %d", &w->r->status);
	w->r->body = malloc(body + 1);
	CHECK(w->r->body != NULL);
	memcpy(w->r->body, end, body);
	w->r->body[body] = '\0';
	w->r->body_len = body;
	memmove(cl->buf, cl->buf + hl + body, cl->len - hl - body);
	cl->len -= hl + body;
	return true;
}

static bool get_response(Client *cl, bool head_only, Resp *r)
{
	Want w = {cl, head_only, r};
	bool ok = run_until(take_response, &w);
	CHECK(ok);
	return ok;
}

/* One request on a fresh connection: method target, extra header lines
 * (each ending in \r\n). Returns the response; the connection is closed. */
static Resp request(const char *method, const char *target, const char *extra)
{
	Resp r = {0};
	Client cl = client_connect();
	char text[1024];
	snprintf(text, sizeof(text), "%s %s HTTP/1.1\r\nHost: test\r\n%s\r\n", method, target,
		 extra != NULL ? extra : "");
	client_send(&cl, text);
	get_response(&cl, strcmp(method, "HEAD") == 0, &r);
	client_close(&cl);
	return r;
}

static Resp get(const char *target)
{
	return request("GET", target, NULL);
}

/* ---- Unit: paths and types ---------------------------------------------------- */

static bool route;

static void check_path(const char *target, enum web_path expect, const char *rel)
{
	char out[PAL_PATH_MAX];
	enum web_path res = web_path_from_target(target, out, sizeof(out), &route);
	if (res != expect)
		fprintf(stderr, "path %s: %d, expected %d\n", target, (int)res, (int)expect);
	CHECK(res == expect);
	if (rel != NULL && res == WEB_PATH_OK)
		CHECK_STREQ(out, rel);
}

static void test_paths(void)
{
	check_path("/", WEB_PATH_OK, "index.html");
	check_path("/app.js", WEB_PATH_OK, "app.js");
	check_path("/sub/page.txt?x=1#y", WEB_PATH_OK, "sub/page.txt");
	check_path("/sub/", WEB_PATH_OK, "sub/index.html");
	check_path("//sub//page.txt", WEB_PATH_OK, "sub/page.txt");
	check_path("/a%20b%C3%A9", WEB_PATH_OK, "a b\xc3\xa9");
	check_path("/?../../etc/passwd", WEB_PATH_OK, "index.html");
	check_path("/a..b/c.d", WEB_PATH_OK, "a..b/c.d");
	/* Never outside webdir. */
	check_path("/..", WEB_PATH_BAD, NULL);
	check_path("/../secret.txt", WEB_PATH_BAD, NULL);
	check_path("/sub/../../secret.txt", WEB_PATH_BAD, NULL);
	check_path("/%2e%2e/secret.txt", WEB_PATH_BAD, NULL);
	check_path("/%2E%2e", WEB_PATH_BAD, NULL);
	check_path("/.", WEB_PATH_BAD, NULL);
	check_path("/./index.html", WEB_PATH_BAD, NULL);
	check_path("/..%2fsecret.txt", WEB_PATH_BAD, NULL);
	check_path("/%2F..%2Fsecret.txt", WEB_PATH_BAD, NULL);
	check_path("/sub%2fpage.txt", WEB_PATH_BAD, NULL);
	check_path("/..\\secret.txt", WEB_PATH_BAD, NULL);
	check_path("/%5c..%5csecret.txt", WEB_PATH_BAD, NULL);
	check_path("/index.html%00.txt", WEB_PATH_BAD, NULL);
	check_path("/%0a", WEB_PATH_BAD, NULL);
	check_path("/%7f", WEB_PATH_BAD, NULL);
	check_path("/%", WEB_PATH_BAD, NULL);
	check_path("/%4", WEB_PATH_BAD, NULL);
	check_path("/%zz", WEB_PATH_BAD, NULL);
	check_path("http://h/../secret.txt", WEB_PATH_BAD, NULL);
	check_path("*", WEB_PATH_BAD, NULL);
	check_path("secret.txt", WEB_PATH_BAD, NULL);
	/* Hidden files. */
	check_path("/.hidden", WEB_PATH_NOT_FOUND, NULL);
	check_path("/.git/config", WEB_PATH_NOT_FOUND, NULL);
	check_path("/%2egit/config", WEB_PATH_NOT_FOUND, NULL);
	check_path("/...", WEB_PATH_NOT_FOUND, NULL);
	/* Too long for the output. */
	char small[8];
	CHECK(web_path_from_target("/abcdefgh", small, sizeof(small), &route) == WEB_PATH_NOT_FOUND);
	CHECK(web_path_from_target("/abcdef/", small, sizeof(small), &route) == WEB_PATH_NOT_FOUND);
	CHECK(web_path_from_target("/abcdef", small, sizeof(small), &route) == WEB_PATH_OK);

	/* Which unmatched paths fall back to the entry document. */
	check_path("/a/b.js", WEB_PATH_OK, "a/b.js");
	CHECK(!route);
	check_path("/a.d/b", WEB_PATH_OK, "a.d/b");
	CHECK(route);
	check_path("/graphs/x?y=a.b", WEB_PATH_OK, "graphs/x");
	CHECK(route);
	check_path("/sub/", WEB_PATH_OK, "sub/index.html");
	CHECK(route);
	check_path("/", WEB_PATH_OK, "index.html");
	CHECK(route);
	check_path("/sub/index.html", WEB_PATH_OK, "sub/index.html");
	CHECK(!route);
}

static void test_content_types(void)
{
	CHECK_STREQ(web_content_type("index.html"), "text/html; charset=utf-8");
	CHECK_STREQ(web_content_type("a/b.JS"), "text/javascript; charset=utf-8");
	CHECK_STREQ(web_content_type("m.mjs"), "text/javascript; charset=utf-8");
	CHECK_STREQ(web_content_type("s.css"), "text/css; charset=utf-8");
	CHECK_STREQ(web_content_type("d.json"), "application/json");
	CHECK_STREQ(web_content_type("i.svg"), "image/svg+xml");
	CHECK_STREQ(web_content_type("i.png"), "image/png");
	CHECK_STREQ(web_content_type("i.jpg"), "image/jpeg");
	CHECK_STREQ(web_content_type("favicon.ico"), "image/x-icon");
	CHECK_STREQ(web_content_type("f.woff2"), "font/woff2");
	CHECK_STREQ(web_content_type("m.wasm"), "application/wasm");
	CHECK_STREQ(web_content_type("app.js.map"), "application/json");
	CHECK_STREQ(web_content_type("r.txt"), "text/plain; charset=utf-8");
	CHECK_STREQ(web_content_type("x.bin"), "application/octet-stream");
	CHECK_STREQ(web_content_type("noext"), "application/octet-stream");
	CHECK_STREQ(web_content_type("dir.d/noext"), "application/octet-stream");
}

/* ---- Serving ------------------------------------------------------------------ */

static void test_serve_files(void)
{
	setup();
	Resp r = get("/");
	CHECK(r.status == 200);
	CHECK_STREQ(r.body, INDEX);
	CHECK_STREQ(header(&r, "Content-Type"), "text/html; charset=utf-8");
	CHECK(strtoul(header(&r, "Content-Length"), NULL, 10) == strlen(INDEX));
	CHECK_STREQ(header(&r, "X-Content-Type-Options"), "nosniff");
	CHECK_STREQ(header(&r, "Cache-Control"), "no-cache");
	CHECK_STREQ(header(&r, "Vary"), "Accept-Encoding");
	CHECK(header(&r, "Content-Encoding") == NULL);
	CHECK(header(&r, "Connection") == NULL);
	resp_free(&r);

	r = get("/sub/page.txt?cache=1");
	CHECK(r.status == 200);
	CHECK_STREQ(r.body, "nested\n");
	CHECK_STREQ(header(&r, "Content-Type"), "text/plain; charset=utf-8");
	resp_free(&r);

	r = get("/style.css");
	CHECK(r.status == 200 && strcmp(r.body, "body{}") == 0);
	CHECK_STREQ(header(&r, "Content-Type"), "text/css; charset=utf-8");
	resp_free(&r);

	r = get("/empty.txt");
	CHECK(r.status == 200 && r.body_len == 0);
	CHECK_STREQ(header(&r, "Content-Length"), "0");
	resp_free(&r);

	/* HEAD: the same head, no body. */
	r = request("HEAD", "/style.css", NULL);
	CHECK(r.status == 200 && r.body_len == 0);
	CHECK_STREQ(header(&r, "Content-Length"), "6");
	resp_free(&r);
	teardown();
}

static void test_not_found_and_fallback(void)
{
	setup();
	/* A missing asset with an extension: a real 404. */
	Resp r = get("/missing.js");
	CHECK(r.status == 404);
	CHECK_STREQ(header(&r, "Content-Type"), "text/plain; charset=utf-8");
	CHECK_STREQ(r.body, "404 Not Found\n");
	resp_free(&r);
	r = get("/sub/missing.css");
	CHECK(r.status == 404);
	resp_free(&r);

	/* An extension-less route: the entry document. */
	r = get("/graphs/1234-abcd");
	CHECK(r.status == 200);
	CHECK_STREQ(r.body, INDEX);
	CHECK_STREQ(header(&r, "Content-Type"), "text/html; charset=utf-8");
	resp_free(&r);
	r = get("/sub"); /* a directory */
	CHECK(r.status == 200 && strcmp(r.body, INDEX) == 0);
	resp_free(&r);
	r = get("/sub/"); /* no sub/index.html */
	CHECK(r.status == 200 && strcmp(r.body, INDEX) == 0);
	resp_free(&r);

	/* Hidden files are not served. */
	r = get("/.hidden");
	CHECK(r.status == 404);
	resp_free(&r);
	teardown();
}

static void test_gzip(void)
{
	setup();
	Resp r = request("GET", "/app.js", "Accept-Encoding: gzip, deflate, br\r\n");
	CHECK(r.status == 200);
	CHECK_STREQ(r.body, "GZIP-OF-APP-JS");
	CHECK_STREQ(header(&r, "Content-Encoding"), "gzip");
	CHECK_STREQ(header(&r, "Content-Type"), "text/javascript; charset=utf-8");
	CHECK_STREQ(header(&r, "Vary"), "Accept-Encoding");
	resp_free(&r);

	r = get("/app.js");
	CHECK(r.status == 200);
	CHECK_STREQ(r.body, "console.log(1);");
	CHECK(header(&r, "Content-Encoding") == NULL);
	CHECK_STREQ(header(&r, "Vary"), "Accept-Encoding");
	resp_free(&r);

	r = request("GET", "/app.js", "Accept-Encoding: gzip;q=0, br\r\n");
	CHECK(r.status == 200);
	CHECK_STREQ(r.body, "console.log(1);");
	CHECK(header(&r, "Content-Encoding") == NULL);
	resp_free(&r);

	/* Stored only compressed. */
	r = request("GET", "/only.mjs", "Accept-Encoding: gzip\r\n");
	CHECK(r.status == 200 && strcmp(r.body, "GZIP-ONLY") == 0);
	CHECK_STREQ(header(&r, "Content-Type"), "text/javascript; charset=utf-8");
	resp_free(&r);
	r = get("/only.mjs");
	CHECK(r.status == 404);
	resp_free(&r);
	teardown();
}

static void test_methods(void)
{
	setup();
	/* No body: answered, the connection persists. */
	Client cl = client_connect();
	client_send(&cl, "DELETE /index.html HTTP/1.1\r\nHost: t\r\n\r\n");
	Resp r = {0};
	get_response(&cl, false, &r);
	CHECK(r.status == 405);
	CHECK_STREQ(header(&r, "Allow"), "GET, HEAD");
	CHECK(header(&r, "Connection") == NULL);
	resp_free(&r);
	client_send(&cl, "GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
	get_response(&cl, false, &r);
	CHECK(r.status == 200 && strcmp(r.body, "body{}") == 0);
	resp_free(&r);
	client_close(&cl);

	/* With a body: answered, then closed without reading the body. */
	cl = client_connect();
	client_send(&cl, "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 11\r\n\r\nhello world");
	get_response(&cl, false, &r);
	CHECK(r.status == 405);
	CHECK_STREQ(header(&r, "Allow"), "GET, HEAD");
	CHECK_STREQ(header(&r, "Connection"), "close");
	CHECK_STREQ(r.body, "405 Method Not Allowed\n");
	resp_free(&r);
	CHECK(run_until(client_eof, &cl));
	client_close(&cl);
	teardown();
}

/* Every attempt is refused, and nothing of the file outside webdir leaks. */
static void test_traversal(void)
{
	setup();
	static const char *const targets[] = {
		"/../secret.txt",
		"/%2e%2e/secret.txt",
		"/%2E%2E%2Fsecret.txt",
		"/..%2Fsecret.txt",
		"/sub/../../secret.txt",
		"/sub/%2e%2e/%2e%2e/secret.txt",
		"//../secret.txt",
		"/..\\secret.txt",
		"/%5c..%5csecret.txt",
		"/%00",
		"/index.html%00.txt",
		"/%2e%2e%2f%2e%2e%2fsecret.txt",
		"http://test/../secret.txt",
		"/./secret.txt",
	};
	for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
		Resp r = get(targets[i]);
		if (r.status != 400 && r.status != 404)
			fprintf(stderr, "traversal %s: %d\n", targets[i], r.status);
		CHECK(r.status == 400 || r.status == 404);
		CHECK(r.body != NULL && strstr(r.body, "SECRET") == NULL);
		resp_free(&r);
	}
	teardown();
}

static void test_bad_requests(void)
{
	setup();
	/* Malformed: 400 and close. */
	Client cl = client_connect();
	client_send(&cl, "GET  / HTTP/1.1\r\nHost: t\r\n\r\n");
	Resp r = {0};
	get_response(&cl, false, &r);
	CHECK(r.status == 400);
	CHECK_STREQ(header(&r, "Connection"), "close");
	resp_free(&r);
	CHECK(run_until(client_eof, &cl));
	client_close(&cl);

	/* Oversized head: 431 and close, the rest of it discarded. */
	cl = client_connect();
	char big[HTTP_HEAD_MAX + 512];
	size_t n = (size_t)snprintf(big, sizeof(big), "GET / HTTP/1.1\r\nHost: t\r\nX-Big: ");
	memset(big + n, 'a', sizeof(big) - n - 5);
	memcpy(big + sizeof(big) - 5, "\r\n\r\n", 5);
	client_send(&cl, big);
	get_response(&cl, false, &r);
	CHECK(r.status == 431);
	CHECK_STREQ(header(&r, "Connection"), "close");
	resp_free(&r);
	CHECK(run_until(client_eof, &cl));
	client_close(&cl);

	/* HTTP/2 preface: 505. */
	cl = client_connect();
	client_send(&cl, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");
	get_response(&cl, false, &r);
	CHECK(r.status == 505);
	resp_free(&r);
	CHECK(run_until(client_eof, &cl));
	client_close(&cl);
	teardown();
}

/* ---- Connections --------------------------------------------------------------- */

static void test_keep_alive(void)
{
	setup();
	Client cl = client_connect();
	Resp r = {0};
	/* Two requests, one after the other. */
	client_send(&cl, "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
	get_response(&cl, false, &r);
	CHECK(r.status == 200 && strcmp(r.body, INDEX) == 0);
	resp_free(&r);
	client_send(&cl, "HEAD /sub/page.txt HTTP/1.1\r\nHost: t\r\n\r\n");
	get_response(&cl, true, &r);
	CHECK(r.status == 200 && r.body_len == 0);
	resp_free(&r);
	client_send(&cl, "GET /sub/page.txt HTTP/1.1\r\nHost: t\r\n\r\n");
	get_response(&cl, false, &r);
	CHECK(r.status == 200 && strcmp(r.body, "nested\n") == 0);
	resp_free(&r);

	/* Pipelined: three in one send, answered in order; the last one
	 * closes. */
	client_send(&cl, "GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n"
			 "GET /missing.png HTTP/1.1\r\nHost: t\r\n\r\n"
			 "GET /app.js HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
	get_response(&cl, false, &r);
	CHECK(r.status == 200 && strcmp(r.body, "body{}") == 0);
	resp_free(&r);
	get_response(&cl, false, &r);
	CHECK(r.status == 404);
	resp_free(&r);
	get_response(&cl, false, &r);
	CHECK(r.status == 200 && strcmp(r.body, "console.log(1);") == 0);
	CHECK_STREQ(header(&r, "Connection"), "close");
	resp_free(&r);
	CHECK(run_until(client_eof, &cl));
	CHECK(cl.len == 0);
	client_close(&cl);

	/* HTTP/1.0 closes after its response. */
	cl = client_connect();
	client_send(&cl, "GET /style.css HTTP/1.0\r\n\r\n");
	get_response(&cl, false, &r);
	CHECK(r.status == 200);
	CHECK_STREQ(header(&r, "Connection"), "close");
	resp_free(&r);
	CHECK(run_until(client_eof, &cl));
	client_close(&cl);
	teardown();
}

typedef struct {
	Client *cl;
	size_t want;
} Count;

static bool client_has(void *arg)
{
	Count *c = arg;
	client_poll(c->cl);
	return c->cl->len >= c->want;
}

static bool server_stalled(void *arg)
{
	(void)arg;
	return loop.conn_count == 1 && loop_conn_pending(loop.conns[0]) > 0;
}

static void test_large_file_slow_reader(void)
{
	setup();
	Client cl = client_connect_rcvbuf(4096);
	cl.no_read = true;
	client_send(&cl, "GET /big.bin HTTP/1.1\r\nHost: t\r\n\r\n");
	/* The client does not read: the socket fills, the server waits with
	 * its buffer pending (and has not read the file ahead). */
	CHECK(run_until(server_stalled, NULL));
	for (int i = 0; i < 20; i++)
		loop_run_once(&loop);
	CHECK(server_stalled(NULL));
	CHECK(loop_conn_pending(loop.conns[0]) <= WEB_OUT_CAP);

	/* Now it reads: every byte arrives intact. */
	cl.no_read = false;
	Resp r = {0};
	get_response(&cl, false, &r);
	CHECK(r.status == 200);
	CHECK_STREQ(header(&r, "Content-Type"), "application/octet-stream");
	CHECK(r.body_len == BIG_SIZE);
	size_t bad = 0;
	for (size_t i = 0; i < r.body_len; i++)
		if ((uint8_t)r.body[i] != pattern(i))
			bad++;
	CHECK(bad == 0);
	resp_free(&r);

	/* The connection is still usable. */
	client_send(&cl, "GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
	get_response(&cl, false, &r);
	CHECK(r.status == 200 && strcmp(r.body, "body{}") == 0);
	resp_free(&r);
	client_close(&cl);
	teardown();
}

static void short_timeouts(WebServerConfig *cfg)
{
	cfg->idle_timeout_ms = 100;
	cfg->head_timeout_ms = 150;
	cfg->send_timeout_ms = 150;
}

static void test_timeouts(void)
{
	setup_with(short_timeouts);
	/* Idle: closed without a response. */
	Client cl = client_connect();
	uint64_t t0 = pal_uptime_ms();
	CHECK(run_until(client_eof, &cl));
	CHECK(pal_uptime_ms() - t0 >= 100);
	CHECK(cl.len == 0);
	client_close(&cl);

	/* An incomplete head: 408 and close. */
	cl = client_connect();
	client_send(&cl, "GET / HTTP/1.1\r\nHost: t\r\n");
	Resp r = {0};
	get_response(&cl, false, &r);
	CHECK(r.status == 408);
	CHECK_STREQ(header(&r, "Connection"), "close");
	resp_free(&r);
	CHECK(run_until(client_eof, &cl));
	client_close(&cl);

	/* Idle after a response (keep-alive). */
	cl = client_connect();
	client_send(&cl, "GET /style.css HTTP/1.1\r\nHost: t\r\n\r\n");
	get_response(&cl, false, &r);
	CHECK(r.status == 200);
	resp_free(&r);
	CHECK(run_until(client_eof, &cl));
	CHECK(cl.len == 0);
	client_close(&cl);

	/* A reader that stops: the send times out and the connection goes. */
	cl = client_connect_rcvbuf(4096);
	cl.no_read = true;
	client_send(&cl, "GET /big.bin HTTP/1.1\r\nHost: t\r\n\r\n");
	CHECK(run_until(server_stalled, NULL));
	uint64_t end = pal_uptime_ms() + WAIT_MS;
	while (loop.conn_count > 0 && pal_uptime_ms() < end)
		loop_run_once(&loop);
	CHECK(loop.conn_count == 0);
	client_close(&cl);
	teardown();
}

static void test_close_with_connections(void)
{
	setup();
	Client a = client_connect();
	Client b = client_connect();
	client_send(&b, "GET /big.bin HTTP/1.1\r\nHost: t\r\n\r\n");
	Count c = {&b, 1000};
	CHECK(run_until(client_has, &c));
	CHECK(loop.conn_count == 2);
	/* Closing the server closes both (asan: nothing leaks). */
	web_server_close(server);
	server = NULL;
	CHECK(loop.conn_count == 0 && loop.listener_count == 0);
	CHECK(run_until(client_eof, &a));
	client_close(&a);
	client_close(&b);
	loop_deinit(&loop);
	runtime_deinit(&rt);

	/* The loop's shutdown closes the listener first; closing the server
	 * afterwards is still safe. */
	setup();
	Client d = client_connect();
	pal_request_stop();
	loop_run(&loop);
	CHECK(loop.listener_count == 0);
	CHECK(web_server_port(server) == 0);
	client_close(&d);
	teardown();
}

static void test_open_config(void)
{
	runtime_init(&rt);
	CHECK(loop_init(&loop, &rt) == PAL_OK);
	Config cfg;
	ConfigDefaults d = {.confdir = root, .webdir = webdir, .datadir = root};
	CHECK(config_init(&cfg, &d) == CONFIG_OK);
	snprintf(cfg.host, sizeof(cfg.host), "127.0.0.1");
	/* A free port: bound once, then released. */
	pal_socket s;
	uint16_t port = 0;
	CHECK(pal_socket_listen("127.0.0.1", 0, 1, &s) == PAL_OK);
	CHECK(pal_socket_local_port(s, &port) == PAL_OK);
	pal_socket_close(s);
	cfg.port = port;
	CHECK(web_server_open_config(&loop, &cfg, &server) == PAL_OK);
	CHECK(web_server_port(server) == port);
	Resp r = get("/");
	CHECK(r.status == 200 && strcmp(r.body, INDEX) == 0);
	resp_free(&r);
	/* The same port again: refused (startup refuses to start). */
	WebServer *second = (WebServer *)&second;
	CHECK(web_server_open_config(&loop, &cfg, &second) == PAL_ERR_ADDR_IN_USE);
	CHECK(second == NULL);
	cfg.port = 0;
	CHECK(web_server_open_config(&loop, &cfg, &second) == PAL_ERR_INVALID);
	web_server_close(server);
	server = NULL;

	WebServerConfig wc;
	web_server_config_defaults(&wc, "127.0.0.1", 0, "");
	CHECK(web_server_open(&loop, &wc, &second) == PAL_ERR_INVALID);
	web_server_config_defaults(&wc, "127.0.0.1", 0, webdir);
	wc.out_cap = 100;
	CHECK(web_server_open(&loop, &wc, &second) == PAL_ERR_INVALID);
	CHECK(loop.tick_count == 0);
	loop_deinit(&loop);
	runtime_deinit(&rt);
}

/* ---- PAL streaming reads -------------------------------------------------------- */

static void test_pal_file(void)
{
	char path[PAL_PATH_MAX];
	FMT(path, "%s/big.bin", webdir);
	pal_file f;
	uint64_t size = 0;
	CHECK(pal_file_open(path, &f, &size) == PAL_OK);
	CHECK(size == BIG_SIZE);
	static uint8_t chunk[70000];
	size_t total = 0, len, bad = 0;
	while (pal_file_read(f, chunk, sizeof(chunk), &len) == PAL_OK && len > 0) {
		for (size_t i = 0; i < len; i++)
			if (chunk[i] != pattern(total + i))
				bad++;
		total += len;
	}
	CHECK(total == BIG_SIZE && bad == 0);
	CHECK(pal_file_read(f, chunk, sizeof(chunk), &len) == PAL_OK && len == 0);
	pal_file_close(f);

	CHECK(pal_file_open(webdir, &f, NULL) == PAL_ERR_INVALID);
	CHECK(f == PAL_FILE_NONE);
	FMT(path, "%s/nope", webdir);
	CHECK(pal_file_open(path, &f, NULL) == PAL_ERR_NOT_FOUND);
	FMT(path, "%s/index.html/x", webdir);
	CHECK(pal_file_open(path, &f, NULL) == PAL_ERR_NOT_FOUND);
	/* A FIFO: refused without blocking. */
	FMT(path, "%s/fifo", root);
	CHECK(mkfifo(path, 0600) == 0);
	CHECK(pal_file_open(path, &f, NULL) == PAL_ERR_INVALID);
	CHECK(unlink(path) == 0);
	pal_file_close(PAL_FILE_NONE);
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	make_webdir();
	RUN(test_paths);
	RUN(test_content_types);
	RUN(test_pal_file);
	RUN(test_serve_files);
	RUN(test_not_found_and_fallback);
	RUN(test_gzip);
	RUN(test_methods);
	RUN(test_traversal);
	RUN(test_bad_requests);
	RUN(test_keep_alive);
	RUN(test_large_file_slow_reader);
	RUN(test_timeouts);
	RUN(test_close_with_connections);
	RUN(test_open_config);
	remove_webdir();
	return test_result();
}
