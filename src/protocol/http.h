/*
 * HTTP/1.1 request heads (RFC 9112): an incremental parser shared by the web
 * server (engine/web_server.h, 05 §3) and the WebSocket upgrade (02 §0, plan
 * task 14). It parses the request line and the header fields, nothing else:
 * a request body is never read (only GET and HEAD are served, 05 §3), and the
 * bytes after the head stay in the buffer for the caller (the next pipelined
 * request, or WebSocket frames after an upgrade).
 *
 * Naming: functions are prefixed http_, the types are HttpRequest and
 * HttpHeader, the result enum HTTP_PARSE_ / http_parse.
 *
 * Use: give the parser a buffer (http_request_init); read into its free space
 * (http_request_space) and commit what was read (http_request_commit), or
 * copy bytes in (http_request_feed); both parse what is new. Bytes may arrive
 * in any split, down to one at a time. Once the head is complete, the request
 * line and the header fields are NUL-terminated strings inside the buffer;
 * http_request_next drops the head and keeps the bytes after it.
 *
 * Decisions (plan task 13, 05 §3):
 *  - bounds: the whole head (request line, header lines, the empty line) must
 *    fit the caller's buffer (the web server's is HTTP_HEAD_MAX, 8 KiB), and
 *    there are at most HTTP_HEADERS_MAX (32) header fields. A buffer filled
 *    before the request line ends is HTTP_PARSE_URI_TOO_LONG (414), one filled
 *    later or too many fields HTTP_PARSE_HEAD_TOO_LARGE (431);
 *  - lines end in CRLF or a bare LF (RFC 9112 §2.2); empty lines before the
 *    request line are skipped;
 *  - the request line is method SP target SP version, single spaces: the
 *    method a token, the target visible ASCII (0x21-0x7E), the version
 *    "HTTP/1.<d>"; another major version is HTTP_PARSE_VERSION (505);
 *  - a field is name ":" OWS value OWS: the name a token, no space before the
 *    colon, no line folding; the value may hold HT, visible ASCII and bytes
 *    0x80-0xFF, no other control byte. Names compare case-insensitively;
 *  - HTTP/1.1 needs exactly one Host field, HTTP/1.0 at most one;
 *  - a body is announced by Content-Length (digits only, every copy equal)
 *    or Transfer-Encoding; both at once is malformed (request smuggling).
 *    The parser only reports it (has_body); the web server answers such a
 *    request and closes the connection instead of reading the body;
 *  - every other malformation is HTTP_PARSE_BAD (400). Results other than
 *    HTTP_PARSE_INCOMPLETE are final until http_request_next or
 *    http_request_init.
 *
 * Memory: no allocation; the request points into the caller's buffer, which
 * must outlive it. Portable core (01 §0): no PAL calls, no logging.
 */
#ifndef HAKKAS_PROTOCOL_HTTP_H
#define HAKKAS_PROTOCOL_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The web server's head buffer per connection, bytes. A port's build may
 * define it. */
#ifndef HTTP_HEAD_MAX
#define HTTP_HEAD_MAX 8192
#endif

/* The most header fields in one request. A port's build may define it. */
#ifndef HTTP_HEADERS_MAX
#define HTTP_HEADERS_MAX 32
#endif

enum http_parse {
	HTTP_PARSE_INCOMPLETE = 0,	/* more bytes needed */
	HTTP_PARSE_COMPLETE = 1,	/* the head is complete and valid */
	HTTP_PARSE_BAD = -1,		/* malformed: 400 */
	HTTP_PARSE_URI_TOO_LONG = -2,	/* request line beyond the buffer: 414 */
	HTTP_PARSE_HEAD_TOO_LARGE = -3, /* head beyond the buffer, or too many fields: 431 */
	HTTP_PARSE_VERSION = -4,	/* not HTTP/1.x: 505 */
};

/* The status code that answers a parse error (400, 414, 431, 505); 0 for
 * HTTP_PARSE_INCOMPLETE and HTTP_PARSE_COMPLETE. */
int http_parse_status(enum http_parse result);

/* The reason phrase of a status code this code sends ("Not Found"), or "". */
const char *http_reason(int status);

/* One header field; both strings NUL-terminated inside the buffer. The value
 * has no leading or trailing whitespace and may be empty. */
typedef struct {
	const char *name;
	const char *value;
} HttpHeader;

typedef struct {
	char *buf;
	size_t cap;
	size_t len; /* bytes in buf */

	/* The scanner (private). */
	size_t scan;	   /* next byte to look at */
	size_t start;	   /* where the request line starts (after skipped empty lines) */
	size_t line_start; /* where the current line starts */
	size_t lines;	   /* complete non-empty lines so far */
	enum http_parse result;

	/* Valid once the result is HTTP_PARSE_COMPLETE. */
	size_t head_len;      /* bytes of the head, the final empty line included */
	const char *method;   /* e.g. "GET" (case-sensitive, RFC 9110 §9.1) */
	const char *target;   /* the request-target as sent, e.g. "/a/b.js?x=1" */
	unsigned version_minor; /* 1 for HTTP/1.1, 0 for HTTP/1.0 */
	HttpHeader headers[HTTP_HEADERS_MAX];
	size_t header_count;
	bool has_body;		 /* Content-Length > 0 or Transfer-Encoding present */
	uint64_t content_length; /* 0 without the field */
} HttpRequest;

/* Makes r an empty request over buf (cap > 0 bytes). */
void http_request_init(HttpRequest *r, char *buf, size_t cap);

/* The free space after the bytes held: *dst receives where to read to; the
 * return value is how many bytes fit (0 when full). */
size_t http_request_space(HttpRequest *r, char **dst);

/* Adds n bytes that were written to the space (http_request_space) and
 * parses what is new. Returns the parse result (also when n is 0, e.g. to
 * parse the bytes kept by http_request_next). */
enum http_parse http_request_commit(HttpRequest *r, size_t n);

/* Copies as much of data (len bytes) as fits into the buffer and parses it;
 * *consumed (if not NULL) receives the bytes taken. Returns the result. */
enum http_parse http_request_feed(HttpRequest *r, const void *data, size_t len,
				  size_t *consumed);

/* Parses the bytes not looked at yet (http_request_commit does it). */
enum http_parse http_request_parse(HttpRequest *r);

/* The bytes after a complete head (the next request, or the first WebSocket
 * frames): a pointer into the buffer and their count in *len. NULL, *len 0,
 * before the head is complete. */
const char *http_request_leftover(const HttpRequest *r, size_t *len);

/* Starts the next request: drops a complete head and moves the bytes after
 * it to the front of the buffer (parse them with http_request_commit(r, 0));
 * after an error or before completion, drops everything. */
void http_request_next(HttpRequest *r);

/* The value of the first field named name (case-insensitive), or NULL. */
const char *http_request_header(const HttpRequest *r, const char *name);

/* How many fields are named name (case-insensitive): e.g. a WebSocket
 * upgrade needs exactly one Sec-WebSocket-Key. */
size_t http_request_header_count(const HttpRequest *r, const char *name);

/* True if a field named name holds token in its comma-separated list,
 * case-insensitively, ignoring parameters after ';' and surrounding
 * whitespace: Connection: keep-alive, Upgrade; Upgrade: websocket. Every
 * field of that name is searched (RFC 9110 §5.3). */
bool http_request_header_has_token(const HttpRequest *r, const char *name, const char *token);

/* Whether the connection persists after this request's response (decided,
 * plan task 13): HTTP/1.1 without "Connection: close". HTTP/1.0 always
 * closes. */
bool http_request_keep_alive(const HttpRequest *r);

/* Whether Accept-Encoding allows coding ("gzip"), RFC 9110 §12.5.3: an entry
 * naming it (or "x-gzip" for "gzip") with a weight above zero, or failing
 * that, "*" with a weight above zero. q=0 (also "0.0", "0.000") refuses; an
 * entry with an invalid weight is ignored. Without the field, or with an
 * empty one, false: a pre-compressed asset is only sent to a client that
 * asks for it (05 §3). */
bool http_request_accepts_encoding(const HttpRequest *r, const char *coding);

/* ASCII case-insensitive equality of two NUL-terminated strings (the core
 * has no strcasecmp: strings.h is a POSIX header). */
bool http_str_ieq(const char *a, const char *b);

#endif
