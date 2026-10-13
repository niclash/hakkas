/*
 * POSIX PAL: initialization, the monotonic clock, the wall clock and the
 * random source (01 §0, §3.4; 02 §2.5 item 1). Storage is in storage.c.
 */
#include "platform/pal.h"

#include <errno.h>
#include <sys/random.h>
#include <time.h>

#include "system/log.h"

static uint64_t clock_base_ms;
static bool clock_started;
static uint64_t clock_last_ms;

static bool monotonic_ms(uint64_t *out)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return false;
	*out = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
	return true;
}

/* Latches the engine start time, once. */
static bool clock_start(void)
{
	if (clock_started)
		return true;
	if (!monotonic_ms(&clock_base_ms))
		return false;
	clock_started = true;
	return true;
}

enum pal_err pal_init(void)
{
	if (!clock_start()) {
		log_error("monotonic clock unavailable (errno %d)", errno);
		return PAL_ERR_UNAVAILABLE;
	}
	return PAL_OK;
}

uint64_t pal_uptime_ms(void)
{
	/* Tolerates a call before pal_init: the first call then starts the clock. */
	uint64_t now;
	if (!clock_start() || !monotonic_ms(&now))
		return clock_last_ms; /* cannot happen with CLOCK_MONOTONIC */
	uint64_t uptime = now - clock_base_ms;
	if (uptime > clock_last_ms)
		clock_last_ms = uptime;
	return clock_last_ms;
}

int64_t pal_wall_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return 0; /* cannot happen with CLOCK_REALTIME */
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

enum pal_err pal_random_bytes(void *buf, size_t len)
{
	if (buf == NULL && len > 0)
		return PAL_ERR_INVALID;
	unsigned char *p = buf;
	while (len > 0) {
		/* Flags 0: the urandom pool, blocking only until it is first seeded. */
		ssize_t n = getrandom(p, len, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			log_error("random source failed (errno %d)", errno);
			return PAL_ERR_UNAVAILABLE;
		}
		p += n;
		len -= (size_t)n;
	}
	return PAL_OK;
}
