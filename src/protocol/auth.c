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

static const CommandDef auth_commands[] = {
	{"challenge", false, 0},
	{"login", false, 0},
	{"logout", false, 0},
	{NULL, false, 0},
};

/* The hardware environment's messages (02 §3.7) are not specified yet. */
static const CommandDef environment_commands[] = {
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
	{"clear", true, AUTH_CAP_PROTOCOL_GRAPH}, /* a component graph, 02 §3.4.3 */
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
		{"auth", auth_commands},	       {"environment", environment_commands},
		{"runtime", runtime_commands},	       {"network", network_commands},
		{"graph", graph_commands},	       {"component", component_commands},
		{"trace", trace_commands},
	};
	AuthCommandRule rule = {false, true, 0};
	if (protocol == NULL || command == NULL)
		return rule;
	for (size_t p = 0; p < sizeof(protocols) / sizeof(protocols[0]); p++) {
		if (strcmp(protocol, protocols[p].protocol) != 0)
			continue;
		/* auth is never secret-gated (02 §3.1). */
		rule.gated = protocols[p].commands != auth_commands;
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

/* A chain's resume entry (02 §2.3.1). */
typedef struct {
	char id[AUTH_RESUME_SIZE]; /* "": the entry is free */
	char user[USERS_NAME_MAX + 1];
	uint8_t key[CRYPTO_SHA256_SIZE];	/* ResumeKey */
	uint8_t stored_key[USERS_KEY_SIZE]; /* the user's, at the password login */
	uint64_t chain;
	uint64_t created;	/* the password login */
	uint64_t last_activity; /* of any session of the chain */
	uint64_t last_used;	/* a use sequence number, for LRU eviction */
} ResumeEntry;

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
	ResumeEntry resume[HAKKAS_AUTH_RESUME_MAX];
	uint64_t chain_seq;
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
	char ch_resume[AUTH_RESUME_SIZE]; /* "": a password login */
	uint64_t nonce_expires;

	/* A delayed challenge (back-off): answered at pending_at. */
	bool pending;
	uint64_t pending_at;
	char pending_user[USERS_NAME_MAX + 1];
	char pending_cnonce[AUTH_CNONCE_TEXT_MAX + 1];
	char pending_resume[AUTH_RESUME_SIZE];

	/* VERIFIED and AUTHENTICATED: the session. */
	char name[USERS_NAME_MAX + 1];
	enum user_role role;
	uint32_t caps;
	char secret[AUTH_SECRET_SIZE];
	uint64_t created; /* the chain's password login */
	uint64_t last_activity;
	/* Its chain (02 §2.3.1); VERIFIED: the entry issue_secret makes. */
	uint64_t chain;
	uint8_t resume_key[CRYPTO_SHA256_SIZE];
	uint8_t stored_key[USERS_KEY_SIZE];
	bool resumed;
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
	crypto_zeroize(c->ch_resume, sizeof(c->ch_resume));
	c->iterations = 0;
	c->nonce_expires = 0;
}

static void clear_pending(AuthConn *c)
{
	c->pending = false;
	c->pending_at = 0;
	crypto_zeroize(c->pending_user, sizeof(c->pending_user));
	crypto_zeroize(c->pending_cnonce, sizeof(c->pending_cnonce));
	crypto_zeroize(c->pending_resume, sizeof(c->pending_resume));
}

static void clear_session(AuthConn *c)
{
	crypto_zeroize(c->name, sizeof(c->name));
	crypto_zeroize(c->secret, sizeof(c->secret));
	crypto_zeroize(c->resume_key, sizeof(c->resume_key));
	crypto_zeroize(c->stored_key, sizeof(c->stored_key));
	c->caps = 0;
	c->created = c->last_activity = 0;
	c->chain = 0;
	c->resumed = false;
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

/* ---- Resume entries (02 §2.3.1) ---------------------------------------------------- */

static void resume_free(ResumeEntry *e)
{
	crypto_zeroize(e, sizeof(*e));
}

static uint64_t resume_end(const Auth *a, const ResumeEntry *e)
{
	uint64_t idle = e->last_activity + a->idle_ms;
	uint64_t life = e->created + a->max_ms;
	return idle < life ? idle : life;
}

/* Frees the entries whose chain has expired. */
static void resume_expire(Auth *a, uint64_t now)
{
	for (size_t i = 0; i < HAKKAS_AUTH_RESUME_MAX; i++) {
		ResumeEntry *e = &a->resume[i];
		if (e->id[0] != '\0' && now >= resume_end(a, e))
			resume_free(e);
	}
}

/* The live entry with this id, or NULL. */
static ResumeEntry *resume_find(Auth *a, const char *id, uint64_t now)
{
	resume_expire(a, now);
	for (size_t i = 0; i < HAKKAS_AUTH_RESUME_MAX; i++) {
		ResumeEntry *e = &a->resume[i];
		if (e->id[0] != '\0' && strlen(id) == AUTH_RESUME_LEN &&
		    crypto_equal(e->id, id, AUTH_RESUME_LEN))
			return e;
	}
	return NULL;
}

/* The chain's entry, or NULL. */
static ResumeEntry *resume_of_chain(Auth *a, uint64_t chain)
{
	for (size_t i = 0; chain != 0 && i < HAKKAS_AUTH_RESUME_MAX; i++)
		if (a->resume[i].id[0] != '\0' && a->resume[i].chain == chain)
			return &a->resume[i];
	return NULL;
}

/* A free entry, else the least recently used one (freed). */
static ResumeEntry *resume_slot(Auth *a)
{
	ResumeEntry *e = &a->resume[0];
	for (size_t i = 0; i < HAKKAS_AUTH_RESUME_MAX; i++) {
		ResumeEntry *x = &a->resume[i];
		if (x->id[0] == '\0')
			return x;
		if (x->last_used < e->last_used)
			e = x;
	}
	resume_free(e);
	return e;
}

/* Ends the entries of users no longer in the users file or with another
 * stored_key (a password change). */
static void resume_check_users(Auth *a)
{
	for (size_t i = 0; i < HAKKAS_AUTH_RESUME_MAX; i++) {
		ResumeEntry *e = &a->resume[i];
		if (e->id[0] == '\0')
			continue;
		const UserRecord *r = users_find(a->users, e->user);
		if (r == NULL || !crypto_equal(r->stored_key, e->stored_key, USERS_KEY_SIZE))
			resume_free(e);
	}
}

/* Whether id is the base64url of AUTH_RESUME_RANDOM_BYTES bytes. */
static bool resume_id_valid(const char *id)
{
	uint8_t buf[AUTH_RESUME_RANDOM_BYTES + 2];
	size_t n = 0;
	return id != NULL && strlen(id) == AUTH_RESUME_LEN &&
	       crypto_base64url_decode(id, AUTH_RESUME_LEN, buf, sizeof(buf), &n) &&
	       n == AUTH_RESUME_RANDOM_BYTES;
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

/* Make the challenge for user (valid), cnonce (valid) and resume (valid,
 * or "" for a password login) on c at now: c is CHALLENGED and *out its
 * reply. False (logged; c unchanged) if the random source or a hash
 * fails. */
static bool make_challenge(Auth *a, AuthConn *c, const char *user, const char *cnonce,
			   const char *resume, uint64_t now, AuthChallenge *out)
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
	resume_check_users(a);
	AuthChallenge ch = {0};
	const UserRecord *r = users_find(a->users, user);
	if (resume[0] != '\0') {
		/* No salt, no iterations: no key stretching (02 §2.3.1); the
		 * same reply for an unknown id. */
		ch.resume = true;
	} else if (r != NULL) {
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
	snprintf(c->ch_resume, sizeof(c->ch_resume), "%s", resume);
	c->iterations = ch.iterations;
	c->nonce_expires = now + AUTH_NONCE_TTL_MS;
	c->state = AUTH_STATE_CHALLENGED;
	*out = ch;
	return true;
}

static enum auth_result op_challenge(void *self, AuthConn *c, const char *user,
				     const char *cnonce, const char *resume, AuthChallenge *out)
{
	Auth *a = self;
	if (c->state == AUTH_STATE_AUTHENTICATED)
		return AUTH_FAILED; /* log out first */
	clear_challenge(c);
	clear_pending(c);
	clear_session(c);
	c->state = AUTH_STATE_UNAUTHENTICATED;
	if (!users_name_valid(user) || !cnonce_valid(cnonce) ||
	    (resume != NULL && !resume_id_valid(resume)))
		return AUTH_FAILED;
	if (resume == NULL)
		resume = "";

	uint64_t now = now_ms(a);
	uint64_t ready = rate_ready_at(a, c->peer, now);
	if (ready > now) {
		c->pending = true;
		c->pending_at = ready;
		snprintf(c->pending_user, sizeof(c->pending_user), "%s", user);
		snprintf(c->pending_cnonce, sizeof(c->pending_cnonce), "%s", cnonce);
		snprintf(c->pending_resume, sizeof(c->pending_resume), "%s", resume);
		return AUTH_DELAYED;
	}
	return make_challenge(a, c, user, cnonce, resume, now, out) ? AUTH_OK : AUTH_FAILED;
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

/* The challenge a login answers, copied out of the connection (the login
 * uses it up). */
typedef struct {
	char user[USERS_NAME_MAX + 1];
	char nonce[AUTH_NONCE_TEXT_SIZE];
	char salt[AUTH_SALT_TEXT_SIZE];
	uint32_t iterations;
	char resume[AUTH_RESUME_SIZE];
} UsedChallenge;

/* A decoded proof's buffer. */
#define PROOF_BUF_SIZE CRYPTO_BASE64_DECODED_MAX(CRYPTO_BASE64_LEN(CRYPTO_SHA256_SIZE))

/* Decodes a proof: the base64 of exactly 32 bytes. */
static bool decode_proof(const char *proof, uint8_t out[PROOF_BUF_SIZE])
{
	size_t n = 0;
	return proof != NULL && strlen(proof) == CRYPTO_BASE64_LEN(CRYPTO_SHA256_SIZE) &&
	       crypto_base64_decode(proof, strlen(proof), out, PROOF_BUF_SIZE, &n) &&
	       n == CRYPTO_SHA256_SIZE;
}

/* A password login (02 §2.3): whether the proof is right; then the user's
 * record, the server signature, and the chain's resume key (02 §2.3.1). */
static bool verify_password(Auth *a, const UsedChallenge *ch, const char *proof,
			    const UserRecord **user, AuthProof *result,
			    uint8_t resume_key[CRYPTO_SHA256_SIZE])
{
	uint8_t received[PROOF_BUF_SIZE] = {0};
	bool ok = decode_proof(proof, received);

	/* AuthMessage (02 §2.3, not RFC 5802's). */
	char msg[USERS_NAME_MAX + AUTH_NONCE_TEXT_SIZE + AUTH_SALT_TEXT_SIZE + 16];
	int len = snprintf(msg, sizeof(msg), "%s,%s,%s,%lu", ch->user, ch->nonce, ch->salt,
			   (unsigned long)ch->iterations);
	ok = ok && len > 0 && (size_t)len < sizeof(msg);

	/* The same work for an unknown user, against a key nobody has. */
	const UserRecord *r = users_find(a->users, ch->user);
	uint8_t fake[CRYPTO_SHA256_SIZE], sig[CRYPTO_SHA256_SIZE];
	uint8_t client_key[CRYPTO_SHA256_SIZE], check[CRYPTO_SHA256_SIZE];
	char fake_in[USERS_NAME_MAX + 8];
	snprintf(fake_in, sizeof(fake_in), "key,%s", ch->user);
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

	if (ok) {
		uint8_t server_sig[CRYPTO_SHA256_SIZE];
		char key_in[sizeof("Resume Key,") + AUTH_NONCE_TEXT_SIZE];
		snprintf(key_in, sizeof(key_in), "Resume Key,%s", ch->nonce);
		ok = crypto_hmac_sha256(r->server_key, USERS_KEY_SIZE, msg, (size_t)len,
					server_sig) &&
		     crypto_base64_encode(server_sig, sizeof(server_sig), result->server_signature,
					  sizeof(result->server_signature)) > 0 &&
		     crypto_hmac_sha256(r->stored_key, USERS_KEY_SIZE, key_in, strlen(key_in),
					resume_key);
		crypto_zeroize(server_sig, sizeof(server_sig));
		crypto_zeroize(key_in, sizeof(key_in));
	}
	crypto_zeroize(received, sizeof(received));
	crypto_zeroize(fake, sizeof(fake));
	crypto_zeroize(sig, sizeof(sig));
	crypto_zeroize(client_key, sizeof(client_key));
	crypto_zeroize(check, sizeof(check));
	crypto_zeroize(msg, sizeof(msg));
	*user = r;
	return ok;
}

/* A resume (02 §2.3.1): whether the proof is right for a live entry of the
 * user; then the user's record, the server signature and the entry. */
static bool verify_resume(Auth *a, const UsedChallenge *ch, const char *proof, uint64_t now,
			  const UserRecord **user, AuthProof *result, ResumeEntry **entry)
{
	uint8_t received[PROOF_BUF_SIZE] = {0};
	bool ok = decode_proof(proof, received);

	char msg[sizeof("Server,") + USERS_NAME_MAX + AUTH_NONCE_TEXT_SIZE + AUTH_RESUME_SIZE + 4];
	const size_t skip = strlen("Server,");
	int len = snprintf(msg, sizeof(msg), "Server,%s,%s,%s", ch->user, ch->nonce, ch->resume);
	ok = ok && len > 0 && (size_t)len < sizeof(msg);

	/* The same work for an unknown id, against a key nobody has. */
	ResumeEntry *e = resume_find(a, ch->resume, now);
	if (e != NULL && strcmp(e->user, ch->user) != 0)
		e = NULL;
	const UserRecord *r = users_find(a->users, ch->user);
	uint8_t fake[CRYPTO_SHA256_SIZE], expected[CRYPTO_SHA256_SIZE];
	char fake_in[USERS_NAME_MAX + 8];
	snprintf(fake_in, sizeof(fake_in), "resume,%s", ch->user);
	bool hashed = crypto_hmac_sha256(a->server_secret, a->server_secret_len, fake_in,
					 strlen(fake_in), fake);
	const uint8_t *key = e != NULL ? e->key : fake;
	hashed = hashed && len > 0 &&
		 crypto_hmac_sha256(key, CRYPTO_SHA256_SIZE, msg + skip, (size_t)len - skip,
				    expected);
	bool match = hashed && crypto_equal(received, expected, CRYPTO_SHA256_SIZE);
	ok = ok && e != NULL && r != NULL && match;

	if (ok) {
		uint8_t server_sig[CRYPTO_SHA256_SIZE];
		ok = crypto_hmac_sha256(e->key, CRYPTO_SHA256_SIZE, msg, (size_t)len, server_sig) &&
		     crypto_base64_encode(server_sig, sizeof(server_sig), result->server_signature,
					  sizeof(result->server_signature)) > 0;
		crypto_zeroize(server_sig, sizeof(server_sig));
	}
	crypto_zeroize(received, sizeof(received));
	crypto_zeroize(fake, sizeof(fake));
	crypto_zeroize(expected, sizeof(expected));
	crypto_zeroize(msg, sizeof(msg));
	*user = r;
	*entry = e;
	return ok;
}

static enum auth_result op_verify_proof(void *self, AuthConn *c, const char *user,
					const char *nonce, const char *resume, const char *proof,
					AuthProof *out)
{
	Auth *a = self;
	uint64_t now = now_ms(a);

	/* The challenge is used up by this attempt, whatever its outcome. */
	bool challenged = c->state == AUTH_STATE_CHALLENGED;
	bool ok = challenged && now < c->nonce_expires;
	UsedChallenge ch;
	memcpy(ch.user, c->ch_user, sizeof(ch.user));
	memcpy(ch.nonce, c->nonce, sizeof(ch.nonce));
	memcpy(ch.salt, c->salt, sizeof(ch.salt));
	memcpy(ch.resume, c->ch_resume, sizeof(ch.resume));
	ch.iterations = c->iterations;
	clear_challenge(c);
	if (c->state == AUTH_STATE_CHALLENGED || c->state == AUTH_STATE_VERIFIED)
		c->state = AUTH_STATE_UNAUTHENTICATED;
	if (!challenged)
		return login_failed(a, c, user, now);

	ok = ok && user != NULL && strcmp(user, ch.user) == 0;
	ok = ok && nonce != NULL && strlen(nonce) == strlen(ch.nonce) &&
	     crypto_equal(nonce, ch.nonce, strlen(ch.nonce));
	/* A resume answers a resume challenge of the same id, a password login
	 * a password challenge. */
	bool resuming = ch.resume[0] != '\0';
	ok = ok && (resuming ? resume != NULL && strcmp(resume, ch.resume) == 0 : resume == NULL);

	AuthProof result;
	const UserRecord *r = NULL;
	ResumeEntry *e = NULL;
	uint8_t key[CRYPTO_SHA256_SIZE];
	bool verified = resuming ? verify_resume(a, &ch, proof, now, &r, &result, &e)
				 : verify_password(a, &ch, proof, &r, &result, key);
	ok = ok && verified;
	crypto_zeroize(&ch, sizeof(ch));
	if (!ok) {
		crypto_zeroize(key, sizeof(key));
		return login_failed(a, c, user, now);
	}

	result.role = r->role;
	result.capabilities = auth_role_capabilities(r->role);
	clear_session(c);
	snprintf(c->name, sizeof(c->name), "%s", r->name);
	c->role = r->role;
	if (resuming) {
		/* The id is used once: issue_secret gives the chain a new one. */
		memcpy(c->resume_key, e->key, sizeof(c->resume_key));
		memcpy(c->stored_key, e->stored_key, sizeof(c->stored_key));
		c->chain = e->chain;
		c->created = e->created;
		c->resumed = true;
		resume_free(e);
	} else {
		memcpy(c->resume_key, key, sizeof(c->resume_key));
		memcpy(c->stored_key, r->stored_key, sizeof(c->stored_key));
		c->chain = ++a->chain_seq;
		c->created = now;
	}
	crypto_zeroize(key, sizeof(key));
	c->state = AUTH_STATE_VERIFIED;
	rate_success(a, c->peer, now);
	log_info("login: user '%s' from %s as %s%s", r->name, c->peer, users_role_name(r->role),
		 c->resumed ? " (resumed)" : "");
	*out = result;
	return AUTH_OK;
}

/* ---- Sessions (02 §2.5) ------------------------------------------------------------ */

static bool op_issue_secret(void *self, AuthConn *c, char out[AUTH_SECRET_SIZE],
			    char resume[AUTH_RESUME_SIZE])
{
	Auth *a = self;
	if (c->state != AUTH_STATE_VERIFIED)
		return false;
	uint8_t rnd[AUTH_SECRET_RANDOM_BYTES + AUTH_RESUME_RANDOM_BYTES];
	if (a->random_bytes(rnd, sizeof(rnd)) != PAL_OK) {
		log_error("login: the random source failed");
		reset(c);
		return false;
	}
	uint64_t now = now_ms(a);
	crypto_base64url_encode(rnd, AUTH_SECRET_RANDOM_BYTES, c->secret, sizeof(c->secret));
	memcpy(out, c->secret, AUTH_SECRET_SIZE);

	/* The chain's entry (02 §2.3.1), with a new id. */
	resume_expire(a, now);
	ResumeEntry *e = resume_slot(a);
	crypto_base64url_encode(rnd + AUTH_SECRET_RANDOM_BYTES, AUTH_RESUME_RANDOM_BYTES, e->id,
				sizeof(e->id));
	crypto_zeroize(rnd, sizeof(rnd));
	snprintf(e->user, sizeof(e->user), "%s", c->name);
	memcpy(e->key, c->resume_key, sizeof(e->key));
	memcpy(e->stored_key, c->stored_key, sizeof(e->stored_key));
	e->chain = c->chain;
	e->created = c->created;
	e->last_activity = now;
	e->last_used = ++a->use_seq;
	memcpy(resume, e->id, AUTH_RESUME_SIZE);
	crypto_zeroize(c->resume_key, sizeof(c->resume_key));
	crypto_zeroize(c->stored_key, sizeof(c->stored_key));

	c->caps = auth_role_capabilities(c->role);
	c->last_activity = now;
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
	Auth *a = self;
	if (c->state == AUTH_STATE_AUTHENTICATED) {
		log_info("logout: user '%s' from %s", c->name, c->peer);
		ResumeEntry *e = resume_of_chain(a, c->chain);
		if (e != NULL)
			resume_free(e);
	}
	reset(c);
}

static void op_activity(void *self, AuthConn *c)
{
	Auth *a = self;
	uint64_t now = now_ms(a);
	if (!session_live(c, now))
		return;
	c->last_activity = now;
	/* The chain's idle timeout counts every session's activity. */
	ResumeEntry *e = resume_of_chain(a, c->chain);
	if (e != NULL) {
		e->last_activity = now;
		e->last_used = ++a->use_seq;
	}
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
			char resume[AUTH_RESUME_SIZE];
			memcpy(user, c->pending_user, sizeof(user));
			memcpy(cnonce, c->pending_cnonce, sizeof(cnonce));
			memcpy(resume, c->pending_resume, sizeof(resume));
			clear_pending(c);
			AuthChallenge ch;
			bool made = make_challenge(a, c, user, cnonce, resume, now, &ch);
			crypto_zeroize(resume, sizeof(resume));
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
	resume_expire(a, now);
	for (size_t i = 0; i < HAKKAS_AUTH_RESUME_MAX; i++)
		if (a->resume[i].id[0] != '\0')
			next = min_u64(next, resume_end(a, &a->resume[i]) - now);
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
