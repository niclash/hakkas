/*
 * Tests for the POSIX PAL (01 §0.1): clock, random source, storage. Storage
 * tests run in a fresh temporary directory that is removed afterwards.
 */
#define _XOPEN_SOURCE 700

#include "test.h"

#include <ftw.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "platform/pal.h"

static char root[PAL_PATH_MAX];

/* root + "/" + name. */
static const char *at(const char *name)
{
	static char buf[4][PAL_PATH_MAX];
	static int next;
	char *p = buf[next++ % 4];
	int n = snprintf(p, PAL_PATH_MAX, "%s/%s", root, name);
	CHECK(n > 0 && n < PAL_PATH_MAX);
	return p;
}

static void put(const char *name, const char *text)
{
	CHECK(pal_storage_write(at(name), text, strlen(text)) == PAL_OK);
}

/* Reads name and compares it with expected. */
static bool content_is(const char *name, const char *expected)
{
	char buf[256];
	size_t len = 0;
	if (pal_storage_read(at(name), buf, sizeof(buf), &len) != PAL_OK)
		return false;
	return len == strlen(expected) && memcmp(buf, expected, len) == 0;
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
	(void)st;
	(void)flag;
	(void)ftw;
	return remove(path);
}

static void sleep_ms(long ms)
{
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
	while (nanosleep(&ts, &ts) != 0)
		;
}

static void test_clock(void)
{
	CHECK(pal_init() == PAL_OK);
	uint64_t a = pal_uptime_ms();
	uint64_t b = pal_uptime_ms();
	CHECK(b >= a);
	sleep_ms(30);
	uint64_t c = pal_uptime_ms();
	CHECK(c >= b + 25);
	/* Since pal_init, not since host boot. */
	CHECK(c < 60000);
	/* pal_init again does not restart the clock. */
	CHECK(pal_init() == PAL_OK);
	CHECK(pal_uptime_ms() >= c);
}

static void test_random(void)
{
	unsigned char a[32], b[32], zero[32] = {0};
	memset(a, 0, sizeof(a));
	memset(b, 0, sizeof(b));
	CHECK(pal_random_bytes(a, sizeof(a)) == PAL_OK);
	CHECK(pal_random_bytes(b, sizeof(b)) == PAL_OK);
	CHECK(memcmp(a, zero, sizeof(a)) != 0);
	CHECK(memcmp(a, b, sizeof(a)) != 0);
	CHECK(pal_random_bytes(a, 0) == PAL_OK);
	CHECK(pal_random_bytes(NULL, 0) == PAL_OK);
	CHECK(pal_random_bytes(NULL, 4) == PAL_ERR_INVALID);

	/* A large request is served completely (getrandom may return short). */
	static unsigned char big[100000];
	CHECK(pal_random_bytes(big, sizeof(big)) == PAL_OK);
	size_t zeros = 0;
	for (size_t i = 0; i < sizeof(big); i++)
		zeros += big[i] == 0;
	CHECK(zeros < sizeof(big) / 100);
}

static void test_write_read(void)
{
	put("a.json", "{\"x\":1}");
	CHECK(content_is("a.json", "{\"x\":1}"));

	/* Exact fit, binary content and the empty file. */
	char buf[8];
	size_t len = 0;
	const char bin[3] = {'\0', '\xff', 'z'};
	CHECK(pal_storage_write(at("bin"), bin, sizeof(bin)) == PAL_OK);
	CHECK(pal_storage_read(at("bin"), buf, 3, &len) == PAL_OK);
	CHECK(len == 3 && memcmp(buf, bin, 3) == 0);
	CHECK(pal_storage_write(at("empty"), NULL, 0) == PAL_OK);
	len = 99;
	CHECK(pal_storage_read(at("empty"), buf, sizeof(buf), &len) == PAL_OK);
	CHECK(len == 0);
	CHECK(pal_storage_read(at("empty"), NULL, 0, &len) == PAL_OK);

	struct pal_stat st;
	CHECK(pal_storage_stat(at("a.json"), &st) == PAL_OK);
	CHECK(!st.is_dir && st.size == 7);
	CHECK(pal_storage_stat(root, &st) == PAL_OK);
	CHECK(st.is_dir && st.size == 0);
}

static void test_read_errors(void)
{
	char buf[4];
	size_t len = 0;
	CHECK(pal_storage_read(at("missing"), buf, sizeof(buf), &len) == PAL_ERR_NOT_FOUND);
	CHECK(pal_storage_read(at("nodir/x"), buf, sizeof(buf), &len) == PAL_ERR_NOT_FOUND);

	/* Buffer too small: the file size is reported. */
	put("long", "0123456789");
	CHECK(pal_storage_read(at("long"), buf, sizeof(buf), &len) == PAL_ERR_TOO_BIG);
	CHECK(len == 10);
	CHECK(pal_storage_read(at("long"), buf, 9, &len) == PAL_ERR_TOO_BIG);

	CHECK(pal_storage_read(root, buf, sizeof(buf), &len) == PAL_ERR_INVALID);
	CHECK(pal_storage_read(at("long"), buf, sizeof(buf), NULL) == PAL_ERR_INVALID);
	CHECK(pal_storage_read(NULL, buf, sizeof(buf), &len) == PAL_ERR_INVALID);
	CHECK(pal_storage_read("", buf, sizeof(buf), &len) == PAL_ERR_INVALID);
}

static void test_atomic_write_replaces(void)
{
	put("g.json", "old content, longer than the new one");
	put("g.json", "new");
	CHECK(content_is("g.json", "new"));
	CHECK(!pal_storage_exists(at("g.json.tmp")));

	/* Files are private to the owner (they include the credential store). */
	struct stat st;
	CHECK(stat(at("g.json"), &st) == 0 && (st.st_mode & 0777) == 0600);

	/* A stale .tmp from an earlier crash is overwritten, not appended to. */
	put("h.json.tmp", "stale stale stale");
	put("h.json", "fresh");
	CHECK(content_is("h.json", "fresh"));
	CHECK(!pal_storage_exists(at("h.json.tmp")));
}

static void test_write_errors(void)
{
	/* Missing directory: nothing is created. */
	CHECK(pal_storage_write(at("nodir/x.json"), "x", 1) == PAL_ERR_NOT_FOUND);
	CHECK(!pal_storage_exists(at("nodir")));

	/* Writing over a directory fails and leaves no .tmp behind. */
	CHECK(pal_storage_mkdir(at("adir")) == PAL_OK);
	CHECK(pal_storage_write(at("adir"), "x", 1) == PAL_ERR_INVALID);
	CHECK(pal_storage_exists(at("adir")));
	CHECK(!pal_storage_exists(at("adir.tmp")));

	CHECK(pal_storage_write(at("x"), NULL, 1) == PAL_ERR_INVALID);

	/* The path plus ".tmp" must fit PAL_PATH_MAX. */
	char path[PAL_PATH_MAX + 8];
	memset(path, 'a', sizeof(path) - 1);
	path[sizeof(path) - 1] = '\0';
	CHECK(pal_storage_write(path, "x", 1) == PAL_ERR_NAME_TOO_LONG);
	path[PAL_PATH_MAX - 4] = '\0';
	CHECK(pal_storage_write(path, "x", 1) == PAL_ERR_NAME_TOO_LONG);
	path[PAL_PATH_MAX - 4] = 'a';
	path[PAL_PATH_MAX - 1] = '\0';
	char buf[4];
	size_t len;
	CHECK(pal_storage_read(path, buf, sizeof(buf), &len) == PAL_ERR_NOT_FOUND);
	path[PAL_PATH_MAX - 1] = 'a';
	path[PAL_PATH_MAX] = '\0';
	CHECK(pal_storage_read(path, buf, sizeof(buf), &len) == PAL_ERR_NAME_TOO_LONG);
}

static void test_rename(void)
{
	put("r1", "one");
	put("r2", "two");
	CHECK(pal_storage_rename(at("r1"), at("r2")) == PAL_OK);
	CHECK(!pal_storage_exists(at("r1")));
	CHECK(content_is("r2", "one"));

	CHECK(pal_storage_rename(at("r2"), at("r3")) == PAL_OK);
	CHECK(content_is("r3", "one"));

	/* Into a subdirectory. */
	CHECK(pal_storage_mkdir(at("rsub")) == PAL_OK);
	CHECK(pal_storage_rename(at("r3"), at("rsub/r4")) == PAL_OK);
	CHECK(content_is("rsub/r4", "one"));

	CHECK(pal_storage_rename(at("missing"), at("r5")) == PAL_ERR_NOT_FOUND);
	CHECK(!pal_storage_exists(at("r5")));
	CHECK(pal_storage_rename(at("rsub"), at("r6")) == PAL_ERR_INVALID);
}

static void test_remove_exists(void)
{
	put("del", "x");
	CHECK(pal_storage_exists(at("del")));
	CHECK(pal_storage_remove(at("del")) == PAL_OK);
	CHECK(!pal_storage_exists(at("del")));
	CHECK(pal_storage_remove(at("del")) == PAL_ERR_NOT_FOUND);
	CHECK(pal_storage_stat(at("del"), NULL) == PAL_ERR_NOT_FOUND);

	CHECK(pal_storage_mkdir(at("ddir")) == PAL_OK);
	CHECK(pal_storage_remove(at("ddir")) == PAL_ERR_INVALID);
	CHECK(pal_storage_exists(at("ddir")));
	CHECK(!pal_storage_exists(NULL));
	CHECK(!pal_storage_exists(""));
}

static void test_mkdir(void)
{
	CHECK(pal_storage_mkdir(at("graphs")) == PAL_OK);
	CHECK(pal_storage_mkdir(at("graphs")) == PAL_OK);
	struct pal_stat st;
	CHECK(pal_storage_stat(at("graphs"), &st) == PAL_OK && st.is_dir);
	put("afile", "x");
	CHECK(pal_storage_mkdir(at("afile")) == PAL_ERR_EXISTS);
	CHECK(pal_storage_mkdir(at("no/parent")) == PAL_ERR_NOT_FOUND);
}

struct listing {
	int files, dirs, calls, stop_after;
	bool saw_a, saw_b, saw_sub;
};

static bool collect(void *ctx, const char *name, bool is_dir)
{
	struct listing *l = ctx;
	l->calls++;
	if (is_dir)
		l->dirs++;
	else
		l->files++;
	CHECK(strcmp(name, ".") != 0 && strcmp(name, "..") != 0);
	CHECK(strchr(name, '/') == NULL);
	l->saw_a |= strcmp(name, "a.json") == 0 && !is_dir;
	l->saw_b |= strcmp(name, "b.json") == 0 && !is_dir;
	l->saw_sub |= strcmp(name, "sub") == 0 && is_dir;
	return l->stop_after == 0 || l->calls < l->stop_after;
}

static void test_list(void)
{
	CHECK(pal_storage_mkdir(at("ls")) == PAL_OK);
	CHECK(pal_storage_mkdir(at("ls/sub")) == PAL_OK);
	put("ls/a.json", "a");
	put("ls/b.json", "b");
	put("ls/sub/c.json", "c");

	struct listing l = {0};
	CHECK(pal_storage_list(at("ls"), collect, &l) == PAL_OK);
	CHECK(l.files == 2 && l.dirs == 1);
	CHECK(l.saw_a && l.saw_b && l.saw_sub);

	/* Stopping early. */
	l = (struct listing){.stop_after = 1};
	CHECK(pal_storage_list(at("ls"), collect, &l) == PAL_OK);
	CHECK(l.calls == 1);

	/* Empty directory. */
	CHECK(pal_storage_mkdir(at("ls/sub/empty")) == PAL_OK);
	l = (struct listing){0};
	CHECK(pal_storage_list(at("ls/sub/empty"), collect, &l) == PAL_OK);
	CHECK(l.calls == 0);

	CHECK(pal_storage_list(at("ls/missing"), collect, &l) == PAL_ERR_NOT_FOUND);
	CHECK(pal_storage_list(at("ls/a.json"), collect, &l) == PAL_ERR_NOT_FOUND);
	CHECK(pal_storage_list(at("ls"), NULL, &l) == PAL_ERR_INVALID);
}

static void test_remove_tmp(void)
{
	CHECK(pal_storage_mkdir(at("tmp")) == PAL_OK);
	CHECK(pal_storage_mkdir(at("tmp/sub.tmp")) == PAL_OK);
	put("tmp/a.json", "a");
	put("tmp/a.json.tmp", "torn");
	put("tmp/b.json.tmp", "torn");
	put("tmp/tmp", "keep");
	put("tmp/x.tmpl", "keep");
	put("tmp/sub.tmp/c.tmp", "not recursive");

	size_t removed = 99;
	CHECK(pal_storage_remove_tmp(at("tmp"), &removed) == PAL_OK);
	CHECK(removed == 2);
	CHECK(!pal_storage_exists(at("tmp/a.json.tmp")));
	CHECK(!pal_storage_exists(at("tmp/b.json.tmp")));
	CHECK(content_is("tmp/a.json", "a"));
	CHECK(content_is("tmp/tmp", "keep"));
	CHECK(content_is("tmp/x.tmpl", "keep"));
	CHECK(pal_storage_exists(at("tmp/sub.tmp")));
	CHECK(pal_storage_exists(at("tmp/sub.tmp/c.tmp")));

	CHECK(pal_storage_remove_tmp(at("tmp"), &removed) == PAL_OK);
	CHECK(removed == 0);
	CHECK(pal_storage_remove_tmp(at("tmp"), NULL) == PAL_OK);
	CHECK(pal_storage_remove_tmp(at("tmp/missing"), &removed) == PAL_ERR_NOT_FOUND);
	CHECK(removed == 0);
}

static void test_err_str(void)
{
	CHECK_STREQ(pal_err_str(PAL_OK), "ok");
	CHECK_STREQ(pal_err_str(PAL_ERR_TOO_BIG), "too big");
	CHECK_STREQ(pal_err_str((enum pal_err)1), "?");
}

int main(void)
{
	const char *base = getenv("TMPDIR");
	snprintf(root, sizeof(root), "%s/hakkas-pal-XXXXXX", base && *base ? base : "/tmp");
	if (mkdtemp(root) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	RUN(test_clock);
	RUN(test_random);
	RUN(test_write_read);
	RUN(test_read_errors);
	RUN(test_atomic_write_replaces);
	RUN(test_write_errors);
	RUN(test_rename);
	RUN(test_remove_exists);
	RUN(test_mkdir);
	RUN(test_list);
	RUN(test_remove_tmp);
	RUN(test_err_str);

	if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
	return test_result();
}
