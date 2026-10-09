/*
 * The mbedTLS configuration of hakkas (01 §6, third_party/mbedtls/README.md).
 *
 * Used as MBEDTLS_CONFIG_FILE, so it replaces mbedTLS's default
 * mbedtls_config.h entirely: only what is defined here is enabled. hakkas
 * needs the primitives of 02 §2 (SCRAM-SHA-256: SHA-256, HMAC-SHA256,
 * PBKDF2-HMAC-SHA256, base64, a constant-time compare) and of the WebSocket
 * handshake (RFC 6455 §4.2.2: SHA-1, base64), and nothing else: no TLS, no
 * X.509, no PSA crypto, no entropy or DRBG (random bytes come from the PAL,
 * 01 §0.1), no file IO, no threading, no time.
 *
 * The modules compiled are those in HAKKAS_THIRD_PARTY_SOURCES
 * (cmake/HakkasSources.cmake): sha1.c, sha256.c, md.c, pkcs5.c, base64.c,
 * constant_time.c and platform_util.c. A port that uses its own copy of
 * mbedTLS (e.g. Zephyr's module) must enable at least these options.
 */
#ifndef HAKKAS_MBEDTLS_CONFIG_H
#define HAKKAS_MBEDTLS_CONFIG_H

/* SHA-1 (the WebSocket handshake's Sec-WebSocket-Accept, RFC 6455 §4.2.2). */
#define MBEDTLS_SHA1_C

/* SHA-256 (SCRAM-SHA-256, 02 §2). SHA-224 is part of the same module and is
 * left out. */
#define MBEDTLS_SHA256_C

/* The generic message-digest layer: HMAC (mbedtls_md_hmac*) and the digest
 * PBKDF2 runs on. */
#define MBEDTLS_MD_C

/* PKCS#5: PBKDF2-HMAC (mbedtls_pkcs5_pbkdf2_hmac_ext). Its PBES2 parsing
 * needs ASN.1 and cipher support, which stay disabled. */
#define MBEDTLS_PKCS5_C

/* Base64 (RFC 4648 §4). */
#define MBEDTLS_BASE64_C

/* constant_time.c (mbedtls_ct_memcmp) and platform_util.c
 * (mbedtls_platform_zeroize) are always compiled and need no option. */

#endif
