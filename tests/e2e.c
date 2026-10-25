/*
 * End-to-end test helpers (e2e.h): the engine in-process, real WebSocket
 * clients, the SCRAM client side, JSON comparison.
 */
#include "e2e.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"
#include "platform/pal.h"
#include "protocol/auth.h"
#include "protocol/users.h"
#include "system/crypto.h"
#include "system/log.h"

int test_failures;

Hakkas *e2e_hakkas;
char e2e_root[PAL_PATH_MAX], e2e_confdir[PAL_PATH_MAX], e2e_basedir[PAL_PATH_MAX],
	e2e_webdir[PAL_PATH_MAX];

/* ---- Logs ---------------------------------------------------------------------- */

static char logs[256 * 1024];
static size_t logs_len;

static void log_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)level;
	if (logs_len + len + 2 < sizeof(logs)) {
		memcpy(logs + logs_len, line, len);
		logs_len += len;
		logs[logs_len++] = '\n';
		logs[logs_len] = '\0';
	}
}

const char *e2e_logs(void)
{
	return logs;
}

void e2e_logs_clear(void)
{
	logs_len = 0;
	logs[0] = '\0';
}

/* ---- Files --------------------------------------------------------------------- */

const char *e2e_path(const char *dir, const char *name)
{
	static char buf[4][PAL_PATH_MAX];
	static int next;
	char *p = buf[next++ % 4];
	int n = snprintf(p, PAL_PATH_MAX, "%s/%s", dir, name);
	CHECK(n > 0 && n < PAL_PATH_MAX);
	return p;
}

void e2e_put(const char *path, const char *text)
{
	CHECK(pal_storage_write(path, text, strlen(text)) == PAL_OK);
}

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb;
	(void)flag;
	(void)ftw;
	return remove(path);
}

void e2e_setup(const char *name)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(log_add_sink(log_sink, NULL));
	log_set_threshold(LOG_LEVEL_INFO);
	const char *base = getenv("TMPDIR");
	snprintf(e2e_root, sizeof(e2e_root), "%s/hakkas-%s-XXXXXX", base && *base ? base : "/tmp",
		 name);
	if (mkdtemp(e2e_root) == NULL) {
		perror("mkdtemp");
		exit(1);
	}
	snprintf(e2e_confdir, sizeof(e2e_confdir), "%s", e2e_path(e2e_root, "conf"));
	snprintf(e2e_basedir, sizeof(e2e_basedir), "%s", e2e_path(e2e_root, "base"));
	snprintf(e2e_webdir, sizeof(e2e_webdir), "%s", e2e_path(e2e_root, "web"));
	CHECK(pal_storage_mkdir(e2e_confdir) == PAL_OK);
	CHECK(pal_storage_mkdir(e2e_basedir) == PAL_OK);
	CHECK(pal_storage_mkdir(e2e_webdir) == PAL_OK);
	CHECK(pal_storage_mkdir(e2e_path(e2e_basedir, "graphs")) == PAL_OK);
	e2e_put(e2e_path(e2e_webdir, "index.html"), E2E_INDEX_HTML);
	static UserStore users;
	users_init(&users);
	static const struct {
		const char *name;
		enum user_role role;
	} test_users[] = {
		{"alice", USER_ROLE_EDITOR},
		{"bob", USER_ROLE_EDITOR},
		{"oscar", USER_ROLE_OPERATOR},
		{"vera", USER_ROLE_VIEWER},
	};
	for (size_t i = 0; i < sizeof(test_users) / sizeof(test_users[0]); i++)
		CHECK(users_add(&users, test_users[i].name, test_users[i].role, E2E_PASSWORD,
				strlen(E2E_PASSWORD), E2E_ITERATIONS,
				pal_random_bytes) == USERS_OK);
	CHECK(users_save(&users, e2e_path(e2e_confdir, "users.json")) == USERS_OK);
}

void e2e_teardown(void)
{
	log_remove_sink(log_sink, NULL);
	if (nftw(e2e_root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
}

/* ---- Engine -------------------------------------------------------------------- */

void e2e_config(Config *cfg)
{
	ConfigDefaults d = {.confdir = e2e_confdir, .webdir = e2e_webdir, .datadir = e2e_basedir};
	CHECK(config_init(cfg, &d) == CONFIG_OK);
	CHECK(config_load(cfg) == CONFIG_OK);
	CHECK(config_set(cfg, "host", "127.0.0.1", "test") == CONFIG_OK);
	CHECK(config_set(cfg, "log.level", "info", "test") == CONFIG_OK);
}

static uint64_t short_tick(void *ctx, uint64_t now_ms)
{
	(void)ctx;
	(void)now_ms;
	return E2E_TICK_MS;
}

bool e2e_start_params(const Config *cfg, const HakkasParams *params)
{
	e2e_logs_clear();
	bool ok = hakkas_create(cfg, params, &e2e_hakkas);
	CHECK(ok == (e2e_hakkas != NULL));
	if (ok)
		CHECK(hakkas_add_tick(e2e_hakkas, short_tick, NULL));
	return ok;
}

/* The processes to start, in load order (e2e_start_with). */
static char starts[16][HAKKAS_ID_MAX + 1];
static size_t start_count;

static void note_loaded(void *ctx, const HakkasLoaded *l)
{
	(void)ctx;
	CHECK(start_count < sizeof(starts) / sizeof(starts[0]));
	if (l->started && start_count < sizeof(starts) / sizeof(starts[0]))
		memcpy(starts[start_count++], l->id, sizeof(l->id));
}

bool e2e_start_with(const Config *cfg)
{
	HakkasParams p = {.ephemeral_ports = true};
	if (!e2e_start_params(cfg, &p))
		return false;
	start_count = 0;
	hakkas_process_load_saved(e2e_hakkas, note_loaded, NULL, NULL);
	for (size_t i = 0; i < start_count; i++)
		CHECK(hakkas_process_start(e2e_hakkas, starts[i], NULL) == HAKKAS_OK);
	return true;
}

bool e2e_start(void)
{
	static Config cfg;
	e2e_config(&cfg);
	return e2e_start_with(&cfg);
}

void e2e_stop(void)
{
	if (e2e_hakkas == NULL)
		return;
	pal_request_stop();
	hakkas_run(e2e_hakkas);
	hakkas_remove_tick(e2e_hakkas, short_tick, NULL);
	hakkas_destroy(e2e_hakkas);
	e2e_hakkas = NULL;
}

void e2e_iterate(int n)
{
	for (int i = 0; i < n; i++)
		hakkas_run_once(e2e_hakkas);
}

bool e2e_run_until(bool (*pred)(void *arg), void *arg)
{
	for (int i = 0; i < E2E_WAIT_ITERS; i++) {
		if (pred(arg))
			return true;
		hakkas_run_once(e2e_hakkas);
	}
	return pred(arg);
}

/* ---- JSON ---------------------------------------------------------------------- */

char *e2e_q(const char *text)
{
	size_t len = strlen(text);
	char *p = malloc(len + 1);
	CHECK(p != NULL);
	for (size_t i = 0; i <= len; i++)
		p[i] = text[i] == '\'' ? '"' : text[i];
	return p;
}

/* Whether a is like e: the same JSON value, object members in any order,
 * arrays in order, where the string "*" in e matches any value. */
static bool json_like(const cJSON *a, const cJSON *e)
{
	if (cJSON_IsString(e) && strcmp(e->valuestring, "*") == 0)
		return a != NULL;
	if (a == NULL || e == NULL)
		return false;
	if (cJSON_IsObject(e) || cJSON_IsArray(e)) {
		if ((cJSON_IsObject(e) && !cJSON_IsObject(a)) ||
		    (cJSON_IsArray(e) && !cJSON_IsArray(a)) ||
		    cJSON_GetArraySize(a) != cJSON_GetArraySize(e))
			return false;
		const cJSON *ai = a->child;
		for (const cJSON *ei = e->child; ei != NULL; ei = ei->next, ai = ai->next) {
			const cJSON *match = ai;
			if (cJSON_IsObject(e))
				match = cJSON_GetObjectItemCaseSensitive(a, ei->string);
			if (!json_like(match, ei))
				return false;
		}
		return true;
	}
	return cJSON_Compare(a, e, true);
}

bool e2e_json_eq(const char *actual, const char *expected)
{
	char *e = e2e_q(expected);
	cJSON *ja = actual != NULL ? cJSON_Parse(actual) : NULL;
	cJSON *je = cJSON_Parse(e);
	CHECK(je != NULL); /* the test's own JSON */
	bool eq = ja != NULL && je != NULL && json_like(ja, je);
	cJSON_Delete(ja);
	cJSON_Delete(je);
	free(e);
	return eq;
}

const char *e2e_payload_str(const char *message, const char *key)
{
	static char buf[4][256];
	static int next;
	char *p = buf[next++ % 4];
	p[0] = '\0';
	cJSON *m = message != NULL ? cJSON_Parse(message) : NULL;
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(
		cJSON_GetObjectItemCaseSensitive(m, "payload"), key);
	if (cJSON_IsString(v))
		snprintf(p, 256, "%s", v->valuestring);
	else if (cJSON_IsNumber(v))
		snprintf(p, 256, "%.17g", v->valuedouble);
	cJSON_Delete(m);
	return p;
}

/* ---- Sockets ------------------------------------------------------------------- */

bool e2e_tcp_connect(E2eClient *c, uint16_t port, int rcvbuf)
{
	*c = (E2eClient){.fd = -1};
	c->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	CHECK(c->fd >= 0);
	if (c->fd < 0)
		return false;
	if (rcvbuf > 0)
		CHECK(setsockopt(c->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) == 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bool ok = connect(c->fd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
		  fcntl(c->fd, F_SETFL, O_NONBLOCK) == 0;
	CHECK(ok);
	return ok;
}

void e2e_close(E2eClient *c)
{
	if (c->fd >= 0)
		close(c->fd);
	free(c->buf);
	free(c->text);
	free(c->frame);
	*c = (E2eClient){.fd = -1};
}

void e2e_send_bytes(E2eClient *c, const void *data, size_t len)
{
	const uint8_t *p = data;
	int stalls = 0;
	while (len > 0) {
		ssize_t n = send(c->fd, p, len, MSG_NOSIGNAL);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			CHECK(++stalls < E2E_WAIT_ITERS);
			if (stalls >= E2E_WAIT_ITERS)
				return;
			hakkas_run_once(e2e_hakkas);
			continue;
		}
		if (n <= 0)
			return; /* the server closed: the test sees it on receive */
		p += n;
		len -= (size_t)n;
	}
}

/* Reads whatever the socket has. */
static void client_poll(E2eClient *c)
{
	for (;;) {
		if (c->cap - c->len < 65536) {
			c->cap = c->cap * 2 + 65536;
			c->buf = realloc(c->buf, c->cap);
			CHECK(c->buf != NULL);
		}
		ssize_t n = recv(c->fd, c->buf + c->len, c->cap - c->len, 0);
		if (n > 0) {
			c->len += (size_t)n;
			continue;
		}
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			c->eof = true;
		return;
	}
}

static void consume(E2eClient *c, size_t n)
{
	memmove(c->buf, c->buf + n, c->len - n);
	c->len -= n;
}

typedef struct {
	E2eClient *c;
	char *head;
} WantHead;

/* The response head (up to the empty line), taken from the buffer. */
static bool take_head(void *arg)
{
	WantHead *w = arg;
	client_poll(w->c);
	for (size_t i = 0; i + 4 <= w->c->len; i++) {
		if (memcmp(w->c->buf + i, "\r\n\r\n", 4) == 0) {
			size_t hl = i + 4 < 2047 ? i + 4 : 2047;
			memcpy(w->head, w->c->buf, hl);
			w->head[hl] = '\0';
			consume(w->c, i + 4);
			return true;
		}
	}
	return w->c->eof;
}

int e2e_read_head(E2eClient *c, char head[2048])
{
	head[0] = '\0';
	WantHead w = {c, head};
	CHECK(e2e_run_until(take_head, &w));
	int status = 0;
	sscanf(head, "HTTP/1.1 %d", &status);
	return status;
}

/* The upgrade request with this Origin (NULL: none) on a new connection
 * with SO_RCVBUF rcvbuf (0: the default): the response's status. */
static int upgrade(E2eClient *c, const char *origin, int rcvbuf)
{
	if (!e2e_tcp_connect(c, hakkas_ws_port(e2e_hakkas), rcvbuf))
		return 0;
	char req[512], origin_field[256] = "", head[2048];
	if (origin != NULL)
		snprintf(origin_field, sizeof(origin_field), "Origin: %s\r\n", origin);
	snprintf(req, sizeof(req),
		 "GET / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nUpgrade: websocket\r\n"
		 "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		 "Sec-WebSocket-Version: 13\r\n%s\r\n",
		 hakkas_ws_port(e2e_hakkas), origin_field);
	e2e_send_bytes(c, req, strlen(req));
	int status = e2e_read_head(c, head);
	if (status == 101)
		CHECK(strstr(head, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
	return status;
}

int e2e_upgrade(E2eClient *c, const char *origin)
{
	return upgrade(c, origin, 0);
}

bool e2e_connect_rcvbuf(E2eClient *c, int rcvbuf)
{
	char origin[64];
	snprintf(origin, sizeof(origin), "http://127.0.0.1:%u", hakkas_web_port(e2e_hakkas));
	int status = upgrade(c, origin, rcvbuf);
	CHECK(status == 101);
	return status == 101;
}

bool e2e_connect(E2eClient *c)
{
	return e2e_connect_rcvbuf(c, 0);
}

/* ---- Frames -------------------------------------------------------------------- */

size_t e2e_frame(uint8_t *out, enum ws_opcode op, bool fin, const void *payload, size_t len)
{
	size_t p = 0;
	out[p++] = (uint8_t)((fin ? 0x80 : 0) | op);
	if (len < 126) {
		out[p++] = (uint8_t)(0x80 | len);
	} else if (len <= 0xFFFF) {
		out[p++] = 0x80 | 126;
		out[p++] = (uint8_t)(len >> 8);
		out[p++] = (uint8_t)len;
	} else {
		out[p++] = 0x80 | 127;
		for (int i = 7; i >= 0; i--)
			out[p++] = (uint8_t)((uint64_t)len >> (8 * i));
	}
	static const uint8_t key[4] = {0x12, 0x34, 0x56, 0x78};
	memcpy(out + p, key, 4);
	p += 4;
	const uint8_t *in = payload;
	for (size_t i = 0; i < len; i++)
		out[p + i] = in[i] ^ key[i & 3];
	return p + len;
}

void e2e_send_text(E2eClient *c, const char *text)
{
	size_t len = strlen(text);
	uint8_t *f = malloc(len + WS_FRAME_HEADER_MAX);
	CHECK(f != NULL);
	e2e_send_bytes(c, f, e2e_frame(f, WS_OP_TEXT, true, text, len));
	free(f);
}

char *e2e_envelope(const char *protocol, const char *command, const char *payload,
		   const char *secret)
{
	char *pl = e2e_q(payload);
	size_t size = strlen(pl) + strlen(protocol) + strlen(command) + strlen(secret) + 128;
	char *msg = malloc(size);
	CHECK(msg != NULL);
	snprintf(msg, size,
		 "{\"protocol\":\"%s\",\"command\":\"%s\",\"payload\":%s,\"secret\":\"%s\"}",
		 protocol, command, pl, secret);
	free(pl);
	return msg;
}

void e2e_send_secret(E2eClient *c, const char *protocol, const char *command,
		     const char *payload, const char *secret)
{
	char *msg = e2e_envelope(protocol, command, payload, secret);
	e2e_send_text(c, msg);
	free(msg);
}

void e2e_send(E2eClient *c, const char *protocol, const char *command, const char *payload)
{
	e2e_send_secret(c, protocol, command, payload, c->secret);
}

/* ---- Receiving ----------------------------------------------------------------- */

/* 02 §1 "Server → Client": the shape every server message has. */
static void check_shape(const char *text)
{
	cJSON *m = cJSON_Parse(text);
	CHECK(cJSON_IsObject(m));
	const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(m, "protocol");
	const cJSON *command = cJSON_GetObjectItemCaseSensitive(m, "command");
	const cJSON *payload = cJSON_GetObjectItemCaseSensitive(m, "payload");
	CHECK(cJSON_IsString(protocol) && cJSON_IsString(command));
	CHECK(cJSON_GetObjectItemCaseSensitive(m, "secret") == NULL);
	if (cJSON_IsString(protocol) && cJSON_IsString(command)) {
		bool ready = strcmp(protocol->valuestring, "component") == 0 &&
			     strcmp(command->valuestring, "componentsready") == 0;
		bool logout = strcmp(protocol->valuestring, "auth") == 0 &&
			      strcmp(command->valuestring, "logout") == 0;
		if (ready)
			CHECK(cJSON_IsNumber(payload));
		else if (logout)
			CHECK(payload == NULL);
		else
			CHECK(cJSON_IsObject(payload));
	}
	cJSON_Delete(m);
}

/* Takes one whole frame from the buffer: true if there was one (text and
 * close are kept), or the connection ended. */
static bool take_frame(void *arg)
{
	E2eClient *c = arg;
	client_poll(c);
	WsFrameHeader h;
	int hl = c->len >= 2 ? ws_frame_header_parse(c->buf, c->len, &h) : 0;
	if (hl <= 0 || c->len - (size_t)hl < h.len)
		return c->eof;
	CHECK(h.fin && !h.masked && h.rsv == 0);
	size_t n = (size_t)hl + (size_t)h.len;
	free(c->frame);
	c->frame = malloc(n);
	CHECK(c->frame != NULL);
	memcpy(c->frame, c->buf, n);
	c->frame_len = n;
	free(c->text);
	c->text = NULL;
	if (h.opcode == WS_OP_TEXT) {
		c->text = malloc(h.len + 1);
		memcpy(c->text, c->buf + hl, h.len);
		c->text[h.len] = '\0';
	} else if (h.opcode == WS_OP_CLOSE) {
		c->closed = true;
		c->close_code = h.len >= 2 ? (uint16_t)(c->buf[hl] << 8 | c->buf[hl + 1]) : 0;
	} else if (h.opcode == WS_OP_PONG) {
		c->pongs++;
		consume(c, n);
		return take_frame(c); /* not a message: the next frame */
	} else {
		CHECK(h.opcode == WS_OP_TEXT || h.opcode == WS_OP_CLOSE);
	}
	consume(c, n);
	return true;
}

const char *e2e_recv(E2eClient *c)
{
	free(c->text);
	c->text = NULL;
	if (c->closed)
		return NULL;
	if (!e2e_run_until(take_frame, c) || c->text == NULL)
		return NULL;
	check_shape(c->text);
	return c->text;
}

/* Whether msg is protocol:command, silently. */
static bool is_cmd(const char *msg, const char *protocol, const char *command)
{
	cJSON *m = msg != NULL ? cJSON_Parse(msg) : NULL;
	const cJSON *p = cJSON_GetObjectItemCaseSensitive(m, "protocol");
	const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(m, "command");
	bool ok = cJSON_IsString(p) && cJSON_IsString(cmd) &&
		  strcmp(p->valuestring, protocol) == 0 && strcmp(cmd->valuestring, command) == 0;
	cJSON_Delete(m);
	return ok;
}

/* e2e_match with the payload parsed (NULL: any). */
static bool match_json(const char *msg, const char *protocol, const char *command,
		       const cJSON *payload)
{
	cJSON *m = msg != NULL ? cJSON_Parse(msg) : NULL;
	bool ok = is_cmd(msg, protocol, command) &&
		  (payload == NULL ||
		   json_like(cJSON_GetObjectItemCaseSensitive(m, "payload"), payload));
	cJSON_Delete(m);
	if (!ok) {
		char *e = payload != NULL ? cJSON_PrintUnformatted(payload) : NULL;
		fprintf(stderr, "  got:      %s\n  expected: %s:%s %s\n", msg ? msg : "(nothing)",
			protocol, command, e ? e : "(any payload)");
		free(e);
	}
	return ok;
}

bool e2e_match(const char *msg, const char *protocol, const char *command, const char *payload)
{
	cJSON *want = NULL;
	if (payload != NULL) {
		char *e = e2e_q(payload);
		want = cJSON_Parse(e);
		free(e);
		CHECK(want != NULL); /* the test's own JSON */
		if (want == NULL)
			return false;
	}
	bool ok = match_json(msg, protocol, command, want);
	cJSON_Delete(want);
	return ok;
}

bool e2e_expect(E2eClient *c, const char *protocol, const char *command, const char *payload)
{
	return e2e_match(e2e_recv(c), protocol, command, payload);
}

bool e2e_expect_error(E2eClient *c, const char *message, const char *graph)
{
	cJSON *want = cJSON_CreateObject();
	cJSON_AddStringToObject(want, "stack", "");
	cJSON_AddStringToObject(want, "message", message);
	if (graph != NULL)
		cJSON_AddStringToObject(want, "graph", graph);
	bool ok = match_json(e2e_recv(c), "network", "error", want);
	cJSON_Delete(want);
	return ok;
}

/* Sends the barrier request (see e2e_quiet); whether reply is its reply. */
static void send_barrier(E2eClient *c)
{
	if (c->secret[0] != '\0')
		e2e_send(c, "trace", "clear", "{'graph':'e2e-sync'}");
	else
		e2e_send(c, "runtime", "getruntime", "{}");
}

static bool is_barrier(const E2eClient *c, const char *reply)
{
	if (c->secret[0] != '\0')
		return is_cmd(reply, "trace", "clear") &&
		       strcmp(e2e_payload_str(reply, "graph"), "e2e-sync") == 0;
	return is_cmd(reply, "runtime", "runtime") && strstr(reply, "\"capabilities\":[]") != NULL;
}

bool e2e_quiet(E2eClient *c)
{
	send_barrier(c);
	const char *m = e2e_recv(c);
	if (!is_barrier(c, m)) {
		fprintf(stderr, "  expected nothing before the barrier, got: %s\n",
			m ? m : "(nothing)");
		return false;
	}
	return true;
}

int e2e_collect(E2eClient *c, char ***msgs)
{
	*msgs = NULL;
	size_t n = 0, cap = 0;
	send_barrier(c);
	for (;;) {
		const char *m = e2e_recv(c);
		if (m == NULL) {
			fprintf(stderr, "  no barrier reply after %zu messages\n", n);
			e2e_free_msgs(*msgs, (int)n);
			*msgs = NULL;
			return -1;
		}
		if (is_barrier(c, m))
			return (int)n;
		if (n == cap) {
			cap = cap == 0 ? 64 : cap * 2;
			*msgs = realloc(*msgs, cap * sizeof(**msgs));
			CHECK(*msgs != NULL);
		}
		(*msgs)[n++] = c->text;
		c->text = NULL;
	}
}

void e2e_free_msgs(char **msgs, int n)
{
	for (int i = 0; i < n; i++)
		free(msgs[i]);
	free(msgs);
}

bool e2e_silent(E2eClient *c, int n)
{
	for (int i = 0; i < n; i++) {
		hakkas_run_once(e2e_hakkas);
		client_poll(c);
		if (c->len > 0)
			return false;
	}
	return true;
}

bool e2e_expect_close(E2eClient *c, uint16_t code)
{
	while (!c->closed) {
		free(c->text);
		c->text = NULL;
		if (!e2e_run_until(take_frame, c) || (c->text == NULL && !c->closed))
			break;
	}
	if (!c->closed || c->close_code != code) {
		fprintf(stderr, "  close: got %s %u, expected %u\n",
			c->closed ? "code" : "no close frame,", c->close_code, code);
		return false;
	}
	uint8_t f[WS_FRAME_HEADER_MAX + 2];
	uint8_t status[2] = {(uint8_t)(code >> 8), (uint8_t)code};
	e2e_send_bytes(c, f, e2e_frame(f, WS_OP_CLOSE, true, status, 2));
	return e2e_wait_eof(c);
}

static bool eof_pred(void *arg)
{
	E2eClient *c = arg;
	client_poll(c);
	return c->eof;
}

bool e2e_wait_eof(E2eClient *c)
{
	return e2e_run_until(eof_pred, c);
}

/* ---- Login --------------------------------------------------------------------- */

bool e2e_login_password(E2eClient *c, const char *user, const char *password)
{
	char payload[512];
	snprintf(payload, sizeof(payload), "{'user':'%s','cnonce':'MDEyMzQ1Njc4OWFiY2RlZg=='}",
		 user);
	e2e_send_secret(c, "auth", "challenge", payload, "");
	const char *reply = e2e_recv(c);
	if (!e2e_match(reply, "auth", "challenge", NULL))
		return false;
	char salt64[64], nonce[256];
	snprintf(salt64, sizeof(salt64), "%s", e2e_payload_str(reply, "salt"));
	snprintf(nonce, sizeof(nonce), "%s", e2e_payload_str(reply, "nonce"));
	unsigned iterations = (unsigned)strtoul(e2e_payload_str(reply, "iterations"), NULL, 10);
	CHECK(strncmp(nonce, "MDEyMzQ1Njc4OWFiY2RlZg==", 24) == 0 && strlen(nonce) == 48);

	uint8_t salt[16], salted[32], ckey[32], stored[32], skey[32], csig[32], proof[32], ssig[32];
	size_t n = 0;
	CHECK(crypto_base64_decode(salt64, strlen(salt64), salt, sizeof(salt), &n) && n == 16);
	CHECK(crypto_pbkdf2_hmac_sha256(password, strlen(password), salt, 16, iterations, salted,
					32));
	CHECK(crypto_hmac_sha256(salted, 32, "Client Key", 10, ckey));
	CHECK(crypto_sha256(ckey, 32, stored));
	CHECK(crypto_hmac_sha256(salted, 32, "Server Key", 10, skey));
	char am[512];
	snprintf(am, sizeof(am), "%s,%s,%s,%u", user, nonce, salt64, iterations);
	CHECK(crypto_hmac_sha256(stored, 32, am, strlen(am), csig));
	for (int i = 0; i < 32; i++)
		proof[i] = ckey[i] ^ csig[i];
	CHECK(crypto_hmac_sha256(skey, 32, am, strlen(am), ssig));
	char proof64[64], ssig64[64];
	crypto_base64_encode(proof, 32, proof64, sizeof(proof64));
	crypto_base64_encode(ssig, 32, ssig64, sizeof(ssig64));
	snprintf(payload, sizeof(payload), "{'user':'%s','nonce':'%s','proof':'%s'}", user, nonce,
		 proof64);
	e2e_send_secret(c, "auth", "login", payload, "");
	reply = e2e_recv(c);
	if (!is_cmd(reply, "auth", "login"))
		return false;
	snprintf(c->secret, sizeof(c->secret), "%s", e2e_payload_str(reply, "secret"));
	CHECK_STREQ(e2e_payload_str(reply, "server_signature"), ssig64);
	CHECK(strlen(c->secret) == AUTH_SECRET_LEN);
	/* ResumeKey = HMAC(StoredKey, "Resume Key," + nonce). */
	snprintf(c->user, sizeof(c->user), "%s", user);
	snprintf(c->resume, sizeof(c->resume), "%s", e2e_payload_str(reply, "resume"));
	CHECK(strlen(c->resume) == AUTH_RESUME_LEN);
	snprintf(am, sizeof(am), "Resume Key,%s", nonce);
	CHECK(crypto_hmac_sha256(stored, 32, am, strlen(am), c->resume_key));
	return true;
}

bool e2e_resume(E2eClient *c, const E2eClient *from)
{
	char payload[512];
	snprintf(payload, sizeof(payload),
		 "{'user':'%s','cnonce':'MDEyMzQ1Njc4OWFiY2RlZg==','resume':'%s'}", from->user,
		 from->resume);
	e2e_send_secret(c, "auth", "challenge", payload, "");
	const char *reply = e2e_recv(c);
	if (!e2e_match(reply, "auth", "challenge", "{'nonce':'*'}"))
		return false;
	char nonce[256];
	snprintf(nonce, sizeof(nonce), "%s", e2e_payload_str(reply, "nonce"));
	CHECK(strncmp(nonce, "MDEyMzQ1Njc4OWFiY2RlZg==", 24) == 0 && strlen(nonce) == 48);

	char am[512];
	uint8_t mac[32];
	char proof64[64], ssig64[64];
	snprintf(am, sizeof(am), "Server,%s,%s,%s", from->user, nonce, from->resume);
	CHECK(crypto_hmac_sha256(from->resume_key, 32, am + 7, strlen(am + 7), mac));
	crypto_base64_encode(mac, 32, proof64, sizeof(proof64));
	CHECK(crypto_hmac_sha256(from->resume_key, 32, am, strlen(am), mac));
	crypto_base64_encode(mac, 32, ssig64, sizeof(ssig64));
	snprintf(payload, sizeof(payload), "{'user':'%s','nonce':'%s','resume':'%s','proof':'%s'}",
		 from->user, nonce, from->resume, proof64);
	e2e_send_secret(c, "auth", "login", payload, "");
	reply = e2e_recv(c);
	if (!is_cmd(reply, "auth", "login"))
		return false;
	snprintf(c->secret, sizeof(c->secret), "%s", e2e_payload_str(reply, "secret"));
	CHECK_STREQ(e2e_payload_str(reply, "server_signature"), ssig64);
	CHECK(strlen(c->secret) == AUTH_SECRET_LEN);
	char next[40];
	snprintf(next, sizeof(next), "%s", e2e_payload_str(reply, "resume"));
	CHECK(strlen(next) == AUTH_RESUME_LEN && strcmp(next, from->resume) != 0);
	uint8_t key[32];
	memcpy(key, from->resume_key, sizeof(key)); /* from may be c */
	snprintf(c->user, sizeof(c->user), "%s", from->user);
	memcpy(c->resume, next, sizeof(next));
	memcpy(c->resume_key, key, sizeof(key));
	return true;
}

bool e2e_login(E2eClient *c, const char *user)
{
	return e2e_login_password(c, user, E2E_PASSWORD);
}
