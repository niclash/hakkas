#include "system/base64.h"

#include <stdint.h>

size_t base64_encode(const void *data, size_t len, char *out, size_t size)
{
	static const char alphabet[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const uint8_t *in = data;
	size_t need = BASE64_ENCODED_LEN(len);
	if (out == NULL || size <= need)
		return 0;
	char *p = out;
	size_t i = 0;
	for (; i + 3 <= len; i += 3) {
		uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
		*p++ = alphabet[v >> 18];
		*p++ = alphabet[(v >> 12) & 0x3f];
		*p++ = alphabet[(v >> 6) & 0x3f];
		*p++ = alphabet[v & 0x3f];
	}
	if (i < len) {
		/* One or two bytes left: two or three characters, then padding. */
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < len)
			v |= (uint32_t)in[i + 1] << 8;
		*p++ = alphabet[v >> 18];
		*p++ = alphabet[(v >> 12) & 0x3f];
		*p++ = i + 1 < len ? alphabet[(v >> 6) & 0x3f] : '=';
		*p++ = '=';
	}
	*p = '\0';
	return need;
}
