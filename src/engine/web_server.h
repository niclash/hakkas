/*
 * The web server (05 §3): serves the frontend's static assets from webdir on
 * port, over the engine loop (engine/loop.h), with the HTTP/1.1 request
 * parser of protocol/http.h. It does not handle the FBP protocol, which has
 * its own WebSocket listener on port + 1 (02 §0, plan task 14).
 *
 * Naming: functions are prefixed web_server_ (web_ for the helpers), the
 * types are WebServer and WebServerConfig.
 *
 * Behaviour (decided, plan task 13; 05 §3 "Implementation decisions"):
 *  - GET and HEAD (HEAD: the GET response without its body, RFC 9110 §9.3.2);
 *    any other method is 405 with "Allow: GET, HEAD". No authentication.
 *  - Paths (web_path_from_target): origin-form only; the query is dropped;
 *    percent-decoded; a NUL, '/' or '\' written as %XX, a raw '\', a control
 *    byte, a "." or ".." segment, or an invalid escape is 400; a segment
 *    starting with '.' (hidden files: .git, .env) is 404; empty segments
 *    collapse; a path ending in '/' gets "index.html". The result is always
 *    relative to webdir. webdir itself is trusted: symbolic links inside it
 *    are followed (POSIX), as the PAL opens paths as given.
 *  - Unmatched paths: a path whose last segment has no '.' (a client-side
 *    route such as /graphs/x, or a directory path ending in '/') is
 *    answered 200 with the entry document
 *    (index.html); one with an extension (a missing /app.js) is 404, so a
 *    script is never answered with HTML.
 *  - The environment's pictures (04 §1, plan task 77): a path
 *    environment/pictures/<name>, <name> one segment, is the file <name> of
 *    picturedir, never a file of webdir and never the entry document; 404
 *    without a picturedir. Pre-compression and headers as for an asset.
 *  - Pre-compressed assets: when Accept-Encoding allows gzip
 *    (http_request_accepts_encoding: q=0 refuses), <file>.gz is served if it
 *    exists, with Content-Encoding: gzip and the original's Content-Type;
 *    otherwise <file>. Every file response carries Vary: Accept-Encoding.
 *  - Headers: Content-Type by extension (web_content_type), Content-Length
 *    always, Cache-Control: no-cache (a frontend update is picked up; there
 *    are no validators, so a revalidation re-downloads), X-Content-Type-
 *    Options: nosniff. No Date (no wall clock, 00-README open question 11).
 *  - Connections: HTTP/1.1 keep-alive (http_request_keep_alive), one request
 *    at a time: reading pauses while a response is pending, and pipelined
 *    bytes wait in the head buffer. The connection closes after a response
 *    to HTTP/1.0, "Connection: close", a request with a body (it is never
 *    read), a parse error (400, 414, 431, 505), a timeout (408), 500 or 503.
 *    Before closing, the input already received is read and discarded, so
 *    the close does not reset the connection and lose the response.
 *  - Streaming: a file is read in chunks into the connection's outgoing
 *    buffer as it drains (on_drained); it is never loaded whole. A file that
 *    ends before its announced size aborts the connection.
 *  - Timeouts, checked by a tick hook: idle (no byte of the next request,
 *    after the accept or a response), the head (from its first byte until it
 *    is complete: 408 and close), sending (no progress while a response is
 *    written out: abort).
 *  - Limits: max_conns connections at once (the listener's hard cap; more are
 *    closed unseen), out_cap bytes of outgoing buffer and head_max bytes of
 *    head per connection.
 *  - Logging: one info line per response (peer, method, path without the
 *    query, status, body bytes); storage failures at warn. Never headers.
 *
 * Threading: engine thread only.
 */
#ifndef HAKKAS_ENGINE_WEB_SERVER_H
#define HAKKAS_ENGINE_WEB_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine/loop.h"
#include "platform/pal.h"
#include "system/config.h"

/* Defaults; a port's build may define each. */
#ifndef WEB_MAX_CONNS
#define WEB_MAX_CONNS 16 /* browsers open up to 6 per origin */
#endif
#ifndef WEB_OUT_CAP
#define WEB_OUT_CAP 8192
#endif
#define WEB_IDLE_TIMEOUT_MS 15000
#define WEB_HEAD_TIMEOUT_MS 10000
#define WEB_SEND_TIMEOUT_MS 30000

/* The SPA's entry document, served for "/" and for unmatched routes. */
#define WEB_ENTRY_DOCUMENT "index.html"

/* Where the environment's pictures are served, relative to the root. */
#define WEB_PICTURES_PATH "environment/pictures/"

/* The smallest out_cap: a response head must fit. */
#define WEB_OUT_CAP_MIN 1024

typedef struct WebServer WebServer;

typedef struct {
	const char *host;   /* numeric address */
	uint16_t port;	    /* 0: an ephemeral port (web_server_port) */
	const char *webdir; /* at most PAL_PATH_MAX - 1 bytes */
	const char *picturedir; /* the environment's pictures; NULL or "": none */
	uint32_t max_conns;
	size_t out_cap;	 /* >= WEB_OUT_CAP_MIN */
	size_t head_max; /* > 0 */
	uint32_t idle_timeout_ms, head_timeout_ms, send_timeout_ms;
} WebServerConfig;

/* cfg with host, port and webdir, the rest the defaults above. */
void web_server_config_defaults(WebServerConfig *cfg, const char *host, uint16_t port,
				const char *webdir);

/* Opens the web server: the listener on host/port and a tick hook. Returns
 * PAL_OK, the listener's error (logged by loop_listen), PAL_ERR_INVALID for
 * a bad cfg, PAL_ERR_UNAVAILABLE when out of memory or tick hooks. */
enum pal_err web_server_open(Loop *loop, const WebServerConfig *cfg, WebServer **out);

/* Opens it from the configuration (05 §1.1: host, port, webdir,
 * environment.pictures; the rest the
 * defaults): startup step 8 (05 §1.4, plan task 19); a failure refuses
 * startup. */
enum pal_err web_server_open_config(Loop *loop, const Config *cfg, WebServer **out);

/* As web_server_open_config, but on port (0: an ephemeral port) instead of
 * cfg->port. For the engine's test-only ephemeral ports (engine/engine.h);
 * a real startup uses web_server_open_config. */
enum pal_err web_server_open_config_at(Loop *loop, const Config *cfg, uint16_t port,
				       WebServer **out);

/* Closes its listener and connections (unless the loop's shutdown closed
 * them already, as loop_run does) and removes its tick hook. Call it after
 * loop_run returns and before loop_deinit, or between iterations; not from a
 * callback. NULL is ignored. */
void web_server_close(WebServer *srv);

/* The port it listens on. */
uint16_t web_server_port(const WebServer *srv);

/* ---- Helpers (exposed for tests) ---------------------------------------------- */

enum web_path {
	WEB_PATH_OK = 0,
	WEB_PATH_BAD,	    /* 400 */
	WEB_PATH_NOT_FOUND, /* 404: hidden, or too long to exist */
};

/* Maps a request-target to a path relative to webdir (no leading '/', see
 * "Paths" above) in out (cap bytes, NUL-terminated). *route receives whether
 * an unmatched path falls back to the entry document: the requested path's
 * last segment has no '.' (a directory path ending in '/' included). */
enum web_path web_path_from_target(const char *target, char *out, size_t cap, bool *route);

/* The Content-Type for path's extension (case-insensitive): html, htm, js,
 * mjs, css, json, map, txt, svg, png, jpg, jpeg, gif, webp, ico, woff,
 * woff2, wasm; application/octet-stream for anything else. */
const char *web_content_type(const char *path);

#endif
