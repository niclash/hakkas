/*
 * WebSocket (RFC 6455) pieces that need no sockets: the handshake's accept
 * key, the frame header codec, close codes, the UTF-8 validator of text
 * messages and the upgrade's Origin check (02 §0). The server on the engine
 * loop is engine/ws_server.h (plan task 14).
 *
 * Naming: functions, types and macros are prefixed ws_ / Ws / WS_.
 *
 * Portable core (01 §0): no PAL calls, no logging, no allocation.
 */
#ifndef HAKKAS_PROTOCOL_WEBSOCKET_H
#define HAKKAS_PROTOCOL_WEBSOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The GUID appended to Sec-WebSocket-Key (RFC 6455 §1.3). */
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* Lengths of the base64 text of Sec-WebSocket-Key (16 bytes) and of
 * Sec-WebSocket-Accept (a SHA-1 digest), without the NUL. */
#define WS_KEY_LEN 24
#define WS_ACCEPT_LEN 28

/* The longest frame header: 2 bytes, a 64-bit length, a masking key. */
#define WS_FRAME_HEADER_MAX 14

/* The longest control frame payload (RFC 6455 §5.5). */
#define WS_CONTROL_MAX 125

/* The longest close reason: a control payload minus the 2-byte code. */
#define WS_CLOSE_REASON_MAX (WS_CONTROL_MAX - 2)

enum ws_opcode {
	WS_OP_CONTINUATION = 0x0,
	WS_OP_TEXT = 0x1,
	WS_OP_BINARY = 0x2,
	WS_OP_CLOSE = 0x8,
	WS_OP_PING = 0x9,
	WS_OP_PONG = 0xA,
};

/* The close codes this server sends (RFC 6455 §7.4.1). */
enum ws_close_code {
	WS_CLOSE_NORMAL = 1000,
	WS_CLOSE_GOING_AWAY = 1001,
	WS_CLOSE_PROTOCOL_ERROR = 1002,
	WS_CLOSE_UNSUPPORTED_DATA = 1003, /* a binary message: text frames only (02 §0) */
	WS_CLOSE_INVALID_DATA = 1007,	  /* a text message that is not UTF-8 */
	WS_CLOSE_POLICY = 1008,
	WS_CLOSE_TOO_BIG = 1009, /* a message beyond max_message_bytes (02 §0) */
	WS_CLOSE_INTERNAL_ERROR = 1011,
};

/* Whether a close code received in a close frame is valid (RFC 6455 §7.4,
 * and the IANA registry's 1012-1014): 1000-1003, 1007-1014, 3000-4999. 1005,
 * 1006 and 1015 must never be sent; the rest is reserved. */
bool ws_close_code_valid(unsigned code);

/* Checks a Sec-WebSocket-Key (RFC 6455 §4.1 item 7: the strict base64 of 16
 * bytes, 24 characters) and writes its Sec-WebSocket-Accept, base64 of the
 * SHA-1 of the key and WS_GUID (§4.2.2), NUL-terminated, into out. False for
 * an invalid key (or a failure of the hash). */
bool ws_accept_key(const char *key, char out[WS_ACCEPT_LEN + 1]);

/* ---- Frames ------------------------------------------------------------------- */

typedef struct {
	bool fin;
	uint8_t rsv; /* the RSV1-3 bits, 0-7 */
	uint8_t opcode;
	bool masked;
	uint8_t mask[4];
	uint64_t len; /* payload bytes */
} WsFrameHeader;

/* The size of the header whose first two bytes are b: 2 to 14. */
size_t ws_frame_header_size(const uint8_t b[2]);

/* Parses the frame header at buf (len bytes available). Returns its size, 0
 * if more bytes are needed, -1 if it is invalid: a 64-bit length with the
 * most significant bit set (RFC 6455 §5.2). Everything else (opcodes, RSV
 * bits, masking, control frame rules) is the caller's to check. */
int ws_frame_header_parse(const uint8_t *buf, size_t len, WsFrameHeader *h);

/* Writes the header of a final, unmasked frame (a server's, RFC 6455 §5.1)
 * with opcode and a payload of len bytes into out, with the shortest length
 * encoding. Returns its size (2, 4 or 10). */
size_t ws_frame_header_write(uint8_t out[WS_FRAME_HEADER_MAX], enum ws_opcode opcode,
			     uint64_t len);

/* ---- UTF-8 -------------------------------------------------------------------- */

/*
 * An incremental UTF-8 validator (RFC 3629, as RFC 6455 §8.1 requires of text
 * messages): the bytes may arrive in any split, e.g. across the fragments of
 * a message. Overlong forms, surrogates (U+D800-U+DFFF) and code points above
 * U+10FFFF are invalid; U+0000 is valid.
 */
typedef struct {
	uint8_t need;	/* continuation bytes still expected; WS_UTF8_BAD once invalid */
	uint8_t lo, hi; /* the range of the next continuation byte */
} WsUtf8;

#define WS_UTF8_BAD 0xFF

static inline void ws_utf8_init(WsUtf8 *u)
{
	*u = (WsUtf8){0, 0x80, 0xBF};
}

/* Feeds n more bytes. False once the input is invalid (and from then on). */
bool ws_utf8_feed(WsUtf8 *u, const uint8_t *p, size_t n);

/* True if the bytes fed so far end on a character boundary (valid input). */
static inline bool ws_utf8_complete(const WsUtf8 *u)
{
	return u->need == 0;
}

/* Whether the n bytes at p are complete, valid UTF-8. */
bool ws_utf8_valid(const void *p, size_t n);

/* ---- Origin ------------------------------------------------------------------- */

/*
 * The upgrade's own-origin check (02 §0, decided in plan task 14): origin
 * (the Origin field) names the host and web port the frontend was served
 * from: "<scheme>://<host>[:<port>]" and nothing else (no path, no
 * userinfo), where
 *  - the scheme is "http", or "https" when tls is on (case-insensitive);
 *  - the host equals the host of host_field (the request's Host field,
 *    without its port) case-insensitively; an IPv6 address stays in its
 *    brackets on both sides and is compared as text;
 *  - the port, or the scheme's default (80, 443) without one, is web_port.
 * The Host field's own port (the WebSocket port) is not compared.
 */
bool ws_origin_is_own(const char *origin, const char *host_field, bool tls, uint16_t web_port);

#endif
