#include "protocol/http.h"

#include <string.h>

int http_parse_status(enum http_parse result)
{
	switch (result) {
	case HTTP_PARSE_BAD:
		return 400;
	case HTTP_PARSE_URI_TOO_LONG:
		return 414;
	case HTTP_PARSE_HEAD_TOO_LARGE:
		return 431;
	case HTTP_PARSE_VERSION:
		return 505;
	case HTTP_PARSE_INCOMPLETE:
	case HTTP_PARSE_COMPLETE:
		break;
	}
	return 0;
}

const char *http_reason(int status)
{
	switch (status) {
	case 101:
		return "Switching Protocols";
	case 200:
		return "OK";
	case 400:
		return "Bad Request";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 408:
		return "Request Timeout";
	case 414:
		return "URI Too Long";
	case 426:
		return "Upgrade Required";
	case 431:
		return "Request Header Fields Too Large";
	case 500:
		return "Internal Server Error";
	case 503:
		return "Service Unavailable";
	case 505:
		return "HTTP Version Not Supported";
	default:
		return "";
	}
}

/* ---- Characters --------------------------------------------------------------- */

static unsigned char lower(unsigned char c)
{
	return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

bool http_str_ieq(const char *a, const char *b)
{
	for (;; a++, b++) {
		if (lower((unsigned char)*a) != lower((unsigned char)*b))
			return false;
		if (*a == '\0')
			return true;
	}
}

/* Case-insensitive equality of len bytes at a with the string b. */
static bool ieq_n(const char *a, size_t len, const char *b)
{
	for (size_t i = 0; i < len; i++, b++)
		if (*b == '\0' || lower((unsigned char)a[i]) != lower((unsigned char)*b))
			return false;
	return *b == '\0';
}

/* tchar, RFC 9110 §5.6.2. */
static bool is_tchar(unsigned char c)
{
	if ((c >= '0' && c <= '9') || (lower(c) >= 'a' && lower(c) <= 'z'))
		return true;
	switch (c) {
	case '!':
	case '#':
	case '$':
	case '%':
	case '&':
	case '\'':
	case '*':
	case '+':
	case '-':
	case '.':
	case '^':
	case '_':
	case '`':
	case '|':
	case '~':
		return true;
	default:
		return false;
	}
}

static bool is_ows(char c)
{
	return c == ' ' || c == '\t';
}

static bool is_digit(char c)
{
	return c >= '0' && c <= '9';
}

/* ---- Buffer ------------------------------------------------------------------- */

static void reset_scan(HttpRequest *r)
{
	r->scan = 0;
	r->start = 0;
	r->line_start = 0;
	r->lines = 0;
	r->result = HTTP_PARSE_INCOMPLETE;
	r->head_len = 0;
	r->method = NULL;
	r->target = NULL;
	r->version_minor = 0;
	r->header_count = 0;
	r->has_body = false;
	r->content_length = 0;
}

void http_request_init(HttpRequest *r, char *buf, size_t cap)
{
	r->buf = buf;
	r->cap = cap;
	r->len = 0;
	reset_scan(r);
}

size_t http_request_space(HttpRequest *r, char **dst)
{
	*dst = r->buf + r->len;
	return r->cap - r->len;
}

enum http_parse http_request_commit(HttpRequest *r, size_t n)
{
	if (n > r->cap - r->len)
		n = r->cap - r->len;
	r->len += n;
	return http_request_parse(r);
}

enum http_parse http_request_feed(HttpRequest *r, const void *data, size_t len, size_t *consumed)
{
	char *dst;
	size_t n = http_request_space(r, &dst);
	if (n > len)
		n = len;
	memcpy(dst, data, n);
	if (consumed != NULL)
		*consumed = n;
	return http_request_commit(r, n);
}

const char *http_request_leftover(const HttpRequest *r, size_t *len)
{
	if (r->result != HTTP_PARSE_COMPLETE) {
		*len = 0;
		return NULL;
	}
	*len = r->len - r->head_len;
	return r->buf + r->head_len;
}

void http_request_next(HttpRequest *r)
{
	size_t keep = 0;
	if (r->result == HTTP_PARSE_COMPLETE) {
		keep = r->len - r->head_len;
		memmove(r->buf, r->buf + r->head_len, keep);
	}
	r->len = keep;
	reset_scan(r);
}

/* ---- The head ------------------------------------------------------------------ */

static enum http_parse fail(HttpRequest *r, enum http_parse result)
{
	r->result = result;
	return result;
}

/* The line starting at *pos within the complete head: [*b, *e) without its
 * CR LF; *pos moves past the LF. */
static void next_line(const HttpRequest *r, size_t *pos, size_t *b, size_t *e)
{
	size_t i = *pos;
	while (r->buf[i] != '\n')
		i++;
	*b = *pos;
	*e = i > *pos && r->buf[i - 1] == '\r' ? i - 1 : i;
	*pos = i + 1;
}

/* The request line [b, e): method SP target SP HTTP/1.d. */
static enum http_parse parse_request_line(HttpRequest *r, size_t b, size_t e)
{
	char *p = r->buf + b, *end = r->buf + e;
	char *method = p;
	while (p < end && is_tchar((unsigned char)*p))
		p++;
	if (p == method || p == end || *p != ' ')
		return HTTP_PARSE_BAD;
	*p++ = '\0';
	char *target = p;
	while (p < end && (unsigned char)*p > 0x20 && (unsigned char)*p < 0x7f)
		p++;
	if (p == target || p == end || *p != ' ')
		return HTTP_PARSE_BAD;
	*p++ = '\0';
	if (end - p != 8 || memcmp(p, "HTTP/", 5) != 0 || !is_digit(p[5]) || p[6] != '.' ||
	    !is_digit(p[7]))
		return HTTP_PARSE_BAD;
	if (p[5] != '1')
		return HTTP_PARSE_VERSION;
	r->version_minor = (unsigned)(p[7] - '0');
	*end = '\0'; /* the CR or LF */
	r->method = method;
	r->target = target;
	return HTTP_PARSE_COMPLETE;
}

/* A header line [b, e): name ":" OWS value OWS. */
static enum http_parse parse_field(HttpRequest *r, size_t b, size_t e)
{
	char *p = r->buf + b, *end = r->buf + e;
	if (is_ows(*p))
		return HTTP_PARSE_BAD; /* obsolete line folding (RFC 9112 §5.2) */
	char *name = p;
	while (p < end && is_tchar((unsigned char)*p))
		p++;
	if (p == name || p == end || *p != ':')
		return HTTP_PARSE_BAD; /* also whitespace before the colon (§5.1) */
	*p++ = '\0';
	while (p < end && is_ows(*p))
		p++;
	char *value = p, *ve = end;
	while (ve > value && is_ows(ve[-1]))
		ve--;
	for (const char *q = value; q < ve; q++) {
		unsigned char c = (unsigned char)*q;
		if ((c < 0x20 && c != '\t') || c == 0x7f)
			return HTTP_PARSE_BAD;
	}
	*ve = '\0'; /* trailing whitespace, the CR or the LF */
	if (r->header_count == HTTP_HEADERS_MAX)
		return HTTP_PARSE_HEAD_TOO_LARGE;
	r->headers[r->header_count++] = (HttpHeader){name, value};
	return HTTP_PARSE_COMPLETE;
}

/* Content-Length: digits only, every copy the same value. */
static enum http_parse parse_framing(HttpRequest *r)
{
	bool have_length = false, have_te = false;
	for (size_t i = 0; i < r->header_count; i++) {
		const HttpHeader *h = &r->headers[i];
		if (http_str_ieq(h->name, "Transfer-Encoding")) {
			have_te = true;
		} else if (http_str_ieq(h->name, "Content-Length")) {
			const char *v = h->value;
			if (*v == '\0')
				return HTTP_PARSE_BAD;
			uint64_t n = 0;
			for (; *v != '\0'; v++) {
				if (!is_digit(*v) || n > (UINT64_MAX - 9) / 10)
					return HTTP_PARSE_BAD;
				n = n * 10 + (uint64_t)(*v - '0');
			}
			if (have_length && n != r->content_length)
				return HTTP_PARSE_BAD;
			have_length = true;
			r->content_length = n;
		}
	}
	if (have_te && have_length)
		return HTTP_PARSE_BAD;
	r->has_body = have_te || r->content_length > 0;
	return HTTP_PARSE_COMPLETE;
}

/* The head [start, head_len) is complete: parse it. */
static enum http_parse finish(HttpRequest *r)
{
	size_t pos = r->start, b, e;
	next_line(r, &pos, &b, &e);
	enum http_parse res = parse_request_line(r, b, e);
	while (res == HTTP_PARSE_COMPLETE) {
		next_line(r, &pos, &b, &e);
		if (b == e)
			break; /* the empty line ending the head */
		res = parse_field(r, b, e);
	}
	if (res == HTTP_PARSE_COMPLETE) {
		size_t hosts = http_request_header_count(r, "Host");
		if (hosts > 1 || (hosts == 0 && r->version_minor >= 1))
			res = HTTP_PARSE_BAD; /* RFC 9112 §3.2 */
	}
	if (res == HTTP_PARSE_COMPLETE)
		res = parse_framing(r);
	return fail(r, res);
}

enum http_parse http_request_parse(HttpRequest *r)
{
	if (r->result != HTTP_PARSE_INCOMPLETE)
		return r->result;
	while (r->scan < r->len) {
		size_t i = r->scan++;
		unsigned char c = (unsigned char)r->buf[i];
		if (c != '\n') {
			/* Binary garbage (e.g. a TLS handshake) fails at once,
			 * not when the buffer is full. */
			if (r->lines == 0 && c != '\r' && (c < 0x20 || c == 0x7f))
				return fail(r, HTTP_PARSE_BAD);
			continue;
		}
		size_t end = i > r->line_start && r->buf[i - 1] == '\r' ? i - 1 : i;
		if (end == r->line_start) {
			if (r->lines == 0) {
				/* An empty line before the request line (§2.2). */
				r->start = i + 1;
				r->line_start = i + 1;
				continue;
			}
			r->head_len = i + 1;
			return finish(r);
		}
		r->lines++;
		if (r->lines - 1 > HTTP_HEADERS_MAX)
			return fail(r, HTTP_PARSE_HEAD_TOO_LARGE);
		r->line_start = i + 1;
	}
	if (r->len == r->cap)
		return fail(r, r->lines == 0 ? HTTP_PARSE_URI_TOO_LONG : HTTP_PARSE_HEAD_TOO_LARGE);
	return HTTP_PARSE_INCOMPLETE;
}

/* ---- Fields --------------------------------------------------------------------- */

const char *http_request_header(const HttpRequest *r, const char *name)
{
	for (size_t i = 0; i < r->header_count; i++)
		if (http_str_ieq(r->headers[i].name, name))
			return r->headers[i].value;
	return NULL;
}

size_t http_request_header_count(const HttpRequest *r, const char *name)
{
	size_t n = 0;
	for (size_t i = 0; i < r->header_count; i++)
		if (http_str_ieq(r->headers[i].name, name))
			n++;
	return n;
}

/* One element of a comma-separated field value. */
typedef struct {
	const char *b, *e;   /* its name, trimmed */
	const char *pb, *pe; /* its parameters, after the first ';' (pb == pe: none) */
} ListItem;

/* Splits the element starting at p into *it; returns where the next one
 * starts, or NULL at the end. */
static const char *list_next(const char *p, ListItem *it)
{
	if (p == NULL || *p == '\0')
		return NULL;
	const char *end = p;
	while (*end != '\0' && *end != ',')
		end++;
	const char *semi = p;
	while (semi < end && *semi != ';')
		semi++;
	it->b = p;
	it->e = semi;
	while (it->b < it->e && is_ows(*it->b))
		it->b++;
	while (it->e > it->b && is_ows(it->e[-1]))
		it->e--;
	it->pb = semi < end ? semi + 1 : end;
	it->pe = end;
	return *end == ',' ? end + 1 : end;
}

bool http_request_header_has_token(const HttpRequest *r, const char *name, const char *token)
{
	for (size_t i = 0; i < r->header_count; i++) {
		if (!http_str_ieq(r->headers[i].name, name))
			continue;
		ListItem it;
		for (const char *p = r->headers[i].value; (p = list_next(p, &it)) != NULL;)
			if (ieq_n(it.b, (size_t)(it.e - it.b), token))
				return true;
	}
	return false;
}

bool http_request_keep_alive(const HttpRequest *r)
{
	return r->version_minor >= 1 && !http_request_header_has_token(r, "Connection", "close");
}

/* The weight of an Accept-Encoding entry's parameters [pb, pe): 1 without a
 * "q", 0 for a zero weight, -1 for an invalid one (RFC 9110 §12.4.2:
 * "0" ["." 0*3DIGIT] / "1" ["." 0*3"0"]). */
static int weight(const char *pb, const char *pe)
{
	int result = 1;
	while (pb < pe) {
		const char *end = pb;
		while (end < pe && *end != ';')
			end++;
		const char *b = pb, *e = end;
		while (b < e && is_ows(*b))
			b++;
		while (e > b && is_ows(e[-1]))
			e--;
		if (e - b >= 2 && lower((unsigned char)b[0]) == 'q' && b[1] == '=') {
			const char *v = b + 2;
			size_t n = (size_t)(e - v);
			if (n == 0 || (v[0] != '0' && v[0] != '1') || (n > 1 && v[1] != '.') || n > 5)
				return -1;
			bool zero = v[0] == '0';
			for (size_t k = 2; k < n; k++) {
				if (!is_digit(v[k]) || (v[0] == '1' && v[k] != '0'))
					return -1;
				if (v[k] != '0')
					zero = false;
			}
			result = zero ? 0 : 1;
		}
		pb = end < pe ? end + 1 : pe;
	}
	return result;
}

bool http_request_accepts_encoding(const HttpRequest *r, const char *coding)
{
	bool gzip = http_str_ieq(coding, "gzip");
	int named = -1, star = -1; /* the best weight seen: -1 none, 0 refused, 1 accepted */
	for (size_t i = 0; i < r->header_count; i++) {
		if (!http_str_ieq(r->headers[i].name, "Accept-Encoding"))
			continue;
		ListItem it;
		for (const char *p = r->headers[i].value; (p = list_next(p, &it)) != NULL;) {
			size_t n = (size_t)(it.e - it.b);
			int w = weight(it.pb, it.pe);
			if (w < 0)
				continue;
			if (ieq_n(it.b, n, coding) || (gzip && ieq_n(it.b, n, "x-gzip"))) {
				if (w > named)
					named = w;
			} else if (ieq_n(it.b, n, "*")) {
				if (w > star)
					star = w;
			}
		}
	}
	return named >= 0 ? named > 0 : star > 0;
}
