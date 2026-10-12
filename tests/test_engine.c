/*
 * Engine startup and shutdown (engine/engine.h, 05 §1.4) over the POSIX PAL,
 * in-process, in a fresh temporary confdir/basedir/webdir with the
 * provisioned test users of e2e.h and a saved, started graph. The engine
 * listens on ephemeral loopback ports (the test-only
 * EngineParams.ephemeral_ports); a small client checks over real sockets
 * that the web server serves index.html, that the WebSocket accepts the own
 * Origin, answers runtime:getruntime before login, takes a full SCRAM login
 * and answers graph:connect with the loaded graph's snapshot. Then a stop
 * request ends engine_run, and no descriptor is left open. The startup
 * failures: a taken port (web and WebSocket), tls.enabled, a missing datadir
 * or confdir, a broken graph file in process mode (refused) and daemon mode
 * (skipped). The client and the waits are the end-to-end helpers (e2e.h).
 */
#include "e2e.h"

#include <dirent.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "blocktypes/block_types.h"
#include "cJSON.h"
#include "engine/engine.h"
#include "platform/pal.h"

#define GOOD_GRAPH                                                                                 \
	"{\"id\":\"g1\",\"name\":\"Test graph\",\"started\":true,"                                 \
	"\"blocks\":{\"a\":{\"component\":\"math/Add2\",\"metadata\":{\"x\":10,\"y\":20}}},"      \
	"\"connections\":[{\"src\":{\"data\":2},\"tgt\":{\"node\":\"a\",\"port\":\"in1\"}}]}"

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

/* e2e_start_params with cfg, in the mode, on ephemeral ports unless
 * !ephemeral. */
static bool start_with(const Config *cfg, enum engine_load load, const char *files, bool ephemeral)
{
	EngineParams p = {.load = load, .graph_files = files, .ephemeral_ports = ephemeral};
	return e2e_start_params(cfg, &p);
}

static bool start(enum engine_load load, const char *files)
{
	static Config cfg;
	e2e_config(&cfg);
	return start_with(&cfg, load, files, true);
}

/* ---- Tests ------------------------------------------------------------------ */

static void test_run(void)
{
	int fds = fd_count();
	CHECK(start(ENGINE_LOAD_ALL, NULL));
	if (e2e_engine == NULL)
		return;
	const Config *cfg = engine_config(e2e_engine);
	CHECK(engine_web_port(e2e_engine) != 0 && engine_ws_port(e2e_engine) != 0);
	CHECK(strlen(cfg->engine_id) == 36); /* a UUID: no engine_id configured */
	char ready[256];
	snprintf(ready, sizeof(ready),
		 "engine %s ready: web server on 127.0.0.1 port %u, WebSocket on port %u",
		 cfg->engine_id, engine_web_port(e2e_engine), engine_ws_port(e2e_engine));
	CHECK(strstr(e2e_logs(), ready) != NULL);
	CHECK(strstr(e2e_logs(), "1 graph loaded") != NULL);
	/* The generated server secret is in the file, never in the log. */
	CHECK(strlen(cfg->auth.server_secret) == 44);
	char file[1024];
	size_t len = 0;
	CHECK(pal_storage_read(cfg->file, file, sizeof(file) - 1, &len) == PAL_OK);
	file[len] = '\0';
	CHECK(strstr(file, cfg->auth.server_secret) != NULL);
	CHECK(strstr(e2e_logs(), cfg->auth.server_secret) == NULL);
	/* The leftover .tmp files are gone. */
	CHECK(!pal_storage_exists(e2e_path(e2e_path(e2e_basedir, "graphs"), "old.json.tmp")));
	CHECK(!pal_storage_exists(e2e_path(e2e_confdir, "hakkas.json.tmp")));
	CHECK(pal_storage_exists(e2e_path(e2e_webdir, "keep.tmp"))); /* not a directory it writes */

	/* The web server: GET / is index.html. */
	E2eClient web;
	CHECK(e2e_tcp_connect(&web, engine_web_port(e2e_engine), 0));
	const char *get = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
	e2e_send_bytes(&web, get, strlen(get));
	char head[2048];
	CHECK(e2e_read_head(&web, head) == 200);
	CHECK(e2e_wait_eof(&web));
	CHECK(web.len == strlen(E2E_INDEX_HTML) &&
	      memcmp(web.buf, E2E_INDEX_HTML, web.len) == 0);
	e2e_close(&web);

	/* The WebSocket: getruntime before login, the login, graph:connect. */
	E2eClient ws;
	CHECK(e2e_connect(&ws));
	e2e_send(&ws, "runtime", "getruntime", "{}");
	const char *m = e2e_recv(&ws);
	CHECK(e2e_match(m, "runtime", "runtime", NULL));
	CHECK(m != NULL && strstr(m, "\"capabilities\":[]") != NULL);
	CHECK_STREQ(e2e_payload_str(m, "id"), cfg->engine_id);
	CHECK(e2e_login(&ws, "alice"));
	e2e_send(&ws, "graph", "connect", "{'id':'g1'}");
	EXPECT(&ws, "graph", "clear",
	       "{'id':'g1','name':'Test graph','main':true,'description':'','icon':''}");
	EXPECT(&ws, "graph", "addnode",
	       "{'graph':'g1','component':'math/Add2','id':'a','metadata':{'x':10,'y':20}}");
	EXPECT(&ws, "graph", "addinitial",
	       "{'graph':'g1','src':{'data':'2'},'tgt':{'node':'a','port':'in1'}}");
	/* The graph was started at boot (05 §2.1 started). */
	e2e_send(&ws, "network", "getstatus", "{'graph':'g1'}");
	m = e2e_recv(&ws);
	CHECK(e2e_match(m, "network", "status", NULL));
	CHECK(m != NULL && strstr(m, "\"started\":true") != NULL);
	CHECK(strstr(e2e_logs(), ws.secret) == NULL);

	/* A stop request ends the run and closes the connection. */
	e2e_stop();
	CHECK(e2e_wait_eof(&ws));
	e2e_close(&ws);
	CHECK(fd_count() == fds);
	CHECK(strstr(e2e_logs(), "engine loop stopping") != NULL);
}

/* engine_start fails with a log line containing what; nothing is left open. */
static void refused(const Config *cfg, enum engine_load load, const char *files, bool ephemeral,
		    const char *what)
{
	int fds = fd_count();
	CHECK(!start_with(cfg, load, files, ephemeral));
	if (strstr(e2e_logs(), what) == NULL)
		fprintf(stderr, "expected \"%s\" in:\n%s", what, e2e_logs());
	CHECK(strstr(e2e_logs(), what) != NULL);
	CHECK(strstr(e2e_logs(), "the engine did not start") != NULL);
	CHECK(e2e_engine == NULL);
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
	e2e_config(&cfg);
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
	e2e_config(&cfg);
	snprintf(port, sizeof(port), "%u", (unsigned)(p - 1));
	CHECK(config_set(&cfg, "port", port, "test") == CONFIG_OK);
	refused(&cfg, ENGINE_LOAD_ALL, NULL, false, "cannot open the WebSocket listener");
	close(held);
}

static void test_other_failures(void)
{
	static Config cfg;
	e2e_config(&cfg);
	CHECK(config_set(&cfg, "tls.enabled", "true", "test") == CONFIG_OK);
	refused(&cfg, ENGINE_LOAD_ALL, NULL, true, "TLS (tls.enabled) is not supported yet");

	e2e_config(&cfg);
	CHECK(config_set(&cfg, "datadir", e2e_path(e2e_root, "missing"), "test") == CONFIG_OK);
	refused(&cfg, ENGINE_LOAD_ALL, NULL, true, "cannot create directory");

	/* No secret yet and a confdir that does not exist: the file cannot be
	 * written. */
	ConfigDefaults d = {.confdir = e2e_path(e2e_root, "no-conf"),
			    .webdir = e2e_webdir,
			    .datadir = e2e_basedir};
	CHECK(config_init(&cfg, &d) == CONFIG_OK);
	CHECK(config_load(&cfg) == CONFIG_OK);
	CHECK(config_set(&cfg, "log.level", "info", "test") == CONFIG_OK);
	refused(&cfg, ENGINE_LOAD_ALL, NULL, true, "with the generated auth.server_secret");
}

static void test_broken_graph(void)
{
	char graphs[PAL_PATH_MAX], bad[PAL_PATH_MAX], good[PAL_PATH_MAX];
	snprintf(graphs, sizeof(graphs), "%s", e2e_path(e2e_basedir, "graphs"));
	snprintf(bad, sizeof(bad), "%s", e2e_path(graphs, "bad.json"));
	snprintf(good, sizeof(good), "%s", e2e_path(graphs, "g1.json"));
	e2e_put(bad, "{\"id\":");
	char list[2 * PAL_PATH_MAX + 2];
	snprintf(list, sizeof(list), "%s:%s", good, bad);

	/* process: a file that does not load refuses startup. */
	static Config cfg;
	e2e_config(&cfg);
	refused(&cfg, ENGINE_LOAD_LIST, list, true, "run process: a graph file did not load");
	CHECK(strstr(e2e_logs(), "bad.json") != NULL);
	e2e_config(&cfg);
	refused(&cfg, ENGINE_LOAD_LIST, e2e_path(graphs, "none.json"), true, "did not load");

	/* process with only the good file starts. */
	int fds = fd_count();
	CHECK(start(ENGINE_LOAD_LIST, good));
	CHECK(strstr(e2e_logs(), "1 graph loaded") != NULL);
	e2e_stop();

	/* daemon: the broken file is logged and skipped. */
	CHECK(start(ENGINE_LOAD_ALL, NULL));
	CHECK(strstr(e2e_logs(), "bad.json") != NULL);
	CHECK(strstr(e2e_logs(), "starting with the others") != NULL);
	CHECK(strstr(e2e_logs(), "1 graph loaded") != NULL);
	e2e_stop();
	CHECK(fd_count() == fds);
	CHECK(pal_storage_remove(bad) == PAL_OK);
}

/* Startup step 6 (05 §1.4): the composite types of <datadir>/types in
 * dependency order, the leftovers logged and skipped; a graph using one
 * loads with a real instance; the types are gone after the shutdown. */
static void test_types(void)
{
	char types[PAL_PATH_MAX], graphs[PAL_PATH_MAX], graph[PAL_PATH_MAX];
	snprintf(types, sizeof(types), "%s", e2e_path(e2e_basedir, "types"));
	snprintf(graphs, sizeof(graphs), "%s", e2e_path(e2e_basedir, "graphs"));
	snprintf(graph, sizeof(graph), "%s", e2e_path(graphs, "g2.json"));
	CHECK(pal_storage_mkdir(types) == PAL_OK);
	/* t/A needs t/B, which sorts after it; t/X and t/Y need each other. */
	e2e_put(e2e_path(types, "t%2FA.json"),
		"{\"name\":\"t/A\",\"blocks\":[{\"name\":\"b\",\"type\":\"t/B\"}],"
		"\"inports\":[{\"name\":\"in\",\"tgt\":\"b.in\"}],"
		"\"outports\":[{\"name\":\"out\",\"src\":\"b.out\"}]}");
	e2e_put(e2e_path(types, "t%2FB.json"),
		"{\"name\":\"t/B\",\"blocks\":[{\"name\":\"a\",\"type\":\"math/Add2\"}],"
		"\"initials\":[{\"src\":1,\"tgt\":\"a.in2\"}],"
		"\"inports\":[{\"name\":\"in\",\"tgt\":\"a.in1\"}],"
		"\"outports\":[{\"name\":\"out\",\"src\":\"a.out\"}]}");
	e2e_put(e2e_path(types, "t%2FX.json"),
		"{\"name\":\"t/X\",\"blocks\":[{\"name\":\"y\",\"type\":\"t/Y\"}]}");
	e2e_put(e2e_path(types, "t%2FY.json"),
		"{\"name\":\"t/Y\",\"blocks\":[{\"name\":\"x\",\"type\":\"t/X\"}]}");
	e2e_put(graph, "{\"id\":\"g2\",\"started\":true,\"blocks\":{\"c\":{\"component\":"
		       "\"t/A\"}},\"connections\":[{\"src\":{\"data\":2},\"tgt\":{\"node\":\"c\","
		       "\"port\":\"in\"}}]}");

	CHECK(start(ENGINE_LOAD_ALL, NULL));
	if (e2e_engine == NULL)
		return;
	CHECK(strstr(e2e_logs(), "2 composite types loaded") != NULL);
	CHECK(strstr(e2e_logs(), "t%2FX.json: composite type skipped: Unknown component: t/Y") !=
	      NULL);
	CHECK(strstr(e2e_logs(), "t%2FY.json: composite type skipped: Unknown component: t/X") !=
	      NULL);
	CHECK(strstr(e2e_logs(), "2 graphs loaded") != NULL);
	CHECK(strstr(e2e_logs(), "placeholder") == NULL);
	CHECK(block_types_get("t/A") != NULL && block_types_get("t/X") == NULL);

	/* Over the wire: the instance (two levels deep) is one node, and the
	 * type's source is its file's canonical form. */
	E2eClient a;
	CHECK(e2e_connect(&a));
	CHECK(e2e_login(&a, "alice"));
	e2e_send(&a, "graph", "connect", "{'id':'g2'}");
	EXPECT(&a, "graph", "clear", NULL);
	EXPECT(&a, "graph", "addnode",
	       "{'graph':'g2','component':'t/A','id':'c','metadata':{'x':50,'y':50}}");
	EXPECT(&a, "graph", "addinitial",
	       "{'graph':'g2','src':{'data':'2'},'tgt':{'node':'c','port':'in'}}");
	QUIET(&a);
	e2e_send(&a, "component", "getsource", "{'name':'t/B'}");
	const char *r = e2e_recv(&a);
	CHECK(r != NULL && e2e_match(r, "component", "source",
				     "{'name':'t/B','language':'json','library':'hakkas',"
				     "'code':'*','tests':''}"));
	cJSON *m = cJSON_Parse(r != NULL ? r : "");
	const cJSON *code =
		cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "payload"),
						 "code");
	CHECK(cJSON_IsString(code));
	if (cJSON_IsString(code))
		CHECK_STREQ(code->valuestring,
			    "{\"name\":\"t/B\",\"icon\":\"\",\"description\":\"\",\"blocks\":"
			    "[{\"name\":\"a\",\"type\":\"math/Add2\"}],\"edges\":[],\"initials\":"
			    "[{\"src\":1,\"tgt\":\"a.in2\"}],\"inports\":[{\"name\":\"in\","
			    "\"tgt\":\"a.in1\"}],\"outports\":[{\"name\":\"out\",\"src\":"
			    "\"a.out\"}]}");
	cJSON_Delete(m);
	e2e_close(&a);
	e2e_stop();
	CHECK(block_types_get("t/A") == NULL && block_types_get("t/B") == NULL);
	CHECK(pal_storage_remove(graph) == PAL_OK);
}

/* ---- Setup ------------------------------------------------------------------ */

int main(void)
{
	e2e_setup("engine");
	e2e_put(e2e_path(e2e_webdir, "keep.tmp"), "x");
	e2e_put(e2e_path(e2e_path(e2e_basedir, "graphs"), "g1.json"), GOOD_GRAPH);
	e2e_put(e2e_path(e2e_path(e2e_basedir, "graphs"), "old.json.tmp"), "leftover");
	e2e_put(e2e_path(e2e_confdir, "hakkas.json.tmp"), "leftover");

	RUN(test_run);
	RUN(test_port_in_use);
	RUN(test_other_failures);
	RUN(test_broken_graph);
	RUN(test_types);

	e2e_teardown();
	return test_result();
}
