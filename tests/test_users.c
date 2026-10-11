/*
 * The credential store (protocol/users.h, 02 §2.2): names and roles, the
 * key derivation, adding, changing and removing users, printing and parsing
 * the users file with every kind of malformed content, and loading and
 * saving it through the POSIX PAL in a fresh temporary directory.
 */
#define _XOPEN_SOURCE 700

#include "test.h"

#include <ftw.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

#include "platform/pal.h"
#include "protocol/users.h"
#include "system/crypto.h"
#include "system/log.h"

static char root[PAL_PATH_MAX];

/* Log lines (level and message), one per line. */
static char logs[16384];

static void log_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)len;
	size_t used = strlen(logs);
	snprintf(logs + used, sizeof(logs) - used, "%s %s\n", log_level_name(level), line);
}

/* A deterministic random source: 0, 1, 2, ... */
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

static UserStore store, store2;

static void test_names_and_roles(void)
{
	CHECK(users_name_valid("alice"));
	CHECK(users_name_valid("a"));
	CHECK(users_name_valid("12345678901234567890123456789012")); /* 32 bytes */
	CHECK(!users_name_valid("123456789012345678901234567890123"));
	CHECK(!users_name_valid(""));
	CHECK(!users_name_valid(NULL));
	CHECK(users_name_valid("J\xc3\xb6rg"));		     /* UTF-8 */
	CHECK(users_name_valid("with space, comma \"quote\"")); /* any printable */
	CHECK(!users_name_valid("bad\xc3"));		     /* cut sequence */
	CHECK(!users_name_valid("\xed\xa0\x80"));	     /* a surrogate */
	CHECK(!users_name_valid("tab\there"));
	CHECK(!users_name_valid("new\nline"));
	CHECK(!users_name_valid("del\x7f"));

	const char *names[] = {"viewer", "operator", "editor", "admin"};
	for (int i = 0; i < 4; i++) {
		enum user_role r = USER_ROLE_ADMIN;
		CHECK(users_role_parse(names[i], &r));
		CHECK(r == (enum user_role)i);
		CHECK_STREQ(users_role_name((enum user_role)i), names[i]);
	}
	enum user_role r = USER_ROLE_EDITOR;
	CHECK(!users_role_parse("Admin", &r));
	CHECK(!users_role_parse("root", &r));
	CHECK(!users_role_parse(NULL, &r));
	CHECK(r == USER_ROLE_EDITOR);
	CHECK_STREQ(users_role_name((enum user_role)7), "?");
}

/* The record's keys are those of 02 §2.2 for password. */
static void check_keys(const UserRecord *r, const char *password)
{
	uint8_t salted[32], client_key[32], stored[32], server[32];
	CHECK(crypto_pbkdf2_hmac_sha256(password, strlen(password), r->salt, USERS_SALT_SIZE,
					r->iterations, salted, 32));
	CHECK(crypto_hmac_sha256(salted, 32, "Client Key", 10, client_key));
	CHECK(crypto_sha256(client_key, 32, stored));
	CHECK(crypto_hmac_sha256(salted, 32, "Server Key", 10, server));
	CHECK(memcmp(stored, r->stored_key, 32) == 0);
	CHECK(memcmp(server, r->server_key, 32) == 0);
}

static void test_changes(void)
{
	users_init(&store);
	CHECK(store.count == 0);
	rnd_next = 0;
	CHECK(users_add(&store, "alice", USER_ROLE_EDITOR, "pencil", 6, 7, fake_random) ==
	      USERS_OK);
	CHECK(store.count == 1);
	const UserRecord *r = users_find(&store, "alice");
	CHECK(r != NULL && r->role == USER_ROLE_EDITOR && r->iterations == 7);
	for (int i = 0; r != NULL && i < USERS_SALT_SIZE; i++)
		CHECK(r->salt[i] == i); /* the random source's bytes */
	if (r != NULL)
		check_keys(r, "pencil");
	CHECK(users_find(&store, "Alice") == NULL);
	CHECK(users_find(&store, NULL) == NULL);

	CHECK(users_add(&store, "alice", USER_ROLE_VIEWER, "x", 1, 1, fake_random) ==
	      USERS_ERR_EXISTS);
	CHECK(users_add(&store, "", USER_ROLE_VIEWER, "x", 1, 1, fake_random) ==
	      USERS_ERR_INVALID);
	CHECK(users_add(&store, "bob", (enum user_role)9, "x", 1, 1, fake_random) ==
	      USERS_ERR_INVALID);
	CHECK(users_add(&store, "bob", USER_ROLE_VIEWER, "", 0, 1, fake_random) ==
	      USERS_ERR_INVALID);
	CHECK(users_add(&store, "bob", USER_ROLE_VIEWER, "x", 1, 0, fake_random) ==
	      USERS_ERR_INVALID);
	static char long_pw[USERS_PASSWORD_MAX + 1];
	memset(long_pw, 'p', sizeof(long_pw));
	CHECK(users_add(&store, "bob", USER_ROLE_VIEWER, long_pw, USERS_PASSWORD_MAX + 1, 1,
			fake_random) == USERS_ERR_INVALID);
	CHECK(users_add(&store, "bob", USER_ROLE_VIEWER, long_pw, USERS_PASSWORD_MAX, 1,
			fake_random) == USERS_OK);
	rnd_fail = true;
	CHECK(users_add(&store, "carol", USER_ROLE_VIEWER, "x", 1, 1, fake_random) ==
	      USERS_ERR_UNAVAILABLE);
	rnd_fail = false;
	CHECK(store.count == 2);

	/* passwd: new salt and keys, same role. */
	UserRecord before = *users_find(&store, "alice");
	CHECK(users_passwd(&store, "alice", "secret", 6, 3, fake_random) == USERS_OK);
	r = users_find(&store, "alice");
	CHECK(r->role == USER_ROLE_EDITOR && r->iterations == 3);
	CHECK(memcmp(r->salt, before.salt, USERS_SALT_SIZE) != 0);
	check_keys(r, "secret");
	CHECK(users_passwd(&store, "nobody", "x", 1, 1, fake_random) == USERS_ERR_NOT_FOUND);
	rnd_fail = true;
	CHECK(users_passwd(&store, "alice", "other", 5, 1, fake_random) ==
	      USERS_ERR_UNAVAILABLE);
	rnd_fail = false;
	CHECK(memcmp(users_find(&store, "alice"), r, sizeof(*r)) == 0); /* unchanged */

	/* remove keeps the order. */
	CHECK(users_add(&store, "carol", USER_ROLE_OPERATOR, "x", 1, 1, fake_random) == USERS_OK);
	CHECK(users_remove(&store, "bob") == USERS_OK);
	CHECK(store.count == 2);
	CHECK_STREQ(store.users[0].name, "alice");
	CHECK_STREQ(store.users[1].name, "carol");
	CHECK(users_remove(&store, "bob") == USERS_ERR_NOT_FOUND);

	/* At most HAKKAS_USERS_MAX. */
	users_init(&store);
	for (int i = 0; i < HAKKAS_USERS_MAX; i++) {
		char name[16];
		snprintf(name, sizeof(name), "user%d", i);
		CHECK(users_add(&store, name, USER_ROLE_VIEWER, "x", 1, 1, fake_random) ==
		      USERS_OK);
	}
	CHECK(users_add(&store, "onemore", USER_ROLE_VIEWER, "x", 1, 1, fake_random) ==
	      USERS_ERR_FULL);
}

static void test_round_trip(void)
{
	users_init(&store);
	rnd_next = 100;
	CHECK(users_add(&store, "alice", USER_ROLE_ADMIN, "pencil", 6, 1, fake_random) ==
	      USERS_OK);
	store.users[0].iterations = 100000;
	CHECK(users_add(&store, "J\xc3\xb6rg \"q\"", USER_ROLE_VIEWER, "pw", 2, 1, fake_random) ==
	      USERS_OK);
	/* The largest count, set by hand (PBKDF2 with it would take hours). */
	store.users[1].iterations = 4294967295u;

	size_t len = 0;
	char *text = users_serialize(&store, &len);
	CHECK(text != NULL && len == strlen(text) && text[len - 1] == '\n');
	/* Members in the order of 02 §2.2, the base64 of the bytes. */
	const char *u = strstr(text, "\"user\"");
	const char *ro = strstr(text, "\"role\"");
	const char *sa = strstr(text, "\"salt\"");
	const char *it = strstr(text, "\"iterations\"");
	const char *st = strstr(text, "\"stored_key\"");
	const char *se = strstr(text, "\"server_key\"");
	CHECK(u && ro && sa && it && st && se && u < ro && ro < sa && sa < it && it < st &&
	      st < se);
	char salt64[32];
	crypto_base64_encode(store.users[0].salt, 16, salt64, sizeof(salt64));
	CHECK(strstr(text, salt64) != NULL);
	CHECK(strstr(text, "100000") != NULL);
	CHECK(strstr(text, "4294967295") != NULL);
	CHECK(strstr(text, "\"admin\"") != NULL);

	CHECK(users_parse(&store2, text, len, "t") == USERS_OK);
	CHECK(store2.count == 2);
	CHECK(memcmp(&store2.users[0], &store.users[0], sizeof(UserRecord)) == 0);
	CHECK(memcmp(&store2.users[1], &store.users[1], sizeof(UserRecord)) == 0);
	free(text);

	users_init(&store);
	text = users_serialize(&store, &len);
	CHECK_STREQ(text, "[]\n");
	free(text);
}

/* A valid record with one member's text replaced: "%s" placeholders. */
#define RECORD_FMT                                                                                 \
	"{\"user\": %s, \"role\": %s, \"salt\": %s, \"iterations\": %s, "                         \
	"\"stored_key\": %s, \"server_key\": %s}"
#define USER_OK "\"alice\""
#define ROLE_OK "\"viewer\""
#define SALT_OK "\"AAECAwQFBgcICQoLDA0ODw==\""
#define ITER_OK "4096"
#define KEY_OK "\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\""

static char doc[USERS_FILE_MAX + 1024];

static void make_doc(const char *user, const char *role, const char *salt, const char *iter,
		     const char *stored, const char *server)
{
	char rec[1024];
	snprintf(rec, sizeof(rec), RECORD_FMT, user, role, salt, iter, stored, server);
	snprintf(doc, sizeof(doc), "[%s]", rec);
}

/* doc is malformed, the store left empty, and the log mentions want but
 * no value. */
static void malformed(const char *want)
{
	logs[0] = '\0';
	store.count = 3;
	CHECK(users_parse(&store, doc, strlen(doc), "users.json") == USERS_ERR_MALFORMED);
	CHECK(store.count == 0);
	if (strstr(logs, want) == NULL)
		fprintf(stderr, "malformed: log \"%s\", expected \"%s\"\n", logs, want);
	CHECK(strstr(logs, want) != NULL);
	CHECK(strstr(logs, "AAECAwQF") == NULL); /* no salt or key in the log */
	CHECK(strncmp(logs, "error ", 6) == 0 && strstr(logs, ": users.json: ") != NULL);
}

static void test_parse(void)
{
	/* No users. */
	const char *empty[] = {"", "  \n\t\r\n", "[]", " [ ] \n"};
	for (int i = 0; i < 4; i++) {
		store.count = 5;
		CHECK(users_parse(&store, empty[i], strlen(empty[i]), "t") == USERS_OK);
		CHECK(store.count == 0);
	}

	make_doc(USER_OK, ROLE_OK, SALT_OK, ITER_OK, KEY_OK, KEY_OK);
	CHECK(users_parse(&store, doc, strlen(doc), "t") == USERS_OK);
	CHECK(store.count == 1 && store.users[0].iterations == 4096);
	CHECK(store.users[0].role == USER_ROLE_VIEWER);
	CHECK(store.users[0].salt[15] == 15 && store.users[0].stored_key[31] == 31);
	make_doc(USER_OK, ROLE_OK, SALT_OK, "4.096e3", KEY_OK, KEY_OK);
	CHECK(users_parse(&store, doc, strlen(doc), "t") == USERS_OK);
	make_doc(USER_OK, ROLE_OK, SALT_OK, "1", KEY_OK, KEY_OK);
	CHECK(users_parse(&store, doc, strlen(doc), "t") == USERS_OK);

	snprintf(doc, sizeof(doc), "[");
	malformed("not valid JSON");
	snprintf(doc, sizeof(doc), "{}");
	malformed("expected a JSON array of users");
	snprintf(doc, sizeof(doc), "\"x\"");
	malformed("expected a JSON array of users");
	snprintf(doc, sizeof(doc), "[1]");
	malformed("user 1: expected an object");
	snprintf(doc, sizeof(doc), "[{\"user\": \"a\"}]");
	malformed("user 1: missing \"role\"");

	make_doc(USER_OK, ROLE_OK, SALT_OK, ITER_OK, KEY_OK, KEY_OK);
	doc[strlen(doc) - 2] = '\0';
	strcat(doc, ", \"password\": \"x\"}]");
	malformed("user 1: unknown member");
	make_doc(USER_OK, ROLE_OK, SALT_OK, ITER_OK, KEY_OK, KEY_OK);
	doc[strlen(doc) - 2] = '\0';
	strcat(doc, ", \"role\": \"admin\"}]");
	malformed("user 1: duplicate member \"role\"");

	make_doc("\"\"", ROLE_OK, SALT_OK, ITER_OK, KEY_OK, KEY_OK);
	malformed("user 1: user: expected 1-32 bytes");
	make_doc("\"123456789012345678901234567890123\"", ROLE_OK, SALT_OK, ITER_OK, KEY_OK,
		 KEY_OK);
	malformed("user 1: user:");
	make_doc("\"a\\nb\"", ROLE_OK, SALT_OK, ITER_OK, KEY_OK, KEY_OK);
	malformed("user 1: user:");
	make_doc("7", ROLE_OK, SALT_OK, ITER_OK, KEY_OK, KEY_OK);
	malformed("user 1: user:");
	make_doc(USER_OK, "\"root\"", SALT_OK, ITER_OK, KEY_OK, KEY_OK);
	malformed("user 1: role: expected");
	make_doc(USER_OK, "null", SALT_OK, ITER_OK, KEY_OK, KEY_OK);
	malformed("user 1: role:");
	make_doc(USER_OK, ROLE_OK, "\"AAECAwQFBgcICQoLDA0O\"", ITER_OK, KEY_OK, KEY_OK); /* 15 */
	malformed("user 1: salt: expected the base64 of 16 bytes");
	/* 17 bytes, then without padding */
	make_doc(USER_OK, ROLE_OK, "\"AAECAwQFBgcICQoLDA0ODxA=\"", ITER_OK, KEY_OK, KEY_OK);
	malformed("user 1: salt:");
	make_doc(USER_OK, ROLE_OK, "\"AAECAwQFBgcICQoLDA0ODw\"", ITER_OK, KEY_OK, KEY_OK);
	malformed("user 1: salt:");
	make_doc(USER_OK, ROLE_OK, "\"AAECAwQFBgcICQoLDA0OD!==\"", ITER_OK, KEY_OK, KEY_OK);
	malformed("user 1: salt:");
	const char *bad_iter[] = {"0", "-1", "1.5", "4294967296", "\"4096\"", "true", "1e999"};
	for (int i = 0; i < 7; i++) {
		make_doc(USER_OK, ROLE_OK, SALT_OK, bad_iter[i], KEY_OK, KEY_OK);
		malformed("user 1: iterations: expected an integer 1-4294967295");
	}
	make_doc(USER_OK, ROLE_OK, SALT_OK, ITER_OK, SALT_OK, KEY_OK);
	malformed("user 1: stored_key: expected the base64 of 32 bytes");
	make_doc(USER_OK, ROLE_OK, SALT_OK, ITER_OK, KEY_OK, "\"\"");
	malformed("user 1: server_key:");

	/* A duplicate user, in the second record. */
	char rec[1024];
	snprintf(rec, sizeof(rec), RECORD_FMT, USER_OK, ROLE_OK, SALT_OK, ITER_OK, KEY_OK, KEY_OK);
	snprintf(doc, sizeof(doc), "[%s, %s]", rec, rec);
	malformed("user 2: duplicate user \"alice\"");

	/* More than HAKKAS_USERS_MAX. */
	strcpy(doc, "[");
	for (int i = 0; i <= HAKKAS_USERS_MAX; i++) {
		char name[16];
		snprintf(name, sizeof(name), "\"u%d\"", i);
		snprintf(rec, sizeof(rec), RECORD_FMT, name, ROLE_OK, SALT_OK, ITER_OK, KEY_OK,
			 KEY_OK);
		if (i > 0)
			strcat(doc, ",");
		strcat(doc, rec);
	}
	strcat(doc, "]");
	malformed("more than 32 users");
	/* Exactly the most. */
	char *last = strrchr(doc, '{');
	last[-1] = ']';
	last[0] = '\0';
	CHECK(users_parse(&store, doc, strlen(doc), "t") == USERS_OK);
	CHECK(store.count == HAKKAS_USERS_MAX);
}

/* ---- The file ---------------------------------------------------------------------- */

static const char *path_of(const char *name)
{
	static char buf[PAL_PATH_MAX];
	CHECK(snprintf(buf, sizeof(buf), "%s/%s", root, name) < PAL_PATH_MAX);
	return buf;
}

static void test_file(void)
{
	char path[PAL_PATH_MAX];
	snprintf(path, sizeof(path), "%s", path_of("users.json"));

	store.count = 1;
	CHECK(users_load(&store, path) == USERS_ERR_NOT_FOUND);
	CHECK(store.count == 0);
	logs[0] = '\0';
	CHECK(users_load_startup(&store, path) == USERS_ERR_NOT_FOUND);
	CHECK(strstr(logs, "warn") != NULL && strstr(logs, "every login is refused") != NULL);

	users_init(&store);
	rnd_next = 0;
	CHECK(users_add(&store, "alice", USER_ROLE_EDITOR, "pencil", 6, 16, fake_random) ==
	      USERS_OK);
	CHECK(users_add(&store, "bob", USER_ROLE_VIEWER, "pw", 2, 16, fake_random) == USERS_OK);
	CHECK(users_save(&store, path) == USERS_OK);
	CHECK(users_load(&store2, path) == USERS_OK);
	CHECK(store2.count == 2);
	CHECK(memcmp(&store, &store2, sizeof(store)) == 0);
	logs[0] = '\0';
	CHECK(users_load_startup(&store2, path) == USERS_OK);
	CHECK(store2.count == 2);
	CHECK(strstr(logs, "2 users") != NULL);
	CHECK(!pal_storage_exists(path_of("users.json.tmp")));

	/* An empty file: no users, a warning. */
	CHECK(pal_storage_write(path, "", 0) == PAL_OK);
	logs[0] = '\0';
	CHECK(users_load_startup(&store2, path) == USERS_OK);
	CHECK(store2.count == 0);
	CHECK(strstr(logs, "holds no users: every login is refused") != NULL);

	/* Malformed: no users, an error, startup goes on. */
	CHECK(pal_storage_write(path, "[{\"user\": 1}]", 13) == PAL_OK);
	logs[0] = '\0';
	CHECK(users_load_startup(&store2, path) == USERS_ERR_MALFORMED);
	CHECK(store2.count == 0);
	CHECK(strstr(logs, "error") != NULL && strstr(logs, "every login is refused") != NULL);

	/* Too large. */
	static char big[USERS_FILE_MAX + 2];
	memset(big, ' ', sizeof(big));
	big[0] = '[';
	big[sizeof(big) - 1] = ']';
	CHECK(pal_storage_write(path, big, sizeof(big)) == PAL_OK);
	logs[0] = '\0';
	CHECK(users_load(&store2, path) == USERS_ERR_MALFORMED);
	CHECK(strstr(logs, "more than") != NULL);

	/* A directory, an unwritable path. */
	CHECK(pal_storage_mkdir(path_of("dir")) == PAL_OK);
	CHECK(users_load(&store2, path_of("dir")) == USERS_ERR_STORAGE);
	CHECK(users_save(&store, path_of("missing/users.json")) == USERS_ERR_STORAGE);
}

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb;
	(void)flag;
	(void)ftw;
	return remove(path);
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(log_add_sink(log_sink, NULL));
	log_set_threshold(LOG_LEVEL_FINE);
	const char *base = getenv("TMPDIR");
	snprintf(root, sizeof(root), "%s/hakkas-users-XXXXXX", base && *base ? base : "/tmp");
	if (mkdtemp(root) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	RUN(test_names_and_roles);
	RUN(test_changes);
	RUN(test_round_trip);
	RUN(test_parse);
	RUN(test_file);

	log_remove_sink(log_sink, NULL);
	if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
	return test_result();
}
