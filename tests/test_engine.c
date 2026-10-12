/*
 * Engine startup and shutdown (engine/engine.h, 05 §1.4) over the POSIX PAL,
 * in-process, in a fresh temporary confdir/basedir/webdir with a
 * provisioned test user (a low iteration count, so logins are fast) and a
 * saved, started graph. The engine listens on ephemeral loopback ports (the
 * test-only EngineParams.ephemeral_ports); a small client checks over real
 * sockets that the web server serves index.html, that the WebSocket accepts
 * the own Origin, answers runtime:getruntime before login, takes a full
 * SCRAM login and answers graph:connect with the loaded graph's snapshot.
 * Then a stop request ends engine_run, and no descriptor is left open. The
 * startup failures: a taken port (web and WebSocket), tls.enabled, a missing
 * datadir or confdir, a broken graph file in process mode (refused) and
 * daemon mode (skipped). Waiting is always for a condition, with a generous
 * deadline; a tick hook bounds every poll to 10 ms.
 */
#include "test.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"
#include "engine/engine.h"
#include "protocol/auth.h"
#include "protocol/users.h"
#include "protocol/websocket.h"
#include "system/crypto.h"
#include "system/log.h"

#define WAIT_MS 5000
#define INDEX_HTML "<!doctype html><title>hakkas test</title>\n"
#define GOOD_GRAPH                                                                                 \
	"{\"id\":\"g1\",\"name\":\"Test graph\",\"started\":true,"                                 \
	"\"blocks\":{\"a\":{\"component\":\"math/Add2\",\"metadata\":{\"x\":10,\"y\":20}}},"      \
	"\"connections\":[{\"src\":{\"data\":2},\"tgt\":{\"node\":\"a\",\"port\":\"in1\"}}]}"

static char root[PAL_PATH_MAX];
static char confdir[PAL_PATH_MAX], basedir[PAL_PATH_MAX], webdir[PAL_PATH_MAX];
static Engine *engine;

/* ---- Helpers ------------------------------------------------------------------ */

static char logs[65536];

static void log_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)level;
	size_t used = strlen(logs);
	if (used + len + 2 < sizeof(logs)) {
		memcpy(logs + used, line, len);
		logs[used + len] = '\n';
		logs[used + len + 1] = '\0';
	}
}

static const char *join(const char *dir, const char *name)
{
	static char buf[4][PAL_PATH_MAX];
	static int next;
	char *p = buf[next++ % 4];
	int n = snprintf(p, PAL_PATH_MAX, "%s/%s", dir, name);
	CHECK(n > 0 && n < PAL_PATH_MAX);
	return p;
}

static void put(const char *path, const char *text)
{
	CHECK(pal_storage_write(path, text, strlen(text)) == PAL_OK);
}

/* Open descriptors of this process. */
static int fd_count(void)
{
	DIR *d = opendir("/proc/self/fd");
	CHECK(d != NULL);
	if (d == NULL)
		return -1;
	int n = 0;
	while (readdir(d) != NULL)
		n++;
	closedir(d);
	return n;
}

/* The configuration as the CLI builds it: defaults, the file, host. */
static void make_config(Config *cfg)
{
	ConfigDefaults d = {.confdir = confdir, .webdir = webdir, .datadir = basedir};
	CHECK(config_init(cfg, &d) == CONFIG_OK);
	CHECK(config_load(cfg) == CONFIG_OK);
	CHECK(config_set(cfg, "host", "127.0.0.1", "test") == CONFIG_OK);
	CHECK(config_set(cfg, "log.level", "info", "test") == CONFIG_OK);
}

static uint64_t short_tick(void *ctx, uint64_t now_ms)
{
	(void)ctx;
	(void)now_ms;
	return 10;
}

/* engine_start with cfg, in the mode, on ephemeral ports unless !ephemeral;
 * logs cleared first. */
static bool start_with(const Config *cfg, enum engine_load load, const char *files, bool ephemeral)
{
	logs[0] = '\0';
	EngineParams p = {.load = load, .graph_files = files, .ephemeral_ports = ephemeral};
	bool ok = engine_start(cfg, &p, &engine);
	CHECK(ok == (engine != NULL));
	if (ok)
		CHECK(loop_add_tick(engine_loop(engine), short_tick, NULL));
	return ok;
}

static bool start(enum engine_load load, const char *files)
{
	static Config cfg;
	make_config(&cfg);
	return start_with(&cfg, load, files, true);
}

/* A stop request, then engine_run returns, then the shutdown. */
static void stop(void)
{
	pal_request_stop();
	engine_run(engine);
	loop_remove_tick(engine_loop(engine), short_tick, NULL);
	engine_stop(engine);
	engine = NULL;
}

typedef bool (*pred_fn)(void *arg);

static bool run_until(pred_fn pred, void *arg)
{
	uint64_t end = pal_uptime_ms() + WAIT_MS;
	while (!pred(arg)) {
		if (pal_uptime_ms() > end)
			return false;
		loop_run_once(engine_loop(engine));
	}
	return true;
}

/* ---- Client ----------------------------------------------------------------- */

typedef struct {
	int fd;
	uint8_t *buf;
	size_t len, cap;
	bool eof;
} Client;

static Client client_connect(uint16_t port)
{
	Client cl = {0};
	cl.fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	CHECK(cl.fd >= 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(connect(cl.fd, (struct sockaddr *)&a, sizeof(a)) == 0);
	CHECK(fcntl(cl.fd, F_SETFL, O_NONBLOCK) == 0);
	return cl;
}

static void client_close(Client *cl)
{
	close(cl->fd);
	free(cl->buf);
	memset(cl, 0, sizeof(*cl));
}

static void client_send(Client *cl, const void *data, size_t len)
{
	const uint8_t *p = data;
	while (len > 0) {
		ssize_t n = send(cl->fd, p, len, MSG_NOSIGNAL);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			loop_run_once(engine_loop(engine));
			continue;
		}
		CHECK(n > 0);
		if (n <= 0)
			return;
		p += n;
		len -= (size_t)n;
	}
}

static void client_poll(Client *cl)
{
	for (;;) {
		if (cl->cap - cl->len < 65536) {
			cl->cap = cl->cap * 2 + 65536;
			cl->buf = realloc(cl->buf, cl->cap);
			CHECK(cl->buf != NULL);
		}
		ssize_t n = recv(cl->fd, cl->buf + cl->len, cl->cap - cl->len, 0);
		if (n > 0) {
			cl->len += (size_t)n;
			continue;
		}
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			cl->eof = true;
		return;
	}
}

static bool client_eof(void *arg)
{
	Client *cl = arg;
	client_poll(cl);
	return cl->eof;
}

static void consume(Client *cl, size_t n)
{
	memmove(cl->buf, cl->buf + n, cl->len - n);
	cl->len -= n;
}

/* The response head (up to the empty line), taken from the buffer. */
static bool take_head(void *arg)
{
	Client *cl = ((void **)arg)[0];
	char *head = ((void **)arg)[1];
	client_poll(cl);
	for (size_t i = 0; i + 4 <= cl->len; i++) {
		if (memcmp(cl->buf + i, "\r\n\r\n", 4) == 0) {
			size_t hl = i + 4 < 2047 ? i + 4 : 2047;
			memcpy(head, cl->buf, hl);
			head[hl] = '\0';
			consume(cl, i + 4);
			return true;
		}
	}
	return cl->eof;
}

static int get_head(Client *cl, char head[2048])
{
	void *arg[2] = {cl, head};
	head[0] = '\0';
	CHECK(run_until(take_head, arg));
	int status = 0;
	sscanf(head, "HTTP/1.1 %d", &status);
	return status;
}

/* A masked client text frame. */
static void send_text(Client *cl, const char *text)
{
	size_t len = strlen(text);
	uint8_t *f = malloc(len + WS_FRAME_HEADER_MAX);
	CHECK(f != NULL);
	size_t p = 0;
	f[p++] = 0x80 | WS_OP_TEXT;
	if (len < 126) {
		f[p++] = (uint8_t)(0x80 | len);
	} else {
		f[p++] = 0x80 | 126;
		f[p++] = (uint8_t)(len >> 8);
		f[p++] = (uint8_t)len;
	}
	const uint8_t key[4] = {0x12, 0x34, 0x56, 0x78};
	memcpy(f + p, key, 4);
	p += 4;
	for (size_t i = 0; i < len; i++)
		f[p + i] = (uint8_t)text[i] ^ key[i & 3];
	client_send(cl, f, p + len);
	free(f);
}

typedef struct {
	Client *cl;
	char *text; /* malloc'd */
} WantText;

static bool take_text(void *arg)
{
	WantText *w = arg;
	client_poll(w->cl);
	WsFrameHeader h;
	int hl = w->cl->len >= 2 ? ws_frame_header_parse(w->cl->buf, w->cl->len, &h) : 0;
	if (hl <= 0 || w->cl->len - (size_t)hl < h.len)
		return w->cl->eof;
	CHECK(h.opcode == WS_OP_TEXT && h.fin && !h.masked);
	w->text = malloc(h.len + 1);
	memcpy(w->text, w->cl->buf + hl, h.len);
	w->text[h.len] = '\0';
	consume(w->cl, (size_t)hl + (size_t)h.len);
	return true;
}

/* The last message next_text returned. */
static char *last_text;

/* The next text message (valid until the next call or text_free; "" if
 * none came). */
static const char *next_text(Client *cl)
{
	free(last_text);
	WantText w = {cl, NULL};
	CHECK(run_until(take_text, &w));
	last_text = w.text;
	CHECK(last_text != NULL);
	return last_text != NULL ? last_text : "";
}

static void text_free(void)
{
	free(last_text);
	last_text = NULL;
}

static void send_cmd(Client *cl, const char *protocol, const char *command, const char *payload,
		     const char *secret)
{
	char msg[1024];
	snprintf(msg, sizeof(msg),
		 "{\"protocol\":\"%s\",\"command\":\"%s\",\"payload\":%s,\"secret\":\"%s\"}",
		 protocol, command, payload, secret);
	send_text(cl, msg);
}

/* A member of the message's payload as text (a number in %.0f). */
static const char *payload_str(const char *message, const char *key)
{
	static char buf[4][256];
	static int next;
	char *p = buf[next++ % 4];
	p[0] = '\0';
	cJSON *m = cJSON_Parse(message);
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(
		cJSON_GetObjectItemCaseSensitive(m, "payload"), key);
	if (cJSON_IsString(v))
		snprintf(p, 256, "%s", v->valuestring);
	else if (cJSON_IsNumber(v))
		snprintf(p, 256, "%.0f", v->valuedouble);
	cJSON_Delete(m);
	return p;
}

/* An upgraded WebSocket with the own Origin (02 §0). */
static Client ws_connect(void)
{
	Client cl = client_connect(engine_ws_port(engine));
	char req[512], head[2048];
	snprintf(req, sizeof(req),
		 "GET / HTTP/1.1\r\nHost: 127.0.0.1:%u\r\nUpgrade: websocket\r\n"
		 "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		 "Sec-WebSocket-Version: 13\r\nOrigin: http://127.0.0.1:%u\r\n\r\n",
		 engine_ws_port(engine), engine_web_port(engine));
	client_send(&cl, req, strlen(req));
	CHECK(get_head(&cl, head) == 101);
	CHECK(strstr(head, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
	return cl;
}

/* The client side of 02 §2.3; returns the secret ("" on failure). */
static const char *login(Client *cl, const char *user, const char *password)
{
	send_cmd(cl, "environment", "challenge",
		 "{\"user\":\"alice\",\"cnonce\":\"MDEyMzQ1Njc4OWFiY2RlZg==\"}", "");
	const char *reply = next_text(cl);
	char salt64[64], nonce[256];
	snprintf(salt64, sizeof(salt64), "%s", payload_str(reply, "salt"));
	snprintf(nonce, sizeof(nonce), "%s", payload_str(reply, "nonce"));
	unsigned iterations = (unsigned)strtoul(payload_str(reply, "iterations"), NULL, 10);
	CHECK(iterations == 64);

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
	char proof64[64], ssig64[64], payload[512];
	crypto_base64_encode(proof, 32, proof64, sizeof(proof64));
	crypto_base64_encode(ssig, 32, ssig64, sizeof(ssig64));
	snprintf(payload, sizeof(payload), "{\"user\":\"%s\",\"nonce\":\"%s\",\"proof\":\"%s\"}",
		 user, nonce, proof64);
	send_cmd(cl, "environment", "login", payload, "");
	reply = next_text(cl);
	static char secret[64];
	snprintf(secret, sizeof(secret), "%s", payload_str(reply, "secret"));
	CHECK_STREQ(payload_str(reply, "server_signature"), ssig64);
	CHECK_STREQ(payload_str(reply, "role"), "editor");
	CHECK(strlen(secret) == AUTH_SECRET_LEN);
	return secret;
}

/* ---- Tests ------------------------------------------------------------------ */

static void test_run(void)
{
	int fds = fd_count();
	CHECK(start(ENGINE_LOAD_ALL, NULL));
	if (engine == NULL)
		return;
	const Config *cfg = engine_config(engine);
	CHECK(engine_web_port(engine) != 0 && engine_ws_port(engine) != 0);
	CHECK(strlen(cfg->engine_id) == 36); /* a UUID: no engine_id configured */
	char ready[256];
	snprintf(ready, sizeof(ready),
		 "engine %s ready: web server on 127.0.0.1 port %u, WebSocket on port %u",
		 cfg->engine_id, engine_web_port(engine), engine_ws_port(engine));
	CHECK(strstr(logs, ready) != NULL);
	CHECK(strstr(logs, "1 graph loaded") != NULL);
	/* The generated server secret is in the file, never in the log. */
	CHECK(strlen(cfg->auth.server_secret) == 44);
	char file[1024];
	size_t len = 0;
	CHECK(pal_storage_read(cfg->file, file, sizeof(file) - 1, &len) == PAL_OK);
	file[len] = '\0';
	CHECK(strstr(file, cfg->auth.server_secret) != NULL);
	CHECK(strstr(logs, cfg->auth.server_secret) == NULL);
	/* The leftover .tmp files are gone. */
	CHECK(!pal_storage_exists(join(join(basedir, "graphs"), "old.json.tmp")));
	CHECK(!pal_storage_exists(join(confdir, "hakkas.json.tmp")));
	CHECK(pal_storage_exists(join(webdir, "keep.tmp"))); /* not a directory it writes */

	/* The web server: GET / is index.html. */
	Client web = client_connect(engine_web_port(engine));
	const char *get = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
	client_send(&web, get, strlen(get));
	char head[2048];
	CHECK(get_head(&web, head) == 200);
	CHECK(run_until(client_eof, &web));
	CHECK(web.len == strlen(INDEX_HTML) && memcmp(web.buf, INDEX_HTML, web.len) == 0);
	client_close(&web);

	/* The WebSocket: getruntime before login, the login, graph:connect. */
	Client ws = ws_connect();
	send_cmd(&ws, "runtime", "getruntime", "{}", "");
	const char *m = next_text(&ws);
	CHECK(strstr(m, "\"command\":\"runtime\"") != NULL);
	CHECK(strstr(m, "\"capabilities\":[]") != NULL);
	CHECK_STREQ(payload_str(m, "id"), cfg->engine_id);
	char secret[64];
	snprintf(secret, sizeof(secret), "%s", login(&ws, "alice", "pencil"));
	send_cmd(&ws, "graph", "connect", "{\"id\":\"g1\"}", secret);
	m = next_text(&ws);
	CHECK(strstr(m, "\"protocol\":\"graph\",\"command\":\"clear\"") != NULL);
	CHECK_STREQ(payload_str(m, "id"), "g1");
	CHECK_STREQ(payload_str(m, "name"), "Test graph");
	m = next_text(&ws);
	CHECK(strstr(m, "\"command\":\"addnode\"") != NULL);
	CHECK_STREQ(payload_str(m, "id"), "a");
	CHECK_STREQ(payload_str(m, "component"), "math/Add2");
	m = next_text(&ws);
	CHECK(strstr(m, "\"command\":\"addinitial\"") != NULL);
	CHECK(strstr(m, "\"tgt\":{\"node\":\"a\",\"port\":\"in1\"}") != NULL);
	/* The graph was started at boot (05 §2.1 started). */
	send_cmd(&ws, "network", "getstatus", "{\"graph\":\"g1\"}", secret);
	m = next_text(&ws);
	CHECK(strstr(m, "\"command\":\"status\"") != NULL);
	CHECK(strstr(m, "\"started\":true") != NULL);
	CHECK(strstr(logs, secret) == NULL);

	/* A stop request ends the run and closes the connection. */
	stop();
	CHECK(client_eof(&ws));
	client_close(&ws);
	text_free();
	CHECK(fd_count() == fds);
	CHECK(strstr(logs, "engine loop stopping") != NULL);
}

/* engine_start fails with a log line containing what; nothing is left open. */
static void refused(const Config *cfg, enum engine_load load, const char *files, bool ephemeral,
		    const char *what)
{
	int fds = fd_count();
	CHECK(!start_with(cfg, load, files, ephemeral));
	if (strstr(logs, what) == NULL)
		fprintf(stderr, "expected \"%s\" in:\n%s", what, logs);
	CHECK(strstr(logs, what) != NULL);
	CHECK(strstr(logs, "the engine did not start") != NULL);
	CHECK(engine == NULL);
	CHECK(fd_count() == fds);
}

/* A listening socket on 127.0.0.1 (port 0: any); its port in *port. */
static int hold_port(uint16_t want, uint16_t *port)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(want)};
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
		if (fd >= 0)
			close(fd);
		return -1;
	}
	socklen_t al = sizeof(a);
	getsockname(fd, (struct sockaddr *)&a, &al);
	*port = ntohs(a.sin_port);
	return fd;
}

static void test_port_in_use(void)
{
	static Config cfg;
	char port[16];
	uint16_t p = 0, q = 0;
	/* The web port taken. */
	int held = hold_port(0, &p);
	CHECK(held >= 0 && p > 1);
	make_config(&cfg);
	snprintf(port, sizeof(port), "%u", (unsigned)p);
	CHECK(config_set(&cfg, "port", port, "test") == CONFIG_OK);
	refused(&cfg, ENGINE_LOAD_ALL, NULL, false, "cannot open the web server on 127.0.0.1 port");
	close(held);

	/* The WebSocket port (port + 1) taken, the web port free: a held port
	 * whose predecessor is free. */
	held = -1;
	for (int i = 0; i < 50 && held < 0; i++) {
		held = hold_port(0, &p);
		int probe = held >= 0 && p > 1 ? hold_port((uint16_t)(p - 1), &q) : -1;
		if (probe >= 0) {
			close(probe);
		} else if (held >= 0) {
			close(held);
			held = -1;
		}
	}
	CHECK(held >= 0);
	make_config(&cfg);
	snprintf(port, sizeof(port), "%u", (unsigned)(p - 1));
	CHECK(config_set(&cfg, "port", port, "test") == CONFIG_OK);
	refused(&cfg, ENGINE_LOAD_ALL, NULL, false, "cannot open the WebSocket listener");
	close(held);
}

static void test_other_failures(void)
{
	static Config cfg;
	make_config(&cfg);
	CHECK(config_set(&cfg, "tls.enabled", "true", "test") == CONFIG_OK);
	refused(&cfg, ENGINE_LOAD_ALL, NULL, true, "TLS (tls.enabled) is not supported yet");

	make_config(&cfg);
	CHECK(config_set(&cfg, "datadir", join(root, "missing"), "test") == CONFIG_OK);
	refused(&cfg, ENGINE_LOAD_ALL, NULL, true, "cannot create directory");

	/* No secret yet and a confdir that does not exist: the file cannot be
	 * written. */
	ConfigDefaults d = {.confdir = join(root, "no-conf"), .webdir = webdir, .datadir = basedir};
	CHECK(config_init(&cfg, &d) == CONFIG_OK);
	CHECK(config_load(&cfg) == CONFIG_OK);
	CHECK(config_set(&cfg, "log.level", "info", "test") == CONFIG_OK);
	refused(&cfg, ENGINE_LOAD_ALL, NULL, true, "with the generated auth.server_secret");
}

static void test_broken_graph(void)
{
	char graphs[PAL_PATH_MAX], bad[PAL_PATH_MAX], good[PAL_PATH_MAX];
	snprintf(graphs, sizeof(graphs), "%s", join(basedir, "graphs"));
	snprintf(bad, sizeof(bad), "%s", join(graphs, "bad.json"));
	snprintf(good, sizeof(good), "%s", join(graphs, "g1.json"));
	put(bad, "{\"id\":");
	char list[2 * PAL_PATH_MAX + 2];
	snprintf(list, sizeof(list), "%s:%s", good, bad);

	/* process: a file that does not load refuses startup. */
	static Config cfg;
	make_config(&cfg);
	refused(&cfg, ENGINE_LOAD_LIST, list, true, "run process: a graph file did not load");
	CHECK(strstr(logs, "bad.json") != NULL);
	make_config(&cfg);
	refused(&cfg, ENGINE_LOAD_LIST, join(graphs, "none.json"), true, "did not load");

	/* process with only the good file starts. */
	int fds = fd_count();
	CHECK(start(ENGINE_LOAD_LIST, good));
	CHECK(strstr(logs, "1 graph loaded") != NULL);
	if (engine != NULL)
		stop();

	/* daemon: the broken file is logged and skipped. */
	CHECK(start(ENGINE_LOAD_ALL, NULL));
	CHECK(strstr(logs, "bad.json") != NULL);
	CHECK(strstr(logs, "starting with the others") != NULL);
	CHECK(strstr(logs, "1 graph loaded") != NULL);
	if (engine != NULL)
		stop();
	CHECK(fd_count() == fds);
	CHECK(pal_storage_remove(bad) == PAL_OK);
}

/* ---- Setup ------------------------------------------------------------------ */

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb;
	(void)flag;
	(void)ftw;
	return remove(path);
}

static void setup(void)
{
	snprintf(confdir, sizeof(confdir), "%s", join(root, "conf"));
	snprintf(basedir, sizeof(basedir), "%s", join(root, "base"));
	snprintf(webdir, sizeof(webdir), "%s", join(root, "web"));
	CHECK(pal_storage_mkdir(confdir) == PAL_OK);
	CHECK(pal_storage_mkdir(basedir) == PAL_OK);
	CHECK(pal_storage_mkdir(webdir) == PAL_OK);
	CHECK(pal_storage_mkdir(join(basedir, "graphs")) == PAL_OK);
	put(join(webdir, "index.html"), INDEX_HTML);
	put(join(webdir, "keep.tmp"), "x");
	put(join(join(basedir, "graphs"), "g1.json"), GOOD_GRAPH);
	put(join(join(basedir, "graphs"), "old.json.tmp"), "leftover");
	put(join(confdir, "hakkas.json.tmp"), "leftover");
	/* The test user, with a low iteration count so a login is fast. */
	static UserStore users;
	users_init(&users);
	CHECK(users_add(&users, "alice", USER_ROLE_EDITOR, "pencil", 6, 64, pal_random_bytes) ==
	      USERS_OK);
	CHECK(users_save(&users, join(confdir, "users.json")) == USERS_OK);
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(log_add_sink(log_sink, NULL));
	log_set_threshold(LOG_LEVEL_INFO);
	const char *base = getenv("TMPDIR");
	snprintf(root, sizeof(root), "%s/hakkas-engine-XXXXXX", base && *base ? base : "/tmp");
	if (mkdtemp(root) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	setup();

	RUN(test_run);
	RUN(test_port_in_use);
	RUN(test_other_failures);
	RUN(test_broken_graph);

	log_remove_sink(log_sink, NULL);
	if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
	return test_result();
}
