/*
 * Authorization (02 §2): the SCRAM-SHA-256 login of 02 §2.3, sessions and
 * their secrets (02 §2.5), roles and capabilities (02 §2.4), login rate
 * limiting (02 §2.5 item 7) and its logging (item 8). Pure logic: no wire
 * JSON (the protocol dispatcher, plan task 17, maps the environment:*
 * messages onto it) and no PAL call (the clock and the random source are
 * parameters).
 *
 * Naming: functions are prefixed auth_, types Auth*, constants AUTH_.
 *
 * The interface (02 §2.5 item 10): the protocol layer talks to an
 * Authorizer, a table of operations (AuthOps) and its object, so its tests
 * can substitute a fake. auth_create makes the real one; auth_authorizer
 * gives its Authorizer. Each WebSocket connection gets an AuthConn from
 * open (in on_open) and gives it back with close (in on_close: the session
 * ends, item 2).
 *
 * The connection's state (02 §2.3): UNAUTHENTICATED -> CHALLENGED (a
 * challenge was answered) -> VERIFIED (verify_proof accepted the proof) ->
 * AUTHENTICATED (issue_secret issued the session's secret). The flow:
 *
 *	environment:challenge  ->  challenge()     AUTH_OK: reply with *out
 *	                                           AUTH_DELAYED: on_challenge
 *	                                           replies later (back-off)
 *	                                           AUTH_FAILED: "Login failed"
 *	environment:login      ->  verify_proof()  AUTH_OK: issue_secret(),
 *	                                           reply secret, role,
 *	                                           capabilities, signature
 *	                                           AUTH_FAILED: "Login failed"
 *	                                           AUTH_FAILED_CLOSE: "Login
 *	                                           failed", then close (1008)
 *	environment:logout     ->  revoke_secret()
 *	any client message     ->  activity()      (the idle timeout)
 *	a gated message        ->  validate_secret()  false: "Not authenticated"
 *	a command              ->  auth_command_rule(), auth_permits() with
 *	                           capabilities_for()   false: "Not permitted"
 *
 * Every failure of a challenge or a login is AUTH_FAILED (or
 * AUTH_FAILED_CLOSE), whatever the cause, so the reply never says which part
 * failed (02 §2.3).
 *
 * Decisions (plan task 16, recorded in 02 §2.3-§2.5):
 *  - The challenge: the user must be a valid name (protocol/users.h) and
 *    cnonce the strict base64 (RFC 4648 §4) of AUTH_CNONCE_MIN to
 *    AUTH_CNONCE_MAX bytes, else AUTH_FAILED (not counted as a failed
 *    login). The nonce is cnonce followed by the base64 of
 *    AUTH_NONCE_RANDOM_BYTES random bytes, valid once, for AUTH_NONCE_TTL_MS
 *    on this connection. A new challenge replaces the previous one. A
 *    challenge on an AUTHENTICATED connection is refused (AUTH_FAILED, not
 *    counted): log out first. An unknown user gets salt =
 *    HMAC(server_secret, user)[0..16] and USERS_DEFAULT_ITERATIONS.
 *  - The login: user and nonce must equal the challenge's (the challenge is
 *    consumed by any login attempt, right or wrong) and proof must be the
 *    base64 of 32 bytes; AuthMessage = user "," nonce "," salt ","
 *    iterations exactly as 02 §2.3 (salt the base64 text sent, iterations in
 *    decimal). The proof check runs the same hashing for an unknown user,
 *    against a key derived from server_secret, so both take the same time.
 *    Every failed login (no challenge, an expired or reused nonce, a wrong
 *    user, nonce or proof, an unknown user) counts.
 *  - Rate limiting: the 3rd failed login on a connection is
 *    AUTH_FAILED_CLOSE. Per source address, from the 5th consecutive
 *    failure on, a challenge is answered no earlier than 1 s after the
 *    address's last failure, doubling per further failure up to 60 s
 *    (failures 5, 6, 7 ... -> 1, 2, 4 ... 32, 60 s). The reply is deferred,
 *    never refused: challenge returns AUTH_DELAYED, the tick hook makes the
 *    challenge when due and hands it to on_challenge, and the loop never
 *    blocks. On the wire the challenge reply simply arrives late. A new
 *    challenge meanwhile replaces the pending one (same due time); a login
 *    meanwhile has no challenge and fails. A success from the address, or
 *    AUTH_QUIET_RESET_MS without a failure, resets its count. The table has
 *    HAKKAS_AUTH_RATE_ADDRS entries; a new address evicts the least
 *    recently used.
 *  - Sessions: the secret is AUTH_SECRET_RANDOM_BYTES random bytes in
 *    base64url (22 characters), valid only on its connection, compared in
 *    constant time. A session ends after idle_timeout_s without activity()
 *    or max_lifetime_s after issue_secret; from that instant validate_secret
 *    is false and capabilities_for 0, and the tick hook (auth_tick) returns
 *    the connection to UNAUTHENTICATED and calls on_expired, so the protocol
 *    layer sends environment:logout and stops delivering events. A logout
 *    (revoke_secret) or close ends it at once, without on_expired.
 *  - The users file (plan task 18): re-read through load_users at every
 *    challenge made (also a delayed one, when it is made); see AuthConfig.
 *  - Logging (02 §2.5 item 8): a login's success (info) or failure (warn)
 *    with the user name and source address only; a session's end (info),
 *    a closing connection and a back-off (info). Never a secret, proof,
 *    nonce, salt, password, key or resume id.
 *
 * Resuming a session (02 §2.3.1, plan task 46): a login issues a resume id
 * (issue_secret), kept in a table with the resume key both sides derive,
 * ResumeKey = HMAC(StoredKey, "Resume Key," + nonce). A challenge and a
 * login that carry a resume id resume the session of that id's chain
 * without the password: the challenge has no salt and no iterations, the
 * proof is HMAC(ResumeKey, user "," nonce "," id) and the server signature
 * HMAC(ResumeKey, "Server," user "," nonce "," id). Decisions:
 *  - A chain is the password login and the sessions resumed from it. It
 *    has one entry: a resume replaces it with a new id (an id is used once)
 *    and the same key. Every session of the chain has the password login's
 *    start (created), so the maximum lifetime counts from it; the idle
 *    timeout counts from the last activity of any of its sessions. The
 *    entry ends with that deadline, at a logout of any of its sessions, and
 *    when the users file read for a challenge no longer has its user or has
 *    another stored_key (a password change); not when a connection closes
 *    (a reload).
 *  - A resumed session gets the user's role from the users file as read
 *    for its challenge.
 *  - A malformed id (not the base64url of AUTH_RESUME_RANDOM_BYTES bytes)
 *    is a refused challenge (AUTH_FAILED, not counted); an unknown,
 *    expired or another user's id gets a normal challenge and its login
 *    fails after the same hashing. A failed resume does not end the entry
 *    (an id is seen on the wire, so anyone could otherwise end a chain).
 *  - The table has HAKKAS_AUTH_RESUME_MAX entries; a new one evicts the
 *    least recently used (its session goes on; only resuming it fails).
 *
 * Threading: engine thread only. Callbacks (AuthEvents) are called from
 * auth_tick only and must not call close.
 */
#ifndef HAKKAS_PROTOCOL_AUTH_H
#define HAKKAS_PROTOCOL_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform/pal.h"
#include "protocol/users.h"
#include "system/crypto.h"

/* ---- Capabilities and commands (02 §2.4) ------------------------------------------ */

/* The FBP capabilities, as bits, in the order of runtime:runtime's
 * allCapabilities (02 §3.2.1). */
enum auth_capability {
	AUTH_CAP_PROTOCOL_RUNTIME = 1u << 0,
	AUTH_CAP_PROTOCOL_GRAPH = 1u << 1,
	AUTH_CAP_PROTOCOL_COMPONENT = 1u << 2,
	AUTH_CAP_GRAPH_READONLY = 1u << 3,
	AUTH_CAP_NETWORK_STATUS = 1u << 4,
	AUTH_CAP_NETWORK_PERSIST = 1u << 5,
	AUTH_CAP_NETWORK_DATA = 1u << 6,
	AUTH_CAP_NETWORK_CONTROL = 1u << 7,
};

#define AUTH_CAP_COUNT 8
#define AUTH_CAPS_ALL ((1u << AUTH_CAP_COUNT) - 1u)

/* The name of capability bit i (0 to AUTH_CAP_COUNT - 1; bit 1u << i), e.g.
 * "protocol:runtime"; NULL otherwise. Listing the names of the set bits for
 * i = 0, 1, ... gives the 02 §3.2.1 order. */
const char *auth_capability_name(unsigned i);

/* The capabilities of a role (02 §2.4): viewer protocol:component,
 * network:status, network:data, graph:readonly; operator also
 * network:control; editor also protocol:graph, protocol:runtime,
 * network:persist; admin the same as editor. 0 for an invalid role. */
uint32_t auth_role_capabilities(enum user_role role);

/* What a client command needs (02 §2.4, §2.5 item 5). */
typedef struct {
	/* A client command of 02 §3 (otherwise the dispatcher answers with
	 * its generic error, after the secret check: unknown commands are
	 * gated, except in environment). */
	bool known;
	/* It must carry the connection's current secret: every command but
	 * environment:* and runtime:getruntime. */
	bool gated;
	/* The session needs one of these capabilities; 0: none. */
	uint32_t caps_any;
} AuthCommandRule;

/*
 * The rule for protocol:command (02 §2.4):
 *  - environment:challenge, login, logout: not gated, no capability (any
 *    other environment command: not gated, unknown);
 *  - runtime:getruntime: not gated, no capability; runtime:new_graph,
 *    delete_graph: protocol:runtime;
 *  - graph:connect, graph:list (the read-only graph commands), every
 *    component:* (list, getsource) and trace:* (start, stop, clear, dump):
 *    graph:readonly or protocol:graph;
 *  - every other graph command of 02 §3.4 (rename, addnode, removenode,
 *    renamenode, changenode, addedge, removeedge, changeedge, addinitial,
 *    removeinitial, addinport, removeinport, renameinport, addoutport,
 *    removeoutport, renameoutport, addgroup, removegroup, renamegroup,
 *    changegroup): protocol:graph;
 *  - network:getstatus: network:status; edges: network:data; start, stop:
 *    network:control; persist: network:persist;
 *  - anything else (NULL included): unknown, gated, no capability.
 */
AuthCommandRule auth_command_rule(const char *protocol, const char *command);

/* Whether a session with caps may run a command with this rule. */
static inline bool auth_permits(uint32_t caps, const AuthCommandRule *rule)
{
	return rule->caps_any == 0 || (caps & rule->caps_any) != 0;
}

/* ---- The authorizer interface (02 §2.5 item 10) ------------------------------------ */

#define AUTH_CNONCE_MIN 16	     /* bytes of a client nonce, decoded */
#define AUTH_CNONCE_MAX 64
#define AUTH_NONCE_RANDOM_BYTES 16   /* the server's part of the nonce */
#define AUTH_NONCE_TTL_MS 30000	     /* a challenge's lifetime */
#define AUTH_SECRET_RANDOM_BYTES 16  /* 128 bits */
#define AUTH_SECRET_LEN CRYPTO_BASE64URL_LEN(AUTH_SECRET_RANDOM_BYTES) /* 22 */
#define AUTH_CONN_FAILURES_MAX 3     /* failed logins that close a connection */
#define AUTH_BACKOFF_AFTER 5	     /* failures of an address before the back-off */
#define AUTH_BACKOFF_BASE_MS 1000
#define AUTH_BACKOFF_MAX_MS 60000
#define AUTH_QUIET_RESET_MS (15u * 60u * 1000u)
#define AUTH_RESUME_RANDOM_BYTES 16  /* a resume id (02 §2.3.1) */
#define AUTH_RESUME_LEN CRYPTO_BASE64URL_LEN(AUTH_RESUME_RANDOM_BYTES) /* 22 */
#define AUTH_RESUME_SIZE (AUTH_RESUME_LEN + 1)

/* Source addresses the rate limiter remembers; a port's build may define it. */
#ifndef HAKKAS_AUTH_RATE_ADDRS
#define HAKKAS_AUTH_RATE_ADDRS 16
#endif

/* Sessions that can be resumed (02 §2.3.1); a port's build may define it. */
#ifndef HAKKAS_AUTH_RESUME_MAX
#define HAKKAS_AUTH_RESUME_MAX 16
#endif

/* Text sizes, NUL included. */
#define AUTH_SALT_TEXT_SIZE (CRYPTO_BASE64_LEN(USERS_SALT_SIZE) + 1)
#define AUTH_CNONCE_TEXT_MAX CRYPTO_BASE64_LEN(AUTH_CNONCE_MAX)
#define AUTH_NONCE_TEXT_SIZE                                                                       \
	(AUTH_CNONCE_TEXT_MAX + CRYPTO_BASE64_LEN(AUTH_NONCE_RANDOM_BYTES) + 1)
#define AUTH_SIGNATURE_TEXT_SIZE (CRYPTO_BASE64_LEN(CRYPTO_SHA256_SIZE) + 1)
#define AUTH_SECRET_SIZE (AUTH_SECRET_LEN + 1)

enum auth_state {
	AUTH_STATE_UNAUTHENTICATED = 0,
	AUTH_STATE_CHALLENGED,
	AUTH_STATE_VERIFIED, /* proof accepted, secret not issued yet */
	AUTH_STATE_AUTHENTICATED,
};

enum auth_result {
	AUTH_OK = 0,
	AUTH_FAILED,	   /* "Login failed" */
	AUTH_FAILED_CLOSE, /* "Login failed", then close the connection */
	AUTH_DELAYED,	   /* challenge: on_challenge replies later */
};

/* The environment:challenge reply's payload (02 §2.3; 02 §2.3.1 when
 * resume: then only the nonce, salt "" and iterations 0). */
typedef struct {
	bool resume;
	char salt[AUTH_SALT_TEXT_SIZE]; /* base64 */
	uint32_t iterations;
	char nonce[AUTH_NONCE_TEXT_SIZE]; /* cnonce + base64 of the server's bytes */
} AuthChallenge;

/* What verify_proof gives on success, for the environment:login reply. */
typedef struct {
	enum user_role role;
	uint32_t capabilities;
	char server_signature[AUTH_SIGNATURE_TEXT_SIZE]; /* base64 */
} AuthProof;

typedef struct AuthConn AuthConn;

/* The protocol layer's per-connection callbacks; user is open's. */
typedef struct {
	/* The session expired (02 §2.5 item 3): send environment:logout and
	 * stop event delivery; the connection is UNAUTHENTICATED. */
	void (*on_expired)(void *user);
	/* A delayed challenge (AUTH_DELAYED) is ready: send it as the
	 * environment:challenge reply. NULL if making it failed (the random
	 * source): reply "Login failed". */
	void (*on_challenge)(void *user, const AuthChallenge *challenge);
} AuthEvents;

typedef struct {
	/* A new connection from peer_addr (the rate limiter's key, logged);
	 * events (borrowed, may be NULL) are called with user. NULL when out
	 * of memory: refuse the connection. */
	AuthConn *(*open)(void *self, const char *peer_addr, const AuthEvents *events,
			  void *user);
	/* The connection closed: its session and challenge end. NULL is
	 * ignored. */
	void (*close)(void *self, AuthConn *c);
	/* A client message arrived (any message): the idle timeout restarts. */
	void (*activity)(void *self, AuthConn *c);
	/* environment:challenge {user, cnonce[, resume]}; resume NULL for a
	 * password login. AUTH_OK (*out filled), AUTH_DELAYED, AUTH_FAILED. */
	enum auth_result (*challenge)(void *self, AuthConn *c, const char *user,
				      const char *cnonce, const char *resume,
				      AuthChallenge *out);
	/* environment:login {user, nonce[, resume], proof}; resume NULL for a
	 * password login (it must match the challenge's). AUTH_OK (*out
	 * filled; the connection is VERIFIED), AUTH_FAILED,
	 * AUTH_FAILED_CLOSE. */
	enum auth_result (*verify_proof)(void *self, AuthConn *c, const char *user,
					 const char *nonce, const char *resume, const char *proof,
					 AuthProof *out);
	/* After verify_proof's AUTH_OK: start the session, write its secret
	 * (AUTH_SECRET_LEN characters and a NUL) to out and the chain's new
	 * resume id (AUTH_RESUME_LEN characters and a NUL, 02 §2.3.1) to
	 * resume. False (connection UNAUTHENTICATED; reply "Login failed") if
	 * not VERIFIED or the random source failed. */
	bool (*issue_secret)(void *self, AuthConn *c, char out[AUTH_SECRET_SIZE],
			     char resume[AUTH_RESUME_SIZE]);
	/* Whether secret is this connection's current, unexpired one. */
	bool (*validate_secret)(void *self, AuthConn *c, const char *secret);
	/* environment:logout: the session (or challenge) ends, and its
	 * chain's resume entry; UNAUTHENTICATED. */
	void (*revoke_secret)(void *self, AuthConn *c);
	/* The session's capabilities; 0 unless AUTHENTICATED and unexpired
	 * (so non-zero means events may be delivered, 02 §2.5 item 6). */
	uint32_t (*capabilities_for)(void *self, const AuthConn *c);
	/* The connection's state (an expired session still reads
	 * AUTHENTICATED until the tick hook ends it). */
	enum auth_state (*state)(void *self, const AuthConn *c);
} AuthOps;

typedef struct {
	const AuthOps *ops;
	void *self;
} Authorizer;

/* ---- The real authorizer ----------------------------------------------------------- */

typedef struct Auth Auth;

/* The engine time in ms (e.g. loop_now_ms of the engine loop). */
typedef uint64_t (*auth_clock_fn)(void *ctx);

/* Reads the users file into *out (users_loader, protocol/users.h, with the
 * file's path as ctx; a fake in tests), as users_load: USERS_OK,
 * USERS_ERR_NOT_FOUND and USERS_ERR_MALFORMED leave *out the users to use
 * (empty for the two failures); any other status means the file could not
 * be read. */
typedef enum users_status (*auth_users_fn)(void *ctx, UserStore *out);

typedef struct {
	/* The users (borrowed; must outlive the Auth): users_load_startup's
	 * store, which load_users re-reads into. An empty store refuses every
	 * login (02 §2.2). */
	UserStore *users;
	/* Re-reads the users file whenever a challenge is made (02 §2.2;
	 * decided, plan task 18), so a user added, removed or given a new
	 * password takes effect at the next login; sessions already open are
	 * not affected. A missing, empty or malformed file means no users (as
	 * at startup, 05 §1.4 step 3); a file that cannot be read (a storage
	 * error, out of memory) keeps the users of the previous read, with a
	 * warning. NULL: the users are never re-read. */
	auth_users_fn load_users;
	void *load_users_ctx;
	/* auth.server_secret (05 §1.1), at least CONFIG_SECRET_MIN (16)
	 * bytes: the HMAC key is the string's bytes as stored (copied). */
	const char *server_secret;
	uint32_t idle_timeout_s; /* auth.idle_timeout_s, > 0 */
	uint32_t max_lifetime_s; /* auth.max_lifetime_s, > 0 */
	auth_clock_fn clock;
	void *clock_ctx;
	pal_random_fn random_bytes; /* pal_random_bytes */
} AuthConfig;

/* Makes the real authorizer (05 §1.4 step 3). False (logged) for an invalid
 * cfg (no users store, clock or random source, a server secret shorter than
 * 16 or longer than 128 bytes, a zero timeout) or out of memory. */
bool auth_create(const AuthConfig *cfg, Auth **out);

/* Frees it, and any AuthConn still open (without callbacks). NULL is
 * ignored. */
void auth_destroy(Auth *a);

/* Its Authorizer interface. */
Authorizer auth_authorizer(Auth *a);

/* The tick hook (a loop_tick_fn, 01 §3.2.1; ctx is the Auth): ends expired
 * sessions (on_expired), challenges and resume entries, answers due delayed
 * challenges (on_challenge). Returns the ms until it next needs to run, or UINT64_MAX
 * if nothing is pending. Register it with loop_add_tick (plan task 19). */
uint64_t auth_tick(void *ctx, uint64_t now_ms);

#endif
