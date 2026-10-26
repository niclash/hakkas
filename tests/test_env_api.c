/*
 * The environment through the local C API (hakkas.h, 02 §6; plan task 22)
 * over the whole engine in-process (e2e.h), with a fake environment:
 * startup step 5 (describe, validate, start; each failure refuses startup)
 * and the stop at shutdown; sensor values pushed from another thread
 * through the sink reaching a bound input (the PAL's handoff, the loop);
 * the wall clock from the environment — what the loop gives the runtime,
 * the processes' start times and the wire's timestamps (02 §3.3) — and the
 * platform's clock without one; hakkas_bindings_apply with bindings files
 * (applied, skipped, every file and refusal failure), hakkas_binding_add /
 * _remove / _count / _at and the BINDING_* events.
 */
#include "e2e.h"

#include <pthread.h>
#include <stdlib.h>

#include "app/graph_files.h"
#include "platform/pal.h"
#include "protocol/messages.h"

/* 2026-01-02T03:04:05.678Z */
#define FAKE_WALL_MS 1767323045678LL

static const HakkasEnvPort out_ports[] = {
	{.id = "ain", .type = HAKKAS_ENV_NUMBER, .addressable = true, .count = 4},
	{.id = "temp", .type = HAKKAS_ENV_NUMBER},
};
static const HakkasEnvPort in_ports[] = {
	{.id = "relay", .type = HAKKAS_ENV_BOOL, .addressable = true, .count = 2},
};

typedef struct {
	HakkasEnvDescription desc;
	HakkasEnvSink *sink;
	int starts, stops;
	int start_rc;
	char writes[1024];
	int64_t wall;
} Fake;

static Fake fake;

static const HakkasEnvDescription *fake_describe(void *ctx)
{
	return &((Fake *)ctx)->desc;
}

static int fake_start(void *ctx, HakkasEnvSink *sink)
{
	Fake *f = ctx;
	f->starts++;
	f->sink = sink;
	if (f->start_rc == 0) {
		HakkasValue v = linkable_from_f64(21.5); /* a value at hand, in start */
		CHECK(hakkas_env_sink_current(sink, 1, 0, &v));
	}
	return f->start_rc;
}

static void fake_stop(void *ctx)
{
	((Fake *)ctx)->stops++;
}

static void fake_write(void *ctx, int port, int index, const HakkasValue *v)
{
	Fake *f = ctx;
	size_t len = strlen(f->writes);
	snprintf(f->writes + len, sizeof(f->writes) - len, "%s[%d]=%s;", in_ports[port].id, index,
		 linkable_to_bool(v) ? "true" : "false");
}

static int64_t fake_wall(void *ctx)
{
	return ((Fake *)ctx)->wall;
}

static HakkasEnvironment spi = {fake_describe, fake_start, fake_stop, fake_write, fake_wall,
				&fake};

static void fake_reset(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.desc = (HakkasEnvDescription){.name = "fake",
					   .in_ports = in_ports,
					   .in_port_count = 1,
					   .out_ports = out_ports,
					   .out_port_count = 2};
	fake.wall = FAKE_WALL_MS;
}

static Hakkas *h;

static bool start_with_env(void)
{
	static Config cfg;
	e2e_config(&cfg);
	HakkasParams p = {.environment = &spi, .ephemeral_ports = true};
	if (!e2e_start_params(&cfg, &p))
		return false;
	h = e2e_hakkas;
	return true;
}

/* ---- Events ------------------------------------------------------------------------ */

static char bind_log[1024];

static void on_event(void *ctx, const HakkasEvent *ev)
{
	(void)ctx;
	if (ev->type != HAKKAS_EVENT_BINDING_ADDED && ev->type != HAKKAS_EVENT_BINDING_REMOVED)
		return;
	size_t len = strlen(bind_log);
	snprintf(bind_log + len, sizeof(bind_log) - len, "%c%s:%s%s%s[%u];",
		 ev->type == HAKKAS_EVENT_BINDING_ADDED ? '+' : '-', ev->process, ev->name,
		 ev->actuator ? ">" : "<", ev->env_port, (unsigned)ev->env_index);
}

/* Block's ports in1, in2 and out exposed as process ports "<block>.in1", ...
 * (bindings name process ports, 04 §4). */
static void expose(const char *process, const char *block)
{
	char name[32], port[8];
	for (int i = 1; i <= 2; i++) {
		snprintf(port, sizeof(port), "in%d", i);
		snprintf(name, sizeof(name), "%s.%s", block, port);
		CHECK(hakkas_port_add(h, process, HAKKAS_INPORT, name, block, port, NULL) ==
		      HAKKAS_OK);
	}
	snprintf(name, sizeof(name), "%s.out", block);
	CHECK(hakkas_port_add(h, process, HAKKAS_OUTPORT, name, block, "out", NULL) == HAKKAS_OK);
}

/* ---- Startup and shutdown ------------------------------------------------------- */

static void test_startup(void)
{
	fake_reset();
	CHECK(start_with_env());
	CHECK(fake.starts == 1 && fake.stops == 0);
	CHECK(hakkas_environment(h) == &fake.desc);
	CHECK(strstr(e2e_logs(), "environment fake: 5 sensor channels, 2 actuator channels, 0 "
				 "modules") != NULL);
	e2e_stop();
	CHECK(fake.stops == 1);

	/* Each failure refuses startup, with nothing left over (asan). */
	static Config cfg;
	e2e_config(&cfg);
	HakkasParams p = {.environment = &spi, .ephemeral_ports = true};
	fake_reset();
	HakkasEnvPort bad = out_ports[0];
	bad.count = 0;
	fake.desc.out_ports = &bad;
	fake.desc.out_port_count = 1;
	CHECK(!e2e_start_params(&cfg, &p));
	CHECK(strstr(e2e_logs(), ": environment description: outPorts[0]: count: "
				 "expected 1 to 1024\n") != NULL);
	CHECK(strstr(e2e_logs(), ": the engine did not start\n") != NULL);
	CHECK(fake.starts == 0);
	fake_reset();
	fake.start_rc = 3;
	CHECK(!e2e_start_params(&cfg, &p));
	CHECK(strstr(e2e_logs(), ": the environment did not start (3)\n") != NULL);
	CHECK(fake.starts == 1 && fake.stops == 0);
	e2e_hakkas = NULL;
}

/* ---- Sensor values from another thread ------------------------------------------ */

#define THREAD_VALUES 200

static void *producer(void *arg)
{
	(void)arg;
	for (int i = 1; i <= THREAD_VALUES; i++) {
		HakkasValue v = linkable_from_f64(i);
		CHECK(hakkas_env_sink_push(fake.sink, 0, 3, &v));
	}
	return NULL;
}

/* Whether fake.writes contains arg. */
static bool relay_is(void *arg)
{
	return strstr(fake.writes, (const char *)arg) != NULL;
}

static void test_sensor_thread(void)
{
	fake_reset();
	CHECK(start_with_env());
	CHECK(hakkas_subscribe(h, on_event, NULL));
	CHECK(hakkas_process_create(h, "p", "P", NULL, NULL, NULL, NULL) == HAKKAS_OK);
	/* a.out = a.in1 + a.in2 drives relay[1] (true once it is non-zero);
	 * b compares: b.out = a.out >= 200 drives relay[2]. */
	CHECK(hakkas_block_add(h, "p", "a", "math/Add2", NULL, NULL) == HAKKAS_OK);
	CHECK(hakkas_block_add(h, "p", "b", "compare/GreaterEqual", NULL, NULL) == HAKKAS_OK);
	CHECK(hakkas_connection_add(h, "p", "a", "out", "b", "in1", NULL, NULL) == HAKKAS_OK);
	expose("p", "a");
	expose("p", "b");
	HakkasValue limit = linkable_from_f64(THREAD_VALUES);
	CHECK(hakkas_initial_add(h, "p", "b", "in2", &limit, NULL) == HAKKAS_OK);
	CHECK(hakkas_binding_add(h, "p", "a.in1", "ain", 3, NULL) == HAKKAS_OK);
	CHECK(hakkas_binding_add(h, "p", "b.out", "relay", 2, NULL) == HAKKAS_OK);
	CHECK_STREQ(bind_log, "+p:a.in1<ain[3];+p:b.out>relay[2];");
	CHECK(hakkas_process_start(h, "p", NULL) == HAKKAS_OK);
	CHECK_STREQ(fake.writes, "relay[2]=false;");

	/* The engine thread polls while another thread delivers: the newest
	 * value always arrives (the handoff drops the oldest when full). */
	pthread_t t;
	CHECK(pthread_create(&t, NULL, producer, NULL) == 0);
	CHECK(e2e_run_until(relay_is, "relay[2]=true;"));
	pthread_join(t, NULL);
	e2e_iterate(2);
	CHECK_STREQ(fake.writes, "relay[2]=false;relay[2]=true;");
	hakkas_unsubscribe(h, on_event, NULL);
	bind_log[0] = '\0';
	e2e_stop();
}

/* ---- The wall clock ---------------------------------------------------------------- */

static void test_wall_clock(void)
{
	fake_reset();
	CHECK(start_with_env());
	CHECK(hakkas_process_create(h, "w", "W", NULL, NULL, NULL, NULL) == HAKKAS_OK);
	E2eClient c = {0};
	CHECK(e2e_connect(&c) && e2e_login(&c, "alice"));
	QUIET(&c);

	/* The wire's timestamps are the environment's wall clock (02 §3.3). */
	e2e_send(&c, "network", "start", "{'graph':'w'}");
	EXPECT(&c, "network", "started",
	       "{'graph':'w','time':'2026-01-02T03:04:05Z','started':true,'running':true,"
	       "'debug':false}");
	HakkasProcessStatus st;
	CHECK(hakkas_process_status(h, "w", &st, NULL) == HAKKAS_OK);
	CHECK(st.wall_started_ms == FAKE_WALL_MS);
	fake.wall += 60000;
	e2e_send(&c, "network", "stop", "{'graph':'w'}");
	EXPECT(&c, "network", "stopped",
	       "{'graph':'w','time':'2026-01-02T03:04:05Z','uptime':'*','started':false,"
	       "'running':false,'debug':false}");
	e2e_send(&c, "network", "start", "{'graph':'w'}");
	EXPECT(&c, "network", "started",
	       "{'graph':'w','time':'2026-01-02T03:05:05Z','started':true,'running':true,"
	       "'debug':false}");
	/* The C API brings the runtime's clock up to date before a command. */
	fake.wall = FAKE_WALL_MS + 3600000;
	CHECK(hakkas_process_stop(h, "w", NULL) == HAKKAS_OK);
	CHECK(hakkas_process_start(h, "w", NULL) == HAKKAS_OK);
	CHECK(hakkas_process_status(h, "w", &st, NULL) == HAKKAS_OK);
	CHECK(st.wall_started_ms == FAKE_WALL_MS + 3600000);
	e2e_close(&c);
	e2e_stop();

	/* Without an environment: the platform's clock. */
	static Config cfg;
	e2e_config(&cfg);
	HakkasParams none = {.ephemeral_ports = true};
	CHECK(e2e_start_params(&cfg, &none));
	h = e2e_hakkas;
	CHECK(hakkas_environment(h) == NULL);
	CHECK(hakkas_process_create(h, "w", "W", NULL, NULL, NULL, NULL) == HAKKAS_OK);
	int64_t before = pal_wall_ms();
	CHECK(hakkas_process_start(h, "w", NULL) == HAKKAS_OK);
	int64_t after = pal_wall_ms();
	CHECK(hakkas_process_status(h, "w", &st, NULL) == HAKKAS_OK);
	CHECK(st.wall_started_ms >= before - 5 && st.wall_started_ms <= after + 5);

	/* And no bindings. */
	const char *path = e2e_path(e2e_root, "b0.json");
	e2e_put(path, "{\"environment\": \"fake\", \"bindings\": []}");
	HakkasMessage msg;
	CHECK(hakkas_bindings_apply(h, "w", path, NULL, NULL, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "No environment");
	CHECK(hakkas_binding_add(h, "w", "x.in1", "ain", 1, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "No environment");
	e2e_stop();
}

/* ---- Bindings files and single bindings ----------------------------------------- */

static void test_bindings(void)
{
	fake_reset();
	CHECK(start_with_env());
	CHECK(hakkas_subscribe(h, on_event, NULL));
	CHECK(hakkas_process_create(h, "p", "P", NULL, NULL, NULL, NULL) == HAKKAS_OK);
	CHECK(hakkas_block_add(h, "p", "a", "math/Add2", NULL, NULL) == HAKKAS_OK);
	CHECK(hakkas_block_add(h, "p", "b", "math/Add2", NULL, NULL) == HAKKAS_OK);
	expose("p", "a");
	expose("p", "b");

	const char *path = e2e_path(e2e_root, "b.json");
	e2e_put(path, "{\"environment\": \"fake\", \"bindings\": ["
		      "{\"port\": \"a.in1\", \"env\": \"temp\"},"
		      "{\"port\": \"a.in2\", \"env\": \"ain\", \"index\": 7},"
		      "{\"port\": \"a.out\", \"env\": \"relay\", \"index\": 1},"
		      "{\"port\": \"b.in1\", \"env\": \"pressure\"}]}");
	size_t applied = 0, skipped = 0;
	HakkasMessage msg;
	e2e_logs_clear();
	CHECK(hakkas_bindings_apply(h, "p", path, &applied, &skipped, &msg) == HAKKAS_OK);
	CHECK(applied == 2 && skipped == 2);
	CHECK_STREQ(msg.text, "");
	CHECK_STREQ(bind_log, "+p:a.in1<temp[0];+p:a.out>relay[1];");
	CHECK(strstr(e2e_logs(), "bindings[1]: Index out of range: ain[7]: skipped") != NULL);
	CHECK(strstr(e2e_logs(), "bindings[3]: Unknown environment port: pressure: skipped") !=
	      NULL);
	CHECK(hakkas_binding_count(h) == 2);
	HakkasBinding b;
	CHECK(hakkas_binding_at(h, 1, &b));
	CHECK_STREQ(b.process, "p");
	CHECK_STREQ(b.port, "a.out");
	CHECK_STREQ(b.env_port, "relay");
	CHECK(b.env_index == 1 && b.actuator);
	CHECK(!hakkas_binding_at(h, 2, &b) && b.process == NULL);
	CHECK(hakkas_process_start(h, "p", NULL) == HAKKAS_OK);
	CHECK_STREQ(fake.writes, "relay[1]=true;"); /* a.out = 21.5 */

	/* A second application skips every entry with a warning: a.in1 has
	 * its source, relay[1] its driver (04 §4, warn and skip). */
	bind_log[0] = '\0';
	e2e_logs_clear();
	CHECK(hakkas_bindings_apply(h, "p", path, &applied, &skipped, &msg) == HAKKAS_OK);
	CHECK(applied == 0 && skipped == 4);
	CHECK(strstr(e2e_logs(), "bindings[0]: Input already connected: a.in1: skipped") != NULL);
	CHECK(strstr(e2e_logs(), "bindings[2]: Actuator already driven: relay[1]: skipped") !=
	      NULL);
	CHECK_STREQ(bind_log, "");

	/* The file's failures. */
	CHECK(hakkas_bindings_apply(h, NULL, path, NULL, NULL, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "Invalid field: graph");
	CHECK(hakkas_bindings_apply(h, "p", NULL, NULL, NULL, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "Invalid field: bindings");
	CHECK(hakkas_bindings_apply(h, "", path, NULL, NULL, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "Invalid id");
	CHECK(hakkas_bindings_apply(h, "q", path, NULL, NULL, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "Unknown graph: q");
	CHECK(hakkas_bindings_apply(h, "p", "", NULL, NULL, &msg) == HAKKAS_ERR_INVALID);
	CHECK(hakkas_bindings_apply(h, "p", e2e_path(e2e_root, "none.json"), NULL, NULL, &msg) ==
	      HAKKAS_ERR_NOT_FOUND);
	e2e_put(path, "{\"environment\": \"fake\", \"bindings\": [{\"env\": \"temp\"}]}");
	CHECK(hakkas_bindings_apply(h, "p", path, NULL, NULL, &msg) == HAKKAS_ERR_MALFORMED);
	CHECK_STREQ(msg.text, "bindings[0]: port: expected a non-empty string");
	/* The earlier form, block ports, is not read (plan task 70). */
	e2e_put(path, "{\"environment\": \"fake\", \"bindings\": [{\"block\": \"a\","
		      " \"port\": \"in1\", \"env\": \"temp\"}]}");
	CHECK(hakkas_bindings_apply(h, "p", path, NULL, NULL, &msg) == HAKKAS_ERR_MALFORMED);
	CHECK_STREQ(msg.text, "bindings[0]: block: block ports are not bound; bind a process port");
	CHECK_STREQ(bind_log, "");
	/* Another environment's file: a warning, then applied (04 §4). */
	e2e_put(path, "{\"environment\": \"colibri\", \"bindings\": ["
		      "{\"port\": \"b.in2\", \"env\": \"temp\"}]}");
	e2e_logs_clear();
	CHECK(hakkas_bindings_apply(h, "p", path, &applied, &skipped, &msg) == HAKKAS_OK);
	CHECK(applied == 1 && skipped == 0);
	CHECK(strstr(e2e_logs(), "b.json: Environment mismatch: colibri (running: fake)\n") !=
	      NULL);
	CHECK_STREQ(bind_log, "+p:b.in2<temp[0];");
	bind_log[0] = '\0';
	CHECK(hakkas_binding_remove(h, "p", "b.in2", "temp", 0, &msg) == HAKKAS_OK);
	bind_log[0] = '\0';

	/* Single bindings: the argument checks, then the environment's. */
	CHECK(hakkas_binding_add(h, NULL, "b.in1", "ain", 1, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "Invalid field: graph");
	CHECK(hakkas_binding_add(h, "p", NULL, "ain", 1, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "Invalid field: port");
	CHECK(hakkas_binding_add(h, "p", "b.in1", NULL, 1, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "Invalid field: env");
	CHECK(hakkas_binding_add(h, "p", "b", "ain", 1, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "Unknown port: b");
	CHECK(hakkas_binding_add(h, "q", "b.in1", "ain", 1, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "Unknown graph: q");
	CHECK(hakkas_binding_add(h, "p", "b.in1", "ain", 0, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "Index required: ain");
	CHECK(hakkas_binding_add(h, "p", "b.out", "relay", 1, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "Actuator already driven: relay[1]");
	CHECK_STREQ(bind_log, "");
	CHECK(hakkas_binding_add(h, "p", "b.in1", "ain", 1, &msg) == HAKKAS_OK);
	CHECK_STREQ(bind_log, "+p:b.in1<ain[1];");

	/* Removing: a sensor-bound input reverts to its default. */
	bind_log[0] = '\0';
	fake.writes[0] = '\0';
	CHECK(hakkas_binding_remove(h, "p", "a.in1", "temp", 0, &msg) == HAKKAS_OK);
	CHECK_STREQ(bind_log, "-p:a.in1<temp[0];");
	CHECK_STREQ(fake.writes, "relay[1]=false;"); /* a.out = 0 */
	CHECK(hakkas_binding_remove(h, "p", "a.in1", "temp", 0, &msg) == HAKKAS_REFUSED);
	CHECK_STREQ(msg.text, "No such binding");
	/* Saved with the process (plan task 62): its bindings file, in creation
	 * order; a binding change makes the process unsaved; deleting the
	 * process removes the file. */
	char saved[PAL_PATH_MAX];
	snprintf(saved, sizeof(saved), "%s", e2e_path(e2e_path(e2e_basedir, "bindings"), "p.json"));
	CHECK(hakkas_process_persist(h, "p", &msg) == HAKKAS_OK);
	char *text;
	size_t len;
	CHECK(graph_files_read(saved, 4096, &text, &len) == GRAPH_FILES_OK);
	CHECK_STREQ(text, "{\"environment\":\"fake\",\"bindings\":[{\"port\":\"a.out\","
			  "\"env\":\"relay\",\"index\":1},{\"port\":\"b.in1\",\"env\":\"ain\","
			  "\"index\":1}]}");
	free(text);
	HakkasProcessStatus st;
	CHECK(hakkas_process_status(h, "p", &st, NULL) == HAKKAS_OK && !st.unsaved);
	CHECK(hakkas_binding_add(h, "p", "a.in1", "temp", 0, &msg) == HAKKAS_OK);
	CHECK(hakkas_process_status(h, "p", &st, NULL) == HAKKAS_OK && st.unsaved);
	CHECK(hakkas_binding_remove(h, "p", "a.in1", "temp", 0, &msg) == HAKKAS_OK);
	/* A deleted process's bindings go before its deletion. */
	bind_log[0] = '\0';
	CHECK(hakkas_process_delete(h, "p", "P", NULL) == HAKKAS_OK);
	CHECK(!pal_storage_exists(saved));
	CHECK_STREQ(bind_log, "-p:a.out>relay[1];-p:b.in1<ain[1];");
	CHECK(hakkas_binding_count(h) == 0);
	hakkas_unsubscribe(h, on_event, NULL);
	bind_log[0] = '\0';
	e2e_stop();
}

/* ---- The environment on the wire (02 §3.7, plan task 62) --------------------------- */

/* Every message waiting for c, dropped. */
static void drain(E2eClient *c)
{
	char **msgs;
	int n = e2e_collect(c, &msgs);
	CHECK(n >= 0);
	e2e_free_msgs(msgs, n);
}

static void test_wire(void)
{
	fake_reset();
	static const HakkasEnvModule mods[] = {{.has_slot = true, .slot = 0, .builtin = true}};
	static const uint32_t builtin[] = {0};
	static const HakkasEnvCarrier carrier = {.type = "c/C8", .slots = 7, .builtin = builtin,
						 .builtin_count = 1};
	fake.desc.modules = mods;
	fake.desc.module_count = 1;
	fake.desc.carrier = &carrier;
	CHECK(start_with_env());
	CHECK(hakkas_process_create(h, "p", "P", NULL, NULL, NULL, NULL) == HAKKAS_OK);
	CHECK(hakkas_block_add(h, "p", "a", "math/Add2", NULL, NULL) == HAKKAS_OK);
	expose("p", "a");
	E2eClient ed, other, viewer;
	CHECK(e2e_connect(&ed) && e2e_login(&ed, "alice"));
	CHECK(e2e_connect(&other) && e2e_login(&other, "oscar"));
	CHECK(e2e_connect(&viewer) && e2e_login(&viewer, "vera"));

	/* describe: the description's JSON form, to the sender only. */
	e2e_send(&viewer, "environment", "describe", "{}");
	EXPECT(&viewer, "environment", "description",
	       "{'name':'fake','outPorts':[{'id':'ain','type':'number','addressable':true,"
	       "'count':4},{'id':'temp','type':'number'}],'inPorts':[{'id':'relay',"
	       "'type':'bool','addressable':true,'count':2}],'modules':[{'slot':0,"
	       "'builtin':true,'ports':[]}],'carrier':{'type':'c/C8','slots':7,"
	       "'builtin':[{'slot':0}],'comm':[]}}");
	QUIET(&ed);

	/* bind: an event to every client viewing the process, the sender too;
	 * the process becomes unsaved. */
	e2e_send(&other, "graph", "connect", "{'id':'p'}");
	drain(&other);
	e2e_send(&ed, "environment", "bind",
		 "{'graph':'p','port':'a.in1','env':'ain','index':2}");
	EXPECT(&ed, "environment", "bind",
	       "{'graph':'p','port':'a.in1','env':'ain','index':2}");
	EXPECT(&other, "environment", "bind",
	       "{'graph':'p','port':'a.in1','env':'ain','index':2}");
	drain(&ed); /* its network:status: unsaved */
	drain(&other);
	e2e_send(&ed, "environment", "bind", "{'graph':'p','port':'a.out','env':'relay',"
					     "'index':1}");
	EXPECT(&ed, "environment", "bind",
	       "{'graph':'p','port':'a.out','env':'relay','index':1}");
	drain(&ed);
	drain(&other);
	drain(&viewer);
	HakkasProcessStatus st;
	CHECK(hakkas_process_status(h, "p", &st, NULL) == HAKKAS_OK && st.unsaved);

	/* getbindings: every process's, to the sender only. */
	e2e_send(&viewer, "environment", "getbindings", "{}");
	EXPECT(&viewer, "environment", "bindings",
	       "{'bindings':[{'graph':'p','port':'a.in1','env':'ain','index':2},"
	       "{'graph':'p','port':'a.out','env':'relay','index':1}]}");
	QUIET(&other);

	/* Refusals, to the sender only, with "graph" when it is a valid id. */
	static const struct {
		const char *payload, *message, *graph;
	} bad[] = {
		{"{}", "Invalid field: graph", NULL},
		{"{'graph':'p','block':'a','env':'ain'}", "Invalid field: port", "p"},
		{"{'graph':'p','port':'a.in2'}", "Invalid field: env", "p"},
		{"{'graph':'p','port':'a.in2','env':'ain','index':0}",
		 "Invalid field: index", "p"},
		{"{'graph':'p','port':'a.in2','env':'ain','index':1.5}",
		 "Invalid field: index", "p"},
		{"{'graph':'p','port':'a.in2','env':'ain','index':'1'}",
		 "Invalid field: index", "p"},
		{"{'graph':'','port':'a.in2','env':'ain','index':1}", "Invalid id", NULL},
		{"{'graph':'q','port':'a.in2','env':'ain','index':1}", "Unknown graph: q",
		 "q"},
		{"{'graph':'p','port':'a.in2','env':'nope'}",
		 "Unknown environment port: nope", "p"},
		{"{'graph':'p','port':'a.in2','env':'ain'}", "Index required: ain", "p"},
		{"{'graph':'p','port':'a.in2','env':'ain','index':9}",
		 "Index out of range: ain[9]", "p"},
		{"{'graph':'p','port':'zz.in2','env':'ain','index':1}",
		 "Unknown port: zz.in2", "p"},
		{"{'graph':'p','port':'a.out','env':'ain','index':1}",
		 "Invalid binding direction", "p"},
		{"{'graph':'p','port':'a.in1','env':'temp'}",
		 "Input already connected: a.in1", "p"},
		{"{'graph':'p','port':'a.out','env':'relay','index':1}",
		 "Actuator already driven: relay[1]", "p"},
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		e2e_send(&ed, "environment", "bind", bad[i].payload);
		if (!e2e_expect_error(&ed, bad[i].message, bad[i].graph)) {
			fprintf(stderr, "case %zu: %s\n", i, bad[i].payload);
			CHECK(false);
		}
	}
	e2e_send(&ed, "environment", "unbind",
		 "{'graph':'p','port':'a.in2','env':'temp'}");
	EXPECT_ERROR(&ed, "No such binding", "p");
	QUIET(&other);

	/* Capabilities: a viewer and an operator may read, not bind. */
	e2e_send(&viewer, "environment", "bind",
		 "{'graph':'p','port':'a.in2','env':'temp'}");
	EXPECT_ERROR(&viewer, "Not permitted: environment:bind", NULL);
	e2e_send(&other, "environment", "unbind",
		 "{'graph':'p','port':'a.in1','env':'ain','index':2}");
	EXPECT_ERROR(&other, "Not permitted: environment:unbind", NULL);
	e2e_send(&ed, "environment", "frob", "{}");
	EXPECT_ERROR(&ed, "Unknown command: environment:frob", NULL);

	/* unbind: an event like bind's. */
	e2e_send(&ed, "environment", "unbind",
		 "{'graph':'p','port':'a.in1','env':'ain','index':2}");
	EXPECT(&ed, "environment", "unbind",
	       "{'graph':'p','port':'a.in1','env':'ain','index':2}");
	EXPECT(&other, "environment", "unbind",
	       "{'graph':'p','port':'a.in1','env':'ain','index':2}");

	/* watch (plan task 76): every channel with a value, then each change,
	 * sensors and actuators, to the watcher only; a viewer may watch. */
	drain(&ed);
	drain(&other);
	drain(&viewer);
	e2e_send(&viewer, "environment", "watch", "{}");
	EXPECT(&viewer, "environment", "watch", "{}");
	EXPECT(&viewer, "environment", "values", "{'values':[{'env':'temp','value':21.5}]}");
	CHECK(hakkas_process_start(h, "p", NULL) == HAKKAS_OK); /* writes relay[1] */
	drain(&ed);
	EXPECT(&viewer, "environment", "values",
	       "{'values':[{'env':'relay','index':1,'value':false}]}");
	EXPECT(&viewer, "network", "started", NULL);
	HakkasValue v = linkable_from_f64(3.25);
	CHECK(hakkas_env_sink_push(fake.sink, 0, 3, &v));
	EXPECT(&viewer, "environment", "values",
	       "{'values':[{'env':'ain','index':3,'value':3.25}]}");
	QUIET(&ed);
	e2e_send(&viewer, "environment", "unwatch", "{}");
	EXPECT(&viewer, "environment", "unwatch", "{}");
	v = linkable_from_f64(4.5);
	CHECK(hakkas_env_sink_push(fake.sink, 0, 3, &v));
	QUIET(&viewer);
	e2e_close(&ed);
	e2e_close(&other);
	e2e_close(&viewer);
	CHECK(hakkas_process_delete(h, "p", "P", NULL) == HAKKAS_OK);
	e2e_stop();

	/* Without an environment: describe and bind say so. */
	CHECK(e2e_start());
	CHECK(hakkas_process_create(e2e_hakkas, "p", "P", NULL, NULL, NULL, NULL) == HAKKAS_OK);
	CHECK(e2e_connect(&ed) && e2e_login(&ed, "alice"));
	e2e_send(&ed, "environment", "describe", "{}");
	EXPECT_ERROR(&ed, "No environment", NULL);
	e2e_send(&ed, "environment", "bind", "{'graph':'p','port':'a.in1','env':'ain'}");
	EXPECT_ERROR(&ed, "No environment", "p");
	e2e_send(&ed, "environment", "getbindings", "{}");
	EXPECT(&ed, "environment", "bindings", "{'bindings':[]}");
	e2e_send(&ed, "environment", "watch", "{}");
	EXPECT_ERROR(&ed, "No environment", NULL);
	e2e_close(&ed);
	e2e_stop();
}

int main(void)
{
	e2e_setup("env-api");
	RUN(test_startup);
	RUN(test_sensor_thread);
	RUN(test_wall_clock);
	RUN(test_bindings);
	RUN(test_wire);
	e2e_teardown();
	return test_result();
}
