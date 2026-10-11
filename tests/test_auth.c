/*
 * Authorization (protocol/auth.h, 02 §2.3-§2.5) through its Authorizer
 * interface, with a controlled clock and random source and test users made
 * here (none is compiled into the product): a complete SCRAM login computed
 * the way a client does, every failure answered alike, unknown users,
 * nonces, session secrets and their binding, idle and lifetime expiry,
 * logout, close, the role and command tables of 02 §2.4, rate limiting and
 * the back-off schedule, and log lines free of secret material.
 */
#include "test.h"

#include <stdlib.h>

#include "protocol/auth.h"
#include "protocol/users.h"
#include "system/crypto.h"
#include "system/log.h"

/* ---- Fixtures ---------------------------------------------------------------------- */

static uint64_t clock_ms = 1000;

static uint64_t fake_clock(void *ctx)
{
	(void)ctx;
	return clock_ms;
}

static uint8_t rnd_next;
static bool rnd_fail;

static enum pal_err fake_random(void *buf, size_t len)
{
	if (rnd_fail)
		return PAL_ERR_UNAVAILABLE;
	for (size_t i = 0; i < len; i++)
		((uint8_t *)buf)[i] = rnd_next++;
	return PAL_OK;
}

static char logs[32768];

static void log_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)len;
	size_t used = strlen(logs);
	snprintf(logs + used, sizeof(logs) - used, "%s %s\n", log_level_name(level), line);
}

#define SERVER_SECRET "c2VydmVyIHNlY3JldCBmb3IgdGVzdHMgb25seSEhISE="
#define ITERATIONS 64
#define IDLE_S 10
#define LIFE_S 100

static UserStore users;
static Auth *auth;
static Authorizer az;

/* The callbacks, recording what they saw. */
static int expired_calls;
static void *expired_user;
static int challenge_calls;
static bool challenge_null;
static AuthChallenge delivered;

static void on_expired(void *user)
{
	expired_calls++;
	expired_user = user;
}

static void on_challenge(void *user, const AuthChallenge *ch)
{
	(void)user;
	challenge_calls++;
	challenge_null = ch == NULL;
	if (ch != NULL)
		delivered = *ch;
}

static const AuthEvents events = {on_expired, on_challenge};

static void setup(void)
{
	users_init(&users);
	CHECK(users_add(&users, "alice", USER_ROLE_EDITOR, "pencil", 6, ITERATIONS,
			fake_random) == USERS_OK);
	CHECK(users_add(&users, "victor", USER_ROLE_VIEWER, "seeing", 6, ITERATIONS,
			fake_random) == USERS_OK);
	AuthConfig cfg = {
		.users = &users,
		.server_secret = SERVER_SECRET,
		.idle_timeout_s = IDLE_S,
		.max_lifetime_s = LIFE_S,
		.clock = fake_clock,
		.random_bytes = fake_random,
	};
	CHECK(auth_create(&cfg, &auth));
	az = auth_authorizer(auth);
	expired_calls = challenge_calls = 0;
	clock_ms += 24u * 3600u * 1000u; /* far from any earlier test's rate entries */
}

static void teardown(void)
{
	auth_destroy(auth);
	auth = NULL;
}

static AuthConn *open_conn(const char *addr, void *user)
{
	AuthConn *c = az.ops->open(az.self, addr, &events, user);
	CHECK(c != NULL);
	return c;
}

static void close_conn(AuthConn *c)
{
	az.ops->close(az.self, c);
}

/* A client nonce: the base64 of 18 bytes from seed. */
static const char *cnonce_of(int seed)
{
	static char buf[4][64];
	static int next;
	char *p = buf[next++ % 4];
	uint8_t b[18];
	for (int i = 0; i < 18; i++)
		b[i] = (uint8_t)(seed * 31 + i);
	crypto_base64_encode(b, sizeof(b), p, 64);
	return p;
}

/* The client's side of 02 §2.3: the proof for password and the server
 * signature it expects, from the challenge's salt, iterations and nonce. */
static void client_proof(const char *user, const char *password, const AuthChallenge *ch,
			 char proof64[64], char sig64[64])
{
	uint8_t salt[16], salted[32], client_key[32], stored[32], server_key[32];
	uint8_t client_sig[32], proof[32], server_sig[32];
	size_t n = 0;
	CHECK(crypto_base64_decode(ch->salt, strlen(ch->salt), salt, sizeof(salt), &n) && n == 16);
	CHECK(crypto_pbkdf2_hmac_sha256(password, strlen(password), salt, 16, ch->iterations,
					salted, 32));
	CHECK(crypto_hmac_sha256(salted, 32, "Client Key", 10, client_key));
	CHECK(crypto_sha256(client_key, 32, stored));
	CHECK(crypto_hmac_sha256(salted, 32, "Server Key", 10, server_key));
	char msg[512];
	snprintf(msg, sizeof(msg), "%s,%s,%s,%u", user, ch->nonce, ch->salt,
		 (unsigned)ch->iterations);
	CHECK(crypto_hmac_sha256(stored, 32, msg, strlen(msg), client_sig));
	for (int i = 0; i < 32; i++)
		proof[i] = client_key[i] ^ client_sig[i];
	CHECK(crypto_hmac_sha256(server_key, 32, msg, strlen(msg), server_sig));
	crypto_base64_encode(proof, 32, proof64, 64);
	crypto_base64_encode(server_sig, 32, sig64, 64);
}

/* A full login of user with password on c; true on success, the secret in
 * secret (if not NULL). */
static bool login(AuthConn *c, const char *user, const char *password, char *secret)
{
	AuthChallenge ch;
	if (az.ops->challenge(az.self, c, user, cnonce_of(1), &ch) != AUTH_OK)
		return false;
	char proof[64], sig[64];
	client_proof(user, password, &ch, proof, sig);
	AuthProof p;
	if (az.ops->verify_proof(az.self, c, user, ch.nonce, proof, &p) != AUTH_OK)
		return false;
	CHECK_STREQ(p.server_signature, sig);
	char s[AUTH_SECRET_SIZE];
	if (!az.ops->issue_secret(az.self, c, s))
		return false;
	if (secret != NULL)
		memcpy(secret, s, sizeof(s));
	return true;
}

/* One failed login (a login without a challenge) from c. */
static enum auth_result fail_once(AuthConn *c)
{
	AuthProof p;
	return az.ops->verify_proof(az.self, c, "alice", "x", "y", &p);
}

/* ---- Login ------------------------------------------------------------------------- */

static void test_login(void)
{
	setup();
	AuthConn *c = open_conn("192.0.2.1", NULL);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_UNAUTHENTICATED);
	CHECK(az.ops->capabilities_for(az.self, c) == 0);

	/* Step 1: the salt and count of alice's record, the nonce cnonce +
	 * base64 of 16 server bytes. */
	AuthChallenge ch;
	const char *cn = cnonce_of(7);
	CHECK(az.ops->challenge(az.self, c, "alice", cn, &ch) == AUTH_OK);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_CHALLENGED);
	char salt64[32];
	crypto_base64_encode(users_find(&users, "alice")->salt, 16, salt64, sizeof(salt64));
	CHECK_STREQ(ch.salt, salt64);
	CHECK(ch.iterations == ITERATIONS);
	CHECK(strncmp(ch.nonce, cn, strlen(cn)) == 0);
	CHECK(strlen(ch.nonce) == strlen(cn) + 24);
	uint8_t server_part[18];
	size_t n = 0;
	CHECK(crypto_base64_decode(ch.nonce + strlen(cn), 24, server_part, sizeof(server_part),
				   &n) &&
	      n == 16);

	/* Step 2: the client's proof. */
	char proof[64], sig[64];
	client_proof("alice", "pencil", &ch, proof, sig);
	AuthProof p;
	CHECK(az.ops->verify_proof(az.self, c, "alice", ch.nonce, proof, &p) == AUTH_OK);
	CHECK(p.role == USER_ROLE_EDITOR);
	CHECK(p.capabilities == auth_role_capabilities(USER_ROLE_EDITOR));
	CHECK_STREQ(p.server_signature, sig);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_VERIFIED);
	CHECK(az.ops->capabilities_for(az.self, c) == 0); /* no session yet */

	/* The session secret: 22 characters of base64url, 128 random bits. */
	char secret[AUTH_SECRET_SIZE];
	CHECK(az.ops->issue_secret(az.self, c, secret));
	CHECK(strlen(secret) == 22);
	for (int i = 0; i < 22; i++)
		CHECK(strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_",
			     secret[i]) != NULL);
	uint8_t raw[18];
	CHECK(crypto_base64url_decode(secret, 22, raw, sizeof(raw), &n) && n == 16);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_AUTHENTICATED);
	CHECK(az.ops->capabilities_for(az.self, c) == auth_role_capabilities(USER_ROLE_EDITOR));
	CHECK(az.ops->validate_secret(az.self, c, secret));
	char other[AUTH_SECRET_SIZE];
	memcpy(other, secret, sizeof(other));
	other[21] = other[21] == 'A' ? 'B' : 'A';
	CHECK(!az.ops->validate_secret(az.self, c, other));
	CHECK(!az.ops->validate_secret(az.self, c, ""));
	CHECK(!az.ops->validate_secret(az.self, c, NULL));
	other[21] = '\0';
	CHECK(!az.ops->validate_secret(az.self, c, other));
	CHECK(!az.ops->issue_secret(az.self, c, other)); /* only once per login */

	/* A challenge while logged in is refused: log out first. */
	CHECK(az.ops->challenge(az.self, c, "alice", cn, &ch) == AUTH_FAILED);
	CHECK(az.ops->validate_secret(az.self, c, secret));

	/* A viewer gets the viewer's capabilities. */
	AuthConn *v = open_conn("192.0.2.2", NULL);
	CHECK(login(v, "victor", "seeing", NULL));
	CHECK(az.ops->capabilities_for(az.self, v) == auth_role_capabilities(USER_ROLE_VIEWER));

	/* issue_secret without a verified proof, or with a failing random
	 * source. */
	AuthConn *w = open_conn("192.0.2.3", NULL);
	CHECK(!az.ops->issue_secret(az.self, w, secret));
	CHECK(az.ops->challenge(az.self, w, "alice", cn, &ch) == AUTH_OK);
	client_proof("alice", "pencil", &ch, proof, sig);
	CHECK(az.ops->verify_proof(az.self, w, "alice", ch.nonce, proof, &p) == AUTH_OK);
	rnd_fail = true;
	CHECK(!az.ops->issue_secret(az.self, w, secret));
	rnd_fail = false;
	CHECK(az.ops->state(az.self, w) == AUTH_STATE_UNAUTHENTICATED);
	/* A failing random source refuses a challenge too. */
	rnd_fail = true;
	CHECK(az.ops->challenge(az.self, w, "alice", cn, &ch) == AUTH_FAILED);
	rnd_fail = false;

	close_conn(c);
	close_conn(v);
	close_conn(w);
	teardown();
}

static void test_binding(void)
{
	setup();
	AuthConn *a = open_conn("192.0.2.10", NULL);
	AuthConn *b = open_conn("192.0.2.10", NULL);
	char sa[AUTH_SECRET_SIZE], sb[AUTH_SECRET_SIZE];
	CHECK(login(a, "alice", "pencil", sa));
	CHECK(!az.ops->validate_secret(az.self, b, sa)); /* b is not logged in */
	CHECK(login(b, "alice", "pencil", sb));
	CHECK(strcmp(sa, sb) != 0);
	CHECK(!az.ops->validate_secret(az.self, b, sa)); /* a's secret, b's connection */
	CHECK(!az.ops->validate_secret(az.self, a, sb));
	CHECK(az.ops->validate_secret(az.self, a, sa));
	CHECK(az.ops->validate_secret(az.self, b, sb));
	close_conn(a);
	close_conn(b);
	teardown();
}

/* ---- Failures ---------------------------------------------------------------------- */

static void test_unknown_user(void)
{
	setup();
	AuthConn *c = open_conn("192.0.2.20", NULL);
	AuthChallenge ch, ch2, real;
	CHECK(az.ops->challenge(az.self, c, "mallory", cnonce_of(2), &ch) == AUTH_OK);
	/* salt = HMAC(server_secret, user)[0..16], the default count, the
	 * same nonce shape: like a real reply. */
	uint8_t h[32];
	CHECK(crypto_hmac_sha256(SERVER_SECRET, strlen(SERVER_SECRET), "mallory", 7, h));
	char salt64[32];
	crypto_base64_encode(h, 16, salt64, sizeof(salt64));
	CHECK_STREQ(ch.salt, salt64);
	CHECK(ch.iterations == USERS_DEFAULT_ITERATIONS);
	CHECK(strlen(ch.nonce) == strlen(cnonce_of(2)) + 24);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_CHALLENGED);
	/* Deterministic: the same salt on every challenge and connection. */
	AuthConn *d = open_conn("192.0.2.21", NULL);
	CHECK(az.ops->challenge(az.self, d, "mallory", cnonce_of(3), &ch2) == AUTH_OK);
	CHECK_STREQ(ch2.salt, ch.salt);
	CHECK(strcmp(ch2.nonce + strlen(cnonce_of(3)), ch.nonce + strlen(cnonce_of(2))) != 0);
	/* A real user's reply has the same shape. */
	users.users[0].iterations = USERS_DEFAULT_ITERATIONS;
	CHECK(az.ops->challenge(az.self, d, "alice", cnonce_of(3), &real) == AUTH_OK);
	CHECK(strlen(real.salt) == strlen(ch.salt) && real.iterations == ch.iterations);
	CHECK(strlen(real.nonce) == strlen(ch2.nonce));
	users.users[0].iterations = ITERATIONS;

	/* Whatever the client sends, the login fails like any other (a
	 * client would compute its proof with 100000 iterations; any 32
	 * bytes do here). */
	const char *proof = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
	AuthProof p;
	CHECK(az.ops->verify_proof(az.self, c, "mallory", ch.nonce, proof, &p) == AUTH_FAILED);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_UNAUTHENTICATED);
	close_conn(c);
	close_conn(d);
	teardown();
}

/* A fresh connection from a fresh address (so the back-off stays out of the
 * way), challenged for alice. */
static int addr_seq;
static AuthConn *challenged(AuthChallenge *ch)
{
	char addr[32];
	snprintf(addr, sizeof(addr), "198.51.100.%d", ++addr_seq);
	AuthConn *c = open_conn(addr, NULL);
	CHECK(az.ops->challenge(az.self, c, "alice", cnonce_of(addr_seq), ch) == AUTH_OK);
	return c;
}

static void test_login_failures(void)
{
	setup();
	AuthChallenge ch;
	AuthProof p;
	char proof[64], sig[64];

	/* Wrong password. */
	AuthConn *c = challenged(&ch);
	client_proof("alice", "pencil!", &ch, proof, sig);
	CHECK(az.ops->verify_proof(az.self, c, "alice", ch.nonce, proof, &p) == AUTH_FAILED);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_UNAUTHENTICATED);
	close_conn(c);

	/* An expired nonce: valid for 30 s. */
	c = challenged(&ch);
	client_proof("alice", "pencil", &ch, proof, sig);
	clock_ms += AUTH_NONCE_TTL_MS - 1;
	CHECK(az.ops->verify_proof(az.self, c, "alice", ch.nonce, proof, &p) == AUTH_OK);
	close_conn(c);
	c = challenged(&ch);
	client_proof("alice", "pencil", &ch, proof, sig);
	clock_ms += AUTH_NONCE_TTL_MS;
	CHECK(az.ops->verify_proof(az.self, c, "alice", ch.nonce, proof, &p) == AUTH_FAILED);
	close_conn(c);
	/* The tick hook also drops it. */
	c = challenged(&ch);
	CHECK(auth_tick(auth, clock_ms) == AUTH_NONCE_TTL_MS);
	CHECK(auth_tick(auth, clock_ms + AUTH_NONCE_TTL_MS) == UINT64_MAX);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_UNAUTHENTICATED);
	close_conn(c);

	/* A reused nonce: single use, after a failure and after a success. */
	c = challenged(&ch);
	client_proof("alice", "pencil", &ch, proof, sig);
	CHECK(az.ops->verify_proof(az.self, c, "alice", ch.nonce, "AAAA", &p) == AUTH_FAILED);
	CHECK(az.ops->verify_proof(az.self, c, "alice", ch.nonce, proof, &p) == AUTH_FAILED);
	close_conn(c);
	c = challenged(&ch);
	client_proof("alice", "pencil", &ch, proof, sig);
	CHECK(az.ops->verify_proof(az.self, c, "alice", ch.nonce, proof, &p) == AUTH_OK);
	az.ops->revoke_secret(az.self, c);
	CHECK(az.ops->verify_proof(az.self, c, "alice", ch.nonce, proof, &p) == AUTH_FAILED);
	close_conn(c);
	/* A new challenge replaces the old one. */
	c = challenged(&ch);
	client_proof("alice", "pencil", &ch, proof, sig);
	AuthChallenge ch2;
	CHECK(az.ops->challenge(az.self, c, "alice", cnonce_of(99), &ch2) == AUTH_OK);
	CHECK(az.ops->verify_proof(az.self, c, "alice", ch.nonce, proof, &p) == AUTH_FAILED);
	close_conn(c);

	/* A login without a challenge. */
	c = challenged(&ch);
	az.ops->revoke_secret(az.self, c);
	CHECK(az.ops->verify_proof(az.self, c, "alice", ch.nonce, proof, &p) == AUTH_FAILED);
	close_conn(c);

	/* A proof of the wrong length or not base64; a wrong user or nonce. */
	const char *bad_proofs[] = {
		"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHg==",	  /* 31 bytes */
		"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gIQ==", /* 34 bytes */
		"",
		NULL,
		"!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!=",
	};
	for (int i = 0; i < 5; i++) {
		c = challenged(&ch);
		CHECK(az.ops->verify_proof(az.self, c, "alice", ch.nonce, bad_proofs[i], &p) ==
		      AUTH_FAILED);
		close_conn(c);
	}
	c = challenged(&ch);
	client_proof("alice", "pencil", &ch, proof, sig);
	CHECK(az.ops->verify_proof(az.self, c, "victor", ch.nonce, proof, &p) == AUTH_FAILED);
	close_conn(c);
	c = challenged(&ch);
	client_proof("alice", "pencil", &ch, proof, sig);
	char nonce[AUTH_NONCE_TEXT_SIZE];
	strcpy(nonce, ch.nonce);
	nonce[strlen(nonce) - 3] ^= 1;
	CHECK(az.ops->verify_proof(az.self, c, "alice", nonce, proof, &p) == AUTH_FAILED);
	CHECK(az.ops->verify_proof(az.self, c, "alice", NULL, proof, &p) == AUTH_FAILED);
	close_conn(c);

	/* Invalid challenges are refused, and not counted as failed logins. */
	c = open_conn("203.0.113.99", NULL);
	const char *bad_cnonces[] = {
		"AAECAwQFBgcICQoLDA0O",		/* 15 bytes */
		"AAECAwQFBgcICQoLDA0ODw",	/* unpadded */
		"AAECAwQF BgcICQoLDA0ODw==",	/* a space */
		"",
		NULL,
	};
	for (int i = 0; i < 5; i++)
		CHECK(az.ops->challenge(az.self, c, "alice", bad_cnonces[i], &ch) == AUTH_FAILED);
	uint8_t big[65] = {0};
	char big64[128];
	crypto_base64_encode(big, sizeof(big), big64, sizeof(big64));
	CHECK(az.ops->challenge(az.self, c, "alice", big64, &ch) == AUTH_FAILED);
	crypto_base64_encode(big, 64, big64, sizeof(big64));
	CHECK(az.ops->challenge(az.self, c, "alice", big64, &ch) == AUTH_OK);
	const char *bad_users[] = {"", "123456789012345678901234567890123", "a\nb", NULL};
	for (int i = 0; i < 4; i++)
		CHECK(az.ops->challenge(az.self, c, bad_users[i], cnonce_of(1), &ch) ==
		      AUTH_FAILED);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_UNAUTHENTICATED);
	CHECK(login(c, "alice", "pencil", NULL)); /* still allowed */
	close_conn(c);
	teardown();
}

/* ---- Sessions ---------------------------------------------------------------------- */

static void test_expiry(void)
{
	setup();
	int tag;
	AuthConn *c = open_conn("192.0.2.30", &tag);
	char secret[AUTH_SECRET_SIZE];
	CHECK(login(c, "alice", "pencil", secret));
	uint64_t t0 = clock_ms;
	CHECK(auth_tick(auth, t0) == IDLE_S * 1000u);

	/* Idle: each client message restarts the 10 s. */
	clock_ms = t0 + 9999;
	CHECK(auth_tick(auth, clock_ms) == 1);
	CHECK(az.ops->validate_secret(az.self, c, secret));
	az.ops->activity(az.self, c);
	clock_ms = t0 + 9999 + 9999;
	CHECK(auth_tick(auth, clock_ms) == 1);
	CHECK(expired_calls == 0);
	CHECK(az.ops->validate_secret(az.self, c, secret));
	clock_ms++;
	/* Expired from this instant, before the tick hook runs. */
	CHECK(!az.ops->validate_secret(az.self, c, secret));
	CHECK(az.ops->capabilities_for(az.self, c) == 0);
	az.ops->activity(az.self, c); /* does not revive it */
	CHECK(!az.ops->validate_secret(az.self, c, secret));
	CHECK(auth_tick(auth, clock_ms) == UINT64_MAX);
	CHECK(expired_calls == 1 && expired_user == &tag);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_UNAUTHENTICATED);
	CHECK(auth_tick(auth, clock_ms + 1) == UINT64_MAX);
	CHECK(expired_calls == 1);

	/* The socket stays: log in again; the maximum lifetime of 100 s ends
	 * the session even when it is never idle. */
	CHECK(login(c, "alice", "pencil", secret));
	t0 = clock_ms;
	for (int i = 1; i < 20; i++) {
		clock_ms = t0 + (uint64_t)i * 5000;
		az.ops->activity(az.self, c);
		CHECK(auth_tick(auth, clock_ms) ==
		      (i * 5000 + IDLE_S * 1000 < LIFE_S * 1000 ? IDLE_S * 1000u
								 : LIFE_S * 1000u - i * 5000u));
	}
	CHECK(expired_calls == 1);
	clock_ms = t0 + LIFE_S * 1000u - 1;
	az.ops->activity(az.self, c);
	CHECK(az.ops->validate_secret(az.self, c, secret));
	clock_ms++;
	CHECK(!az.ops->validate_secret(az.self, c, secret));
	logs[0] = '\0';
	auth_tick(auth, clock_ms);
	CHECK(expired_calls == 2);
	CHECK(strstr(logs, "maximum lifetime") != NULL);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_UNAUTHENTICATED);
	close_conn(c);
	teardown();
}

static void test_logout_and_close(void)
{
	setup();
	AuthConn *c = open_conn("192.0.2.40", NULL);
	char secret[AUTH_SECRET_SIZE];
	CHECK(login(c, "alice", "pencil", secret));
	az.ops->revoke_secret(az.self, c);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_UNAUTHENTICATED);
	CHECK(!az.ops->validate_secret(az.self, c, secret));
	CHECK(az.ops->capabilities_for(az.self, c) == 0);
	CHECK(auth_tick(auth, clock_ms + LIFE_S * 1000u) == UINT64_MAX);
	CHECK(expired_calls == 0); /* a logout is not an expiry */
	az.ops->revoke_secret(az.self, c); /* twice is harmless */
	char again[AUTH_SECRET_SIZE];
	CHECK(login(c, "alice", "pencil", again));
	CHECK(strcmp(again, secret) != 0);

	/* A close ends the session; nothing fires afterwards. */
	AuthConn *d = open_conn("192.0.2.41", NULL);
	CHECK(login(d, "victor", "seeing", NULL));
	close_conn(d);
	close_conn(c);
	az.ops->close(az.self, NULL);
	CHECK(auth_tick(auth, clock_ms + LIFE_S * 1000u) == UINT64_MAX);
	CHECK(expired_calls == 0);

	/* auth_destroy frees connections still open. */
	AuthConn *e = open_conn("192.0.2.42", NULL);
	CHECK(login(e, "alice", "pencil", NULL));
	teardown();
}

/* ---- Roles and commands (02 §2.4) ------------------------------------------------- */

static void test_capabilities(void)
{
	const char *names[] = {"protocol:runtime", "protocol:graph",  "protocol:component",
			       "graph:readonly",   "network:status",  "network:persist",
			       "network:data",	   "network:control"};
	for (unsigned i = 0; i < AUTH_CAP_COUNT; i++)
		CHECK_STREQ(auth_capability_name(i), names[i]);
	CHECK(auth_capability_name(AUTH_CAP_COUNT) == NULL);

	uint32_t viewer = AUTH_CAP_PROTOCOL_COMPONENT | AUTH_CAP_NETWORK_STATUS |
			  AUTH_CAP_NETWORK_DATA | AUTH_CAP_GRAPH_READONLY;
	uint32_t oper = viewer | AUTH_CAP_NETWORK_CONTROL;
	uint32_t editor = oper | AUTH_CAP_PROTOCOL_GRAPH | AUTH_CAP_PROTOCOL_RUNTIME |
			  AUTH_CAP_NETWORK_PERSIST;
	CHECK(auth_role_capabilities(USER_ROLE_VIEWER) == viewer);
	CHECK(auth_role_capabilities(USER_ROLE_OPERATOR) == oper);
	CHECK(auth_role_capabilities(USER_ROLE_EDITOR) == editor);
	CHECK(auth_role_capabilities(USER_ROLE_ADMIN) == editor);
	CHECK(editor == AUTH_CAPS_ALL);
	CHECK(auth_role_capabilities((enum user_role)9) == 0);
}

typedef struct {
	const char *protocol, *command;
	bool gated;
	uint32_t caps_any;
} Expect;

static void test_command_rules(void)
{
	const uint32_t rd = AUTH_CAP_GRAPH_READONLY | AUTH_CAP_PROTOCOL_GRAPH;
	const uint32_t g = AUTH_CAP_PROTOCOL_GRAPH;
	static const Expect known[] = {
		{"environment", "challenge", false, 0},
		{"environment", "login", false, 0},
		{"environment", "logout", false, 0},
		{"runtime", "getruntime", false, 0},
		{"runtime", "new_graph", true, AUTH_CAP_PROTOCOL_RUNTIME},
		{"runtime", "delete_graph", true, AUTH_CAP_PROTOCOL_RUNTIME},
		{"network", "getstatus", true, AUTH_CAP_NETWORK_STATUS},
		{"network", "edges", true, AUTH_CAP_NETWORK_DATA},
		{"network", "start", true, AUTH_CAP_NETWORK_CONTROL},
		{"network", "stop", true, AUTH_CAP_NETWORK_CONTROL},
		{"network", "persist", true, AUTH_CAP_NETWORK_PERSIST},
		{"graph", "connect", true, 0},
		{"graph", "list", true, 0},
		{"component", "list", true, 0},
		{"component", "getsource", true, 0},
		{"trace", "start", true, 0},
		{"trace", "stop", true, 0},
		{"trace", "clear", true, 0},
		{"trace", "dump", true, 0},
		{"graph", "rename", true, 1},
		{"graph", "addnode", true, 1},
		{"graph", "removenode", true, 1},
		{"graph", "renamenode", true, 1},
		{"graph", "changenode", true, 1},
		{"graph", "addedge", true, 1},
		{"graph", "removeedge", true, 1},
		{"graph", "changeedge", true, 1},
		{"graph", "addinitial", true, 1},
		{"graph", "removeinitial", true, 1},
		{"graph", "addinport", true, 1},
		{"graph", "removeinport", true, 1},
		{"graph", "renameinport", true, 1},
		{"graph", "addoutport", true, 1},
		{"graph", "removeoutport", true, 1},
		{"graph", "renameoutport", true, 1},
		{"graph", "addgroup", true, 1},
		{"graph", "removegroup", true, 1},
		{"graph", "renamegroup", true, 1},
		{"graph", "changegroup", true, 1},
	};
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		const Expect *e = &known[i];
		/* In the table, 0 on a gated graph/component/trace row stands
		 * for "read" and 1 for "protocol:graph". */
		uint32_t want = e->caps_any;
		bool reading = strcmp(e->protocol, "graph") == 0 ||
			       strcmp(e->protocol, "component") == 0 ||
			       strcmp(e->protocol, "trace") == 0;
		if (reading)
			want = want == 1 ? g : rd;
		AuthCommandRule r = auth_command_rule(e->protocol, e->command);
		if (!r.known || r.gated != e->gated || r.caps_any != want)
			fprintf(stderr, "rule %s:%s\n", e->protocol, e->command);
		CHECK(r.known && r.gated == e->gated && r.caps_any == want);
	}

	/* Unknown commands: gated, no capability, except in environment. */
	AuthCommandRule r = auth_command_rule("environment", "whoami");
	CHECK(!r.known && !r.gated && r.caps_any == 0);
	const char *unknown[][2] = {{"graph", "clear"},	    {"runtime", "packet"},
				    {"network", "data"},    {"component", "source"},
				    {"trace", "begin"},	    {"bogus", "list"},
				    {"Graph", "addnode"},   {NULL, "list"},
				    {"graph", NULL}};
	for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
		r = auth_command_rule(unknown[i][0], unknown[i][1]);
		CHECK(!r.known && r.gated && r.caps_any == 0);
	}

	/* Who may do what. */
	uint32_t viewer = auth_role_capabilities(USER_ROLE_VIEWER);
	uint32_t oper = auth_role_capabilities(USER_ROLE_OPERATOR);
	uint32_t editor = auth_role_capabilities(USER_ROLE_EDITOR);
	struct {
		const char *protocol, *command;
		bool none, viewer, op, editor;
	} who[] = {
		{"runtime", "getruntime", true, true, true, true},
		{"graph", "connect", false, true, true, true},
		{"graph", "list", false, true, true, true},
		{"component", "list", false, true, true, true},
		{"trace", "dump", false, true, true, true},
		{"network", "getstatus", false, true, true, true},
		{"network", "edges", false, true, true, true},
		{"network", "start", false, false, true, true},
		{"network", "stop", false, false, true, true},
		{"network", "persist", false, false, false, true},
		{"graph", "addnode", false, false, false, true},
		{"graph", "rename", false, false, false, true},
		{"runtime", "new_graph", false, false, false, true},
		{"runtime", "delete_graph", false, false, false, true},
	};
	for (size_t i = 0; i < sizeof(who) / sizeof(who[0]); i++) {
		r = auth_command_rule(who[i].protocol, who[i].command);
		CHECK(auth_permits(0, &r) == who[i].none);
		CHECK(auth_permits(viewer, &r) == who[i].viewer);
		CHECK(auth_permits(oper, &r) == who[i].op);
		CHECK(auth_permits(editor, &r) == who[i].editor);
	}
	/* protocol:graph alone also reads. */
	r = auth_command_rule("graph", "connect");
	CHECK(auth_permits(AUTH_CAP_PROTOCOL_GRAPH, &r));
}

/* ---- Rate limiting (02 §2.5 item 7) ----------------------------------------------- */

static void test_connection_close(void)
{
	setup();
	AuthConn *c = open_conn("192.0.2.50", NULL);
	CHECK(fail_once(c) == AUTH_FAILED);
	CHECK(fail_once(c) == AUTH_FAILED);
	logs[0] = '\0';
	CHECK(fail_once(c) == AUTH_FAILED_CLOSE);
	CHECK(strstr(logs, "closing the connection") != NULL);
	close_conn(c);
	teardown();
}

/* A challenge from addr on a new connection: AUTH_OK or AUTH_DELAYED. */
static enum auth_result try_challenge(const char *addr, AuthConn **out)
{
	AuthChallenge ch;
	*out = open_conn(addr, NULL);
	return az.ops->challenge(az.self, *out, "alice", cnonce_of(5), &ch);
}

/* n failed logins from addr (new connections, so none is closed early). */
static void fail_n(const char *addr, int n)
{
	for (int i = 0; i < n; i++) {
		AuthConn *c = open_conn(addr, NULL);
		CHECK(fail_once(c) == AUTH_FAILED);
		close_conn(c);
	}
}

static void test_backoff(void)
{
	setup();
	const char *addr = "192.0.2.60";
	AuthConn *c;
	fail_n(addr, 4);
	CHECK(try_challenge(addr, &c) == AUTH_OK); /* no back-off before the 5th */
	close_conn(c);

	/* The 5th failure, then 1 s doubling up to 60 s, counted from the
	 * last failure. */
	const uint64_t schedule[] = {1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000};
	for (int i = 0; i < 8; i++) {
		fail_n(addr, 1);
		uint64_t t_fail = clock_ms;
		clock_ms += schedule[i] / 2;
		challenge_calls = 0;
		CHECK(try_challenge(addr, &c) == AUTH_DELAYED);
		CHECK(az.ops->state(az.self, c) == AUTH_STATE_UNAUTHENTICATED);
		CHECK(auth_tick(auth, t_fail + schedule[i] - 1) == 1);
		CHECK(challenge_calls == 0);
		clock_ms = t_fail + schedule[i];
		CHECK(auth_tick(auth, clock_ms) == AUTH_NONCE_TTL_MS);
		CHECK(challenge_calls == 1 && !challenge_null);
		CHECK(az.ops->state(az.self, c) == AUTH_STATE_CHALLENGED);
		CHECK(strncmp(delivered.nonce, cnonce_of(5), strlen(cnonce_of(5))) == 0);
		/* Once the time has come, a challenge is answered at once. */
		AuthConn *d;
		CHECK(try_challenge(addr, &d) == AUTH_OK);
		close_conn(d);
		close_conn(c);
	}

	/* A new challenge meanwhile replaces the pending one, same due time;
	 * a login meanwhile fails. */
	fail_n(addr, 1);
	uint64_t t_fail = clock_ms;
	CHECK(try_challenge(addr, &c) == AUTH_DELAYED);
	AuthChallenge ch;
	clock_ms += 10000;
	CHECK(az.ops->challenge(az.self, c, "alice", cnonce_of(6), &ch) == AUTH_DELAYED);
	CHECK(auth_tick(auth, clock_ms) == t_fail + 60000 - clock_ms);
	challenge_calls = 0;
	clock_ms = t_fail + 60000;
	auth_tick(auth, clock_ms);
	CHECK(challenge_calls == 1);
	CHECK(strncmp(delivered.nonce, cnonce_of(6), strlen(cnonce_of(6))) == 0);

	/* A success resets the count: then one failure brings no back-off. */
	char proof[64], sig[64];
	client_proof("alice", "pencil", &delivered, proof, sig);
	AuthProof p;
	CHECK(az.ops->verify_proof(az.self, c, "alice", delivered.nonce, proof, &p) == AUTH_OK);
	close_conn(c);
	fail_n(addr, 4);
	CHECK(try_challenge(addr, &c) == AUTH_OK);
	close_conn(c);

	/* 15 minutes of quiet reset it too. */
	fail_n(addr, 1); /* the 5th */
	CHECK(try_challenge(addr, &c) == AUTH_DELAYED);
	close_conn(c);
	clock_ms += AUTH_QUIET_RESET_MS - 1;
	CHECK(try_challenge(addr, &c) == AUTH_OK); /* past the 1 s anyway */
	close_conn(c);
	fail_n(addr, 1);			      /* the 6th */
	CHECK(try_challenge(addr, &c) == AUTH_DELAYED); /* 2 s */
	close_conn(c);
	clock_ms += AUTH_QUIET_RESET_MS;
	CHECK(try_challenge(addr, &c) == AUTH_OK);
	close_conn(c);
	fail_n(addr, 1); /* counted from 1 again */
	CHECK(try_challenge(addr, &c) == AUTH_OK);
	close_conn(c);

	/* Other addresses are not affected. */
	fail_n(addr, 4);
	CHECK(try_challenge(addr, &c) == AUTH_DELAYED);
	close_conn(c);
	CHECK(try_challenge("192.0.2.61", &c) == AUTH_OK);
	close_conn(c);

	/* A delayed challenge whose random source fails: on_challenge(NULL). */
	CHECK(try_challenge(addr, &c) == AUTH_DELAYED);
	rnd_fail = true;
	challenge_calls = 0;
	clock_ms += 2000;
	auth_tick(auth, clock_ms);
	rnd_fail = false;
	CHECK(challenge_calls == 1 && challenge_null);
	CHECK(az.ops->state(az.self, c) == AUTH_STATE_UNAUTHENTICATED);
	close_conn(c);
	teardown();
}

static void test_lru(void)
{
	setup();
	char addr[HAKKAS_AUTH_RATE_ADDRS + 1][32];
	for (int i = 0; i <= HAKKAS_AUTH_RATE_ADDRS; i++)
		snprintf(addr[i], sizeof(addr[i]), "2001:db8::%d", i + 1);
	/* 16 addresses in back-off. */
	for (int i = 0; i < HAKKAS_AUTH_RATE_ADDRS; i++)
		fail_n(addr[i], AUTH_BACKOFF_AFTER);
	AuthConn *c;
	for (int i = 0; i < HAKKAS_AUTH_RATE_ADDRS; i++) {
		CHECK(try_challenge(addr[i], &c) == AUTH_DELAYED);
		close_conn(c);
	}
	/* Use them again, all but addr[3], which becomes the least recently
	 * used; a 17th address evicts it. */
	for (int i = 0; i < HAKKAS_AUTH_RATE_ADDRS; i++) {
		if (i != 3) {
			CHECK(try_challenge(addr[i], &c) == AUTH_DELAYED);
			close_conn(c);
		}
	}
	fail_n(addr[HAKKAS_AUTH_RATE_ADDRS], 1);
	CHECK(try_challenge(addr[3], &c) == AUTH_OK); /* forgotten */
	close_conn(c);
	for (int i = 0; i < HAKKAS_AUTH_RATE_ADDRS; i++) {
		if (i != 3) {
			CHECK(try_challenge(addr[i], &c) == AUTH_DELAYED);
			close_conn(c);
		}
	}
	teardown();
}

/* ---- Logging (02 §2.5 item 8) ------------------------------------------------------ */

/* logs does not contain the text (nor, if base64, its first 12 characters). */
static void absent(const char *what, const char *text)
{
	char head[16];
	snprintf(head, sizeof(head), "%.12s", text);
	if (strstr(logs, text) != NULL || strstr(logs, head) != NULL)
		fprintf(stderr, "the log contains the %s\n", what);
	CHECK(strstr(logs, text) == NULL && strstr(logs, head) == NULL);
}

static void test_logging(void)
{
	setup();
	logs[0] = '\0';
	AuthConn *c = open_conn("192.0.2.70", NULL);
	AuthChallenge ch, bad;
	CHECK(az.ops->challenge(az.self, c, "alice", cnonce_of(8), &ch) == AUTH_OK);
	char proof[64], sig[64], wrong[64], sig2[64];
	client_proof("alice", "pencil", &ch, proof, sig);
	AuthProof p;
	CHECK(az.ops->verify_proof(az.self, c, "alice", ch.nonce, proof, &p) == AUTH_OK);
	char secret[AUTH_SECRET_SIZE];
	CHECK(az.ops->issue_secret(az.self, c, secret));
	az.ops->revoke_secret(az.self, c);
	CHECK(az.ops->challenge(az.self, c, "alice", cnonce_of(9), &bad) == AUTH_OK);
	client_proof("alice", "pencil-wrong", &bad, wrong, sig2);
	CHECK(az.ops->verify_proof(az.self, c, "alice", bad.nonce, wrong, &p) == AUTH_FAILED);
	CHECK(login(c, "alice", "pencil", NULL));
	clock_ms += IDLE_S * 1000u;
	auth_tick(auth, clock_ms);
	close_conn(c);

	/* The facts, with the user name and source address. */
	CHECK(strstr(logs, "info") && strstr(logs, "login: user 'alice' from 192.0.2.70") != NULL);
	CHECK(strstr(logs, "warn") &&
	      strstr(logs, "login failed: user 'alice' from 192.0.2.70") != NULL);
	CHECK(strstr(logs, "logout: user 'alice' from 192.0.2.70") != NULL);
	CHECK(strstr(logs, "expired (idle timeout)") != NULL);
	/* Never a secret, proof, nonce, salt, password or key. */
	absent("password", "pencil");
	absent("secret", secret);
	absent("proof", proof);
	absent("wrong proof", wrong);
	absent("server signature", sig);
	absent("client nonce", cnonce_of(8));
	absent("nonce", ch.nonce + strlen(cnonce_of(8)));
	absent("salt", ch.salt);
	absent("server secret", SERVER_SECRET);
	char key64[64];
	crypto_base64_encode(users_find(&users, "alice")->stored_key, 32, key64, sizeof(key64));
	absent("stored key", key64);
	crypto_base64_encode(users_find(&users, "alice")->server_key, 32, key64, sizeof(key64));
	absent("server key", key64);

	/* A name that is not valid is not logged as such. */
	logs[0] = '\0';
	AuthConn *d = open_conn("192.0.2.71", NULL);
	CHECK(az.ops->verify_proof(az.self, d, "evil\nwarn forged", "n", "p", &p) == AUTH_FAILED);
	CHECK(strstr(logs, "forged") == NULL && strstr(logs, "(invalid name)") != NULL);
	close_conn(d);
	teardown();
}

static void test_create(void)
{
	UserStore s;
	users_init(&s);
	Auth *a = NULL;
	AuthConfig cfg = {
		.users = &s,
		.server_secret = "0123456789abcdef",
		.idle_timeout_s = 1,
		.max_lifetime_s = 1,
		.clock = fake_clock,
		.random_bytes = fake_random,
	};
	CHECK(auth_create(&cfg, &a));
	auth_destroy(a);
	auth_destroy(NULL);
	cfg.server_secret = "0123456789abcde"; /* 15 bytes */
	CHECK(!auth_create(&cfg, &a) && a == NULL);
	cfg.server_secret = NULL;
	CHECK(!auth_create(&cfg, &a));
	cfg.server_secret = "0123456789abcdef";
	cfg.idle_timeout_s = 0;
	CHECK(!auth_create(&cfg, &a));
	cfg.idle_timeout_s = 1;
	cfg.users = NULL;
	CHECK(!auth_create(&cfg, &a));

	/* An empty store refuses every login (02 §2.2). */
	cfg.users = &s;
	CHECK(auth_create(&cfg, &a));
	Authorizer x = auth_authorizer(a);
	AuthConn *c = x.ops->open(x.self, "192.0.2.80", NULL, NULL);
	AuthChallenge ch;
	CHECK(x.ops->challenge(x.self, c, "alice", cnonce_of(1), &ch) == AUTH_OK);
	char proof[64], sig[64];
	client_proof("alice", "pencil", &ch, proof, sig);
	AuthProof p;
	CHECK(x.ops->verify_proof(x.self, c, "alice", ch.nonce, proof, &p) == AUTH_FAILED);
	x.ops->close(x.self, c);
	auth_destroy(a);
}

int main(void)
{
	CHECK(log_add_sink(log_sink, NULL));
	log_set_threshold(LOG_LEVEL_FINE);
	RUN(test_login);
	RUN(test_binding);
	RUN(test_unknown_user);
	RUN(test_login_failures);
	RUN(test_expiry);
	RUN(test_logout_and_close);
	RUN(test_capabilities);
	RUN(test_command_rules);
	RUN(test_connection_close);
	RUN(test_backoff);
	RUN(test_lru);
	RUN(test_logging);
	RUN(test_create);
	log_remove_sink(log_sink, NULL);
	return test_result();
}
