#include "system/uuid.h"

#include <stdint.h>

bool uuid_v4(pal_random_fn random_bytes, char *out)
{
	static const char hex[] = "0123456789abcdef";
	uint8_t b[16];
	if (random_bytes(b, sizeof(b)) != PAL_OK)
		return false;
	b[6] = (uint8_t)((b[6] & 0x0f) | 0x40); /* version 4 */
	b[8] = (uint8_t)((b[8] & 0x3f) | 0x80); /* variant 10 */
	char *p = out;
	for (int i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			*p++ = '-';
		*p++ = hex[b[i] >> 4];
		*p++ = hex[b[i] & 0x0f];
	}
	*p = '\0';
	return true;
}
