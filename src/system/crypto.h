/*
 * Cryptographic primitives and the base64 encodings, for the WebSocket
 * handshake (RFC 6455 §4.2.2: SHA-1 and base64), SCRAM-SHA-256 login and
 * sessions (02 §2: SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256, base64, base64url
 * secrets, a constant-time compare) and the generated auth.server_secret
 * (05 §1.1).
 *
 * Decided (01 §6, plan task 15): a thin layer over the vendored mbedTLS
 * subset (third_party/mbedtls/README.md); no hand-written cryptographic
 * primitives. The rest of hakkas includes only this header, never mbedTLS's.
 * Random bytes are not here: they come from the PAL (pal_random_bytes,
 * 01 §0.1).
 *
 * Every function is safe to call with len 0. Functions returning bool report
 * false on invalid arguments or when mbedTLS fails (HMAC and PBKDF2 allocate
 * a small context on the heap); outputs are then unspecified.
 *
 * Naming: functions and macros are prefixed crypto_ / CRYPTO_.
 */
#ifndef HAKKAS_SYSTEM_CRYPTO_H
#define HAKKAS_SYSTEM_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CRYPTO_SHA1_SIZE 20   /* bytes of a SHA-1 digest */
#define CRYPTO_SHA256_SIZE 32 /* bytes of a SHA-256 digest or HMAC-SHA256 */

/* The length of the base64 text of n bytes (RFC 4648 §4, padded), without
 * the NUL. */
#define CRYPTO_BASE64_LEN(n) ((((n) + 2) / 3) * 4)

/* The length of the base64url text of n bytes (RFC 4648 §5, unpadded),
 * without the NUL: 22 for a 16-byte session secret (02 §2.5 item 1). */
#define CRYPTO_BASE64URL_LEN(n) (((n) * 4 + 2) / 3)

/* An upper bound on the bytes that base64 or base64url text of len
 * characters decodes to. */
#define CRYPTO_BASE64_DECODED_MAX(len) (((len) + 3) / 4 * 3)

/* SHA-1 (FIPS 180-4) of the len bytes at data. Only for the WebSocket
 * handshake (RFC 6455), never for security. */
bool crypto_sha1(const void *data, size_t len, uint8_t out[CRYPTO_SHA1_SIZE]);

/* SHA-256 (FIPS 180-4) of the len bytes at data. */
bool crypto_sha256(const void *data, size_t len, uint8_t out[CRYPTO_SHA256_SIZE]);

/* HMAC-SHA256 (RFC 2104) of the len bytes at data, keyed with key_len bytes
 * at key (any length). */
bool crypto_hmac_sha256(const void *key, size_t key_len, const void *data, size_t len,
			uint8_t out[CRYPTO_SHA256_SIZE]);

/* PBKDF2 (RFC 8018 §5.2) with HMAC-SHA256: derive out_len bytes (at most
 * UINT32_MAX) from password and salt with the given iteration count (at
 * least 1). The SCRAM SaltedPassword is out_len 32 (02 §2.2). */
bool crypto_pbkdf2_hmac_sha256(const void *password, size_t password_len, const void *salt,
			       size_t salt_len, uint32_t iterations, uint8_t *out, size_t out_len);

/* Compare len bytes at a and b in time that depends only on len, not on
 * where they differ (02 §2.3, the proof check). True if equal. */
bool crypto_equal(const void *a, const void *b, size_t len);

/* Clear len bytes at p in a way the compiler does not optimize away, for
 * secrets in buffers about to go out of scope (02 §2.5 item 8). */
void crypto_zeroize(void *p, size_t len);

/* Encode the len bytes at data into out (size bytes) as NUL-terminated
 * base64 text (RFC 4648 §4: standard alphabet, '=' padding). Returns the
 * text's length, or 0 (out unchanged) if out is smaller than
 * CRYPTO_BASE64_LEN(len) + 1. */
size_t crypto_base64_encode(const void *data, size_t len, char *out, size_t size);

/* Decode len characters of base64 text (RFC 4648 §4, padded to a multiple of
 * 4) into out (size bytes), the decoded length into *out_len. Strict: only
 * the alphabet and trailing '=' padding; no whitespace or line breaks. False
 * if the text is not valid or out is too small. */
bool crypto_base64_decode(const char *text, size_t len, void *out, size_t size,
			  size_t *out_len);

/* Encode as NUL-terminated base64url text (RFC 4648 §5: '-' and '_' for '+'
 * and '/', no padding). Returns the text's length, or 0 (out unchanged) if out
 * is smaller than CRYPTO_BASE64URL_LEN(len) + 1. */
size_t crypto_base64url_encode(const void *data, size_t len, char *out, size_t size);

/* Decode unpadded base64url text (RFC 4648 §5): only the base64url alphabet,
 * no '=' and no whitespace. False if the text is not valid (including a
 * length of 4k + 1) or out is too small. */
bool crypto_base64url_decode(const char *text, size_t len, void *out, size_t size,
			     size_t *out_len);

#endif
