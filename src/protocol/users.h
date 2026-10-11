/*
 * The credential store (02 §2.2): the users file's records, parsing and
 * printing the file, deriving a user's SCRAM-SHA-256 keys from a password,
 * and loading and saving the file through the PAL.
 *
 * Naming: functions are prefixed users_, the status enum USERS_ /
 * users_status, the role enum USER_ROLE_ / user_role.
 *
 * The file (02 §2.2; decided in plan task 16, recorded there): a JSON array
 * of records {"user", "role", "salt", "iterations", "stored_key",
 * "server_key"}, every member required and no other member allowed; salt
 * the base64 (RFC 4648 §4, padded) of USERS_SALT_SIZE bytes, the keys of
 * USERS_KEY_SIZE bytes; iterations an integer 1-4294967295; user a valid
 * name (users_name_valid); role one of the four names. A missing file, an
 * empty file (no bytes, or only whitespace) and an empty array all mean no
 * users: every login is refused. Anything else that is wrong (not JSON, not
 * an array, a bad record, two records with the same name, more than
 * HAKKAS_USERS_MAX records, larger than USERS_FILE_MAX) makes the whole file
 * malformed: the store is left empty and the error logged (the file's
 * values never are). The engine then starts with no users rather than
 * refusing to start (users_load_startup): both refuse every login, and the
 * graphs keep controlling the plant.
 *
 * Memory: UserStore is fixed-size data (no heap): HAKKAS_USERS_MAX records
 * of about 120 bytes. Parsing and printing build a temporary cJSON tree.
 *
 * Secrets: no password and no SaltedPassword is ever stored; the derivation
 * zeroizes its intermediate buffers. Nothing here logs a salt or a key.
 *
 * Storage: users.c is pure (the random source is a parameter);
 * users_load, users_load_startup and users_save (protocol/users_file.c) go
 * through the PAL.
 *
 * Threading: engine-thread only (or a command-line tool).
 */
#ifndef HAKKAS_PROTOCOL_USERS_H
#define HAKKAS_PROTOCOL_USERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform/pal.h"
#include "system/crypto.h"

/* The most records the store holds (decided, plan task 16: a fixed bound for
 * the MCU); a file with more is malformed. A port's build may define it. */
#ifndef HAKKAS_USERS_MAX
#define HAKKAS_USERS_MAX 32
#endif

#define USERS_NAME_MAX 32		/* bytes of a user name, without the NUL */
#define USERS_SALT_SIZE 16		/* bytes of a salt */
#define USERS_KEY_SIZE CRYPTO_SHA256_SIZE /* bytes of stored_key, server_key */
#define USERS_DEFAULT_ITERATIONS 100000 /* PBKDF2 iterations (02 §2.2) */

/* The largest users file that is read, in bytes; a larger one is malformed.
 * A record as users_serialize prints it takes about 230 bytes. */
#define USERS_FILE_MAX (512 * HAKKAS_USERS_MAX + 64)

/* The longest password users_set_password takes (the command line's limit,
 * 05 §1.2). */
#define USERS_PASSWORD_MAX 1024

/* The roles of 02 §2.4, least to most privileged. */
enum user_role {
	USER_ROLE_VIEWER = 0,
	USER_ROLE_OPERATOR,
	USER_ROLE_EDITOR,
	USER_ROLE_ADMIN,
};

enum users_status {
	USERS_OK = 0,
	USERS_ERR_MALFORMED = -1,   /* the file is not a valid users file */
	USERS_ERR_STORAGE = -2,	    /* reading or writing the file failed */
	USERS_ERR_NOT_FOUND = -3,   /* no such file (users_load) or user */
	USERS_ERR_EXISTS = -4,	    /* users_add of a name that exists */
	USERS_ERR_FULL = -5,	    /* HAKKAS_USERS_MAX records already */
	USERS_ERR_INVALID = -6,	    /* an invalid name, role, password or iteration count */
	USERS_ERR_NO_MEMORY = -7,
	USERS_ERR_UNAVAILABLE = -8, /* the random source or a hash failed */
};

/* A short, static description of st, for log lines. */
const char *users_status_str(enum users_status st);

/* One user (02 §2.2). name is NUL-terminated. */
typedef struct {
	char name[USERS_NAME_MAX + 1];
	enum user_role role;
	uint8_t salt[USERS_SALT_SIZE];
	uint32_t iterations;
	uint8_t stored_key[USERS_KEY_SIZE]; /* SHA256(ClientKey) */
	uint8_t server_key[USERS_KEY_SIZE]; /* HMAC(SaltedPassword, "Server Key") */
} UserRecord;

/* The users, in file order. */
typedef struct {
	UserRecord users[HAKKAS_USERS_MAX];
	size_t count;
} UserStore;

/* The role's name ("viewer", "operator", "editor", "admin"), "?" otherwise. */
const char *users_role_name(enum user_role role);

/* Parses a role name (exact, lower case) into *out. False otherwise. */
bool users_role_parse(const char *name, enum user_role *out);

/* Whether name is a valid user name (decided, plan task 16): 1-32 bytes of
 * valid UTF-8 without control characters (U+0000-U+001F, U+007F), so a
 * name is always safe to put in a log line. NULL is not. */
bool users_name_valid(const char *name);

/* Empties the store. */
void users_init(UserStore *s);

/* The record called name (exact, byte for byte), or NULL. */
const UserRecord *users_find(const UserStore *s, const char *name);

/* Replaces s with the users file text (len bytes, need not be
 * NUL-terminated); origin names it in log lines. USERS_OK (an empty text,
 * only whitespace or [] is no users), USERS_ERR_MALFORMED (logged; s is
 * then empty), USERS_ERR_NO_MEMORY (s empty). */
enum users_status users_parse(UserStore *s, const char *text, size_t len, const char *origin);

/* The users file text of s (a JSON array, one record per user in store
 * order, members in the order of 02 §2.2, cJSON's indented form, a final
 * newline), NUL-terminated, from malloc, its length in *len (if not NULL).
 * NULL when out of memory. */
char *users_serialize(const UserStore *s, size_t *len);

/* Derives r's credentials from the password (len bytes, 1 to
 * USERS_PASSWORD_MAX) with a fresh random salt and the iteration count
 * (>= 1), as 02 §2.2 defines them: SaltedPassword = PBKDF2-HMAC-SHA256
 * (password, salt, iterations, 32), stored_key = SHA256(HMAC(SaltedPassword,
 * "Client Key")), server_key = HMAC(SaltedPassword, "Server Key"). The
 * intermediate values are zeroized. r's name and role are untouched.
 * USERS_ERR_INVALID, USERS_ERR_UNAVAILABLE (r then unchanged). */
enum users_status users_set_password(UserRecord *r, const char *password, size_t len,
				     uint32_t iterations, pal_random_fn random_bytes);

/* Adds the user name with role and the password's credentials
 * (users_set_password). USERS_ERR_INVALID (name, role, password),
 * USERS_ERR_EXISTS, USERS_ERR_FULL, USERS_ERR_UNAVAILABLE. */
enum users_status users_add(UserStore *s, const char *name, enum user_role role,
			    const char *password, size_t len, uint32_t iterations,
			    pal_random_fn random_bytes);

/* Sets the user name's credentials from a new password (users_set_password);
 * the role stays. USERS_ERR_NOT_FOUND, USERS_ERR_INVALID,
 * USERS_ERR_UNAVAILABLE. */
enum users_status users_passwd(UserStore *s, const char *name, const char *password, size_t len,
			       uint32_t iterations, pal_random_fn random_bytes);

/* Removes the user name, keeping the others' order. USERS_ERR_NOT_FOUND. */
enum users_status users_remove(UserStore *s, const char *name);

/* ---- The file through the PAL (protocol/users_file.c) ----------------------------- */

/* Replaces s with the users file at path (users_parse). USERS_ERR_NOT_FOUND
 * (not logged) if there is none, USERS_ERR_STORAGE (logged) if it cannot be
 * read, USERS_ERR_MALFORMED (logged) also for a file larger than
 * USERS_FILE_MAX. On any failure s is empty. */
enum users_status users_load(UserStore *s, const char *path);

/* The engine's load at startup (05 §1.4 step 3; decided, plan task 16): like
 * users_load, but it never refuses startup. A missing or empty file is
 * logged as a warning, a malformed or unreadable one as an error, both
 * saying every login is refused; s is then empty. Returns users_load's
 * status. */
enum users_status users_load_startup(UserStore *s, const char *path);

/* Writes s to path (users_serialize), atomically (pal_storage_write, 05
 * §2.3; mode 0600 on POSIX). USERS_ERR_STORAGE (logged),
 * USERS_ERR_NO_MEMORY. */
enum users_status users_save(const UserStore *s, const char *path);

#endif
