/*
 * Base64 encoding (RFC 4648 §4: the standard alphabet with '=' padding), for
 * the generated auth.server_secret (05 §1.1).
 *
 * Decided (plan task 11): a small encoder in the core, since mbedTLS (the
 * decided crypto library, 01 §6) is integrated only in plan task 15. Base64
 * is an encoding, not a cryptographic primitive; task 15 may replace this
 * implementation with mbedTLS's, the output being the same.
 *
 * Naming: functions and macros are prefixed base64_ / BASE64_.
 */
#ifndef HAKKAS_SYSTEM_BASE64_H
#define HAKKAS_SYSTEM_BASE64_H

#include <stddef.h>

/* The length of the base64 text of n bytes, without the NUL. */
#define BASE64_ENCODED_LEN(n) ((((n) + 2) / 3) * 4)

/* Encode the len bytes at data into out (size bytes) as NUL-terminated
 * base64 text. Returns the text's length, or 0 (out unchanged) if out is
 * smaller than BASE64_ENCODED_LEN(len) + 1. */
size_t base64_encode(const void *data, size_t len, char *out, size_t size);

#endif
