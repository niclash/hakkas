/*
 * Authorization (02 §2). See protocol/auth.h.
 */
#include "protocol/auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system/config.h"
#include "system/log.h"

/* ---- Capabilities and commands (02 §2.4) ------------------------------------------ */

static const char *const cap_names[AUTH_CAP_COUNT] = {
	"protocol:runtime", "protocol:graph", "protocol:component", "graph:readonly",
	"network:status",   "network:persist", "network:data",	     "network:control",
};

const char *auth_capability_name(unsigned i)
{
	return i < AUTH_CAP_COUNT ? cap_names[i] : NULL;
}

#define CAPS_VIEWER                                                                                \
	(AUTH_CAP_PROTOCOL_COMPONENT | AUTH_CAP_NETWORK_STATUS | AUTH_CAP_NETWORK_DATA |           \
	 AUTH_CAP_GRAPH_READONLY)
#define CAPS_OPERATOR (CAPS_VIEWER | AUTH_CAP_NETWORK_CONTROL)
#define CAPS_EDITOR                                                                                \
	(CAPS_OPERATOR | AUTH_CAP_PROTOCOL_GRAPH | AUTH_CAP_PROTOCOL_RUNTIME |                     \
	 AUTH_CAP_NETWORK_PERSIST)

uint32_t auth_role_capabilities(enum user_role role)
{
	switch (role) {
	case USER_ROLE_VIEWER:
		return CAPS_VIEWER;
	case USER_ROLE_OPERATOR:
		return CAPS_OPERATOR;
	case USER_ROLE_EDITOR:
	case USER_ROLE_ADMIN: /* reserved for remote user management (02 §2.4) */
		return CAPS_EDITOR;
	}
	return 0;
}

#define READ_CAPS (AUTH_CAP_GRAPH_READONLY | AUTH_CAP_PROTOCOL_GRAPH)

typedef struct {
	const char *command;
	bool gated;
	uint32_t caps_any;
} CommandDef;

static const CommandDef environment_commands[] = {
	{"challenge", false, 0},
	{"login", false, 0},
	{"logout", false, 0},
	{NULL, false, 0},
};

static const CommandDef runtime_commands[] = {
	{"getruntime", false, 0},
	{"new_graph", true, AUTH_CAP_PROTOCOL_RUNTIME},
	{"delete_graph", true, AUTH_CAP_PROTOCOL_RUNTIME},
	{NULL, false, 0},
};

static const CommandDef network_commands[] = {
	{"start", true, AUTH_CAP_NETWORK_CONTROL},   {"stop", true, AUTH_CAP_NETWORK_CONTROL},
	{"getstatus", true, AUTH_CAP_NETWORK_STATUS}, {"persist", true, AUTH_CAP_NETWORK_PERSIST},
	{"edges", true, AUTH_CAP_NETWORK_DATA},	     {NULL, false, 0},
};

/* The read-only graph commands first, then the graph-mutating ones
 * (02 §3.4, §3.4.2). */
static const CommandDef graph_commands[] = {
	{"connect", true, READ_CAPS},
	{"list", true, READ_CAPS},
	{"rename", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"addnode", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"removenode", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"renamenode", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"changenode", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"addedge", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"removeedge", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"changeedge", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"addinitial", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"removeinitial", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"addinport", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"removeinport", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"renameinport", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"addoutport", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"removeoutport", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"renameoutport", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"addgroup", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"removegroup", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"renamegroup", true, AUTH_CAP_PROTOCOL_GRAPH},
	{"changegroup", true, AUTH_CAP_PROTOCOL_GRAPH},
	{NULL, false, 0},
};

static const CommandDef component_commands[] = {
	{"list", true, READ_CAPS},
	{"getsource", true, READ_CAPS},
	{NULL, false, 0},
};

static const CommandDef trace_commands[] = {
	{"start", true, READ_CAPS}, {"stop", true, READ_CAPS}, {"clear", true, READ_CAPS},
	{"dump", true, READ_CAPS},  {NULL, false, 0},
};

AuthCommandRule auth_command_rule(const char *protocol, const char *command)
{
	static const struct {
		const char *protocol;
		const CommandDef *commands;
	} protocols[] = {
		{"environment", environment_commands}, {"runtime", runtime_commands},
		{"network", network_commands},	       {"graph", graph_commands},
		{"component", component_commands},     {"trace", trace_commands},
	};
	AuthCommandRule rule = {false, true, 0};
	if (protocol == NULL || command == NULL)
		return rule;
	for (size_t p = 0; p < sizeof(protocols) / sizeof(protocols[0]); p++) {
		if (strcmp(protocol, protocols[p].protocol) != 0)
			continue;
		/* environment is never secret-gated (02 §3.1). */
		rule.gated = protocols[p].commands != environment_commands;
		for (const CommandDef *d = protocols[p].commands; d->command != NULL; d++) {
			if (strcmp(command, d->command) == 0) {
				rule = (AuthCommandRule){true, d->gated, d->caps_any};
				break;
			}
		}
		break;
	}
	return rule;
}

/* ---- State ------------------------------------------------------------------------ */

typedef struct {
	char addr[PAL_ADDR_MAX];
	uint32_t failures; /* consecutive; 0: the entry is free */
	uint64_t last_failure;
	uint64_t last_used; /* a use sequence number, for LRU eviction */
} RateEntry;

struct Auth {
	UserStore *users; /* borrowed; re-read into at every challenge */
	auth_users_fn load_users;
	void *load_users_ctx;
	char server_secret[CONFIG_SECRET_MAX + 1];
	size_t server_secret_len;
	uint64_t idle_ms;
	uint64_t max_ms;
	auth_clock_fn clock;
	void *clock_ctx;
	pal_random_fn random_bytes;
	AuthConn *conns; /* every open connection */
	RateEntry rate[HAKKAS_AUTH_RATE_ADDRS];
	uint64_t use_seq;
};

struct AuthConn {
	Auth *auth;
	AuthConn *prev, *next;
	char peer[PAL_ADDR_MAX];
	const AuthEvents *events;
	void *user;
	enum auth_state state;
	uint32_t failures; /* failed logins on this connection */

	/* CHALLENGED: the challenge sent. */
	char ch_user[USERS_NAME_MAX + 1];
	char nonce[AUTH_NONCE_TEXT_SIZE];
	char salt[AUTH_SALT_TEXT_SIZE];
	uint32_t iterations;
	uint64_t nonce_expires;

	/* A delayed challenge (back-off): answered at pending_at. */
	bool pending;
	uint64_t pending_at;
	char pending_user[USERS_NAME_MAX + 1];
	char pending_cnonce[AUTH_CNONCE_TEXT_MAX + 1];

	/* VERIFIED and AUTHENTICATED: the session. */
	char name[USERS_NAME_MAX + 1];
	enum user_role role;
	uint32_t caps;
	char secret[AUTH_SECRET_SIZE];
	uint64_t created;
	uint64_t last_activity;
};

static uint64_t now_ms(const Auth *a)
{
	return a->clock(a->clock_ctx);
}

/* A user name as logged: names from the network are logged only if valid
 * (no control characters, protocol/users.h). */
static const char *shown(const char *name)
{
	return users_name_valid(name) ? name : "(invalid name)";
}

static void clear_challenge(AuthConn *c)
{
	crypto_zeroize(c->ch_user, sizeof(c->ch_user));
	crypto_zeroize(c->nonce, sizeof(c->nonce));
	crypto_zeroize(c->salt, sizeof(c->salt));
	c->iterations = 0;
	c->nonce_expires = 0;
}

static void clear_pending(AuthConn *c)
{
	c->pending = false;
	c->pending_at = 0;
	crypto_zeroize(c->pending_user, sizeof(c->pending_user));
	crypto_zeroize(c->pending_cnonce, sizeof(c->pending_cnonce));
}

static void clear_session(AuthConn *c)
{
	crypto_zeroize(c->name, sizeof(c->name));
	crypto_zeroize(c->secret, sizeof(c->secret));
	c->caps = 0;
	c->created = c->last_activity = 0;
}

/* Back to UNAUTHENTICATED, forgetting everything but the failure count. */
static void reset(AuthConn *c)
{
	clear_challenge(c);
	clear_pending(c);
	clear_session(c);
	c->state = AUTH_STATE_UNAUTHENTICATED;
}

/* When an AUTHENTICATED connection's session ends (02 §2.5 item 3). */
static uint64_t session_end(const AuthConn *c)
{
	const Auth *a = c->auth;
	uint64_t idle = c->last_activity + a->idle_ms;
	uint64_t life = c->created + a->max_ms;
	return idle < life ? idle : life;
}

static bool session_live(const AuthConn *c, uint64_t now)
{
	return c->state == AUTH_STATE_AUTHENTICATED && now < session_end(c);
}

/* ---- Rate limiting (02 §2.5 item 7) ------------------------------------------------- */

/* addr's entry, or NULL. An entry quiet for AUTH_QUIET_RESET_MS is reset. */
static RateEntry *rate_find(Auth *a, const char *addr, uint64_t now)
{
	for (size_t i = 0; i < HAKKAS_AUTH_RATE_ADDRS; i++) {
		RateEntry *e = &a->rate[i];
		if (e->failures == 0 || strcmp(e->addr, addr) != 0)
			continue;
		if (now - e->last_failure >= AUTH_QUIET_RESET_MS) {
			memset(e, 0, sizeof(*e));
			return NULL;
		}
		e->last_used = ++a->use_seq;
		return e;
	}
	return NULL;
}

/* The back-off after failures consecutive failures: 0 before the 5th, then
 * 1 s doubling up to 60 s. */
static uint64_t backoff_ms(uint32_t failures)
{
	if (failures < AUTH_BACKOFF_AFTER)
		return 0;
	uint32_t shift = failures - AUTH_BACKOFF_AFTER;
	if (shift >= 6) /* 1 s << 6 = 64 s */
		return AUTH_BACKOFF_MAX_MS;
	uint64_t d = (uint64_t)AUTH_BACKOFF_BASE_MS << shift;
	return d < AUTH_BACKOFF_MAX_MS ? d : AUTH_BACKOFF_MAX_MS;
}

/* The earliest time a challenge from addr may be answered (<= now: at once). */
static uint64_t rate_ready_at(Auth *a, const char *addr, uint64_t now)
{
	const RateEntry *e = rate_find(a, addr, now);
	return e == NULL ? 0 : e->last_failure + backoff_ms(e->failures);
}

static void rate_fail(Auth *a, const char *addr, uint64_t now)
{
	RateEntry *e = rate_find(a, addr, now);
	if (e == NULL) {
		/* A free entry, else the least recently used. */
		e = &a->rate[0];
		for (size_t i = 0; i < HAKKAS_AUTH_RATE_ADDRS; i++) {
			RateEntry *x = &a->rate[i];
			if (x->failures == 0) {
				e = x;
				break;
			}
			if (x->last_used < e->last_used)
				e = x;
		}
		memset(e, 0, sizeof(*e));
		snprintf(e->addr, sizeof(e->addr), "%s", addr);
	}
	if (e->failures < UINT32_MAX)
		e->failures++;
	e->last_failure = now;
	e->last_used = ++a->use_seq;
	uint64_t d = backoff_ms(e->failures);
	if (d > 0)
		log_info("%s: %u consecutive failed logins: challenges delayed %llu ms", e->addr,
			 (unsigned)e->failures, (unsigned long long)d);
}

static void rate_success(Auth *a, const char *addr, uint64_t now)
{
	RateEntry *e = rate_find(a, addr, now);
	if (e != NULL)
		memset(e, 0, sizeof(*e));
}

/* ---- Challenge (02 §2.3 step 1) ------------------------------------------------------ */

/* Whether cnonce is the strict base64 of AUTH_CNONCE_MIN-AUTH_CNONCE_MAX
 * bytes. */
static bool cnonce_valid(const char *cnonce)
{
	if (cnonce == NULL)
		return false;
	size_t len = strlen(cnonce);
	uint8_t buf[CRYPTO_BASE64_DECODED_MAX(AUTH_CNONCE_TEXT_MAX)];
	size_t n = 0;
	return len >= CRYPTO_BASE64_LEN(AUTH_CNONCE_MIN) && len <= AUTH_CNONCE_TEXT_MAX &&
	       crypto_base64_decode(cnonce, len, buf, sizeof(buf), &n) && n >= AUTH_CNONCE_MIN &&
	       n <= AUTH_CNONCE_MAX;
}

/* Re-reads the users file, if there is a loader (02 §2.2; decided, plan
 * task 18): a missing, empty or malformed file is no users (as at startup);
 * an unreadable one keeps the users of the previous read. */
static void reload_users(Auth *a)
{
	if (a->load_users == NULL)
		return;
	UserStore *fresh = malloc(sizeof(*fresh));
	if (fresh == NULL) {
		log_warn("authorization: out of memory re-reading the users file: keeping the "
			 "previous users");
		return;
	}
	enum users_status st = a->load_users(a->load_users_ctx, fresh);
	if (st == USERS_OK || st == USERS_ERR_NOT_FOUND || st == USERS_ERR_MALFORMED) {
		if (st == USERS_ERR_MALFORMED)
			log_warn("authorization: the users file is malformed: every login is "
				 "refused until it is repaired");
		memcpy(a->users, fresh, sizeof(*a->users));
	} else {
		log_warn("authorization: the users file could not be read (%s): keeping the "
			 "previous users",
			 users_status_str(st));
	}
	crypto_zeroize(fresh, sizeof(*fresh));
	free(fresh);
}

/* Make the challenge for user (valid) and cnonce (valid) on c at now: c is
 * CHALLENGED and *out its reply. False (logged; c unchanged) if the random
 * source or a hash fails. */
static bool make_challenge(Auth *a, AuthConn *c, const char *user, const char *cnonce,
			   uint64_t now, AuthChallenge *out)
{
	uint8_t rnd[AUTH_NONCE_RANDOM_BYTES];
	char rnd64[CRYPTO_BASE64_LEN(AUTH_NONCE_RANDOM_BYTES) + 1];
	if (a->random_bytes(rnd, sizeof(rnd)) != PAL_OK) {
		log_error("challenge: the random source failed");
		return false;
	}
	crypto_base64_encode(rnd, sizeof(rnd), rnd64, sizeof(rnd64));
	crypto_zeroize(rnd, sizeof(rnd));

	reload_users(a);
	AuthChallenge ch;
	const UserRecord *r = users_find(a->users, user);
	if (r != NULL) {
		crypto_base64_encode(r->salt, USERS_SALT_SIZE, ch.salt, sizeof(ch.salt));
		ch.iterations = r->iterations;
	} else {
		/* An unknown user: a deterministic salt only the server can
		 * compute, and the default count (02 §2.3). */
		uint8_t h[CRYPTO_SHA256_SIZE];
		if (!crypto_hmac_sha256(a->server_secret, a->server_secret_len, user, strlen(user),
					h)) {
			log_error("challenge: HMAC failed");
			return false;
		}
		crypto_base64_encode(h, USERS_SALT_SIZE, ch.salt, sizeof(ch.salt));
		crypto_zeroize(h, sizeof(h));
		ch.iterations = USERS_DEFAULT_ITERATIONS;
	}
	snprintf(ch.nonce, sizeof(ch.nonce), "%s%s", cnonce, rnd64);

	clear_challenge(c);
	snprintf(c->ch_user, sizeof(c->ch_user), "%s", user);
	memcpy(c->nonce, ch.nonce, sizeof(c->nonce));
	memcpy(c->salt, ch.salt, sizeof(c->salt));
	c->iterations = ch.iterations;
	c->nonce_expires = now + AUTH_NONCE_TTL_MS;
	c->state = AUTH_STATE_CHALLENGED;
	*out = ch;
	return true;
}

static enum auth_result op_challenge(void *self, AuthConn *c, const char *user,
				     const char *cnonce, AuthChallenge *out)
{
	Auth *a = self;
	if (c->state == AUTH_STATE_AUTHENTICATED)
		return AUTH_FAILED; /* log out first */
	clear_challenge(c);
	clear_pending(c);
	clear_session(c);
	c->state = AUTH_STATE_UNAUTHENTICATED;
	if (!users_name_valid(user) || !cnonce_valid(cnonce))
		return AUTH_FAILED;

	uint64_t now = now_ms(a);
	uint64_t ready = rate_ready_at(a, c->peer, now);
	if (ready > now) {
		c->pending = true;
		c->pending_at = ready;
		snprintf(c->pending_user, sizeof(c->pending_user), "%s", user);
		snprintf(c->pending_cnonce, sizeof(c->pending_cnonce), "%s", cnonce);
		return AUTH_DELAYED;
	}
	return make_challenge(a, c, user, cnonce, now, out) ? AUTH_OK : AUTH_FAILED;
}

/* ---- Login (02 §2.3 step 2) ------------------------------------------------------- */

static enum auth_result login_failed(Auth *a, AuthConn *c, const char *user, uint64_t now)
{
	if (c->failures < UINT32_MAX)
		c->failures++;
	log_warn("login failed: user '%s' from %s", shown(user), c->peer);
	rate_fail(a, c->peer, now);
	if (c->failures >= AUTH_CONN_FAILURES_MAX) {
		log_info("%s: %u failed logins: closing the connection", c->peer,
			 (unsigned)c->failures);
		return AUTH_FAILED_CLOSE;
	}
	return AUTH_FAILED;
}

static enum auth_result op_verify_proof(void *self, AuthConn *c, const char *user,
					const char *nonce, const char *proof, AuthProof *out)
{
	Auth *a = self;
	uint64_t now = now_ms(a);

	/* The challenge is used up by this attempt, whatever its outcome. */
	bool challenged = c->state == AUTH_STATE_CHALLENGED;
	bool ok = challenged && now < c->nonce_expires;
	char ch_user[USERS_NAME_MAX + 1], ch_nonce[AUTH_NONCE_TEXT_SIZE];
	char salt[AUTH_SALT_TEXT_SIZE];
	uint32_t iterations = c->iterations;
	memcpy(ch_user, c->ch_user, sizeof(ch_user));
	memcpy(ch_nonce, c->nonce, sizeof(ch_nonce));
	memcpy(salt, c->salt, sizeof(salt));
	clear_challenge(c);
	if (c->state == AUTH_STATE_CHALLENGED || c->state == AUTH_STATE_VERIFIED)
		c->state = AUTH_STATE_UNAUTHENTICATED;
	if (!challenged)
		return login_failed(a, c, user, now);

	ok = ok && user != NULL && strcmp(user, ch_user) == 0;
	ok = ok && nonce != NULL && strlen(nonce) == strlen(ch_nonce) &&
	     crypto_equal(nonce, ch_nonce, strlen(ch_nonce));
	uint8_t received[CRYPTO_BASE64_DECODED_MAX(CRYPTO_BASE64_LEN(CRYPTO_SHA256_SIZE))] = {0};
	size_t n = 0;
	ok = ok && proof != NULL && strlen(proof) == CRYPTO_BASE64_LEN(CRYPTO_SHA256_SIZE) &&
	     crypto_base64_decode(proof, strlen(proof), received, sizeof(received), &n) &&
	     n == CRYPTO_SHA256_SIZE;

	/* AuthMessage (02 §2.3, not RFC 5802's). */
	char msg[USERS_NAME_MAX + AUTH_NONCE_TEXT_SIZE + AUTH_SALT_TEXT_SIZE + 16];
	int len = snprintf(msg, sizeof(msg), "%s,%s,%s,%lu", ch_user, ch_nonce, salt,
			   (unsigned long)iterations);
	ok = ok && len > 0 && (size_t)len < sizeof(msg);

	/* The same work for an unknown user, against a key nobody has. */
	const UserRecord *r = users_find(a->users, ch_user);
	uint8_t fake[CRYPTO_SHA256_SIZE], sig[CRYPTO_SHA256_SIZE];
	uint8_t client_key[CRYPTO_SHA256_SIZE], check[CRYPTO_SHA256_SIZE];
	char fake_in[USERS_NAME_MAX + 8];
	snprintf(fake_in, sizeof(fake_in), "key,%s", ch_user);
	bool hashed = crypto_hmac_sha256(a->server_secret, a->server_secret_len, fake_in,
					 strlen(fake_in), fake);
	const uint8_t *stored = r != NULL ? r->stored_key : fake;
	hashed = hashed && len > 0 &&
		 crypto_hmac_sha256(stored, CRYPTO_SHA256_SIZE, msg, (size_t)len, sig);
	for (size_t i = 0; i < CRYPTO_SHA256_SIZE; i++)
		client_key[i] = received[i] ^ sig[i];
	hashed = hashed && crypto_sha256(client_key, sizeof(client_key), check);
	bool match = hashed && crypto_equal(check, stored, CRYPTO_SHA256_SIZE);
	ok = ok && r != NULL && match;

	AuthProof result;
	if (ok) {
		uint8_t server_sig[CRYPTO_SHA256_SIZE];
		ok = crypto_hmac_sha256(r->server_key, USERS_KEY_SIZE, msg, (size_t)len,
					server_sig) &&
		     crypto_base64_encode(server_sig, sizeof(server_sig), result.server_signature,
					  sizeof(result.server_signature)) > 0;
		crypto_zeroize(server_sig, sizeof(server_sig));
	}
	crypto_zeroize(received, sizeof(received));
	crypto_zeroize(fake, sizeof(fake));
	crypto_zeroize(sig, sizeof(sig));
	crypto_zeroize(client_key, sizeof(client_key));
	crypto_zeroize(check, sizeof(check));
	crypto_zeroize(msg, sizeof(msg));
	crypto_zeroize(ch_nonce, sizeof(ch_nonce));
	crypto_zeroize(salt, sizeof(salt));
	if (!ok)
		return login_failed(a, c, user, now);

	result.role = r->role;
	result.capabilities = auth_role_capabilities(r->role);
	clear_session(c);
	snprintf(c->name, sizeof(c->name), "%s", r->name);
	c->role = r->role;
	c->state = AUTH_STATE_VERIFIED;
	rate_success(a, c->peer, now);
	log_info("login: user '%s' from %s as %s", r->name, c->peer, users_role_name(r->role));
	*out = result;
	return AUTH_OK;
}

/* ---- Sessions (02 §2.5) ------------------------------------------------------------ */

static bool op_issue_secret(void *self, AuthConn *c, char out[AUTH_SECRET_SIZE])
{
	Auth *a = self;
	if (c->state != AUTH_STATE_VERIFIED)
		return false;
	uint8_t rnd[AUTH_SECRET_RANDOM_BYTES];
	if (a->random_bytes(rnd, sizeof(rnd)) != PAL_OK) {
		log_error("login: the random source failed");
		reset(c);
		return false;
	}
	crypto_base64url_encode(rnd, sizeof(rnd), c->secret, sizeof(c->secret));
	crypto_zeroize(rnd, sizeof(rnd));
	memcpy(out, c->secret, AUTH_SECRET_SIZE);
	c->caps = auth_role_capabilities(c->role);
	c->created = c->last_activity = now_ms(a);
	c->state = AUTH_STATE_AUTHENTICATED;
	return true;
}

static bool op_validate_secret(void *self, AuthConn *c, const char *secret)
{
	Auth *a = self;
	return session_live(c, now_ms(a)) && secret != NULL && strlen(secret) == AUTH_SECRET_LEN &&
	       crypto_equal(secret, c->secret, AUTH_SECRET_LEN);
}

static void op_revoke_secret(void *self, AuthConn *c)
{
	(void)self;
	if (c->state == AUTH_STATE_AUTHENTICATED)
		log_info("logout: user '%s' from %s", c->name, c->peer);
	reset(c);
}

static void op_activity(void *self, AuthConn *c)
{
	Auth *a = self;
	uint64_t now = now_ms(a);
	if (session_live(c, now))
		c->last_activity = now;
}

static uint32_t op_capabilities_for(void *self, const AuthConn *c)
{
	Auth *a = self;
	return session_live(c, now_ms(a)) ? c->caps : 0;
}

static enum auth_state op_state(void *self, const AuthConn *c)
{
	(void)self;
	return c->state;
}

/* ---- Connections ------------------------------------------------------------------ */

static AuthConn *op_open(void *self, const char *peer_addr, const AuthEvents *events, void *user)
{
	Auth *a = self;
	AuthConn *c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
	c->auth = a;
	snprintf(c->peer, sizeof(c->peer), "%s", peer_addr != NULL ? peer_addr : "");
	c->events = events;
	c->user = user;
	c->next = a->conns;
	if (a->conns != NULL)
		a->conns->prev = c;
	a->conns = c;
	return c;
}

static void op_close(void *self, AuthConn *c)
{
	Auth *a = self;
	if (c == NULL)
		return;
	if (c->state == AUTH_STATE_AUTHENTICATED)
		log_info("session of user '%s' from %s ended: connection closed", c->name,
			 c->peer);
	if (c->prev != NULL)
		c->prev->next = c->next;
	else
		a->conns = c->next;
	if (c->next != NULL)
		c->next->prev = c->prev;
	crypto_zeroize(c, sizeof(*c));
	free(c);
}

static const AuthOps auth_ops = {
	.open = op_open,
	.close = op_close,
	.activity = op_activity,
	.challenge = op_challenge,
	.verify_proof = op_verify_proof,
	.issue_secret = op_issue_secret,
	.validate_secret = op_validate_secret,
	.revoke_secret = op_revoke_secret,
	.capabilities_for = op_capabilities_for,
	.state = op_state,
};

Authorizer auth_authorizer(Auth *a)
{
	return (Authorizer){&auth_ops, a};
}

/* ---- The tick hook ---------------------------------------------------------------- */

static uint64_t min_u64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

uint64_t auth_tick(void *ctx, uint64_t now)
{
	Auth *a = ctx;
	uint64_t next = UINT64_MAX;
	AuthConn *n;
	for (AuthConn *c = a->conns; c != NULL; c = n) {
		n = c->next;
		if (c->pending && now >= c->pending_at) {
			char user[USERS_NAME_MAX + 1], cnonce[AUTH_CNONCE_TEXT_MAX + 1];
			memcpy(user, c->pending_user, sizeof(user));
			memcpy(cnonce, c->pending_cnonce, sizeof(cnonce));
			clear_pending(c);
			AuthChallenge ch;
			bool made = make_challenge(a, c, user, cnonce, now, &ch);
			if (c->events != NULL && c->events->on_challenge != NULL)
				c->events->on_challenge(c->user, made ? &ch : NULL);
		}
		if (c->state == AUTH_STATE_AUTHENTICATED && now >= session_end(c)) {
			bool idle = now >= c->last_activity + a->idle_ms;
			log_info("session of user '%s' from %s expired (%s)", c->name, c->peer,
				 idle ? "idle timeout" : "maximum lifetime");
			reset(c);
			if (c->events != NULL && c->events->on_expired != NULL)
				c->events->on_expired(c->user);
		}
		if (c->state == AUTH_STATE_CHALLENGED && now >= c->nonce_expires) {
			clear_challenge(c);
			c->state = AUTH_STATE_UNAUTHENTICATED;
		}

		if (c->pending)
			next = min_u64(next, c->pending_at - now);
		if (c->state == AUTH_STATE_AUTHENTICATED)
			next = min_u64(next, session_end(c) - now);
		if (c->state == AUTH_STATE_CHALLENGED)
			next = min_u64(next, c->nonce_expires - now);
	}
	return next;
}

/* ---- Creation --------------------------------------------------------------------- */

bool auth_create(const AuthConfig *cfg, Auth **out)
{
	*out = NULL;
	size_t secret_len = cfg->server_secret != NULL ? strlen(cfg->server_secret) : 0;
	if (cfg->users == NULL || cfg->clock == NULL || cfg->random_bytes == NULL ||
	    secret_len < CONFIG_SECRET_MIN || secret_len > CONFIG_SECRET_MAX ||
	    cfg->idle_timeout_s == 0 || cfg->max_lifetime_s == 0) {
		log_error("authorization: invalid configuration");
		return false;
	}
	Auth *a = calloc(1, sizeof(*a));
	if (a == NULL) {
		log_error("authorization: out of memory");
		return false;
	}
	a->users = cfg->users;
	a->load_users = cfg->load_users;
	a->load_users_ctx = cfg->load_users_ctx;
	memcpy(a->server_secret, cfg->server_secret, secret_len);
	a->server_secret_len = secret_len;
	a->idle_ms = (uint64_t)cfg->idle_timeout_s * 1000u;
	a->max_ms = (uint64_t)cfg->max_lifetime_s * 1000u;
	a->clock = cfg->clock;
	a->clock_ctx = cfg->clock_ctx;
	a->random_bytes = cfg->random_bytes;
	*out = a;
	return true;
}

void auth_destroy(Auth *a)
{
	if (a == NULL)
		return;
	while (a->conns != NULL)
		op_close(a, a->conns);
	crypto_zeroize(a, sizeof(*a));
	free(a);
}
