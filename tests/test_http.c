/*
 * The HTTP/1.1 request head parser (protocol/http.h, plan task 13): complete
 * and split input, limits, malformed heads, header lookup (including the
 * WebSocket upgrade's fields, plan task 14), keep-alive and Accept-Encoding.
 */
#include "test.h"

#include "protocol/http.h"

static char buf[HTTP_HEAD_MAX];
static HttpRequest req;

/* Parses text in one piece into the shared request. */
static enum http_parse parse(const char *text)
{
	http_request_init(&req, buf, sizeof(buf));
	return http_request_feed(&req, text, strlen(text), NULL);
}

/* Parses text one byte at a time; checks nothing completes early. */
static enum http_parse parse_bytes(HttpRequest *r, char *b, size_t cap, const char *text)
{
	http_request_init(r, b, cap);
	size_t len = strlen(text);
	enum http_parse res = HTTP_PARSE_INCOMPLETE;
	for (size_t i = 0; i < len && res == HTTP_PARSE_INCOMPLETE; i++) {
		size_t used = 0;
		res = http_request_feed(r, text + i, 1, &used);
		CHECK(used == 1);
	}
	return res;
}

#define UPGRADE                                                                                    \
	"GET /chat HTTP/1.1\r\n"                                                                   \
	"Host: 192.0.2.1:8081\r\n"                                                                 \
	"upgrade: websocket\r\n"                                                                   \
	"Connection: keep-alive, Upgrade\r\n"                                                      \
	"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"                                          \
	"Sec-WebSocket-Version: 13\r\n"                                                            \
	"Origin: http://192.0.2.1:8080\r\n"                                                        \
	"X-Empty:\r\n"                                                                             \
	"X-Spaces: \t a b \t\r\n"                                                                  \
	"\r\n"

static void check_upgrade(const HttpRequest *r)
{
	CHECK_STREQ(r->method, "GET");
	CHECK_STREQ(r->target, "/chat");
	CHECK(r->version_minor == 1);
	CHECK(r->header_count == 8);
	CHECK_STREQ(http_request_header(r, "host"), "192.0.2.1:8081");
	CHECK_STREQ(http_request_header(r, "Upgrade"), "websocket");
	CHECK_STREQ(http_request_header(r, "SEC-WEBSOCKET-KEY"), "dGhlIHNhbXBsZSBub25jZQ==");
	CHECK_STREQ(http_request_header(r, "Sec-WebSocket-Version"), "13");
	CHECK_STREQ(http_request_header(r, "Origin"), "http://192.0.2.1:8080");
	CHECK_STREQ(http_request_header(r, "X-Empty"), "");
	CHECK_STREQ(http_request_header(r, "X-Spaces"), "a b");
	CHECK(http_request_header(r, "Cookie") == NULL);
	CHECK(http_request_header_count(r, "sec-websocket-key") == 1);
	CHECK(http_request_header_count(r, "Cookie") == 0);
	CHECK(http_request_header_has_token(r, "Connection", "upgrade"));
	CHECK(http_request_header_has_token(r, "Connection", "Keep-Alive"));
	CHECK(!http_request_header_has_token(r, "Connection", "close"));
	CHECK(http_request_header_has_token(r, "Upgrade", "WebSocket"));
	CHECK(!r->has_body);
	CHECK(http_request_keep_alive(r));
}

static void test_complete(void)
{
	CHECK(parse(UPGRADE) == HTTP_PARSE_COMPLETE);
	check_upgrade(&req);
	CHECK(req.head_len == strlen(UPGRADE));
	size_t left = 99;
	http_request_leftover(&req, &left);
	CHECK(left == 0);
	/* Final until the next request. */
	CHECK(http_request_parse(&req) == HTTP_PARSE_COMPLETE);
}

static void test_split(void)
{
	/* Byte by byte: the same result. */
	static char b2[HTTP_HEAD_MAX];
	HttpRequest r;
	CHECK(parse_bytes(&r, b2, sizeof(b2), UPGRADE) == HTTP_PARSE_COMPLETE);
	check_upgrade(&r);

	/* Through the read space, in two uneven parts. */
	http_request_init(&r, b2, sizeof(b2));
	const char *text = UPGRADE;
	size_t len = strlen(text), cut = 23;
	char *dst;
	CHECK(http_request_space(&r, &dst) == sizeof(b2));
	memcpy(dst, text, cut);
	CHECK(http_request_commit(&r, cut) == HTTP_PARSE_INCOMPLETE);
	CHECK(http_request_space(&r, &dst) == sizeof(b2) - cut);
	memcpy(dst, text + cut, len - cut);
	CHECK(http_request_commit(&r, len - cut) == HTTP_PARSE_COMPLETE);
	check_upgrade(&r);

	/* Incomplete heads stay incomplete. */
	CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n") == HTTP_PARSE_INCOMPLETE);
	CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\n\r") == HTTP_PARSE_INCOMPLETE);
}

static void test_pipelined(void)
{
	const char *two = "GET /a HTTP/1.1\r\nHost: h\r\n\r\nGET /b HTTP/1.1\r\nHost: h\r\n\r\nWS";
	CHECK(parse(two) == HTTP_PARSE_COMPLETE);
	CHECK_STREQ(req.target, "/a");
	size_t left;
	const char *rest = http_request_leftover(&req, &left);
	CHECK(left == strlen(two) - req.head_len);
	CHECK(rest != NULL && memcmp(rest, "GET /b", 6) == 0);
	http_request_next(&req);
	CHECK(req.len == left);
	CHECK(http_request_commit(&req, 0) == HTTP_PARSE_COMPLETE);
	CHECK_STREQ(req.target, "/b");
	rest = http_request_leftover(&req, &left);
	CHECK(left == 2 && memcmp(rest, "WS", 2) == 0);
	http_request_next(&req);
	CHECK(http_request_commit(&req, 0) == HTTP_PARSE_INCOMPLETE);
	CHECK(req.len == 2);
}

static void test_line_endings(void)
{
	/* Bare LF, and empty lines before the request line. */
	CHECK(parse("\r\n\nGET /x HTTP/1.1\nHost: h\n\n") == HTTP_PARSE_COMPLETE);
	CHECK_STREQ(req.target, "/x");
	CHECK_STREQ(http_request_header(&req, "Host"), "h");
	/* HTTP/1.0 needs no Host and does not persist. */
	CHECK(parse("GET / HTTP/1.0\r\n\r\n") == HTTP_PARSE_COMPLETE);
	CHECK(req.version_minor == 0);
	CHECK(!http_request_keep_alive(&req));
	CHECK(parse("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n") == HTTP_PARSE_COMPLETE);
	CHECK(!http_request_keep_alive(&req));
	CHECK(parse("GET / HTTP/1.1\r\nHost: h\r\nConnection: Close\r\n\r\n") ==
	      HTTP_PARSE_COMPLETE);
	CHECK(!http_request_keep_alive(&req));
	/* A later minor version is HTTP/1.1. */
	CHECK(parse("GET / HTTP/1.7\r\nHost: h\r\n\r\n") == HTTP_PARSE_COMPLETE);
	CHECK(http_request_keep_alive(&req));
}

static void test_limits(void)
{
	char small[64];
	HttpRequest r;
	/* The request line alone overflows: 414. */
	http_request_init(&r, small, sizeof(small));
	char line[100];
	memset(line, 'a', sizeof(line));
	memcpy(line, "GET /", 5);
	CHECK(http_request_feed(&r, line, sizeof(line), NULL) == HTTP_PARSE_URI_TOO_LONG);
	CHECK(http_parse_status(r.result) == 414);
	/* Later: 431, also byte by byte. */
	const char *big = "GET / HTTP/1.1\r\nHost: h\r\nX-Long: 0123456789012345678901234567890123\r\n\r\n";
	CHECK(parse_bytes(&r, small, sizeof(small), big) == HTTP_PARSE_HEAD_TOO_LARGE);
	CHECK(http_parse_status(HTTP_PARSE_HEAD_TOO_LARGE) == 431);
	/* Exactly fitting is fine. */
	const char *fit = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";
	HttpRequest r2;
	char exact[27];
	CHECK(strlen(fit) == sizeof(exact));
	CHECK(parse_bytes(&r2, exact, sizeof(exact), fit) == HTTP_PARSE_COMPLETE);

	/* Too many fields: 431 before the buffer is full. */
	char many[HTTP_HEAD_MAX];
	size_t n = (size_t)snprintf(many, sizeof(many), "GET / HTTP/1.1\r\nHost: h\r\n");
	for (int i = 0; i < HTTP_HEADERS_MAX; i++)
		n += (size_t)snprintf(many + n, sizeof(many) - n, "X-%d: v\r\n", i);
	CHECK(parse(many) == HTTP_PARSE_HEAD_TOO_LARGE);
	/* One fewer fits. */
	n = (size_t)snprintf(many, sizeof(many), "GET / HTTP/1.1\r\nHost: h\r\n");
	for (int i = 0; i < HTTP_HEADERS_MAX - 1; i++)
		n += (size_t)snprintf(many + n, sizeof(many) - n, "X-%d: v\r\n", i);
	snprintf(many + n, sizeof(many) - n, "\r\n");
	CHECK(parse(many) == HTTP_PARSE_COMPLETE);
	CHECK(req.header_count == HTTP_HEADERS_MAX);
}

static void test_malformed(void)
{
	static const char *const bad[] = {
		"GET  / HTTP/1.1\r\nHost: h\r\n\r\n",	      /* two spaces */
		"GET /\r\nHost: h\r\n\r\n",		      /* HTTP/0.9 */
		"GET / HTTP/1.1 \r\nHost: h\r\n\r\n",	      /* trailing space */
		"GET / HTTP/1\r\nHost: h\r\n\r\n",	      /* bad version */
		"GET / http/1.1\r\nHost: h\r\n\r\n",	      /* lower-case name */
		"G(T / HTTP/1.1\r\nHost: h\r\n\r\n",	      /* not a token */
		" GET / HTTP/1.1\r\nHost: h\r\n\r\n",	      /* leading space */
		"GET / HTTP/1.1\r\nHost : h\r\n\r\n",	      /* space before the colon */
		"GET / HTTP/1.1\r\nHost: h\r\n folded\r\n\r\n", /* obs-fold */
		"GET / HTTP/1.1\r\nHost: h\r\nNoColon\r\n\r\n",
		"GET / HTTP/1.1\r\nHost: h\r\n: empty name\r\n\r\n",
		("GET / HTTP/1.1\r\nHost: h\r\nX: a\x01" "b\r\n\r\n"), /* control byte */
		"GET / HTTP/1.1\r\nHost: h\r\nX: a\rb\r\n\r\n",	       /* bare CR */
		"GET / HTTP/1.1\r\n\r\n",			       /* no Host */
		"GET / HTTP/1.1\r\nHost: a\r\nhost: b\r\n\r\n",	       /* two Hosts */
		"GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 1x\r\n\r\n",
		"GET / HTTP/1.1\r\nHost: h\r\nContent-Length:\r\n\r\n",
		"GET / HTTP/1.1\r\nHost: h\r\nContent-Length: -1\r\n\r\n",
		"GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 99999999999999999999999\r\n\r\n",
		"GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
		"GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		enum http_parse res = parse(bad[i]);
		if (res != HTTP_PARSE_BAD)
			fprintf(stderr, "bad[%zu]: %d\n", i, (int)res);
		CHECK(res == HTTP_PARSE_BAD);
	}
	CHECK(http_parse_status(HTTP_PARSE_BAD) == 400);

	/* Binary garbage (a TLS handshake) fails without waiting for a line. */
	char tls[] = {0x16, 0x03, 0x01, 0x02, 0x00};
	http_request_init(&req, buf, sizeof(buf));
	CHECK(http_request_feed(&req, tls, sizeof(tls), NULL) == HTTP_PARSE_BAD);

	/* Another major version: 505. */
	CHECK(parse("GET / HTTP/2.0\r\nHost: h\r\n\r\n") == HTTP_PARSE_VERSION);
	CHECK(http_parse_status(HTTP_PARSE_VERSION) == 505);

	/* An error is final; next starts over. */
	CHECK(parse("GET  / HTTP/1.1\r\n\r\n") == HTTP_PARSE_BAD);
	CHECK(http_request_feed(&req, "x", 1, NULL) == HTTP_PARSE_BAD);
	http_request_next(&req);
	CHECK(req.len == 0);
	CHECK(http_request_feed(&req, "GET / HTTP/1.0\r\n\r\n", 18, NULL) == HTTP_PARSE_COMPLETE);
}

static void test_body_and_values(void)
{
	CHECK(parse("POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 12\r\ncontent-length: 12\r\n\r\n") ==
	      HTTP_PARSE_COMPLETE);
	CHECK(req.has_body && req.content_length == 12);
	CHECK(parse("GET / HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n") == HTTP_PARSE_COMPLETE);
	CHECK(!req.has_body);
	CHECK(parse("GET / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n") ==
	      HTTP_PARSE_COMPLETE);
	CHECK(req.has_body);
	/* obs-text in a value, any method token, the target as sent. */
	CHECK(parse("M-SEARCH /a%20b?q=1&r=%00 HTTP/1.1\r\nHost: h\r\nX: caf\xc3\xa9\r\n\r\n") ==
	      HTTP_PARSE_COMPLETE);
	CHECK_STREQ(req.method, "M-SEARCH");
	CHECK_STREQ(req.target, "/a%20b?q=1&r=%00");
	CHECK_STREQ(http_request_header(&req, "x"), "caf\xc3\xa9");
	CHECK_STREQ(http_reason(404), "Not Found");
	CHECK_STREQ(http_reason(431), "Request Header Fields Too Large");
	CHECK(http_str_ieq("Content-LENGTH", "content-length"));
	CHECK(!http_str_ieq("Content-Length", "Content-Lengt"));
}

/* Accept-Encoding of one header (or none for NULL) allows gzip? */
static bool gzip_ok(const char *value)
{
	char text[512];
	if (value == NULL)
		snprintf(text, sizeof(text), "GET / HTTP/1.1\r\nHost: h\r\n\r\n");
	else
		snprintf(text, sizeof(text), "GET / HTTP/1.1\r\nHost: h\r\nAccept-Encoding: %s\r\n\r\n",
			 value);
	CHECK(parse(text) == HTTP_PARSE_COMPLETE);
	return http_request_accepts_encoding(&req, "gzip");
}

static void test_accept_encoding(void)
{
	CHECK(!gzip_ok(NULL));
	CHECK(!gzip_ok(""));
	CHECK(gzip_ok("gzip"));
	CHECK(gzip_ok("GZIP"));
	CHECK(gzip_ok("gzip, deflate, br, zstd"));
	CHECK(gzip_ok("br;q=1.0, gzip;q=0.8, *;q=0.1"));
	CHECK(gzip_ok("deflate , gzip ; q=0.001"));
	CHECK(gzip_ok("x-gzip"));
	CHECK(gzip_ok("*"));
	CHECK(gzip_ok("gzip;q=1"));
	CHECK(gzip_ok("gzip;q=1.000"));
	CHECK(!gzip_ok("gzip;q=0"));
	CHECK(!gzip_ok("gzip;q=0.0"));
	CHECK(!gzip_ok("gzip; q=0.000"));
	CHECK(!gzip_ok("gzip;Q=0"));
	CHECK(!gzip_ok("gzip;q=0, *"));	    /* named beats * */
	CHECK(!gzip_ok("*;q=0"));
	CHECK(!gzip_ok("deflate, br"));
	CHECK(!gzip_ok("gzipped"));
	CHECK(!gzip_ok("gzip;q=2"));	    /* invalid weight: ignored */
	CHECK(!gzip_ok("gzip;q=0.0001"));
	CHECK(!gzip_ok("gzip;q=1.5"));
	CHECK(!gzip_ok("gzip;q="));
	CHECK(gzip_ok("gzip;q=x, gzip"));   /* the valid entry counts */
	CHECK(gzip_ok("identity;q=0, gzip;level=9"));
	/* Several fields form one list. */
	CHECK(parse("GET / HTTP/1.1\r\nHost: h\r\nAccept-Encoding: br\r\nAccept-Encoding: gzip\r\n\r\n") ==
	      HTTP_PARSE_COMPLETE);
	CHECK(http_request_accepts_encoding(&req, "gzip"));
	CHECK(http_request_accepts_encoding(&req, "br"));
	CHECK(!http_request_accepts_encoding(&req, "deflate"));
}

int main(void)
{
	RUN(test_complete);
	RUN(test_split);
	RUN(test_pipelined);
	RUN(test_line_endings);
	RUN(test_limits);
	RUN(test_malformed);
	RUN(test_body_and_values);
	RUN(test_accept_encoding);
	return test_result();
}
