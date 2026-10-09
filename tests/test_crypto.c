/*
 * The crypto layer (system/crypto.h, 01 §6) over the vendored mbedTLS subset,
 * checked against published test vectors: SHA-1 and SHA-256 (FIPS 180-4
 * examples), HMAC-SHA256 (RFC 4231 §4), PBKDF2-HMAC-SHA256 (RFC 7914 §11),
 * base64 and base64url (RFC 4648 §10), the WebSocket handshake (RFC 6455
 * §1.3), the SCRAM-SHA-256 computation of 02 §2 on the RFC 7677 §3 example,
 * and the constant-time compare.
 */
#include "test.h"

#include <stdint.h>
#include <stdlib.h>

#include "mbedtls/build_info.h" /* only to pin the vendored version */
#include "system/crypto.h"

/* The bytes at p as lowercase hex, in one of a few rotating buffers. */
static const char *hex(const void *p, size_t n)
{
	static char buf[4][2 * 128 + 1];
	static int next;
	char *b = buf[next++ % 4];
	const uint8_t *in = p;
	CHECK(n <= 128);
	for (size_t i = 0; i < n && i < 128; i++)
		snprintf(b + 2 * i, 3, "%02x", in[i]);
	b[2 * (n < 128 ? n : 128)] = '\0';
	return b;
}

static void test_version(void)
{
	/* third_party/mbedtls/README.md: pinned to 3.6.7, built with hakkas'
	 * configuration only. */
	CHECK_STREQ(MBEDTLS_VERSION_STRING, "3.6.7");
#if defined(MBEDTLS_SSL_TLS_C) || defined(MBEDTLS_PSA_CRYPTO_C) ||                                \
	defined(MBEDTLS_ENTROPY_C) || defined(MBEDTLS_CTR_DRBG_C) || !defined(MBEDTLS_PKCS5_C)
	CHECK(!"not hakkas' mbedTLS configuration");
#endif
}

static void test_sha1(void)
{
	/* FIPS 180-4 examples (NIST "SHA1.pdf", "SHA1_LongMessage"), plus the
	 * empty message. */
	uint8_t d[CRYPTO_SHA1_SIZE];
	CHECK(crypto_sha1("abc", 3, d));
	CHECK_STREQ(hex(d, sizeof(d)), "a9993e364706816aba3e25717850c26c9cd0d89d");
	const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	CHECK(crypto_sha1(m, strlen(m), d));
	CHECK_STREQ(hex(d, sizeof(d)), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
	CHECK(crypto_sha1("", 0, d));
	CHECK_STREQ(hex(d, sizeof(d)), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	char *million = malloc(1000000);
	CHECK(million != NULL);
	if (million != NULL) {
		memset(million, 'a', 1000000);
		CHECK(crypto_sha1(million, 1000000, d));
		CHECK_STREQ(hex(d, sizeof(d)), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
		free(million);
	}
}

static void test_sha256(void)
{
	/* FIPS 180-4 examples (NIST "SHA256.pdf"), plus the empty message. */
	uint8_t d[CRYPTO_SHA256_SIZE];
	CHECK(crypto_sha256("abc", 3, d));
	CHECK_STREQ(hex(d, sizeof(d)),
		    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	CHECK(crypto_sha256(m, strlen(m), d));
	CHECK_STREQ(hex(d, sizeof(d)),
		    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(crypto_sha256("", 0, d));
	CHECK_STREQ(hex(d, sizeof(d)),
		    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	char *million = malloc(1000000);
	CHECK(million != NULL);
	if (million != NULL) {
		memset(million, 'a', 1000000);
		CHECK(crypto_sha256(million, 1000000, d));
		CHECK_STREQ(hex(d, sizeof(d)),
			    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
		free(million);
	}
}

static void test_hmac_sha256(void)
{
	/* RFC 4231 §4.2–§4.8, test cases 1–7. */
	uint8_t key[131], data[50], mac[CRYPTO_SHA256_SIZE];

	memset(key, 0x0b, 20);
	CHECK(crypto_hmac_sha256(key, 20, "Hi There", 8, mac));
	CHECK_STREQ(hex(mac, sizeof(mac)),
		    "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

	CHECK(crypto_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, mac));
	CHECK_STREQ(hex(mac, sizeof(mac)),
		    "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

	memset(key, 0xaa, 20);
	memset(data, 0xdd, 50);
	CHECK(crypto_hmac_sha256(key, 20, data, 50, mac));
	CHECK_STREQ(hex(mac, sizeof(mac)),
		    "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

	for (int i = 0; i < 25; i++)
		key[i] = (uint8_t)(i + 1);
	memset(data, 0xcd, 50);
	CHECK(crypto_hmac_sha256(key, 25, data, 50, mac));
	CHECK_STREQ(hex(mac, sizeof(mac)),
		    "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");

	/* Case 5 truncates the output to 128 bits. */
	memset(key, 0x0c, 20);
	CHECK(crypto_hmac_sha256(key, 20, "Test With Truncation", 20, mac));
	CHECK_STREQ(hex(mac, 16), "a3b6167473100ee06e0c796c2955552b");

	/* Keys longer than the block size are hashed first. */
	memset(key, 0xaa, 131);
	const char *d6 = "Test Using Larger Than Block-Size Key - Hash Key First";
	CHECK(crypto_hmac_sha256(key, 131, d6, strlen(d6), mac));
	CHECK_STREQ(hex(mac, sizeof(mac)),
		    "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
	const char *d7 = "This is a test using a larger than block-size key and a larger than "
			 "block-size data. The key needs to be hashed before being used by the "
			 "HMAC algorithm.";
	CHECK(crypto_hmac_sha256(key, 131, d7, strlen(d7), mac));
	CHECK_STREQ(hex(mac, sizeof(mac)),
		    "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

static void test_pbkdf2(void)
{
	/* RFC 7914 §11, the two PBKDF2-HMAC-SHA-256 vectors. */
	uint8_t dk[64];
	CHECK(crypto_pbkdf2_hmac_sha256("passwd", 6, "salt", 4, 1, dk, sizeof(dk)));
	CHECK_STREQ(hex(dk, sizeof(dk)),
		    "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
		    "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
	CHECK(crypto_pbkdf2_hmac_sha256("Password", 8, "NaCl", 4, 80000, dk, sizeof(dk)));
	CHECK_STREQ(hex(dk, sizeof(dk)),
		    "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
		    "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d");
	/* A shorter output is a prefix (one block). */
	CHECK(crypto_pbkdf2_hmac_sha256("passwd", 6, "salt", 4, 1, dk, 20));
	CHECK_STREQ(hex(dk, 20), "55ac046e56e3089fec1691c22544b605f9418521");
	/* RFC 8018 §5.2: at least one iteration. */
	CHECK(!crypto_pbkdf2_hmac_sha256("passwd", 6, "salt", 4, 0, dk, sizeof(dk)));
}

/* RFC 4648 §10. */
static const char *const b64_vectors[][3] = {
	/* data, base64, base64url */
	{"", "", ""},
	{"f", "Zg==", "Zg"},
	{"fo", "Zm8=", "Zm8"},
	{"foo", "Zm9v", "Zm9v"},
	{"foob", "Zm9vYg==", "Zm9vYg"},
	{"fooba", "Zm9vYmE=", "Zm9vYmE"},
	{"foobar", "Zm9vYmFy", "Zm9vYmFy"},
};

static void test_base64(void)
{
	char out[16];
	uint8_t bin[16];
	size_t n;
	for (size_t i = 0; i < sizeof(b64_vectors) / sizeof(b64_vectors[0]); i++) {
		const char *data = b64_vectors[i][0], *text = b64_vectors[i][1];
		size_t len = strlen(data);
		CHECK(crypto_base64_encode(data, len, out, sizeof(out)) == strlen(text));
		CHECK_STREQ(out, text);
		CHECK(CRYPTO_BASE64_LEN(len) == strlen(text));
		n = 99;
		CHECK(crypto_base64_decode(text, strlen(text), bin, sizeof(bin), &n));
		CHECK(n == len && memcmp(bin, data, len) == 0);
	}
	/* The characters for 62 and 63, and the high bits. */
	const uint8_t hi[] = {0xfb, 0xff, 0xbf};
	CHECK(crypto_base64_encode(hi, 3, out, sizeof(out)) == 4);
	CHECK_STREQ(out, "+/+/");
	CHECK(crypto_base64_decode("+/+/", 4, bin, sizeof(bin), &n));
	CHECK(n == 3 && memcmp(bin, hi, 3) == 0);

	/* Too small: no output, as task 11's encoder. */
	strcpy(out, "x");
	CHECK(crypto_base64_encode("foo", 3, out, 4) == 0);
	CHECK_STREQ(out, "x");
	CHECK(crypto_base64_encode("foo", 3, out, 5) == 4);
	CHECK(crypto_base64_encode("", 0, out, 0) == 0);
	CHECK(crypto_base64_decode("Zm9vYg==", 8, bin, 3, &n) == false);
	CHECK(crypto_base64_decode("Zm9vYg==", 8, bin, 4, &n) && n == 4);

	/* Strict: padding required, only at the end, no whitespace. */
	static const char *const bad[] = {
		"Zg", "Zg=", "Z===", "Zg==Zg==", "=Zg=", " Zg==", "Zg== ", "Zg==\n", "Zm9v\r\n",
		"Zm 9v", "Zm9-", "Zm9_", "Zm9\x80",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		CHECK(!crypto_base64_decode(bad[i], strlen(bad[i]), bin, sizeof(bin), &n));

	/* auth.server_secret (05 §1.1): 32 bytes are 44 characters. */
	CHECK(CRYPTO_BASE64_LEN(32) == 44);
}

static void test_base64url(void)
{
	char out[16];
	uint8_t bin[16];
	size_t n;
	for (size_t i = 0; i < sizeof(b64_vectors) / sizeof(b64_vectors[0]); i++) {
		const char *data = b64_vectors[i][0], *text = b64_vectors[i][2];
		size_t len = strlen(data);
		CHECK(crypto_base64url_encode(data, len, out, sizeof(out)) == strlen(text));
		CHECK_STREQ(out, text);
		CHECK(CRYPTO_BASE64URL_LEN(len) == strlen(text));
		n = 99;
		CHECK(crypto_base64url_decode(text, strlen(text), bin, sizeof(bin), &n));
		CHECK(n == len && memcmp(bin, data, len) == 0);
	}
	/* RFC 4648 §5: '-' and '_' for 62 and 63. */
	const uint8_t hi[] = {0xfb, 0xff, 0xbf};
	CHECK(crypto_base64url_encode(hi, 3, out, sizeof(out)) == 4);
	CHECK_STREQ(out, "-_-_");
	CHECK(crypto_base64url_decode("-_-_", 4, bin, sizeof(bin), &n));
	CHECK(n == 3 && memcmp(bin, hi, 3) == 0);

	/* The session secret (02 §2.5 item 1): 16 bytes are 22 characters. */
	uint8_t secret[16];
	for (int i = 0; i < 16; i++)
		secret[i] = (uint8_t)(0xf0 + i);
	char text[CRYPTO_BASE64URL_LEN(16) + 1];
	CHECK(sizeof(text) == 23);
	CHECK(crypto_base64url_encode(secret, 16, text, sizeof(text)) == 22);
	CHECK_STREQ(text, "8PHy8_T19vf4-fr7_P3-_w");
	CHECK(crypto_base64url_decode(text, 22, bin, 16, &n) && n == 16);
	CHECK(memcmp(bin, secret, 16) == 0);
	CHECK(!crypto_base64url_decode(text, 22, bin, 15, &n));

	/* Too small: no output. */
	strcpy(out, "x");
	CHECK(crypto_base64url_encode("f", 1, out, 2) == 0);
	CHECK_STREQ(out, "x");
	CHECK(crypto_base64url_encode("f", 1, out, 3) == 2);

	/* Strict: no padding, no standard-alphabet characters, no whitespace,
	 * not a length of 4k + 1. */
	static const char *const bad[] = {"Zg==", "Zg=", "Z", "Zm9vY", "+/+/", "Zm 9v", "Zm9v\n"};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		CHECK(!crypto_base64url_decode(bad[i], strlen(bad[i]), bin, sizeof(bin), &n));
}

static void test_base64_long(void)
{
	/* Longer than the 48-byte chunks base64url is converted in: both
	 * encodings agree (up to the alphabet and padding) and round-trip. */
	uint8_t data[200], back[200];
	char std[CRYPTO_BASE64_LEN(200) + 1], url[CRYPTO_BASE64URL_LEN(200) + 1];
	for (int i = 0; i < 200; i++)
		data[i] = (uint8_t)(i * 37 + 11);
	for (size_t len = 0; len <= 200; len++) {
		size_t sn = crypto_base64_encode(data, len, std, sizeof(std));
		size_t un = crypto_base64url_encode(data, len, url, sizeof(url));
		CHECK(sn == CRYPTO_BASE64_LEN(len) && un == CRYPTO_BASE64URL_LEN(len));
		bool same = true;
		for (size_t k = 0; k < sn; k++) {
			char c = std[k] == '+' ? '-' : std[k] == '/' ? '_' : std[k];
			if (k < un ? url[k] != c : c != '=')
				same = false;
		}
		CHECK(same);
		size_t n = 0;
		CHECK(crypto_base64_decode(std, sn, back, sizeof(back), &n));
		CHECK(n == len && memcmp(back, data, len) == 0);
		CHECK(crypto_base64url_decode(url, un, back, sizeof(back), &n));
		CHECK(n == len && memcmp(back, data, len) == 0);
		CHECK(CRYPTO_BASE64_DECODED_MAX(sn) >= len && CRYPTO_BASE64_DECODED_MAX(un) >= len);
	}
}

static void test_websocket_accept(void)
{
	/* RFC 6455 §1.3: Sec-WebSocket-Accept is base64(SHA-1(key + GUID)). */
	const char *in = "dGhlIHNhbXBsZSBub25jZQ=="
			 "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	uint8_t d[CRYPTO_SHA1_SIZE];
	char accept[CRYPTO_BASE64_LEN(CRYPTO_SHA1_SIZE) + 1];
	CHECK(crypto_sha1(in, strlen(in), d));
	CHECK(crypto_base64_encode(d, sizeof(d), accept, sizeof(accept)) == 28);
	CHECK_STREQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

static void test_scram_sha256(void)
{
	/* RFC 7677 §3: user "user", password "pencil". This checks the exact
	 * chain of 02 §2.2–§2.3 (SaltedPassword, ClientKey, StoredKey,
	 * ServerKey, ClientSignature, proof, ServerSignature, and the server's
	 * check of the proof) against the RFC's proof and server signature. The
	 * AuthMessage is RFC 5802's; hakkas' own (02 §2.3) is built differently
	 * but goes through the same functions. */
	const char *password = "pencil";
	const char *salt64 = "W22ZaJ0SNY7soEsUEjb6gQ==";
	const uint32_t iterations = 4096;
	const char *auth = "n=user,r=rOprNGfwEbeRWgbNEkqO,"
			   "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
			   "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096,"
			   "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0";

	uint8_t salt[16];
	size_t salt_len;
	CHECK(crypto_base64_decode(salt64, strlen(salt64), salt, sizeof(salt), &salt_len));
	CHECK(salt_len == 16);

	uint8_t salted[32], client_key[32], stored_key[32], server_key[32];
	CHECK(crypto_pbkdf2_hmac_sha256(password, strlen(password), salt, salt_len, iterations,
					salted, sizeof(salted)));
	CHECK(crypto_hmac_sha256(salted, 32, "Client Key", 10, client_key));
	CHECK(crypto_sha256(client_key, 32, stored_key));
	CHECK(crypto_hmac_sha256(salted, 32, "Server Key", 10, server_key));

	/* Client: proof = ClientKey XOR HMAC(StoredKey, AuthMessage). */
	uint8_t client_sig[32], proof[32];
	CHECK(crypto_hmac_sha256(stored_key, 32, auth, strlen(auth), client_sig));
	for (int i = 0; i < 32; i++)
		proof[i] = client_key[i] ^ client_sig[i];
	char text[CRYPTO_BASE64_LEN(32) + 1];
	CHECK(crypto_base64_encode(proof, 32, text, sizeof(text)) == 44);
	CHECK_STREQ(text, "dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=");

	/* Server signature = HMAC(ServerKey, AuthMessage). */
	uint8_t server_sig[32];
	CHECK(crypto_hmac_sha256(server_key, 32, auth, strlen(auth), server_sig));
	CHECK(crypto_base64_encode(server_sig, 32, text, sizeof(text)) == 44);
	CHECK_STREQ(text, "6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=");

	/* Server: from stored_key and the received proof alone, recover
	 * ClientKey and accept iff SHA256(ClientKey) == stored_key, compared in
	 * constant time. A proof with one bit flipped is refused. */
	uint8_t received[32], sig[32], recovered[32], check[32];
	size_t n;
	const char *proof64 = "dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=";
	CHECK(crypto_base64_decode(proof64, strlen(proof64), received, sizeof(received), &n));
	CHECK(n == 32);
	CHECK(crypto_hmac_sha256(stored_key, 32, auth, strlen(auth), sig));
	for (int i = 0; i < 32; i++)
		recovered[i] = received[i] ^ sig[i];
	CHECK(crypto_sha256(recovered, 32, check));
	CHECK(crypto_equal(check, stored_key, 32));
	received[31] ^= 0x01;
	for (int i = 0; i < 32; i++)
		recovered[i] = received[i] ^ sig[i];
	CHECK(crypto_sha256(recovered, 32, check));
	CHECK(!crypto_equal(check, stored_key, 32));

	crypto_zeroize(salted, sizeof(salted));
	uint8_t zero[32] = {0};
	CHECK(memcmp(salted, zero, 32) == 0);
}

static void test_equal(void)
{
	uint8_t a[32], b[32];
	for (int i = 0; i < 32; i++)
		a[i] = b[i] = (uint8_t)(i * 7);
	CHECK(crypto_equal(a, b, 32));
	CHECK(crypto_equal(a, b, 0));
	b[0] ^= 0x80;
	CHECK(!crypto_equal(a, b, 32));
	CHECK(crypto_equal(a + 1, b + 1, 31));
	b[0] = a[0];
	b[31] ^= 0x01;
	CHECK(!crypto_equal(a, b, 32));
	CHECK(crypto_equal(a, b, 31));
}

int main(void)
{
	RUN(test_version);
	RUN(test_sha1);
	RUN(test_sha256);
	RUN(test_hmac_sha256);
	RUN(test_pbkdf2);
	RUN(test_base64);
	RUN(test_base64url);
	RUN(test_base64_long);
	RUN(test_websocket_accept);
	RUN(test_scram_sha256);
	RUN(test_equal);
	return test_result();
}
