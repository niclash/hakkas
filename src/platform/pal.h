/*
 * The Platform Abstraction Layer interface (01 §0): the only functions through
 * which the portable core reaches the platform. Each port implements them
 * once, under src/platform/<port>/ (POSIX now, Zephyr later). This header is
 * portable: it includes no platform header.
 *
 * Naming: functions are prefixed pal_, the status enum PAL_ / pal_err.
 *
 * Implemented so far: initialization, the monotonic millisecond clock, the
 * cryptographic random source, storage with streaming reads (01 §0.1),
 * non-blocking TCP sockets with a poll call, and the cross-context enqueue
 * with the loop's wakeup and stop request (01 §0.2, plan task 12). Still to
 * come: the IO-provider interface (task 22). There is no timer API (decided,
 * plan task 8, 01 §0): timers are deadlines kept by the core
 * (graphs/deadlines.h), and pal_poll's timeout is the only timing primitive
 * the PAL provides (besides the clock).
 * The event loop built on these is portable core code (engine/loop.h). The
 * console log sink is not declared here: each port registers its own with
 * log_add_sink (POSIX: platform/posix/log_stderr.h).
 *
 * Threading: every function here is engine-thread only (01 §3.2), except
 * pal_handoff_push and pal_request_stop, which any context may call.
 */
#ifndef HAKKAS_PLATFORM_PAL_H
#define HAKKAS_PLATFORM_PAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphs/work_queue.h"

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
	PAL_ERR_UNAVAILABLE = -9,   /* the facility (e.g. the random source) is not available,
				     * or out of resources (descriptors, socket buffers) */
	PAL_ERR_WOULD_BLOCK = -10,  /* a non-blocking socket call cannot proceed now; retry
				     * when pal_poll reports the socket ready */
	PAL_ERR_CLOSED = -11,	    /* the peer closed or reset the connection */
	PAL_ERR_ADDR_IN_USE = -12,  /* the listening address and port are taken */
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
	case PAL_ERR_WOULD_BLOCK:
		return "would block";
	case PAL_ERR_CLOSED:
		return "connection closed";
	case PAL_ERR_ADDR_IN_USE:
		return "address in use";
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

/* The signature of pal_random_bytes, for core code that takes the random
 * source as a parameter so that it links no PAL symbol and tests can pass a
 * fake (system/uuid.h, system/config.h). */
typedef enum pal_err (*pal_random_fn)(void *buf, size_t len);

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

/*
 * Streaming reads (decided, plan task 13): a file too large for one buffer
 * (a web asset, 05 §3) is read in chunks through an open file. The handle is
 * an int on both ports (POSIX: a file descriptor; Zephyr: an index into a
 * small fixed table of fs_file_t, so no heap either); a port bounds how many
 * files are open at once, and an open beyond that is PAL_ERR_UNAVAILABLE.
 * Read-only: every write goes through pal_storage_write.
 */

typedef int pal_file;

#define PAL_FILE_NONE (-1)

/* Opens the regular file at path for reading; *size (if not NULL) receives
 * its size. PAL_ERR_NOT_FOUND if there is none, PAL_ERR_INVALID if path is a
 * directory or another kind of entry, PAL_ERR_UNAVAILABLE when out of
 * descriptors or file slots. Never blocks on a special file (POSIX:
 * O_NONBLOCK on open). Close it with pal_file_close. */
enum pal_err pal_file_open(const char *path, pal_file *out, uint64_t *size);

/* Reads the next at most cap (> 0) bytes into buf, sequentially from the
 * start; *len receives the count, 0 at the end of the file. */
enum pal_err pal_file_read(pal_file f, void *buf, size_t cap, size_t *len);

/* Closes the file. PAL_FILE_NONE is ignored. */
void pal_file_close(pal_file f);

/* ---- Sockets (01 §0.2, 02 §0) -------------------------------------------- */

/*
 * A small non-blocking TCP API for a server: listen, accept, read, write,
 * close, and poll over a set of sockets. Every socket is non-blocking: a call
 * that cannot proceed returns PAL_ERR_WOULD_BLOCK instead of waiting, and the
 * event loop (engine/loop.h) waits for readiness in pal_poll only.
 *
 * A socket is an int descriptor on both ports (POSIX file descriptors,
 * Zephyr zsock_* descriptors). TLS (02 §2.5 item 9) is not part of this API:
 * a TLS layer would wrap a connection's read and write in the event loop.
 *
 * Addresses (decided, plan task 12): a numeric IPv4 ("0.0.0.0", "127.0.0.1")
 * or IPv6 ("::", "::1") literal; host names are not resolved. Listening on
 * "::" also accepts IPv4 where the system allows it (IPV6_V6ONLY off). A peer
 * address is text in its usual form; an IPv4 peer reaching an IPv6 listener
 * is shown as plain IPv4 ("192.0.2.1", not "::ffff:192.0.2.1"), so one client
 * has one address for rate limiting (02 §2.5 item 7).
 */

typedef int pal_socket;

#define PAL_SOCKET_NONE (-1)

/* Longest address text, including the NUL (an IPv6 address). */
#define PAL_ADDR_MAX 46

/* A connection's remote end. */
struct pal_peer {
	char addr[PAL_ADDR_MAX];
	uint16_t port;
};

/* Opens a listening TCP socket on host (a numeric address, see above) and
 * port, with the given accept backlog. Port 0 picks a free ephemeral port
 * (pal_socket_local_port tells which). The address may be reused at once after
 * a restart (POSIX: SO_REUSEADDR). PAL_ERR_INVALID for a host that is not a
 * numeric address, PAL_ERR_ADDR_IN_USE if the port is taken, PAL_ERR_DENIED
 * for a privileged port, PAL_ERR_UNAVAILABLE when out of descriptors or the
 * address family or address is not available on this system. */
enum pal_err pal_socket_listen(const char *host, uint16_t port, int backlog, pal_socket *out);

/* The local port a socket is bound to. */
enum pal_err pal_socket_local_port(pal_socket sock, uint16_t *port);

/* Accepts one pending connection on a listening socket: *out is a new
 * non-blocking socket (POSIX: also TCP_NODELAY, since protocol messages are
 * small and latency matters) and, if peer is not NULL, *peer its remote end.
 * PAL_ERR_WOULD_BLOCK if no connection is pending (also when a pending one
 * was aborted by its peer before it was accepted); PAL_ERR_UNAVAILABLE when
 * out of descriptors or memory. */
enum pal_err pal_socket_accept(pal_socket listener, pal_socket *out, struct pal_peer *peer);

/* Reads at most cap (> 0) bytes into buf; *len receives the count, which is
 * > 0 on PAL_OK. PAL_ERR_WOULD_BLOCK if nothing is available,
 * PAL_ERR_CLOSED (*len 0) at the end of the stream or on a reset. */
enum pal_err pal_socket_read(pal_socket sock, void *buf, size_t cap, size_t *len);

/* Writes up to len bytes of data; *written receives the count, which may be
 * less than len (a partial write) and is > 0 on PAL_OK.
 * PAL_ERR_WOULD_BLOCK if nothing could be written now, PAL_ERR_CLOSED if the
 * peer closed or reset the connection. Never raises a signal (POSIX:
 * MSG_NOSIGNAL instead of a process-wide SIGPIPE setting). */
enum pal_err pal_socket_write(pal_socket sock, const void *data, size_t len, size_t *written);

/* Closes the socket. PAL_SOCKET_NONE is ignored. */
void pal_socket_close(pal_socket sock);

/* Readiness bits of struct pal_pollfd. */
#define PAL_POLL_IN 0x1u  /* readable: data, a pending connection, or the end of stream */
#define PAL_POLL_OUT 0x2u /* writable */
#define PAL_POLL_ERR 0x4u /* error or hang-up; reported whether asked for or not */

struct pal_pollfd {
	pal_socket sock;
	uint8_t events;	 /* PAL_POLL_IN and/or PAL_POLL_OUT to wait for */
	uint8_t revents; /* set by pal_poll */
};

/* Waits until a socket of set (n entries) is ready as asked, the wakeup is
 * signalled (pal_handoff_push, pal_request_stop), or timeout_ms ms have
 * passed (0: do not wait). Sets every entry's revents and *ready to the
 * number of entries with any revents bit. Returns PAL_OK also when it
 * returns early for the wakeup or an interrupting signal (*ready may be 0).
 * The wakeup is consumed by the call: the caller then takes the handed-off
 * entries (pal_handoff_take) and checks pal_stop_requested. Requires
 * pal_loop_open. Engine thread only. */
enum pal_err pal_poll(struct pal_pollfd *set, size_t n, uint32_t timeout_ms, size_t *ready);

/* ---- Cross-context enqueue and the loop's wakeup (01 §0.2, §3.2) ---------- */

/*
 * The handoff is the only way a context other than the engine thread (an IO
 * worker, 04 §0; on POSIX also a signal handler) passes anything to the
 * engine: a bounded, locked FIFO of work entries in the PAL. A push wakes
 * pal_poll; the engine loop then takes the entries, in push order, into the
 * work queue on the engine thread. Locking appears here only; the core stays
 * lock-free (01 §3.2 item 8).
 *
 * Overflow (decided, plan task 12): like the work queue (01 §3.2 item 9), a
 * push onto a full handoff drops the oldest entry, releasing its value, and
 * counts it; the engine loop logs a rate-limited warning.
 */

/* Entries the handoff holds; a port's build may define it. */
#ifndef PAL_HANDOFF_CAPACITY
#define PAL_HANDOFF_CAPACITY 64
#endif

/* Opens the loop's wakeup (POSIX: an eventfd) and the handoff, empty, with no
 * drops counted. Called by loop_init (engine/loop.h), which the startup must
 * reach before any IO worker starts (05 §1.4). PAL_ERR_UNAVAILABLE when out
 * of descriptors; PAL_ERR_INVALID if already open. Engine thread only. */
enum pal_err pal_loop_open(void);

/* Releases the handoff's remaining entries, closes the wakeup and clears a
 * stop request. Every IO worker must have stopped pushing. Engine thread
 * only. */
void pal_loop_close(void);

/* Moves *e (any entry type but WORK_NONE; in practice an IO event) into the
 * handoff and wakes pal_poll; *e is left empty (WORK_NONE). The entry's value
 * must be one no other context holds a reference to (a fresh value):
 * Linkable references are not thread-safe. Returns PAL_OK (also when the
 * oldest entry was dropped to make room), PAL_ERR_INVALID for a WORK_NONE
 * entry, PAL_ERR_UNAVAILABLE if the loop is not open (the entry is then
 * released). Any context may call it; it may block briefly on the
 * handoff's lock, so it is not for interrupt handlers. */
enum pal_err pal_handoff_push(WorkEntry *e);

/* Moves up to max handed-off entries, oldest first, into out and returns
 * how many; the caller owns them. *dropped (if not NULL) receives the number
 * of entries dropped on overflow since the previous call. Engine thread
 * only. */
size_t pal_handoff_take(WorkEntry *out, size_t max, uint64_t *dropped);

/* Entries dropped on overflow since pal_loop_open. Any context. */
uint64_t pal_handoff_drops(void);

/* Asks the engine loop to stop, and wakes pal_poll. Any context; on POSIX
 * async-signal-safe (the SIGINT/SIGTERM handlers call it,
 * platform/posix/signals.h). The request stays set until pal_loop_close, so
 * one made before the loop runs stops it at once. */
void pal_request_stop(void);

/* Whether a stop was requested. */
bool pal_stop_requested(void);

/* ---- IO provider (task 22): to come. ------------------------------------ */

#endif
