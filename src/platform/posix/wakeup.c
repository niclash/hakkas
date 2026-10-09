/*
 * POSIX PAL: the event loop's wakeup, the cross-context handoff, the stop
 * request and pal_poll (01 §0.2, §3.2).
 *
 * Decisions (plan task 12):
 *  - The wakeup is one eventfd (Linux, like getrandom; a self-pipe would need
 *    two descriptors). pal_poll always polls it along with the caller's
 *    sockets and reads it to reset it when it fired.
 *  - The handoff is a mutex-protected ring of PAL_HANDOFF_CAPACITY work
 *    entries. A push writes the eventfd only when no wakeup is pending yet
 *    (the flag is cleared by pal_handoff_take), so a burst of pushes costs one
 *    write. A wakeup is never lost: an entry pushed while one is pending is
 *    taken by the take that follows the pending wakeup's poll.
 *  - The stop request is a lock-free atomic flag plus an eventfd write, both
 *    async-signal-safe, so the SIGINT/SIGTERM handlers (signals.c) call
 *    pal_request_stop directly.
 */
#include "platform/pal.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "system/log.h"

_Static_assert(PAL_HANDOFF_CAPACITY >= 1, "PAL_HANDOFF_CAPACITY out of range");

static atomic_int wake_fd = -1;
static atomic_bool stop_flag;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* Guarded by lock. */
static struct {
	bool open;
	bool wake_pending;
	WorkEntry entries[PAL_HANDOFF_CAPACITY];
	size_t head;
	size_t len;
	uint64_t drops;
	uint64_t drops_taken;
} handoff;

/* The pollfd array of pal_poll: the caller's set plus the wakeup. Engine
 * thread only; grows as needed and is freed by pal_loop_close. */
static struct pollfd *fds;
static size_t fds_cap;

static void wake(void)
{
	int fd = atomic_load(&wake_fd);
	if (fd < 0)
		return;
	uint64_t one = 1;
	ssize_t n;
	do {
		n = write(fd, &one, sizeof(one));
	} while (n < 0 && errno == EINTR);
	/* EAGAIN: the counter is saturated, so the wakeup is pending anyway. */
}

enum pal_err pal_loop_open(void)
{
	if (atomic_load(&wake_fd) >= 0)
		return PAL_ERR_INVALID;
	int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (fd < 0) {
		log_error("cannot create the event loop's wakeup (errno %d)", errno);
		return PAL_ERR_UNAVAILABLE;
	}
	pthread_mutex_lock(&lock);
	memset(&handoff, 0, sizeof(handoff));
	handoff.open = true;
	pthread_mutex_unlock(&lock);
	atomic_store(&wake_fd, fd);
	/* A stop requested before the loop opened (e.g. an early SIGINT) still
	 * applies: the flag is kept, and the first poll sees it. */
	if (atomic_load(&stop_flag))
		wake();
	return PAL_OK;
}

void pal_loop_close(void)
{
	pthread_mutex_lock(&lock);
	handoff.open = false;
	while (handoff.len > 0) {
		work_queue_entry_clear(&handoff.entries[handoff.head]);
		handoff.head = (handoff.head + 1) % PAL_HANDOFF_CAPACITY;
		handoff.len--;
	}
	pthread_mutex_unlock(&lock);
	int fd = atomic_exchange(&wake_fd, -1);
	if (fd >= 0)
		close(fd);
	atomic_store(&stop_flag, false);
	free(fds);
	fds = NULL;
	fds_cap = 0;
}

enum pal_err pal_handoff_push(WorkEntry *e)
{
	if (e == NULL || e->type == WORK_NONE)
		return PAL_ERR_INVALID;
	WorkEntry dropped = {.type = WORK_NONE};
	bool need_wake = false;
	pthread_mutex_lock(&lock);
	if (!handoff.open) {
		pthread_mutex_unlock(&lock);
		work_queue_entry_clear(e);
		return PAL_ERR_UNAVAILABLE;
	}
	if (handoff.len == PAL_HANDOFF_CAPACITY) {
		/* Drop the oldest (01 §3.2 item 9); released outside the lock. */
		dropped = handoff.entries[handoff.head];
		handoff.head = (handoff.head + 1) % PAL_HANDOFF_CAPACITY;
		handoff.len--;
		handoff.drops++;
	}
	handoff.entries[(handoff.head + handoff.len) % PAL_HANDOFF_CAPACITY] = *e;
	handoff.len++;
	if (!handoff.wake_pending) {
		handoff.wake_pending = true;
		need_wake = true;
	}
	pthread_mutex_unlock(&lock);
	memset(e, 0, sizeof(*e));
	work_queue_entry_clear(&dropped);
	if (need_wake)
		wake();
	return PAL_OK;
}

size_t pal_handoff_take(WorkEntry *out, size_t max, uint64_t *dropped)
{
	size_t n = 0;
	pthread_mutex_lock(&lock);
	while (n < max && handoff.len > 0) {
		out[n++] = handoff.entries[handoff.head];
		memset(&handoff.entries[handoff.head], 0, sizeof(WorkEntry));
		handoff.head = (handoff.head + 1) % PAL_HANDOFF_CAPACITY;
		handoff.len--;
	}
	/* Entries left over (max reached) are taken by the next call, which the
	 * caller makes before polling again; no new wakeup is needed. */
	handoff.wake_pending = false;
	if (dropped != NULL)
		*dropped = handoff.drops - handoff.drops_taken;
	handoff.drops_taken = handoff.drops;
	pthread_mutex_unlock(&lock);
	return n;
}

uint64_t pal_handoff_drops(void)
{
	pthread_mutex_lock(&lock);
	uint64_t drops = handoff.drops;
	pthread_mutex_unlock(&lock);
	return drops;
}

void pal_request_stop(void)
{
	int saved = errno; /* async-signal-safe: a handler must keep errno */
	atomic_store(&stop_flag, true);
	wake();
	errno = saved;
}

bool pal_stop_requested(void)
{
	return atomic_load(&stop_flag);
}

static short to_poll_events(uint8_t events)
{
	short ev = 0;
	if (events & PAL_POLL_IN)
		ev |= POLLIN;
	if (events & PAL_POLL_OUT)
		ev |= POLLOUT;
	return ev;
}

static uint8_t from_poll_revents(short rev)
{
	uint8_t r = 0;
	if (rev & POLLIN)
		r |= PAL_POLL_IN;
	if (rev & POLLOUT)
		r |= PAL_POLL_OUT;
	if (rev & (POLLERR | POLLHUP | POLLNVAL))
		r |= PAL_POLL_ERR;
	return r;
}

enum pal_err pal_poll(struct pal_pollfd *set, size_t n, uint32_t timeout_ms, size_t *ready)
{
	*ready = 0;
	int wfd = atomic_load(&wake_fd);
	if (wfd < 0 || (set == NULL && n > 0))
		return PAL_ERR_INVALID;
	if (n + 1 > fds_cap) {
		size_t cap = fds_cap == 0 ? 16 : fds_cap;
		while (cap < n + 1)
			cap *= 2;
		struct pollfd *p = realloc(fds, cap * sizeof(*p));
		if (p == NULL)
			return PAL_ERR_UNAVAILABLE;
		fds = p;
		fds_cap = cap;
	}
	for (size_t i = 0; i < n; i++) {
		fds[i].fd = set[i].sock;
		fds[i].events = to_poll_events(set[i].events);
		fds[i].revents = 0;
		set[i].revents = 0;
	}
	fds[n].fd = wfd;
	fds[n].events = POLLIN;
	fds[n].revents = 0;

	int timeout = timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms;
	int rc = poll(fds, (nfds_t)(n + 1), timeout);
	if (rc < 0) {
		/* A signal (e.g. SIGINT's stop request): return so the caller
		 * re-checks; nothing is ready. */
		if (errno == EINTR)
			return PAL_OK;
		return PAL_ERR_IO;
	}
	if (fds[n].revents & POLLIN) {
		uint64_t count;
		ssize_t r;
		do {
			r = read(wfd, &count, sizeof(count));
		} while (r < 0 && errno == EINTR);
	}
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		set[i].revents = from_poll_revents(fds[i].revents);
		if (set[i].revents != 0)
			k++;
	}
	*ready = k;
	return PAL_OK;
}
