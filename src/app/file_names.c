/*
 * Graph and type file names (05 §2.3). See app/file_names.h.
 */
#include "app/file_names.h"

#include <string.h>

static const char hex_digits[] = "0123456789ABCDEF";

/* Kept unencoded: A–Z a–z 0–9 _ - (spelled out: no locale, no ctype). */
static bool is_kept(unsigned char c)
{
	return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
	       c == '_' || c == '-';
}

static int hex_value(char c)
{
	const char *p = c != '\0' ? strchr(hex_digits, c) : NULL;
	return p != NULL ? (int)(p - hex_digits) : -1;
}

static bool fail(char *out, size_t size)
{
	if (size > 0)
		out[0] = '\0';
	return false;
}

bool file_name_encode(const char *name, char *out, size_t size)
{
	if (name == NULL)
		return fail(out, size);
	size_t len = strlen(name);
	if (len == 0 || len > FILE_NAME_ID_MAX)
		return fail(out, size);
	size_t n = 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)name[i];
		if (is_kept(c)) {
			if (n + 1 >= size)
				return fail(out, size);
			out[n++] = (char)c;
		} else {
			if (n + 3 >= size)
				return fail(out, size);
			out[n++] = '%';
			out[n++] = hex_digits[c >> 4];
			out[n++] = hex_digits[c & 0x0f];
		}
	}
	size_t suffix = strlen(FILE_NAME_SUFFIX);
	if (n + suffix >= size)
		return fail(out, size);
	memcpy(out + n, FILE_NAME_SUFFIX, suffix + 1);
	return true;
}

bool file_name_decode(const char *file, char *out, size_t size)
{
	if (file == NULL)
		return fail(out, size);
	size_t len = strlen(file);
	size_t suffix = strlen(FILE_NAME_SUFFIX);
	if (len <= suffix || strcmp(file + len - suffix, FILE_NAME_SUFFIX) != 0)
		return fail(out, size);
	len -= suffix;
	size_t n = 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)file[i];
		if (c == '%') {
			int hi = i + 2 < len ? hex_value(file[i + 1]) : -1;
			int lo = hi >= 0 ? hex_value(file[i + 2]) : -1;
			if (lo < 0)
				return fail(out, size);
			c = (unsigned char)(hi << 4 | lo);
			if (is_kept(c) || c == '\0')
				return fail(out, size); /* not canonical */
			i += 2;
		} else if (!is_kept(c)) {
			return fail(out, size);
		}
		if (n >= FILE_NAME_ID_MAX || n + 1 >= size)
			return fail(out, size);
		out[n++] = (char)c;
	}
	out[n] = '\0';
	return true;
}
