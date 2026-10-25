/*
 * The POSIX external environment link (platform/posix/env_link.h, 04 §0.1;
 * plan task 22) against a fake environment program inside the test: a
 * thread serving a Unix socket with the link's message set. Through the
 * whole engine in-process (e2e.h): the startup wait for the description
 * (the program comes up after hakkas started waiting), the hello, values
 * sent with the description, no wait for sensor values (processes start
 * from their defaults), sensor values reaching a bound input, actuator writes, the
 * clock; a disconnect (inputs keep their last values, a write made
 * meanwhile), the reconnect with the identical description (actuators
 * re-sent with their latest values, the environment's values resuming);
 * a changed description (no more IO, an error); an invalid description and
 * a stop request during the wait, both ending startup; invalid messages.
 * The retry interval is short; every wait is for a condition, with a
 * deadline.
 */
#include "e2e.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "platform/pal.h"
#include "platform/posix/env_link.h"
#include "system/log.h"

#define RETRY_MS 20
#define DEADLINE_MS 5000

#define DESCRIBE                                                                                   \
	"{\"describe\":{\"name\":\"sim\",\"icon\":\"chip\","                                       \
	"\"outPorts\":[{\"id\":\"ain\",\"type\":\"number\",\"addressable\":true,\"count\":2},"     \
	"{\"id\":\"door\",\"type\":\"bool\"}],"                                                    \
	"\"inPorts\":[{\"id\":\"relay\",\"type\":\"bool\",\"addressable\":true,\"count\":2},"      \
	"{\"id\":\"aout\",\"type\":\"number\"}],"                                                  \
	"\"modules\":[{\"slot\":1,\"type\":\"AIV\",\"serial\":\"9\",\"ports\":[\"ain\"]}]}}"
#define CURRENT                                                                                    \
	"{\"value\":{\"port\":\"ain\",\"index\":1,\"value\":4.5}}\n"                               \
	"{\"value\":{\"port\":\"ain\",\"index\":2,\"value\":0}}\n"                                 \
	"{\"value\":{\"port\":\"door\",\"value\":true}}\n"

/* ---- The fake environment program ------------------------------------------------ */

static struct {
	pthread_mutex_t lock;
	pthread_t thread;
	bool running;
	char path[108];
	int listen_fd; /* -1: not listening */
	/* Guarded by lock. */
	bool stop;
	bool drop;	     /* close the connection */
	bool paused;	     /* accept nothing (connections wait in the backlog) */
	int conn;	     /* -1: none */
	int accepts;
	char describe[1024]; /* the line sent on each accept */
	char current[1024];  /* the lines sent after it */
	char received[16384];
} srv = {.lock = PTHREAD_MUTEX_INITIALIZER, .listen_fd = -1, .conn = -1};

static void send_all(int fd, const char *text)
{
	size_t len = strlen(text);
	while (len > 0) {
		ssize_t n = send(fd, text, len, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return;
		text += n;
		len -= (size_t)n;
	}
}

static void *server_main(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&srv.lock);
		bool stop = srv.stop;
		if (srv.drop && srv.conn >= 0) {
			close(srv.conn);
			srv.conn = -1;
		}
		srv.drop = false;
		int conn = srv.conn;
		bool paused = srv.paused;
		pthread_mutex_unlock(&srv.lock);
		if (stop)
			break;
		if (conn < 0 && paused) {
			struct timespec ts = {0, 1000000};
			nanosleep(&ts, NULL);
			continue;
		}
		struct pollfd p = {conn >= 0 ? conn : srv.listen_fd, POLLIN, 0};
		if (poll(&p, 1, 5) <= 0)
			continue;
		if (conn < 0) {
			int fd = accept(srv.listen_fd, NULL, NULL);
			if (fd < 0)
				continue;
			pthread_mutex_lock(&srv.lock);
			srv.conn = fd;
			srv.accepts++;
			char line[2048];
			snprintf(line, sizeof(line), "%s\n%s", srv.describe, srv.current);
			pthread_mutex_unlock(&srv.lock);
			send_all(fd, line);
			continue;
		}
		char buf[1024];
		ssize_t n = recv(conn, buf, sizeof(buf) - 1, 0);
		pthread_mutex_lock(&srv.lock);
		if (n > 0) {
			buf[n] = '\0';
			size_t len = strlen(srv.received);
			snprintf(srv.received + len, sizeof(srv.received) - len, "%s", buf);
		} else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
			close(srv.conn);
			srv.conn = -1;
		}
		pthread_mutex_unlock(&srv.lock);
	}
	return NULL;
}

/* Listen at srv.path and serve (the first program run). */
static void server_up(const char *describe, const char *current)
{
	pthread_mutex_lock(&srv.lock);
	snprintf(srv.describe, sizeof(srv.describe), "%s", describe);
	snprintf(srv.current, sizeof(srv.current), "%s", current);
	srv.received[0] = '\0';
	srv.accepts = 0;
	srv.stop = false;
	pthread_mutex_unlock(&srv.lock);
	unlink(srv.path);
	srv.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	CHECK(srv.listen_fd >= 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", srv.path);
	CHECK(bind(srv.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	CHECK(listen(srv.listen_fd, 4) == 0);
	CHECK(pthread_create(&srv.thread, NULL, server_main, NULL) == 0);
	srv.running = true;
}

static void server_down(void)
{
	if (!srv.running)
		return;
	pthread_mutex_lock(&srv.lock);
	srv.stop = true;
	pthread_mutex_unlock(&srv.lock);
	pthread_join(srv.thread, NULL);
	srv.running = false;
	if (srv.conn >= 0)
		close(srv.conn);
	srv.conn = -1;
	close(srv.listen_fd);
	srv.listen_fd = -1;
	unlink(srv.path);
}

static void server_send(const char *line)
{
	pthread_mutex_lock(&srv.lock);
	if (srv.conn >= 0)
		send_all(srv.conn, line);
	pthread_mutex_unlock(&srv.lock);
}

static void server_set(const char *describe, const char *current)
{
	pthread_mutex_lock(&srv.lock);
	snprintf(srv.describe, sizeof(srv.describe), "%s", describe);
	snprintf(srv.current, sizeof(srv.current), "%s", current);
	pthread_mutex_unlock(&srv.lock);
}

/* Accept nothing while paused (a program that is restarting). */
static void server_pause(bool paused)
{
	pthread_mutex_lock(&srv.lock);
	srv.paused = paused;
	pthread_mutex_unlock(&srv.lock);
}

/* Close the current connection (the program restarting). */
static void server_drop(void)
{
	pthread_mutex_lock(&srv.lock);
	srv.drop = true;
	pthread_mutex_unlock(&srv.lock);
}

static bool server_received(const char *text)
{
	pthread_mutex_lock(&srv.lock);
	bool found = strstr(srv.received, text) != NULL;
	pthread_mutex_unlock(&srv.lock);
	return found;
}

static int server_accepts(void)
{
	pthread_mutex_lock(&srv.lock);
	int n = srv.accepts;
	pthread_mutex_unlock(&srv.lock);
	return n;
}

static void server_clear(void)
{
	pthread_mutex_lock(&srv.lock);
	srv.received[0] = '\0';
	pthread_mutex_unlock(&srv.lock);
}

/* ---- Waiting ------------------------------------------------------------------------ */

static void nap(void)
{
	struct timespec ts = {0, 1000000}; /* 1 ms, then check again */
	nanosleep(&ts, NULL);
}

/* Wait (the engine not running) until the server got text. */
static bool wait_received(const char *text)
{
	uint64_t end = pal_uptime_ms() + DEADLINE_MS;
	while (!server_received(text)) {
		if (pal_uptime_ms() > end) {
			pthread_mutex_lock(&srv.lock);
			fprintf(stderr, "expected \"%s\" in:\n%s\n", text, srv.received);
			pthread_mutex_unlock(&srv.lock);
			return false;
		}
		nap();
	}
	return true;
}

/* Run the engine until pred holds (e2e_run_until's bound in iterations of at
 * most E2E_TICK_MS). */
static PosixEnvLink *link_;

static bool link_in(void *arg)
{
	return posix_env_link_state(link_) == *(enum posix_env_link_state *)arg;
}

static bool wait_state(enum posix_env_link_state st)
{
	return e2e_run_until(link_in, &st);
}

/* ---- The engine's side --------------------------------------------------------------- */

/* A log sink the server thread may read: whether the link said it waits. */
static pthread_mutex_t waiting_lock = PTHREAD_MUTEX_INITIALIZER;
static bool waiting_logged;

static void waiting_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)level;
	(void)len;
	if (strstr(line, "waiting for environment at ") != NULL) {
		pthread_mutex_lock(&waiting_lock);
		waiting_logged = true;
		pthread_mutex_unlock(&waiting_lock);
	}
}

static bool waiting(void)
{
	pthread_mutex_lock(&waiting_lock);
	bool w = waiting_logged;
	pthread_mutex_unlock(&waiting_lock);
	return w;
}

/* The program, started once the link waits for it (the startup wait). */
static void *late_server(void *arg)
{
	(void)arg;
	uint64_t end = pal_uptime_ms() + DEADLINE_MS;
	while (!waiting() && pal_uptime_ms() < end)
		nap();
	server_up(DESCRIBE, CURRENT);
	return NULL;
}

static bool start_engine(void)
{
	CHECK(posix_env_link_create(srv.path, RETRY_MS, &link_));
	static Config cfg;
	e2e_config(&cfg);
	HakkasParams p = {.environment = posix_env_link_environment(link_),
			  .ephemeral_ports = true};
	bool ok = e2e_start_params(&cfg, &p);
	if (ok)
		CHECK(hakkas_add_tick(e2e_hakkas, posix_env_link_tick, link_));
	return ok;
}

static void stop_engine(void)
{
	if (e2e_hakkas != NULL) {
		hakkas_remove_tick(e2e_hakkas, posix_env_link_tick, link_);
		e2e_stop();
	}
	posix_env_link_destroy(link_);
	link_ = NULL;
}

/* A process: a.out = a.in1 + a.in2 with a.in1 bound to ain[1], a.in2 =
 * 0.5; a.out drives aout; n.out = not door drives relay[2]. */
static void build(void)
{
	Hakkas *h = e2e_hakkas;
	CHECK(hakkas_process_create(h, "p", "P", NULL, NULL, NULL, NULL) == HAKKAS_OK);
	CHECK(hakkas_block_add(h, "p", "a", "math/Add2", NULL, NULL) == HAKKAS_OK);
	CHECK(hakkas_block_add(h, "p", "n", "logic/Not", NULL, NULL) == HAKKAS_OK);
	HakkasValue half = linkable_from_f64(0.5);
	CHECK(hakkas_initial_add(h, "p", "a", "in2", &half, NULL) == HAKKAS_OK);
	const char *path = e2e_path(e2e_root, "bindings.json");
	e2e_put(path, "{\"environment\":\"sim\",\"bindings\":["
		      "{\"block\":\"a\",\"port\":\"in1\",\"env\":\"ain\",\"index\":1},"
		      "{\"block\":\"a\",\"port\":\"out\",\"env\":\"aout\"},"
		      "{\"block\":\"n\",\"port\":\"in\",\"env\":\"door\"},"
		      "{\"block\":\"n\",\"port\":\"out\",\"env\":\"relay\",\"index\":2}]}");
	size_t applied = 0;
	HakkasMessage msg;
	CHECK(hakkas_bindings_apply(h, "p", path, &applied, NULL, &msg) == HAKKAS_OK);
	CHECK(applied == 4);
}

/* For e2e_run_until: the server got arg. */
static bool got(void *arg)
{
	return server_received((const char *)arg);
}

/* ---- Tests ------------------------------------------------------------------------- */

static void test_link(void)
{
	/* Startup waits for the program, which comes up later. */
	pthread_t late;
	CHECK(pthread_create(&late, NULL, late_server, NULL) == 0);
	CHECK(start_engine());
	pthread_join(late, NULL);
	CHECK(waiting());
	CHECK(strstr(e2e_logs(), "environment sim at ") != NULL);
	CHECK(posix_env_link_state(link_) == POSIX_ENV_LINK_CONNECTED);
	CHECK(posix_env_link_connections(link_) == 1);
	CHECK(wait_received("{\"hello\":{\"version\":1}}\n"));
	const HakkasEnvDescription *d = hakkas_environment(e2e_hakkas);
	CHECK(d != NULL && strcmp(d->name, "sim") == 0 && d->module_count == 1);

	/* The current values reach the bindings; the start pushes write. */
	build();
	CHECK(hakkas_process_start(e2e_hakkas, "p", NULL) == HAKKAS_OK);
	CHECK(e2e_run_until(got, "{\"write\":{\"port\":\"aout\",\"value\":5}}\n"));
	CHECK(wait_received("{\"write\":{\"port\":\"relay\",\"index\":2,\"value\":false}}\n"));

	/* Changes, both ways. */
	server_clear();
	server_send("{\"value\":{\"port\":\"ain\",\"index\":1,\"value\":10}}\n"
		    "{\"value\":{\"port\":\"door\",\"value\":false}}\n");
	CHECK(e2e_run_until(got, "{\"write\":{\"port\":\"aout\",\"value\":10.5}}\n"));
	CHECK(wait_received("{\"write\":{\"port\":\"relay\",\"index\":2,\"value\":true}}\n"));

	/* The clock: simulated time, advancing from the last one received. */
	server_send("{\"clock\":{\"wall_ms\":1000000000000}}\n");
	HakkasProcessStatus st;
	for (int i = 0; i < E2E_WAIT_ITERS; i++) {
		CHECK(hakkas_process_stop(e2e_hakkas, "p", NULL) == HAKKAS_OK);
		CHECK(hakkas_process_start(e2e_hakkas, "p", NULL) == HAKKAS_OK);
		CHECK(hakkas_process_status(e2e_hakkas, "p", &st, NULL) == HAKKAS_OK);
		if (st.wall_started_ms >= 1000000000000 && st.wall_started_ms < 1000000060000)
			break;
		e2e_iterate(1);
	}
	CHECK(st.wall_started_ms >= 1000000000000 && st.wall_started_ms < 1000000060000);

	/* Invalid messages are logged and ignored; the link carries on. */
	e2e_logs_clear();
	server_clear();
	server_send("nonsense\n{\"value\":{\"port\":\"relay\",\"index\":1,\"value\":true}}\n"
		    "{\"value\":{\"port\":\"ain\",\"index\":3,\"value\":1}}\n"
		    "{\"value\":{\"port\":\"ain\",\"index\":1,\"value\":\"7\"}}\n"
		    "{\"future\":{}}\n"
		    "{\"value\":{\"port\":\"ain\",\"index\":1,\"value\":20}}\n");
	CHECK(e2e_run_until(got, "{\"write\":{\"port\":\"aout\",\"value\":20.5}}\n"));
	e2e_iterate(2); /* the tick logs what the link thread queued */
	const char *logs = e2e_logs();
	CHECK(strstr(logs, ": not a JSON object: ignored") != NULL);
	CHECK(strstr(logs, ": value: port: expected a sensor port's id: ignored") != NULL);
	CHECK(strstr(logs, ": value: no such channel: ignored") != NULL);
	CHECK(strstr(logs, ": value: value: expected a number or a bool: ignored") != NULL);
	CHECK(posix_env_link_state(link_) == POSIX_ENV_LINK_CONNECTED);

	/* A disconnect: the inputs keep their last values; a write made
	 * meanwhile waits; the link reconnects, re-sends every actuator's
	 * latest value, and the environment's values resume. */
	e2e_logs_clear();
	server_set(DESCRIBE, "{\"value\":{\"port\":\"ain\",\"index\":1,\"value\":20}}\n"
			     "{\"value\":{\"port\":\"door\",\"value\":false}}\n");
	int accepts = server_accepts();
	server_clear();
	server_pause(true);
	server_drop();
	CHECK(wait_state(POSIX_ENV_LINK_DISCONNECTED));
	HakkasValue one = linkable_from_f64(1);
	CHECK(hakkas_initial_add(e2e_hakkas, "p", "a", "in2", &one, NULL) == HAKKAS_OK);
	e2e_iterate(3);
	CHECK(posix_env_link_state(link_) == POSIX_ENV_LINK_DISCONNECTED);
	CHECK(!server_received("write"));
	server_pause(false);
	/* a.in1 kept 20: a.out = 21 is written once reconnected; relay[2]
	 * (n.out = true, door being false) is re-sent with its value. */
	CHECK(e2e_run_until(got, "{\"write\":{\"port\":\"aout\",\"value\":21}}\n"));
	CHECK(wait_received("{\"write\":{\"port\":\"relay\",\"index\":2,\"value\":true}}\n"));
	CHECK(server_accepts() == accepts + 1);
	CHECK(posix_env_link_connections(link_) == 2);
	CHECK(posix_env_link_state(link_) == POSIX_ENV_LINK_CONNECTED);
	CHECK(wait_received("{\"hello\":{\"version\":1}}\n"));
	server_send("{\"value\":{\"port\":\"door\",\"value\":true}}\n");
	CHECK(e2e_run_until(got, "{\"write\":{\"port\":\"relay\",\"index\":2,\"value\":false}}\n"));
	e2e_iterate(2); /* the tick logs what the link thread queued */
	CHECK(strstr(e2e_logs(), " disconnected (closed): sensor inputs keep their last values; "
				 "reconnecting") != NULL);
	CHECK(strstr(e2e_logs(), " reconnected") != NULL);

	/* A changed description: no more IO, until a restart. */
	e2e_logs_clear();
	char changed[1024];
	snprintf(changed, sizeof(changed), "%s", DESCRIBE);
	char *count = strstr(changed, "\"count\":2");
	CHECK(count != NULL);
	if (count != NULL)
		count[8] = '3';
	server_set(changed, "{\"value\":{\"port\":\"ain\",\"index\":1,\"value\":30}}\n");
	server_drop();
	CHECK(wait_state(POSIX_ENV_LINK_FAILED));
	e2e_iterate(2);
	CHECK(strstr(e2e_logs(), " changed its description: no environment IO until hakkas is "
				 "restarted") != NULL);
	server_clear();
	accepts = server_accepts();
	CHECK(hakkas_initial_add(e2e_hakkas, "p", "a", "in2", &one, NULL) == HAKKAS_OK);
	HakkasValue two = linkable_from_f64(2);
	CHECK(hakkas_initial_add(e2e_hakkas, "p", "a", "in2", &two, NULL) == HAKKAS_OK);
	e2e_iterate(5);
	CHECK(server_accepts() == accepts);
	CHECK(!server_received("write"));

	stop_engine();
	server_down();
}

/* No waiting for sensor values (04 §0 rule 2, decided 2026-10-13): an
 * environment that describes itself and sends no value yet. Startup ends at
 * the description; the process starts from its inputs' defaults, the start
 * push writing the actuators from them; a value arriving afterwards
 * propagates normally. */
static void test_no_values(void)
{
	server_up(DESCRIBE, "");
	e2e_logs_clear();
	CHECK(start_engine());
	CHECK(strstr(e2e_logs(), "environment sim at ") != NULL);
	CHECK(strstr(e2e_logs(), "current values") == NULL);
	CHECK(posix_env_link_state(link_) == POSIX_ENV_LINK_CONNECTED);

	/* a.in1 (ain[1]) holds its default 0: a.out = 0 + 0.5; door holds
	 * false: relay[2] = not false. */
	build();
	CHECK(hakkas_process_start(e2e_hakkas, "p", NULL) == HAKKAS_OK);
	HakkasProcessStatus st;
	CHECK(hakkas_process_status(e2e_hakkas, "p", &st, NULL) == HAKKAS_OK);
	CHECK(st.started);
	CHECK(e2e_run_until(got, "{\"write\":{\"port\":\"aout\",\"value\":0.5}}\n"));
	CHECK(wait_received("{\"write\":{\"port\":\"relay\",\"index\":2,\"value\":true}}\n"));

	/* The first sensor values, after the start: they propagate. */
	server_clear();
	server_send("{\"value\":{\"port\":\"ain\",\"index\":1,\"value\":3}}\n"
		    "{\"value\":{\"port\":\"door\",\"value\":true}}\n");
	CHECK(e2e_run_until(got, "{\"write\":{\"port\":\"aout\",\"value\":3.5}}\n"));
	CHECK(wait_received("{\"write\":{\"port\":\"relay\",\"index\":2,\"value\":false}}\n"));

	stop_engine();
	server_down();
}

/* Startup ends without the environment: an invalid description refuses it;
 * a stop request during the wait ends it. */
static void test_startup_failures(void)
{
	server_up("{\"describe\":{\"name\":\"\"}}", "");
	CHECK(!start_engine());
	CHECK(strstr(e2e_logs(), ": environment at ") != NULL &&
	      strstr(e2e_logs(), ": invalid description: name: expected 1 to 64 bytes\n") !=
		      NULL);
	CHECK(!posix_env_link_interrupted(link_));
	e2e_hakkas = NULL;
	stop_engine();
	server_down();

	/* No program at all, and a stop request. */
	pal_request_stop();
	CHECK(!start_engine());
	CHECK(posix_env_link_interrupted(link_));
	CHECK(posix_env_link_state(link_) == POSIX_ENV_LINK_STOPPED);
	CHECK(strstr(e2e_logs(), ": stop requested while waiting for the environment at ") != NULL);
	e2e_hakkas = NULL;
	stop_engine();

	/* A path too long for a Unix socket. */
	char long_path[300];
	memset(long_path, 'x', sizeof(long_path) - 1);
	long_path[sizeof(long_path) - 1] = '\0';
	PosixEnvLink *l = NULL;
	CHECK(!posix_env_link_create(long_path, RETRY_MS, &l) && l == NULL);
}

int main(void)
{
	e2e_setup("env-link");
	CHECK(log_add_sink(waiting_sink, NULL));
	/* A short path: a Unix socket address holds about 100 bytes. */
	snprintf(srv.path, sizeof(srv.path), "/tmp/hakkas-env-%d.sock", (int)getpid());
	RUN(test_link);
	RUN(test_no_values);
	RUN(test_startup_failures);
	log_remove_sink(waiting_sink, NULL);
	e2e_teardown();
	return test_result();
}
