/*
 * End-to-end test helpers (plan task 20, 05 §5): the whole engine started
 * in-process over the POSIX PAL (engine/engine.h, on ephemeral loopback
 * ports), and real WebSocket clients on non-blocking sockets that drive it
 * with loop_run_once while they wait.
 *
 *	e2e_setup("catalog");		temp confdir/basedir/webdir, test users
 *	e2e_start();			run daemon over that basedir
 *	E2eClient a;
 *	e2e_connect(&a);		the upgrade, with the own Origin
 *	e2e_login(&a, "alice");		the SCRAM login of 02 §2.3
 *	e2e_send(&a, "graph", "connect", "{'id':'g1'}");
 *	EXPECT(&a, "graph", "clear", "{'id':'g1', ...}");
 *	QUIET(&a);			nothing else was sent to a
 *	e2e_close(&a);
 *	e2e_stop();
 *	e2e_teardown();
 *
 * JSON written in a test uses ' for " (payloads, expected messages): no test
 * string needs a real '. Comparing JSON parses both sides: object members
 * in any order, arrays exactly; in an expected value the string '*' matches
 * any value (a time, a generated id), but the member must be there.
 *
 * Waiting is never a wall-clock sleep: a wait runs loop iterations until
 * its condition holds, at most E2E_WAIT_ITERS of them. A tick hook bounds
 * every poll to E2E_TICK_MS, so a wait that fails ends after a few seconds;
 * one that succeeds usually takes one or two iterations (the loop wakes as
 * soon as a client's bytes arrive, and what an iteration writes is in the
 * client's socket when it returns: loopback). Silence is checked with a
 * barrier (e2e_quiet, e2e_collect): a request whose reply must be the next
 * message, since the engine keeps each connection's messages in order; or,
 * where no request may be sent, by running iterations (e2e_silent).
 *
 * The test users (02 §2.2, provisioned in <confdir>/users.json with a low
 * iteration count so a login is fast): alice and bob (editor), oscar
 * (operator), vera (viewer), all with the password E2E_PASSWORD.
 *
 * Checks inside the helpers count in the test's failures (test.h,
 * TEST_SHARED_FAILURES): include this header instead of test.h.
 */
#ifndef HAKKAS_TESTS_E2E_H
#define HAKKAS_TESTS_E2E_H

#define TEST_SHARED_FAILURES
#include "test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine/engine.h"
#include "protocol/websocket.h"
#include "system/config.h"

#define E2E_WAIT_ITERS 500 /* loop iterations a wait may take */
#define E2E_TICK_MS 10	   /* the longest poll of one iteration */
#define E2E_PASSWORD "pencil"
#define E2E_ITERATIONS 64 /* the test users' PBKDF2 iterations */
/* <webdir>/index.html */
#define E2E_INDEX_HTML "<!doctype html><title>hakkas e2e</title>\n"

/* The running engine (NULL when stopped) and the test's directories. */
extern Engine *e2e_engine;
extern char e2e_root[], e2e_confdir[], e2e_basedir[], e2e_webdir[];

/* ---- Engine -------------------------------------------------------------------- */

/* pal_init, a log sink, a fresh temporary directory named after the test
 * with conf/, base/ (with graphs/), web/ (an index.html) and the test users. */
void e2e_setup(const char *name);

/* Removes the temporary directory and the log sink. */
void e2e_teardown(void);

/* The configuration as the CLI builds it over the test's directories: the
 * defaults, <confdir>/hakkas.json, host 127.0.0.1, log level info. */
void e2e_config(Config *cfg);

/* engine_start with cfg and params, then the tick hook; the captured log
 * cleared first. */
bool e2e_start_params(const Config *cfg, const EngineParams *params);

/* e2e_start_params on ephemeral ports in the mode (files for
 * ENGINE_LOAD_LIST). */
bool e2e_start_with(const Config *cfg, enum engine_load load, const char *files);

/* e2e_start_with the configuration of e2e_config, `run daemon`. */
bool e2e_start(void);

/* A stop request, engine_run, engine_stop. */
void e2e_stop(void);

/* n loop iterations. */
void e2e_iterate(int n);

/* Runs iterations until pred(arg), at most E2E_WAIT_ITERS; its result. */
bool e2e_run_until(bool (*pred)(void *arg), void *arg);

/* What the engine logged since the last start or e2e_logs_clear. */
const char *e2e_logs(void);
void e2e_logs_clear(void);

/* "<dir>/<name>" in a rotating buffer (4 deep). */
const char *e2e_path(const char *dir, const char *name);

/* Writes text to path (PAL storage). */
void e2e_put(const char *path, const char *text);

/* ---- JSON ---------------------------------------------------------------------- */

/* text with every ' turned into ": malloc'd, the caller frees. */
char *e2e_q(const char *text);

/* Whether actual and expected (with ', and '*' for any value) are the same
 * JSON value. */
bool e2e_json_eq(const char *actual, const char *expected);

/* A string or number member of a message's payload, as text ("" if
 * absent), in a rotating buffer (4 deep). */
const char *e2e_payload_str(const char *message, const char *key);

/* ---- Clients ------------------------------------------------------------------- */

typedef struct {
	int fd;
	uint8_t *buf; /* received, not consumed yet */
	size_t len, cap;
	bool eof;	     /* the server closed the TCP connection */
	bool closed;	     /* a close frame came */
	uint16_t close_code; /* its status code (0: none) */
	unsigned pongs;	     /* pong frames received (skipped by e2e_recv) */
	char secret[64];     /* the session's secret after e2e_login ("" before) */
	char *text;	     /* the last message received (e2e_recv) */
	uint8_t *frame;	     /* its whole frame, header included */
	size_t frame_len;
} E2eClient;

/* A TCP connection to port on 127.0.0.1, non-blocking; rcvbuf, if not 0,
 * is set as SO_RCVBUF before connecting (a small one fills quickly). */
bool e2e_tcp_connect(E2eClient *c, uint16_t port, int rcvbuf);

/* An HTTP response head (up to the empty line) into head, taken from the
 * received bytes (the body stays in c->buf): its status code (0: none). */
int e2e_read_head(E2eClient *c, char head[2048]);

/* The upgrade to the engine's WebSocket port with this Origin header (NULL:
 * none): the response's status code. */
int e2e_upgrade(E2eClient *c, const char *origin);

/* An upgraded WebSocket with the own Origin (02 §0); false (checked) if the
 * upgrade failed. e2e_connect_rcvbuf with a small SO_RCVBUF. */
bool e2e_connect(E2eClient *c);
bool e2e_connect_rcvbuf(E2eClient *c, int rcvbuf);

/* Closes the socket (without a close frame) and frees the client. */
void e2e_close(E2eClient *c);

/* Raw bytes, as one or more send calls (running the loop while the socket
 * is full). */
void e2e_send_bytes(E2eClient *c, const void *data, size_t len);

/* A masked client frame of len payload bytes into out (len +
 * WS_FRAME_HEADER_MAX bytes): its size. */
size_t e2e_frame(uint8_t *out, enum ws_opcode op, bool fin, const void *payload, size_t len);

/* One masked text frame. */
void e2e_send_text(E2eClient *c, const char *text);

/* A command envelope (02 §1) with payload (' for ") and secret: malloc'd. */
char *e2e_envelope(const char *protocol, const char *command, const char *payload,
		   const char *secret);

/* Sends a command with the client's secret, or with an explicit one. */
void e2e_send(E2eClient *c, const char *protocol, const char *command, const char *payload);
void e2e_send_secret(E2eClient *c, const char *protocol, const char *command,
		     const char *payload, const char *secret);

/* The next text message (also in c->text, its frame in c->frame), valid
 * until the next call; NULL if a close frame came (c->closed, close_code),
 * the connection ended or nothing came within E2E_WAIT_ITERS iterations.
 * Every message is checked against 02 §1: one JSON object with protocol,
 * command and an object payload (except the two special shapes), never a
 * secret. */
const char *e2e_recv(E2eClient *c);

/* Whether msg is protocol:command with this payload (' for ", '*' for any
 * value; NULL: any payload); prints both on a mismatch. */
bool e2e_match(const char *msg, const char *protocol, const char *command, const char *payload);

/* The next message is protocol:command with this payload (NULL: any). */
bool e2e_expect(E2eClient *c, const char *protocol, const char *command, const char *payload);

/* The next message is the error of 02 §1 with message, and "graph" if graph
 * is not NULL (absent if NULL). */
bool e2e_expect_error(E2eClient *c, const char *message, const char *graph);

/* No message is waiting: a barrier request (trace:clear with the graph id
 * "e2e-sync" once logged in, runtime:getruntime before) whose reply must be
 * the next message. The engine is single-threaded and keeps each
 * connection's messages in order, so anything sent to c before the barrier
 * shows up first. */
bool e2e_quiet(E2eClient *c);

/* Every message before the barrier reply of e2e_quiet: their count, the
 * texts in *msgs (e2e_free_msgs); -1 (NULL) if the barrier reply did not
 * come. */
int e2e_collect(E2eClient *c, char ***msgs);
void e2e_free_msgs(char **msgs, int n);

/* Nothing arrives during n loop iterations (c only polled). */
bool e2e_silent(E2eClient *c, int n);

/* A close frame with code arrives (other messages before it are skipped),
 * the client answers it, and the server closes the TCP connection. */
bool e2e_expect_close(E2eClient *c, uint16_t code);

/* The server closed the TCP connection (within E2E_WAIT_ITERS); what came
 * meanwhile is in c->buf. */
bool e2e_wait_eof(E2eClient *c);

/* The SCRAM login of 02 §2.3 as user with password: true with c->secret
 * set (the server signature checked); false with the reply in c->text. */
bool e2e_login_password(E2eClient *c, const char *user, const char *password);

/* e2e_login_password with E2E_PASSWORD. */
bool e2e_login(E2eClient *c, const char *user);

/* ---- Checks with the caller's line ------------------------------------------------ */

#define EXPECT(c, protocol, command, payload) CHECK(e2e_expect(c, protocol, command, payload))
#define EXPECT_ERROR(c, message, graph)	      CHECK(e2e_expect_error(c, message, graph))
#define QUIET(c)			      CHECK(e2e_quiet(c))

#endif
