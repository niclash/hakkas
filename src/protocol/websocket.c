#include "protocol/websocket.h"

#include <string.h>

#include "system/crypto.h"

bool ws_close_code_valid(unsigned code)
{
	return (code >= 1000 && code <= 1003) || (code >= 1007 && code <= 1014) ||
	       (code >= 3000 && code <= 4999);
}

bool ws_accept_key(const char *key, char out[WS_ACCEPT_LEN + 1])
{
	size_t len = strlen(key);
	uint8_t raw[CRYPTO_BASE64_DECODED_MAX(WS_KEY_LEN)];
	size_t raw_len;
	if (len != WS_KEY_LEN || !crypto_base64_decode(key, len, raw, sizeof(raw), &raw_len) ||
	    raw_len != 16)
		return false;
	char text[WS_KEY_LEN + sizeof(WS_GUID)];
	memcpy(text, key, WS_KEY_LEN);
	memcpy(text + WS_KEY_LEN, WS_GUID, sizeof(WS_GUID));
	uint8_t digest[CRYPTO_SHA1_SIZE];
	if (!crypto_sha1(text, WS_KEY_LEN + sizeof(WS_GUID) - 1, digest))
		return false;
	return crypto_base64_encode(digest, sizeof(digest), out, WS_ACCEPT_LEN + 1) ==
	       WS_ACCEPT_LEN;
}

/* ---- Frames ------------------------------------------------------------------- */

size_t ws_frame_header_size(const uint8_t b[2])
{
	size_t n = 2;
	unsigned len7 = b[1] & 0x7F;
	if (len7 == 126)
		n += 2;
	else if (len7 == 127)
		n += 8;
	if (b[1] & 0x80)
		n += 4;
	return n;
}

int ws_frame_header_parse(const uint8_t *buf, size_t len, WsFrameHeader *h)
{
	if (len < 2 || len < ws_frame_header_size(buf))
		return 0;
	h->fin = (buf[0] & 0x80) != 0;
	h->rsv = (uint8_t)((buf[0] >> 4) & 0x7);
	h->opcode = buf[0] & 0x0F;
	h->masked = (buf[1] & 0x80) != 0;
	unsigned len7 = buf[1] & 0x7F;
	size_t p = 2;
	if (len7 == 126) {
		h->len = (uint64_t)buf[2] << 8 | buf[3];
		p = 4;
	} else if (len7 == 127) {
		if (buf[2] & 0x80)
			return -1;
		h->len = 0;
		for (int i = 0; i < 8; i++)
			h->len = h->len << 8 | buf[2 + i];
		p = 10;
	} else {
		h->len = len7;
	}
	if (h->masked) {
		memcpy(h->mask, buf + p, 4);
		p += 4;
	} else {
		memset(h->mask, 0, 4);
	}
	return (int)p;
}

size_t ws_frame_header_write(uint8_t out[WS_FRAME_HEADER_MAX], enum ws_opcode opcode,
			     uint64_t len)
{
	out[0] = (uint8_t)(0x80 | opcode);
	if (len < 126) {
		out[1] = (uint8_t)len;
		return 2;
	}
	if (len <= 0xFFFF) {
		out[1] = 126;
		out[2] = (uint8_t)(len >> 8);
		out[3] = (uint8_t)len;
		return 4;
	}
	out[1] = 127;
	for (int i = 0; i < 8; i++)
		out[2 + i] = (uint8_t)(len >> (56 - 8 * i));
	return 10;
}

/* ---- UTF-8 -------------------------------------------------------------------- */

bool ws_utf8_feed(WsUtf8 *u, const uint8_t *p, size_t n)
{
	if (u->need == WS_UTF8_BAD)
		return false;
	for (size_t i = 0; i < n; i++) {
		uint8_t b = p[i];
		if (u->need > 0) {
			if (b < u->lo || b > u->hi)
				goto bad;
			u->need--;
			u->lo = 0x80;
			u->hi = 0xBF;
			continue;
		}
		if (b < 0x80)
			continue;
		/* A lead byte: how many continuation bytes follow, and the
		 * range of the first one (which excludes overlong forms,
		 * surrogates and code points above U+10FFFF, RFC 3629 §4). */
		if (b >= 0xC2 && b <= 0xDF) {
			u->need = 1;
		} else if (b == 0xE0) {
			u->need = 2;
			u->lo = 0xA0;
		} else if ((b >= 0xE1 && b <= 0xEC) || b == 0xEE || b == 0xEF) {
			u->need = 2;
		} else if (b == 0xED) {
			u->need = 2;
			u->hi = 0x9F;
		} else if (b == 0xF0) {
			u->need = 3;
			u->lo = 0x90;
		} else if (b >= 0xF1 && b <= 0xF3) {
			u->need = 3;
		} else if (b == 0xF4) {
			u->need = 3;
			u->hi = 0x8F;
		} else {
			goto bad; /* 0x80-0xC1 (continuation, overlong), 0xF5-0xFF */
		}
	}
	return true;
bad:
	u->need = WS_UTF8_BAD;
	return false;
}

bool ws_utf8_valid(const void *p, size_t n)
{
	WsUtf8 u;
	ws_utf8_init(&u);
	return ws_utf8_feed(&u, p, n) && ws_utf8_complete(&u);
}

/* ---- Origin ------------------------------------------------------------------- */

/* Splits an authority "host[:port]" ([b, e)) into the host [*hb, *he) and
 * the port text [*pb, *pe) (empty without a port). An IPv6 host keeps its
 * brackets. False if malformed. */
static bool split_authority(const char *b, const char *e, const char **hb, const char **he,
			    const char **pb, const char **pe)
{
	const char *colon;
	if (b < e && *b == '[') {
		const char *close = memchr(b, ']', (size_t)(e - b));
		if (close == NULL)
			return false;
		colon = close + 1;
		if (colon < e && *colon != ':')
			return false;
	} else {
		colon = memchr(b, ':', (size_t)(e - b));
		if (colon == NULL)
			colon = e;
	}
	*hb = b;
	*he = colon;
	*pb = colon < e ? colon + 1 : e;
	*pe = e;
	return *he > *hb;
}

/* The port text [b, e) as a number 1-65535; 0 if invalid. */
static unsigned parse_port(const char *b, const char *e)
{
	if (e == b || e - b > 5)
		return 0;
	unsigned v = 0;
	for (const char *p = b; p < e; p++) {
		if (*p < '0' || *p > '9')
			return 0;
		v = v * 10 + (unsigned)(*p - '0');
	}
	return v <= 65535 ? v : 0;
}

static bool ieq_range(const char *a, const char *ae, const char *b, const char *be)
{
	if (ae - a != be - b)
		return false;
	for (; a < ae; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z')
			x = (char)(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z')
			y = (char)(y - 'A' + 'a');
		if (x != y)
			return false;
	}
	return true;
}

bool ws_origin_is_own(const char *origin, const char *host_field, bool tls, uint16_t web_port)
{
	if (origin == NULL || host_field == NULL)
		return false;
	const char *scheme = tls ? "https" : "http";
	size_t slen = strlen(scheme);
	const char *sep = strstr(origin, "://");
	if (sep == NULL || !ieq_range(origin, sep, scheme, scheme + slen))
		return false;
	const char *ab = sep + 3;
	const char *ae = ab + strlen(ab);
	for (const char *p = ab; p < ae; p++)
		if (*p == '/' || *p == '?' || *p == '#' || *p == '@' || *p == ' ' || *p == '\t')
			return false;
	const char *ohb, *ohe, *opb, *ope;
	if (!split_authority(ab, ae, &ohb, &ohe, &opb, &ope))
		return false;
	unsigned port = tls ? 443 : 80;
	if (opb < ope || (ohe < ae && *ohe == ':')) {
		port = parse_port(opb, ope);
		if (port == 0)
			return false;
	}
	if (port != web_port)
		return false;
	const char *hb, *he, *pb, *pe;
	const char *fe = host_field + strlen(host_field);
	if (!split_authority(host_field, fe, &hb, &he, &pb, &pe))
		return false;
	return ieq_range(ohb, ohe, hb, he);
}
