/*
 * The FBP WebSocket server (02 §0): RFC 6455 on port + 1, over the engine
 * loop (engine/loop.h), with the upgrade request parsed by protocol/http.h
 * and the framing pieces of protocol/websocket.h. It carries complete text
 * messages between the socket and the protocol layer (plan task 17) and
 * knows nothing of their contents.
 *
 * Naming: functions are prefixed ws_server_ (the server) and ws_ (a
 * connection), the types are WsServer, WsServerConfig, WsConn, WsHandler.
 *
 * Behaviour (decided, plan task 14; 02 §0 "Implementation decisions"):
 *  - Upgrade (RFC 6455 §4.2): the request must be GET over HTTP/1.1 with an
 *    origin-form target (any path; the query is ignored) and no body, else
 *    400. Without "Upgrade: websocket" it is 426 (with "Upgrade: websocket"),
 *    as for a plain HTTP request to this port. "Connection" must list
 *    "Upgrade" (400). Exactly one Sec-WebSocket-Version, "13", else 426 with
 *    "Sec-WebSocket-Version: 13". Exactly one valid Sec-WebSocket-Key (the
 *    base64 of 16 bytes), else 400. Then the Origin check, then the
 *    connection limit. Success is 101 with Sec-WebSocket-Accept; no
 *    subprotocol (Sec-WebSocket-Protocol is ignored) and no extension is
 *    negotiated. A refusal closes the connection.
 *  - Origin (02 §0): exactly one Origin field, which is the server's own
 *    origin (ws_origin_is_own: http, the Host field's host, the web port)
 *    or equals an allowed_origins entry (ASCII case-insensitively); else
 *    403. A request without Origin is refused too (403): browsers always
 *    send it, and a non-browser client (a test tool) can send the device's
 *    own origin. Accepting it would let anything that can forge a header
 *    past the check anyway, but refusing keeps one rule with no exception.
 *  - Connection limit (02 §0): at most max_connections upgraded
 *    connections at once, counted from the 101 until the socket closes
 *    (closing ones included); an upgrade beyond it is 503. Connections still
 *    in the handshake do not count; the listener's hard cap (the loop's
 *    max_conns) is max_connections + WS_PENDING_MAX, beyond which a TCP
 *    connection is closed unseen, and the handshake timeout bounds how long
 *    one may stay pending.
 *  - Frames (RFC 6455 §5): client frames must be masked, else close 1002;
 *    RSV bits (no extension), unknown opcodes, a fragmented or oversized
 *    (> 125 bytes) control frame, a continuation without a message or a new
 *    message inside one: 1002. Binary frames: 1003 (text only, 02 §0).
 *    Fragmented text messages are reassembled; a message (all its
 *    fragments) larger than max_message_bytes is 1009, detected from the
 *    frame header before its payload is buffered. Text must be UTF-8: an
 *    invalid byte is 1007 at once, an incomplete sequence at the end of the
 *    message too. A ping is answered with a pong of the same payload; a
 *    pong is ignored.
 *  - Close (RFC 6455 §7): a close from the client is answered with a close
 *    echoing its code (none without one) and the connection is closed (an
 *    invalid code is 1002, a reason that is not UTF-8 1007). A close by the
 *    server (ws_close, or a protocol error above) sends a close frame and
 *    waits close_timeout_ms for the client's close (frames meanwhile are
 *    parsed only to find it, their contents dropped), then closes. Before a
 *    close, the input already received is read and dropped, so the close
 *    sends FIN rather than a reset that could destroy the close frame.
 *  - Liveness: after ping_interval_ms without any byte from the client, the
 *    server sends a ping; if nothing arrives within pong_timeout_ms more,
 *    the connection is aborted (the peer is gone). The handshake must
 *    complete within handshake_timeout_ms (408 if it started, else closed
 *    silently). All of them are checked by one tick hook.
 *  - Outgoing buffer (01 §3.2.1, §4): during the handshake a connection has
 *    WS_HANDSHAKE_OUT_CAP bytes; on the upgrade it grows to out_cap
 *    (WS_OUT_CAP, 64 KiB by default, a per-build override). Every message
 *    sent is framed into it whole; one that does not fit closes the
 *    connection without a close frame (the buffer is full) and on_close
 *    follows. A burst such as the graph:connect snapshot (02 §4.1.1) must
 *    therefore fit the free space (ws_send_space); on_drained tells when
 *    all of it is free. The Zephyr build sets a smaller WS_OUT_CAP (02 §0:
 *    its memory cost).
 *  - Fairness: on_readable reads at most WS_READ_BUDGET bytes per iteration.
 *  - Memory per connection: the HTTP head buffer (head_max, HTTP_HEAD_MAX)
 *    only until the upgrade; afterwards out_cap, the message buffer while a
 *    message arrives (up to max_message_bytes + 1, released after a message
 *    beyond WS_MSG_KEEP), and the connection state (~1.1 KiB on 64-bit).
 *  - Logging: one info line per refused upgrade (peer, status, cause), per
 *    open and per close (peer, cause); never a message's contents (02 §2.5
 *    item 8: they may carry secrets), never header fields except a refused
 *    Origin.
 *  - TLS (tls.enabled) is not supported yet: ws_server_open_config refuses
 *    to start.
 *
 * Threading: engine thread only.
 */
#ifndef HAKKAS_ENGINE_WS_SERVER_H
#define HAKKAS_ENGINE_WS_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine/loop.h"
#include "platform/pal.h"
#include "protocol/websocket.h"
#include "system/config.h"

/* Defaults; a port's build may define each. */
#ifndef WS_OUT_CAP
#define WS_OUT_CAP (64 * 1024) /* outgoing buffer per upgraded connection */
#endif
#ifndef WS_PENDING_MAX
#define WS_PENDING_MAX 4 /* connections in the handshake beyond max_connections */
#endif
#define WS_HANDSHAKE_OUT_CAP 512 /* outgoing buffer during the handshake */
#define WS_HANDSHAKE_TIMEOUT_MS 10000
#define WS_PING_INTERVAL_MS 30000
#define WS_PONG_TIMEOUT_MS 15000
#define WS_CLOSE_TIMEOUT_MS 5000

/* Bytes read per connection and loop iteration. */
#define WS_READ_BUDGET (16 * 1024)

/* A message buffer larger than this is released after its message. */
#define WS_MSG_KEEP 4096

/* The smallest out_cap: a close frame, a pong, a small message. */
#define WS_OUT_CAP_MIN 1024

typedef struct WsServer WsServer;
typedef struct WsConn WsConn;

/* The protocol layer's callbacks; any may be NULL. ctx is the config's. */
typedef struct {
	/* The upgrade succeeded (the 101 is queued): conn may send from now. */
	void (*on_open)(void *ctx, WsConn *conn);
	/* One complete text message of len bytes, valid UTF-8, at most
	 * max_message_bytes; text is NUL-terminated after them (it may also
	 * hold U+0000). Valid during the call only. */
	void (*on_message)(void *ctx, WsConn *conn, const char *text, size_t len);
	/* The connection is gone (whatever the cause); once, for every conn
	 * that saw on_open. conn is freed after it returns: release the user
	 * context and forget conn (its session, 02 §2.5 item 2). */
	void (*on_close)(void *ctx, WsConn *conn);
	/* The outgoing buffer, after holding data, has been written out
	 * completely (all of ws_send_space is free): e.g. to send a burst
	 * that did not fit before (02 §0, "Outgoing buffer"). Only while
	 * open. */
	void (*on_drained)(void *ctx, WsConn *conn);
} WsHandler;

typedef struct {
	const char *host; /* numeric address */
	uint16_t port;	  /* 0: an ephemeral port (ws_server_port) */
	/* The web server's port, the Origin's (02 §0); 0: the listener's port
	 * - 1. */
	uint16_t web_port;
	uint32_t max_connections; /* > 0 */
	size_t max_message_bytes; /* > 0 */
	/* Extra Origin values accepted (copied). */
	const char *const *allowed_origins;
	size_t allowed_origin_count;
	size_t out_cap;	 /* >= WS_OUT_CAP_MIN */
	size_t head_max; /* > 0 */
	uint32_t handshake_timeout_ms;
	uint32_t ping_interval_ms; /* 0: never ping */
	uint32_t pong_timeout_ms;
	uint32_t close_timeout_ms;
	const WsHandler *handler; /* borrowed; must outlive the server */
	void *ctx;
} WsServerConfig;

/* cfg with host, port and the defaults: max_connections and
 * max_message_bytes as 05 §1.1, no allowed origins, the limits above. */
void ws_server_config_defaults(WsServerConfig *cfg, const char *host, uint16_t port);

/* Opens the server: the listener on host/port and a tick hook. Returns
 * PAL_OK, the listener's error (logged by loop_listen), PAL_ERR_INVALID for
 * a bad cfg, PAL_ERR_UNAVAILABLE when out of memory or tick hooks. */
enum pal_err ws_server_open(Loop *loop, const WsServerConfig *cfg, WsServer **out);

/* Opens it from the configuration (05 §1.1: host, port + 1, max_connections,
 * max_message_bytes, allowed_origins; the rest the defaults): startup step 8
 * (05 §1.4, plan task 19); a failure refuses startup. tls.enabled is refused
 * (logged; PAL_ERR_UNAVAILABLE): TLS is not supported yet. */
enum pal_err ws_server_open_config(Loop *loop, const Config *cfg, const WsHandler *handler,
				   void *ctx, WsServer **out);

/* As ws_server_open_config, but listening on port (0: an ephemeral port)
 * with web_port as the Origin's port (0: the listener's port - 1) instead of
 * cfg->port + 1 and cfg->port; tls.enabled is refused the same way. For the
 * engine's test-only ephemeral ports (engine/engine.h); a real startup uses
 * ws_server_open_config. */
enum pal_err ws_server_open_config_at(Loop *loop, const Config *cfg, uint16_t port,
				      uint16_t web_port, const WsHandler *handler, void *ctx,
				      WsServer **out);

/* Closes its listener and connections (on_close is called for each upgraded
 * one; unless the loop's shutdown closed them already) and removes its tick
 * hook. After loop_run returns and before loop_deinit, or between
 * iterations; not from a callback. NULL is ignored. */
void ws_server_close(WsServer *srv);

/* The port it listens on; 0 once its listener is closed. */
uint16_t ws_server_port(const WsServer *srv);

/* Upgraded connections open now (those closing included). */
uint32_t ws_server_conn_count(const WsServer *srv);

/* ---- Connections (from on_open until on_close returns) --------------------------- */

/* Frames len bytes of UTF-8 text as one text message into the outgoing
 * buffer. False if the connection is not open (closing, closed) or the
 * message does not fit: the connection is then closed (see "Outgoing
 * buffer" above). */
bool ws_send_text(WsConn *conn, const char *text, size_t len);

/* The longest text message ws_send_text takes now without closing. */
size_t ws_send_space(const WsConn *conn);

/* Bytes of the outgoing buffer not written out yet; 0 when it is empty (no
 * on_drained follows then) or the connection is not open. */
size_t ws_send_pending(const WsConn *conn);

/* Starts the close handshake with code (a valid close code, e.g.
 * WS_CLOSE_NORMAL) and an optional reason (UTF-8, at most
 * WS_CLOSE_REASON_MAX bytes; otherwise it is left out). No message is
 * delivered or sent afterwards; on_close follows when the socket closes.
 * Ignored unless open. */
void ws_close(WsConn *conn, uint16_t code, const char *reason);

/* True from on_open until a close starts. */
bool ws_is_open(const WsConn *conn);

/* The peer's address (e.g. rate limiting, 02 §2.5 item 7). */
const struct pal_peer *ws_peer(const WsConn *conn);

/* The protocol layer's per-connection context; NULL until set. */
void ws_set_user(WsConn *conn, void *user);
void *ws_user(const WsConn *conn);

#endif
