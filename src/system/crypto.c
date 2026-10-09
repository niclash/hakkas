#include "system/crypto.h"

#include <limits.h>

#include "mbedtls/base64.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"

/* Base64url text is converted to and from standard base64 in chunks of this
 * many characters (a multiple of 4: 48 bytes), so no buffer of the whole
 * text's size is needed. */
#define CHUNK_CHARS 64
#define CHUNK_BYTES (CHUNK_CHARS / 4 * 3)

/* The largest input whose base64 length (plus the NUL) fits a size_t. */
#define ENCODE_MAX ((SIZE_MAX - 1) / 4 * 3)

bool crypto_sha1(const void *data, size_t len, uint8_t out[CRYPTO_SHA1_SIZE])
{
	return mbedtls_sha1(data, len, out) == 0;
}

bool crypto_sha256(const void *data, size_t len, uint8_t out[CRYPTO_SHA256_SIZE])
{
	return mbedtls_sha256(data, len, out, 0) == 0;
}

bool crypto_hmac_sha256(const void *key, size_t key_len, const void *data, size_t len,
			uint8_t out[CRYPTO_SHA256_SIZE])
{
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	return md != NULL && mbedtls_md_hmac(md, key, key_len, data, len, out) == 0;
}

bool crypto_pbkdf2_hmac_sha256(const void *password, size_t password_len, const void *salt,
			       size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len)
{
	/* RFC 8018 §5.2: c >= 1. mbedTLS takes the count as an unsigned int and
	 * the length as a uint32_t. */
	if (iterations == 0)
		return false;
#if UINT_MAX < UINT32_MAX
	if (iterations > UINT_MAX)
		return false;
#endif
#if SIZE_MAX > UINT32_MAX
	if (out_len > UINT32_MAX)
		return false;
#endif
	return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, password, password_len, salt,
					     salt_len, (unsigned int)iterations,
					     (uint32_t)out_len, out) == 0;
}

bool crypto_equal(const void *a, const void *b, size_t len)
{
	return mbedtls_ct_memcmp(a, b, len) == 0;
}

void crypto_zeroize(void *p, size_t len)
{
	mbedtls_platform_zeroize(p, len);
}

size_t crypto_base64_encode(const void *data, size_t len, char *out, size_t size)
{
	if (out == NULL || len > ENCODE_MAX || size <= CRYPTO_BASE64_LEN(len))
		return 0;
	size_t n = 0;
	if (len > 0 && mbedtls_base64_encode((unsigned char *)out, size, &n, data, len) != 0)
		return 0;
	out[n] = '\0';
	return n;
}

/* A character of the base64 (RFC 4648 §4) or base64url (§5) alphabet, not
 * counting padding. */
static bool is_alphabet(char c, bool url)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		return true;
	return url ? c == '-' || c == '_' : c == '+' || c == '/';
}

bool crypto_base64_decode(const char *text, size_t len, void *out, size_t size,
			  size_t *out_len)
{
	/* mbedTLS skips spaces and line breaks; the protocol's values have
	 * none, so anything but the alphabet and '=' is refused here. Where the
	 * padding may appear, and the length, mbedTLS checks. */
	for (size_t i = 0; i < len; i++) {
		if (!is_alphabet(text[i], false) && text[i] != '=')
			return false;
	}
	size_t n = 0;
	if (len > 0 && mbedtls_base64_decode(out, size, &n, (const unsigned char *)text, len) != 0)
		return false;
	*out_len = n;
	return true;
}

size_t crypto_base64url_encode(const void *data, size_t len, char *out, size_t size)
{
	if (out == NULL || len > ENCODE_MAX || size <= CRYPTO_BASE64URL_LEN(len))
		return 0;
	const unsigned char *in = data;
	size_t o = 0;
	for (size_t i = 0; i < len; i += CHUNK_BYTES) {
		size_t chunk = len - i < CHUNK_BYTES ? len - i : CHUNK_BYTES;
		unsigned char buf[CHUNK_CHARS + 1];
		size_t n;
		if (mbedtls_base64_encode(buf, sizeof(buf), &n, in + i, chunk) != 0)
			return 0;
		/* Only the last chunk can end in padding, which is dropped. */
		for (size_t k = 0; k < n && buf[k] != '='; k++)
			out[o++] = buf[k] == '+' ? '-' : buf[k] == '/' ? '_' : (char)buf[k];
	}
	out[o] = '\0';
	return o;
}

bool crypto_base64url_decode(const char *text, size_t len, void *out, size_t size,
			     size_t *out_len)
{
	for (size_t i = 0; i < len; i++) {
		if (!is_alphabet(text[i], true))
			return false;
	}
	/* 4k characters are 3k bytes; a final 2 or 3 are 1 or 2 more; a final
	 * single character is not valid. */
	size_t rem = len % 4;
	if (rem == 1)
		return false;
	size_t total = len / 4 * 3 + (rem == 0 ? 0 : rem - 1);
	if (total > size)
		return false;
	unsigned char *dst = out;
	size_t o = 0;
	for (size_t i = 0; i < len; i += CHUNK_CHARS) {
		size_t chunk = len - i < CHUNK_CHARS ? len - i : CHUNK_CHARS;
		unsigned char buf[CHUNK_CHARS];
		for (size_t k = 0; k < chunk; k++) {
			char c = text[i + k];
			buf[k] = (unsigned char)(c == '-' ? '+' : c == '_' ? '/' : c);
		}
		/* Only the last chunk can be short of a multiple of 4: pad it. */
		while (chunk % 4 != 0)
			buf[chunk++] = '=';
		size_t n;
		if (mbedtls_base64_decode(dst + o, size - o, &n, buf, chunk) != 0)
			return false;
		o += n;
	}
	*out_len = o;
	return true;
}
