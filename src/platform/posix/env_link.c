/*
 * The POSIX external environment link (04 §0.1). See
 * platform/posix/env_link.h.
 */
#include "platform/posix/env_link.h"

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "cJSON.h"
#include "drivers/env_description.h"
#include "graphs/graph_json.h"
#include "platform/pal.h"
#include "system/log.h"

/* The outgoing buffer, and the room a write line needs at most (a port id of
 * HAKKAS_ENV_ID_MAX bytes escaped, a number, the frame). */
#define OUT_CAP (64 * 1024)
#define OUT_LINE_MAX 1024

/* The incoming buffer's first size; it grows up to a line's maximum. */
#define IN_INITIAL 4096

/* Messages the link thread queues for the engine thread's log. */
#define NOTES_MAX 32
#define NOTE_TEXT_MAX 200

typedef struct {
	enum log_level level;
	char text[NOTE_TEXT_MAX];
} Note;

struct PosixEnvLink {
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	uint32_t retry_ms;
	HakkasEnvironment spi;
	HakkasEnvDescription *desc; /* the first description, owned */
	bool desc_invalid;	    /* startup: the description did not validate */
	HakkasEnvSink *sink;	    /* from start */
	int wake;		    /* eventfd: a write is pending, or stop */
	pthread_t thread;
	bool thread_started;

	/* The connection: the engine thread's during startup, then the link
	 * thread's. */
	int fd;
	bool described; /* this connection has described itself */
	char *in;
	size_t in_len, in_cap;
	char *out;
	size_t out_len;
	unsigned invalid; /* invalid messages on this connection */

	/* Sensor channels during startup: per out_port its first slot, the
	 * current values received (04 §0 rule 2), how many are missing. */
	uint32_t *sens_base;
	Linkable *init_value; /* scalars */
	bool *init_known;
	uint32_t sens_count, init_missing;

	/* Actuator channels: per in_port its first slot. */
	uint32_t *act_base;
	uint32_t act_count;

	_Atomic int state;
	_Atomic uint32_t connections;
	atomic_bool interrupted;

	pthread_mutex_t lock;
	/* Guarded by lock. */
	bool stopping;
	Linkable *act_value; /* scalars: they own nothing */
	bool *act_known;
	bool *act_pending;
	uint32_t *pending; /* ring of act_count slots */
	uint32_t pending_head, pending_len;
	bool clock_known;
	int64_t clock_wall_ms;
	uint64_t clock_mono_ms;
	Note notes[NOTES_MAX];
	size_t note_head, note_len;
	uint64_t note_drops;
};

/* ---- Logging ----------------------------------------------------------------------- */

/* Set by the link thread for itself. */
static _Thread_local bool link_thread;

static void note(PosixEnvLink *l, enum log_level level, const char *fmt, ...)
	LOG_PRINTF_FORMAT(3, 4);

/* Log a line: at once on the engine thread, queued on the link thread (which
 * must not log, 05 §4) for posix_env_link_tick. */
static void note(PosixEnvLink *l, enum log_level level, const char *fmt, ...)
{
	char text[NOTE_TEXT_MAX];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	if (!link_thread) {
		log_at(level, "%s", text);
		return;
	}
	pthread_mutex_lock(&l->lock);
	if (l->note_len == NOTES_MAX) {
		l->note_drops++;
	} else {
		Note *n = &l->notes[(l->note_head + l->note_len++) % NOTES_MAX];
		n->level = level;
		memcpy(n->text, text, sizeof(text));
	}
	pthread_mutex_unlock(&l->lock);
}

/* Log what the link thread queued (engine thread). */
static void log_notes(PosixEnvLink *l)
{
	Note notes[NOTES_MAX];
	size_t n = 0;
	uint64_t drops;
	pthread_mutex_lock(&l->lock);
	while (l->note_len > 0) {
		notes[n++] = l->notes[l->note_head];
		l->note_head = (l->note_head + 1) % NOTES_MAX;
		l->note_len--;
	}
	drops = l->note_drops;
	l->note_drops = 0;
	pthread_mutex_unlock(&l->lock);
	for (size_t i = 0; i < n; i++)
		log_at(notes[i].level, "%s", notes[i].text);
	if (drops > 0)
		log_warn("environment at %s: %llu more message%s not logged", l->path,
			 (unsigned long long)drops, drops == 1 ? "" : "s");
}

uint64_t posix_env_link_tick(void *ctx, uint64_t now_ms)
{
	(void)now_ms;
	log_notes(ctx);
	return UINT64_MAX; /* no deadline: the loop runs at least once a second */
}

/* An invalid message: logged up to POSIX_ENV_LINK_INVALID_LOGGED times per
 * connection, then counted. */
static void invalid(PosixEnvLink *l, const char *what)
{
	l->invalid++;
	if (l->invalid <= POSIX_ENV_LINK_INVALID_LOGGED)
		note(l, LOG_LEVEL_WARN, "environment at %s: %s: ignored%s", l->path, what,
		     l->invalid == POSIX_ENV_LINK_INVALID_LOGGED
			     ? " (further invalid messages of this connection are not logged)"
			     : "");
}

/* ---- The connection ---------------------------------------------------------------- */

static void set_state(PosixEnvLink *l, enum posix_env_link_state st)
{
	atomic_store(&l->state, (int)st);
}

/* A new non-blocking connection to the program, or -1. */
static int link_connect(const PosixEnvLink *l)
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, l->path, strlen(l->path) + 1);
	if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void append_out(PosixEnvLink *l, const char *text)
{
	size_t len = strlen(text);
	if (l->out_len + len + 1 > OUT_CAP)
		return; /* never: callers keep OUT_LINE_MAX free */
	memcpy(l->out + l->out_len, text, len);
	l->out[l->out_len + len] = '\n';
	l->out_len += len + 1;
}

/* A fresh connection: empty buffers, the hello first. */
static void connected(PosixEnvLink *l, int fd)
{
	l->fd = fd;
	l->described = false;
	l->invalid = 0;
	l->in_len = 0;
	l->out_len = 0;
	char hello[48];
	snprintf(hello, sizeof(hello), "{\"hello\":{\"version\":%d}}", POSIX_ENV_LINK_VERSION);
	append_out(l, hello);
}

static void close_conn(PosixEnvLink *l)
{
	if (l->fd >= 0)
		close(l->fd);
	l->fd = -1;
	l->described = false;
	l->in_len = 0;
	l->out_len = 0; /* a partial line: every known value is re-sent */
}

/* Write out what the socket takes. False on an error. */
static bool flush_out(PosixEnvLink *l)
{
	while (l->out_len > 0) {
		ssize_t n = send(l->fd, l->out, l->out_len, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return errno == EAGAIN || errno == EWOULDBLOCK;
		}
		memmove(l->out, l->out + n, l->out_len - (size_t)n);
		l->out_len -= (size_t)n;
	}
	return true;
}

/* Read what is there. 1: data (or nothing yet), 0: the end of the stream,
 * -1: an error. */
static int read_in(PosixEnvLink *l)
{
	for (;;) {
		if (l->in_cap - l->in_len < IN_INITIAL / 2) {
			size_t cap = l->in_cap * 2;
			if (cap > POSIX_ENV_LINK_LINE_MAX + IN_INITIAL)
				cap = POSIX_ENV_LINK_LINE_MAX + IN_INITIAL;
			if (cap <= l->in_cap)
				return 1; /* full: take_lines ends the connection */
			char *in = realloc(l->in, cap);
			if (in == NULL)
				return -1;
			l->in = in;
			l->in_cap = cap;
		}
		ssize_t n = recv(l->fd, l->in + l->in_len, l->in_cap - l->in_len, 0);
		if (n > 0) {
			l->in_len += (size_t)n;
			continue;
		}
		if (n == 0)
			return 0;
		if (errno == EINTR)
			continue;
		return errno == EAGAIN || errno == EWOULDBLOCK ? 1 : -1;
	}
}

/* ---- Messages ------------------------------------------------------------------------- */

/* Mark every actuator channel with a known value pending (a reconnect). */
static void resend_all(PosixEnvLink *l)
{
	pthread_mutex_lock(&l->lock);
	for (uint32_t s = 0; s < l->act_count; s++) {
		if (l->act_known[s] && !l->act_pending[s]) {
			l->act_pending[s] = true;
			l->pending[(l->pending_head + l->pending_len++) % l->act_count] = s;
		}
	}
	pthread_mutex_unlock(&l->lock);
}

enum lines_result {
	LINES_MORE,  /* go on */
	LINES_CLOSE, /* end the connection */
};

/* The sensor channels' table for startup's current values. */
static bool alloc_sensors(PosixEnvLink *l)
{
	const HakkasEnvDescription *d = l->desc;
	l->sens_base = calloc(d->out_port_count + 1, sizeof(*l->sens_base));
	if (l->sens_base == NULL)
		return false;
	uint32_t n = 0;
	for (size_t i = 0; i < d->out_port_count; i++) {
		l->sens_base[i] = n;
		n += env_port_channels(&d->out_ports[i]);
	}
	l->sens_count = n;
	l->init_missing = n;
	l->init_value = calloc(n + 1, sizeof(*l->init_value));
	l->init_known = calloc(n + 1, sizeof(*l->init_known));
	return l->init_value != NULL && l->init_known != NULL;
}

static enum lines_result on_describe(PosixEnvLink *l, const cJSON *body)
{
	char why[ENV_DESCRIPTION_MESSAGE_MAX];
	HakkasEnvDescription *d = env_description_from_json(body, why, sizeof(why));
	if (l->desc == NULL) {
		/* Startup (engine thread). */
		if (d == NULL) {
			log_error("environment at %s: invalid description: %s", l->path, why);
			l->desc_invalid = true;
			return LINES_CLOSE;
		}
		l->desc = d;
		l->described = true;
		if (!alloc_sensors(l)) {
			log_error("environment at %s: out of memory", l->path);
			l->desc_invalid = true;
			return LINES_CLOSE;
		}
		return LINES_MORE;
	}
	bool same = d != NULL && env_description_equal(d, l->desc);
	env_description_free(d);
	if (!same) {
		note(l, LOG_LEVEL_ERROR,
		     "environment at %s changed its description: no environment IO until hakkas is "
		     "restarted",
		     l->path);
		set_state(l, POSIX_ENV_LINK_FAILED);
		return LINES_CLOSE;
	}
	if (!l->described) {
		l->described = true;
		atomic_fetch_add(&l->connections, 1);
		set_state(l, POSIX_ENV_LINK_CONNECTED);
		note(l, LOG_LEVEL_WARN, "environment at %s reconnected", l->path);
		resend_all(l);
	}
	return LINES_MORE;
}

static void on_value(PosixEnvLink *l, const cJSON *body)
{
	if (!l->described) {
		invalid(l, "a value before the description");
		return;
	}
	const cJSON *port = cJSON_GetObjectItemCaseSensitive(body, "port");
	const cJSON *index = cJSON_GetObjectItemCaseSensitive(body, "index");
	const cJSON *value = cJSON_GetObjectItemCaseSensitive(body, "value");
	bool actuator;
	uint32_t p;
	if (!cJSON_IsString(port) ||
	    !env_description_find(l->desc, port->valuestring, &actuator, &p) || actuator) {
		invalid(l, "value: port: expected a sensor port's id");
		return;
	}
	double i = 0;
	if (index != NULL && !cJSON_IsNull(index))
		i = cJSON_IsNumber(index) ? index->valuedouble : -1;
	if (!(i >= 0 && i <= HAKKAS_ENV_COUNT_MAX && i == floor(i))) {
		invalid(l, "value: index: expected a channel index");
		return;
	}
	HakkasValue v;
	if (cJSON_IsBool(value))
		v = linkable_from_bool(cJSON_IsTrue(value));
	else if (cJSON_IsNumber(value))
		v = linkable_from_f64(value->valuedouble);
	else {
		invalid(l, "value: value: expected a number or a bool");
		return;
	}
	if (!env_port_has_channel(&l->desc->out_ports[p], (uint32_t)i)) {
		invalid(l, "value: no such channel");
		return;
	}
	if (l->sink != NULL) {
		if (!hakkas_env_sink_push(l->sink, (int)p, (int)i, &v))
			invalid(l, "value: not delivered");
		return;
	}
	/* Startup: the channel's current value, delivered by start. */
	uint32_t slot = l->sens_base[p] + (i > 0 ? (uint32_t)i - 1 : 0);
	if (!l->init_known[slot]) {
		l->init_known[slot] = true;
		l->init_missing--;
	}
	l->init_value[slot] = v;
}

static void on_clock(PosixEnvLink *l, const cJSON *body)
{
	const cJSON *ms = cJSON_GetObjectItemCaseSensitive(body, "wall_ms");
	double d = cJSON_IsNumber(ms) ? ms->valuedouble : NAN;
	if (!(d == floor(d) && fabs(d) < 9007199254740992.0)) {
		invalid(l, "clock: wall_ms: expected an integer");
		return;
	}
	pthread_mutex_lock(&l->lock);
	l->clock_known = true;
	l->clock_wall_ms = (int64_t)d;
	l->clock_mono_ms = pal_uptime_ms();
	pthread_mutex_unlock(&l->lock);
}

static bool blank(const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
			return false;
	}
	return true;
}

static enum lines_result handle_line(PosixEnvLink *l, const char *text, size_t len)
{
	if (blank(text, len))
		return LINES_MORE;
	const char *end = NULL;
	cJSON *msg = cJSON_ParseWithLengthOpts(text, len, &end, false);
	if (!cJSON_IsObject(msg) || !blank(end, (size_t)(text + len - end))) {
		cJSON_Delete(msg);
		invalid(l, "not a JSON object");
		return LINES_MORE;
	}
	enum lines_result r = LINES_MORE;
	const cJSON *body;
	if ((body = cJSON_GetObjectItemCaseSensitive(msg, "describe")) != NULL)
		r = on_describe(l, body);
	else if ((body = cJSON_GetObjectItemCaseSensitive(msg, "value")) != NULL)
		on_value(l, body);
	else if ((body = cJSON_GetObjectItemCaseSensitive(msg, "clock")) != NULL)
		on_clock(l, body);
	/* Anything else: a later protocol's message, ignored. */
	cJSON_Delete(msg);
	return r;
}

/* Handle every complete line of the input. */
static enum lines_result take_lines(PosixEnvLink *l)
{
	size_t start = 0;
	enum lines_result r = LINES_MORE;
	while (r == LINES_MORE) {
		char *nl = memchr(l->in + start, '\n', l->in_len - start);
		if (nl == NULL)
			break;
		size_t len = (size_t)(nl - (l->in + start));
		r = handle_line(l, l->in + start, len);
		start += len + 1;
	}
	if (r == LINES_CLOSE)
		return r;
	memmove(l->in, l->in + start, l->in_len - start);
	l->in_len -= start;
	if (l->in_len > POSIX_ENV_LINK_LINE_MAX) {
		note(l, LOG_LEVEL_WARN, "environment at %s: a line longer than %d bytes", l->path,
		     POSIX_ENV_LINK_LINE_MAX);
		return LINES_CLOSE;
	}
	return r;
}

/* Pending actuator values into the outgoing buffer, as far as it has room. */
static void fill_out(PosixEnvLink *l)
{
	for (;;) {
		if (l->out_len + OUT_LINE_MAX > OUT_CAP)
			return;
		pthread_mutex_lock(&l->lock);
		if (l->pending_len == 0) {
			pthread_mutex_unlock(&l->lock);
			return;
		}
		uint32_t s = l->pending[l->pending_head];
		l->pending_head = (l->pending_head + 1) % l->act_count;
		l->pending_len--;
		l->act_pending[s] = false;
		Linkable v = l->act_value[s];
		pthread_mutex_unlock(&l->lock);

		/* The channel's port: the last one whose first slot is <= s. */
		uint32_t port = 0;
		while (port + 1 < l->desc->in_port_count && l->act_base[port + 1] <= s)
			port++;
		const HakkasEnvPort *p = &l->desc->in_ports[port];
		cJSON *msg = cJSON_CreateObject();
		cJSON *body = cJSON_AddObjectToObject(msg, "write");
		bool ok = body != NULL && cJSON_AddStringToObject(body, "port", p->id) != NULL;
		if (ok && p->addressable)
			ok = cJSON_AddNumberToObject(body, "index", s - l->act_base[port] + 1) !=
			     NULL;
		if (ok) {
			cJSON *val = v.type == LINKABLE_BOOL
					     ? cJSON_CreateBool(v.as.b)
					     : graph_json_number(linkable_to_f64(&v));
			ok = val != NULL && cJSON_AddItemToObject(body, "value", val);
			if (!ok)
				cJSON_Delete(val);
		}
		char *text = ok ? graph_json_print(msg) : NULL;
		cJSON_Delete(msg);
		if (text != NULL && strlen(text) + 1 <= OUT_LINE_MAX)
			append_out(l, text);
		free(text);
	}
}

/* ---- The link thread --------------------------------------------------------------- */

static bool is_stopping(PosixEnvLink *l)
{
	pthread_mutex_lock(&l->lock);
	bool stopping = l->stopping;
	pthread_mutex_unlock(&l->lock);
	return stopping;
}

static void drain_wake(PosixEnvLink *l)
{
	uint64_t n;
	while (read(l->wake, &n, sizeof(n)) == (ssize_t)sizeof(n))
		;
}

/* Wait for a wakeup (a write, stop) at most timeout_ms (-1: no limit). */
static void wait_wake(PosixEnvLink *l, int timeout_ms)
{
	struct pollfd p = {l->wake, POLLIN, 0};
	if (poll(&p, 1, timeout_ms) > 0)
		drain_wake(l);
}

static void disconnected(PosixEnvLink *l, const char *why)
{
	close_conn(l);
	if (atomic_load(&l->state) == POSIX_ENV_LINK_FAILED)
		return;
	set_state(l, POSIX_ENV_LINK_DISCONNECTED);
	note(l, LOG_LEVEL_WARN,
	     "environment at %s disconnected (%s): sensor inputs keep their last values; "
	     "reconnecting",
	     l->path, why);
}

/* One round on a connection: poll, write, read. */
static void exchange(PosixEnvLink *l)
{
	if (l->described)
		fill_out(l);
	struct pollfd p[2] = {
		{l->fd, (short)(POLLIN | (l->out_len > 0 ? POLLOUT : 0)), 0},
		{l->wake, POLLIN, 0},
	};
	if (poll(p, 2, -1) < 0) {
		if (errno != EINTR)
			disconnected(l, "poll failed");
		return;
	}
	if (p[1].revents & POLLIN)
		drain_wake(l);
	if ((p[0].revents & POLLOUT) && !flush_out(l)) {
		disconnected(l, "write failed");
		return;
	}
	if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
		int r = read_in(l);
		/* What arrived before the end of the stream counts. */
		if (take_lines(l) == LINES_CLOSE) {
			disconnected(l, "protocol");
			return;
		}
		if (r <= 0)
			disconnected(l, r == 0 ? "closed" : "read failed");
	}
}

/* At stop: write out what the socket takes of the pending values, without
 * waiting. */
static void final_flush(PosixEnvLink *l)
{
	if (l->fd < 0 || !l->described)
		return;
	for (;;) {
		fill_out(l);
		size_t before = l->out_len;
		if (before == 0 || !flush_out(l) || l->out_len == before)
			return;
	}
}

static void *link_main(void *arg)
{
	PosixEnvLink *l = arg;
	link_thread = true;
	while (!is_stopping(l)) {
		if (l->fd >= 0) {
			exchange(l);
			continue;
		}
		if (atomic_load(&l->state) == POSIX_ENV_LINK_FAILED) {
			wait_wake(l, -1); /* no IO until restart */
			continue;
		}
		int fd = link_connect(l);
		if (fd < 0) {
			wait_wake(l, (int)l->retry_ms);
			continue;
		}
		connected(l, fd);
	}
	final_flush(l);
	return NULL;
}

/* ---- The SPI (engine thread) ----------------------------------------------------- */

static bool alloc_actuators(PosixEnvLink *l)
{
	const HakkasEnvDescription *d = l->desc;
	l->act_base = calloc(d->in_port_count + 1, sizeof(*l->act_base));
	if (l->act_base == NULL)
		return false;
	uint32_t n = 0;
	for (size_t i = 0; i < d->in_port_count; i++) {
		l->act_base[i] = n;
		n += env_port_channels(&d->in_ports[i]);
	}
	l->act_count = n;
	l->act_value = calloc(n + 1, sizeof(*l->act_value));
	l->act_known = calloc(n + 1, sizeof(*l->act_known));
	l->act_pending = calloc(n + 1, sizeof(*l->act_pending));
	l->pending = calloc(n + 1, sizeof(*l->pending));
	return l->act_value != NULL && l->act_known != NULL && l->act_pending != NULL &&
	       l->pending != NULL;
}

/* The startup wait (04 §0.1): false if a stop was requested. */
static bool wait_or_stop(PosixEnvLink *l)
{
	if (pal_stop_requested())
		return false;
	wait_wake(l, (int)l->retry_ms);
	return !pal_stop_requested();
}

/* Forget a startup that did not complete (the connection ended). */
static void reset_startup(PosixEnvLink *l)
{
	close_conn(l);
	env_description_free(l->desc);
	l->desc = NULL;
	free(l->sens_base);
	free(l->init_value);
	free(l->init_known);
	l->sens_base = NULL;
	l->init_value = NULL;
	l->init_known = NULL;
	l->sens_count = 0;
	l->init_missing = 0;
	set_state(l, POSIX_ENV_LINK_WAITING);
}

static const HakkasEnvDescription *interrupted(PosixEnvLink *l)
{
	reset_startup(l);
	atomic_store(&l->interrupted, true);
	set_state(l, POSIX_ENV_LINK_STOPPED);
	log_info("stop requested while waiting for the environment at %s", l->path);
	return NULL;
}

/* The first sensor channel without a current value, "<id>[<index>]" or
 * "<id>". */
static void first_missing(const PosixEnvLink *l, char *buf, size_t size)
{
	buf[0] = '\0';
	for (size_t p = 0; p < l->desc->out_port_count; p++) {
		const HakkasEnvPort *port = &l->desc->out_ports[p];
		for (uint32_t k = 0; k < env_port_channels(port); k++) {
			if (l->init_known[l->sens_base[p] + k])
				continue;
			if (port->addressable)
				snprintf(buf, size, "%s[%u]", port->id, (unsigned)k + 1);
			else
				snprintf(buf, size, "%s", port->id);
			return;
		}
	}
}

/* Startup (04 §0.1): the description, then the current value of every
 * sensor channel. */
static const HakkasEnvDescription *link_describe(void *ctx)
{
	PosixEnvLink *l = ctx;
	if (l->desc != NULL && l->init_missing == 0)
		return l->desc;
	bool waiting = false, waiting_values = false;
	for (;;) {
		if (l->fd < 0) {
			int fd = link_connect(l);
			if (fd < 0) {
				if (!waiting)
					log_warn("waiting for environment at %s", l->path);
				waiting = true;
				if (!wait_or_stop(l))
					return interrupted(l);
				continue;
			}
			connected(l, fd);
		}
		if (l->desc != NULL && l->init_missing == 0)
			break;
		struct pollfd p = {l->fd, (short)(POLLIN | (l->out_len > 0 ? POLLOUT : 0)), 0};
		int n = poll(&p, 1, (int)l->retry_ms);
		if (n <= 0) {
			if (n < 0 && errno != EINTR)
				reset_startup(l);
			if (pal_stop_requested())
				return interrupted(l);
			if (n == 0 && l->desc != NULL && !waiting_values) {
				char first[HAKKAS_ENV_ID_MAX + 16];
				first_missing(l, first, sizeof(first));
				log_warn("environment at %s: waiting for the current values of %u "
					 "sensor channel%s (%s%s)",
					 l->path, l->init_missing, l->init_missing == 1 ? "" : "s",
					 first, l->init_missing > 1 ? ", ..." : "");
				waiting_values = true;
			}
			continue;
		}
		if ((p.revents & POLLOUT) && !flush_out(l)) {
			reset_startup(l);
			continue;
		}
		if (!(p.revents & (POLLIN | POLLHUP | POLLERR)))
			continue;
		int r = read_in(l);
		enum lines_result lr = take_lines(l);
		if (l->desc_invalid) {
			reset_startup(l);
			set_state(l, POSIX_ENV_LINK_STOPPED);
			return NULL; /* logged: refuses startup */
		}
		if (l->desc != NULL && l->init_missing == 0)
			continue; /* complete: done at the top */
		if (lr == LINES_CLOSE || r <= 0) {
			log_warn("environment at %s closed the connection during startup", l->path);
			reset_startup(l);
			if (!wait_or_stop(l))
				return interrupted(l);
		}
	}
	if (!alloc_actuators(l)) {
		log_error("environment at %s: out of memory", l->path);
		reset_startup(l);
		set_state(l, POSIX_ENV_LINK_STOPPED);
		return NULL;
	}
	atomic_store(&l->connections, 1);
	set_state(l, POSIX_ENV_LINK_CONNECTED);
	log_info("environment %s at %s: described, %u sensor value%s", l->desc->name, l->path,
		 l->sens_count, l->sens_count == 1 ? "" : "s");
	return l->desc;
}

static int link_start(void *ctx, HakkasEnvSink *sink)
{
	PosixEnvLink *l = ctx;
	/* Current values first, on the engine thread (hakkas_env.h). */
	for (size_t p = 0; p < l->desc->out_port_count; p++) {
		const HakkasEnvPort *port = &l->desc->out_ports[p];
		for (uint32_t k = 0; k < env_port_channels(port); k++) {
			uint32_t slot = l->sens_base[p] + k;
			if (l->init_known[slot])
				(void)hakkas_env_sink_current(sink, (int)p,
							      port->addressable ? (int)k + 1 : 0,
							      &l->init_value[slot]);
		}
	}
	l->sink = sink;
	int rc = pthread_create(&l->thread, NULL, link_main, l);
	if (rc != 0) {
		log_error("environment at %s: cannot start the link thread: %s", l->path,
			  strerror(rc));
		l->sink = NULL;
		return -1;
	}
	l->thread_started = true;
	return 0;
}

static void wake_link(PosixEnvLink *l)
{
	uint64_t one = 1;
	ssize_t n;
	do
		n = write(l->wake, &one, sizeof(one));
	while (n < 0 && errno == EINTR);
}

static void link_stop(void *ctx)
{
	PosixEnvLink *l = ctx;
	if (l->thread_started) {
		pthread_mutex_lock(&l->lock);
		l->stopping = true;
		pthread_mutex_unlock(&l->lock);
		wake_link(l);
		pthread_join(l->thread, NULL);
		l->thread_started = false;
	}
	close_conn(l);
	l->sink = NULL;
	set_state(l, POSIX_ENV_LINK_STOPPED);
	log_notes(l);
}

static void link_write(void *ctx, int port, int index, const HakkasValue *value)
{
	PosixEnvLink *l = ctx;
	if (l->desc == NULL || port < 0 || (size_t)port >= l->desc->in_port_count || index < 0 ||
	    !env_port_has_channel(&l->desc->in_ports[port], (uint32_t)index))
		return;
	uint32_t s = l->act_base[port] + (index > 0 ? (uint32_t)index - 1 : 0);
	pthread_mutex_lock(&l->lock);
	l->act_value[s] = value->type == LINKABLE_BOOL ? linkable_from_bool(value->as.b)
						       : linkable_from_f64(linkable_to_f64(value));
	l->act_known[s] = true;
	if (!l->act_pending[s]) {
		l->act_pending[s] = true;
		l->pending[(l->pending_head + l->pending_len++) % l->act_count] = s;
	}
	pthread_mutex_unlock(&l->lock);
	wake_link(l);
}

static int64_t link_wall_ms(void *ctx)
{
	PosixEnvLink *l = ctx;
	pthread_mutex_lock(&l->lock);
	bool known = l->clock_known;
	int64_t wall = l->clock_wall_ms;
	uint64_t mono = l->clock_mono_ms;
	pthread_mutex_unlock(&l->lock);
	return known ? wall + (int64_t)(pal_uptime_ms() - mono) : pal_wall_ms();
}

/* ---- Lifetime --------------------------------------------------------------------- */

bool posix_env_link_create(const char *path, uint32_t retry_ms, PosixEnvLink **out)
{
	*out = NULL;
	PosixEnvLink *l = calloc(1, sizeof(*l));
	if (l == NULL) {
		log_error("environment: out of memory");
		return false;
	}
	size_t len = strlen(path);
	if (len == 0 || len >= sizeof(l->path)) {
		log_error("environment.socket: expected a path of 1 to %zu bytes (a Unix socket "
			  "address)",
			  sizeof(l->path) - 1);
		free(l);
		return false;
	}
	memcpy(l->path, path, len + 1);
	l->retry_ms = retry_ms > 0 ? retry_ms : 1;
	l->fd = -1;
	l->wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	l->in = malloc(IN_INITIAL);
	l->in_cap = IN_INITIAL;
	l->out = malloc(OUT_CAP);
	if (l->wake < 0 || l->in == NULL || l->out == NULL ||
	    pthread_mutex_init(&l->lock, NULL) != 0) {
		log_error("environment at %s: cannot create the link: %s", path, strerror(errno));
		if (l->wake >= 0)
			close(l->wake);
		free(l->in);
		free(l->out);
		free(l);
		return false;
	}
	l->spi = (HakkasEnvironment){
		.describe = link_describe,
		.start = link_start,
		.stop = link_stop,
		.write = link_write,
		.wall_ms = link_wall_ms,
		.ctx = l,
	};
	*out = l;
	return true;
}

const HakkasEnvironment *posix_env_link_environment(PosixEnvLink *l)
{
	return &l->spi;
}

bool posix_env_link_interrupted(const PosixEnvLink *l)
{
	return atomic_load(&l->interrupted);
}

enum posix_env_link_state posix_env_link_state(const PosixEnvLink *l)
{
	return (enum posix_env_link_state)atomic_load(&l->state);
}

uint32_t posix_env_link_connections(const PosixEnvLink *l)
{
	return atomic_load(&l->connections);
}

void posix_env_link_destroy(PosixEnvLink *l)
{
	if (l == NULL)
		return;
	if (l->thread_started)
		link_stop(l);
	close_conn(l);
	close(l->wake);
	pthread_mutex_destroy(&l->lock);
	env_description_free(l->desc);
	free(l->sens_base);
	free(l->init_value);
	free(l->init_known);
	free(l->act_base);
	free(l->act_value);
	free(l->act_known);
	free(l->act_pending);
	free(l->pending);
	free(l->in);
	free(l->out);
	free(l);
}
