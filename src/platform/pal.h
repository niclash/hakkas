/*
 * The Platform Abstraction Layer interface (01 §0): the only functions through
 * which the portable core reaches the platform. Each port implements them
 * once, under src/platform/<port>/ (POSIX now, Zephyr later). This header is
 * portable: it includes no platform header.
 *
 * Naming: functions are prefixed pal_, the status enum PAL_ / pal_err.
 *
 * Implemented so far: initialization, the monotonic millisecond clock, the
 * cryptographic random source and storage (01 §0.1). Still to come, each in
 * its own section below: timers (start/cancel, plan task 8), sockets and the
 * poll-driven event loop (task 12), cross-context enqueue onto the engine
 * queue (task 12), and the IO-provider interface (task 22). The console log
 * sink is not declared here: each port registers its own with log_add_sink
 * (POSIX: platform/posix/log_stderr.h).
 *
 * Threading: every function here is engine-thread only (01 §3.2), except the
 * cross-context enqueue still to come.
 */
#ifndef HAKKAS_PLATFORM_PAL_H
#define HAKKAS_PLATFORM_PAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Status codes ------------------------------------------------------ */

/* Result of a PAL call: PAL_OK, or a negative error code. */
enum pal_err {
	PAL_OK = 0,
	PAL_ERR_INVALID = -1,	    /* bad argument, or a directory where a file was expected */
	PAL_ERR_NOT_FOUND = -2,	    /* the file or a directory on its path does not exist */
	PAL_ERR_EXISTS = -3,	    /* the path exists but is not the kind of entry asked for */
	PAL_ERR_TOO_BIG = -4,	    /* the caller's buffer is too small for the content */
	PAL_ERR_NAME_TOO_LONG = -5, /* the path does not fit PAL_PATH_MAX */
	PAL_ERR_NO_SPACE = -6,	    /* the storage is full */
	PAL_ERR_DENIED = -7,	    /* permission denied or read-only storage */
	PAL_ERR_IO = -8,	    /* any other storage failure */
	PAL_ERR_UNAVAILABLE = -9,   /* the facility (e.g. the random source) is not available */
};

/* A short, static description of a status code, for log lines. */
static inline const char *pal_err_str(enum pal_err err)
{
	switch (err) {
	case PAL_OK:
		return "ok";
	case PAL_ERR_INVALID:
		return "invalid argument";
	case PAL_ERR_NOT_FOUND:
		return "not found";
	case PAL_ERR_EXISTS:
		return "exists";
	case PAL_ERR_TOO_BIG:
		return "too big";
	case PAL_ERR_NAME_TOO_LONG:
		return "name too long";
	case PAL_ERR_NO_SPACE:
		return "no space";
	case PAL_ERR_DENIED:
		return "permission denied";
	case PAL_ERR_IO:
		return "I/O error";
	case PAL_ERR_UNAVAILABLE:
		return "unavailable";
	}
	return "?";
}

/* ---- Initialization ---------------------------------------------------- */

/* Initializes the PAL: clock, random source, storage (05 §1.4 step 2). Call
 * once at startup, after logging is set up and before any other pal_ call.
 * Returns PAL_ERR_UNAVAILABLE if the clock cannot be read. */
enum pal_err pal_init(void);

/* ---- Clock (01 §3.4) ----------------------------------------------------- */

/* Engine uptime: monotonic milliseconds since the engine started, never
 * decreasing and unaffected by wall-clock changes. The engine starts at boot
 * on Zephyr (time since boot) and at pal_init on POSIX (CLOCK_MONOTONIC minus
 * its value at pal_init). */
uint64_t pal_uptime_ms(void);

/* ---- Cryptographic random source (02 §2.5 item 1) ------------------------ */

/* Fills buf with len bytes from the platform's cryptographically secure
 * random generator, for UUIDs, session secrets and the server secret. Returns
 * PAL_ERR_UNAVAILABLE, with buf contents unspecified, if the generator fails;
 * it never falls back to a weaker source. May block until the generator is
 * seeded (early boot only). */
enum pal_err pal_random_bytes(void *buf, size_t len);

/* ---- Storage (01 §0.1, 05 §2.3) ----------------------------------------- */

/*
 * Paths are platform paths, used as given: the core builds them by joining a
 * configured directory (datadir, confdir, or the users file's directory,
 * 05 §1.1) with relative names using '/'. The PAL resolves no root of its
 * own, so "/lfs/graphs/x.json" on Zephyr and "<basedir>/graphs/x.json" on
 * POSIX both reach it unchanged. A path, including the ".tmp" suffix an atomic
 * write appends and the terminating NUL, must fit PAL_PATH_MAX bytes, or the
 * call fails with PAL_ERR_NAME_TOO_LONG.
 *
 * Memory: no storage function allocates on the heap or keeps a pointer
 * to a caller's buffer after it returns. Content is read into, and written
 * from, caller-provided buffers.
 *
 * Only regular files and directories are dealt with; there are no links.
 */

#ifndef PAL_PATH_MAX
#define PAL_PATH_MAX 256
#endif

/* Longest directory entry name, including the terminating NUL (LittleFS's
 * default name limit is 255 bytes). */
#define PAL_NAME_MAX 256

/* Suffix of the temporary file an atomic write goes through (05 §2.3). */
#define PAL_TMP_SUFFIX ".tmp"

struct pal_stat {
	bool is_dir; /* a directory; otherwise a regular file */
	uint64_t size; /* file size in bytes; 0 for a directory */
};

/* Describes the entry at path. info may be NULL to only test existence.
 * Returns PAL_ERR_NOT_FOUND if there is none. */
enum pal_err pal_storage_stat(const char *path, struct pal_stat *info);

/* True if a file or directory exists at path; any error counts as false. */
bool pal_storage_exists(const char *path);

/* Reads the whole file at path into buf (cap bytes) and stores the byte count
 * in *len. The content is raw bytes: nothing is appended, so a caller that
 * wants a NUL-terminated string passes cap one less than its buffer.
 * If the file is larger than cap, returns PAL_ERR_TOO_BIG with *len set to the
 * file size (when known), so the caller can retry with a bigger buffer;
 * buf contents are then unspecified. PAL_ERR_INVALID if path is a directory. */
enum pal_err pal_storage_read(const char *path, void *buf, size_t cap, size_t *len);

/* Atomically replaces the file at path with len bytes of data, by the
 * procedure of 05 §2.3: write <path>.tmp, flush it to stable storage, close
 * it, rename it over path, and (POSIX) sync the directory. After a power loss
 * path holds either the old or the new content, never a torn mix; at worst a
 * leftover <path>.tmp remains, which pal_storage_remove_tmp deletes at
 * startup. This is the only way to write a file: there is no non-atomic
 * write. The directory must exist. On any failure before the rename, the old
 * file is untouched and the .tmp file is removed; a failure to sync the
 * directory after the rename returns PAL_ERR_IO although path already holds
 * the new content. */
enum pal_err pal_storage_write(const char *path, const void *data, size_t len);

/* Renames the file from to to, replacing an existing file at to; atomic, and
 * durable on return (both directories synced on POSIX). Both paths must be
 * on the same storage. PAL_ERR_NOT_FOUND if from does not exist. */
enum pal_err pal_storage_rename(const char *from, const char *to);

/* Removes the file at path. PAL_ERR_NOT_FOUND if there is none,
 * PAL_ERR_INVALID if path is a directory. */
enum pal_err pal_storage_remove(const char *path);

/* Creates the directory at path if it does not exist; its parent must exist
 * (not recursive). PAL_OK if path already is a directory, PAL_ERR_EXISTS if
 * it is a file. Used for <datadir>/graphs and <datadir>/types (05 §2.3). */
enum pal_err pal_storage_mkdir(const char *path);

/* Called once per entry by pal_storage_list. name (no directory part, at most
 * PAL_NAME_MAX - 1 bytes) is only valid during the call. Return true to
 * continue, false to stop listing. */
typedef bool (*pal_storage_list_fn)(void *ctx, const char *name, bool is_dir);

/* Calls fn for each file and subdirectory directly in dir, in unspecified
 * order, without "." and "..", and without building a list in memory. fn must
 * not create, rename or remove entries in dir. Returns PAL_OK also when fn
 * stopped the listing early, PAL_ERR_NOT_FOUND if dir does not exist. */
enum pal_err pal_storage_list(const char *dir, pal_storage_list_fn fn, void *ctx);

/* Deletes every file in dir whose name ends in ".tmp": the leftovers of
 * atomic writes interrupted by a power loss (05 §1.4 step 2). Not recursive.
 * If removed is not NULL, it receives the number of files deleted (also on
 * error, the number deleted until then). PAL_ERR_NOT_FOUND if dir does not
 * exist. */
enum pal_err pal_storage_remove_tmp(const char *dir, size_t *removed);

/* ---- Timers (plan task 8), sockets and event loop, cross-context enqueue
 * (task 12), IO provider (task 22): to come. ------------------------------ */

#endif
