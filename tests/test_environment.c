/*
 * The environment's engine side (plan task 22; 04 §0, §1, §4): the
 * description (validation, its JSON form, equality), the bindings file's
 * parsing, and drivers/environment.h on the Graphs registry with a fake
 * environment and controlled draining — opening (describe, validate,
 * start, the refusals), sensor values reaching every bound input through
 * the work queue (values handed over in start, the engine-wide change
 * threshold, a stopped and a started process, a value for a channel nobody binds yet),
 * no fan-in in either direction, actuator writes (the start push, every
 * propagation, nothing while stopped, one driver per channel, a binding on a
 * running process), every rule of applying a bindings file (refusals, skips,
 * all or nothing), bindings ending with their block, their process or on
 * request (a sensor-bound input then reverts; an actuator keeps its value),
 * and the wall clock (the environment's, else the platform's).
 */
#include "test.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include "blocktypes/block_types.h"
#include "drivers/bindings_file.h"
#include "drivers/env_description.h"
#include "drivers/environment.h"
#include "graphs/graphs.h"
#include "platform/pal.h"
#include "system/log.h"

static void append(char *buf, size_t size, const char *fmt, ...) LOG_PRINTF_FORMAT(3, 4);

static void append(char *buf, size_t size, const char *fmt, ...)
{
	size_t len = strlen(buf);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + len, size - len, fmt, ap);
	va_end(ap);
}

static char logs[4096];

static void log_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)len;
	const char *msg = strstr(line, ": ");
	append(logs, sizeof(logs), "%s %s\n", log_level_name(level), msg != NULL ? msg + 2 : line);
}

/* ---- A fake environment ------------------------------------------------------------ */

/* Sensors: ain (number, 1..4), din (bool, 1..2), temp (number, no index).
 * Actuators: aout (number, 1..2), relay (bool, 1..3), lamp (bool, no
 * index). One module with ain and relay. */
static const HakkasEnvPort out_ports[] = {
	{.id = "ain", .type = HAKKAS_ENV_NUMBER, .unit = "V", .addressable = true, .count = 4},
	{.id = "din", .type = HAKKAS_ENV_BOOL, .addressable = true, .count = 2},
	{.id = "temp", .type = HAKKAS_ENV_NUMBER, .has_min = true, .min = -40, .has_max = true,
	 .max = 120},
};
static const HakkasEnvPort in_ports[] = {
	{.id = "aout", .type = HAKKAS_ENV_NUMBER, .addressable = true, .count = 2},
	{.id = "relay", .type = HAKKAS_ENV_BOOL, .addressable = true, .count = 3},
	{.id = "lamp", .type = HAKKAS_ENV_BOOL},
};
static const char *const module_ports[] = {"ain", "relay"};
static const HakkasEnvModule modules[] = {
	{.has_slot = true, .slot = 3, .type = "AIV", .vendor = "stm32world", .revision = "B",
	 .serial = "0042", .documentation = "https://stm32world.com/wiki/Colibri",
	 .ports = module_ports, .port_count = 2},
};

static HakkasEnvDescription description(void)
{
	return (HakkasEnvDescription){
		.name = "fake",
		.description = "A test environment",
		.icon = "chip",
		.in_ports = in_ports,
		.in_port_count = 3,
		.out_ports = out_ports,
		.out_port_count = 3,
		.modules = modules,
		.module_count = 1,
	};
}

typedef struct {
	HakkasEnvDescription desc;
	bool describe_null;
	int start_rc;
	HakkasEnvSink *sink;
	int starts, stops;
	/* Delivered in start (values at hand, hakkas_env_sink_current, an
	 * option): ain[1] = 1.5, din[2] = true. */
	bool current;
	char writes[1024]; /* "<port>[<index>]=<value>;" each */
	int64_t wall;
} Fake;

static Fake fake;

static const HakkasEnvDescription *fake_describe(void *ctx)
{
	Fake *f = ctx;
	return f->describe_null ? NULL : &f->desc;
}

static int fake_start(void *ctx, HakkasEnvSink *sink)
{
	Fake *f = ctx;
	f->starts++;
	f->sink = sink;
	if (f->start_rc == 0 && f->current) {
		HakkasValue v = linkable_from_f64(1.5);
		CHECK(hakkas_env_sink_current(sink, 0, 1, &v));
		v = linkable_from_bool(true);
		CHECK(hakkas_env_sink_current(sink, 1, 2, &v));
		CHECK(!hakkas_env_sink_current(sink, 0, 5, &v)); /* no such channel */
	}
	return f->start_rc;
}

static void fake_stop(void *ctx)
{
	Fake *f = ctx;
	f->stops++;
}

static void fake_write(void *ctx, int port, int index, const HakkasValue *v)
{
	Fake *f = ctx;
	char value[32];
	if (v->type == LINKABLE_BOOL)
		snprintf(value, sizeof(value), "%s", v->as.b ? "true" : "false");
	else
		snprintf(value, sizeof(value), "%g", v->as.f64);
	CHECK(v->type == (in_ports[port].type == HAKKAS_ENV_BOOL ? LINKABLE_BOOL : LINKABLE_F64));
	if (index > 0)
		append(f->writes, sizeof(f->writes), "%s[%d]=%s;", in_ports[port].id, index, value);
	else
		append(f->writes, sizeof(f->writes), "%s=%s;", in_ports[port].id, value);
}

static int64_t fake_wall(void *ctx)
{
	return ((Fake *)ctx)->wall;
}

static HakkasEnvironment fake_spi(void)
{
	return (HakkasEnvironment){fake_describe, fake_start, fake_stop, fake_write, fake_wall,
				   &fake};
}

/* ---- The registry ------------------------------------------------------------------ */

static Runtime rt;
static Graphs gs;
static GraphsSubscriber sub;
static Environment env;
static char events[4096];

static void on_event(void *ctx, const GraphEvent *ev)
{
	(void)ctx;
	const char *id = ev->graph != NULL ? ev->graph->id : "-";
	const EnvBinding *b = ev->binding;
	switch (ev->type) {
	case GRAPH_EVENT_BINDING_ADDED:
	case GRAPH_EVENT_BINDING_REMOVED: {
		append(events, sizeof(events), "%s:%cbind %s%s%s[%u];", id,
		       ev->type == GRAPH_EVENT_BINDING_ADDED ? '+' : '-', b->name,
		       b->actuator ? ">" : "<", b->env, (unsigned)b->env_index);
		break;
	}
	case GRAPH_EVENT_BLOCK_REMOVED:
		append(events, sizeof(events), "%s:-node %s;", id, ev->block->name);
		break;
	case GRAPH_EVENT_EDGE_REMOVED:
		append(events, sizeof(events), "%s:-edge;", id);
		break;
	case GRAPH_EVENT_GRAPH_STOPPED:
		append(events, sizeof(events), "%s:stopped;", id);
		break;
	case GRAPH_EVENT_GRAPH_DELETED:
		append(events, sizeof(events), "%s:-graph;", id);
		break;
	default:
		break;
	}
}

/* The engine's handoff, single-threaded: straight into the work queue. */
static enum pal_err push_direct(WorkEntry *e)
{
	work_queue_push_entry(&rt.queue, e);
	return PAL_OK;
}

static int64_t platform_clock(void)
{
	return 777;
}

static void drain(void)
{
	runtime_drain(&rt, SIZE_MAX);
}

static Block *block(const char *graph, const char *node)
{
	Graph *g = graphs_find(&gs, graph);
	return g != NULL ? graph_find_block(g, node) : NULL;
}

static const Linkable *in(const char *graph, const char *node, const char *port)
{
	Block *b = block(graph, node);
	int32_t i = b != NULL ? block_input_index(b, port) : -1;
	CHECK(i >= 0);
	static const Linkable none = {0};
	return i >= 0 ? block_input_value(b, (uint32_t)i) : &none;
}

static double num(const char *graph, const char *node, const char *port)
{
	return linkable_to_f64(in(graph, node, port));
}

static double out(const char *graph, const char *node)
{
	Block *b = block(graph, node);
	CHECK(b != NULL);
	return b != NULL ? linkable_to_f64(block_output_value(b, 0)) : -1;
}

static void init(const char *graph, const char *node, const char *port, double v)
{
	Linkable value = linkable_from_f64(v);
	CHECK(graphs_add_initial(&gs, graph, node, port, &value, NULL) == GRAPH_OK);
}

static void start(const char *graph)
{
	CHECK(graphs_start(&gs, graph, NULL) == GRAPH_OK);
}

static void stop(const char *graph)
{
	CHECK(graphs_stop(&gs, graph, NULL) == GRAPH_OK);
}

/* A sensor value from the environment, as the sink delivers it. */
static void sense(int port, int index, HakkasValue v)
{
	CHECK(hakkas_env_sink_push(fake.sink, port, index, &v));
}

/* Every port of block node of graph exposed as a process port named
 * "<node>.<port>" (bindings name process ports, 04 §4). */
static void expose(const char *graph, const char *node)
{
	char name[32];
	for (int i = 1; i <= 2; i++) {
		snprintf(name, sizeof(name), "%s.in%d", node, i);
		CHECK(graphs_add_port(&gs, graph, GRAPH_INPORT, name, node, name + strlen(node) + 1,
				      NULL) == GRAPH_OK);
	}
	snprintf(name, sizeof(name), "%s.out", node);
	CHECK(graphs_add_port(&gs, graph, GRAPH_OUTPORT, name, node, "out", NULL) == GRAPH_OK);
}

/*
 * Two processes, every block math/Add2 (out = in1 + in2):
 *   P: a, b; Q: c, each port exposed as a process port "<block>.<port>".
 *   All stopped. The environment open (current values on when current is
 *   set), the logs and events cleared.
 */
static void build(bool current)
{
	memset(&fake, 0, sizeof(fake));
	fake.desc = description();
	fake.current = current;
	fake.wall = 1700000000000;
	runtime_init(&rt);
	graphs_init(&gs, &rt, block_types_get, pal_random_bytes);
	graphs_subscriber_init(&sub, on_event, NULL);
	graphs_subscribe(&gs, &sub);
	CHECK(graphs_new_graph(&gs, "P", "P", NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_new_graph(&gs, "Q", "Q", NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "P", "a", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "P", "b", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "Q", "c", "math/Add2", NULL, NULL) == GRAPH_OK);
	expose("P", "a");
	expose("P", "b");
	expose("Q", "c");
	environment_init(&env, platform_clock);
	HakkasEnvironment spi = fake_spi();
	CHECK(environment_open(&env, &rt, &spi, push_direct));
	events[0] = '\0';
	logs[0] = '\0';
}

static void teardown(void)
{
	environment_close(&env);
	graphs_deinit(&gs);
	runtime_deinit(&rt);
}

/* One binding (as the C API makes it) of a process port of graph; its
 * status, the message in msg. */
static char msg[ENVIRONMENT_MESSAGE_MAX];

static enum environment_status bind(const char *graph, const char *port, const char *env_port,
				    uint32_t index)
{
	EnvBindingRequest req = {port, env_port, index};
	return environment_bind(&env, graphs_find(&gs, graph), &req, msg, sizeof(msg));
}

static enum environment_status unbind(const char *graph, const char *port, const char *env_port,
				      uint32_t index)
{
	EnvBindingRequest req = {port, env_port, index};
	return environment_unbind(&env, graphs_find(&gs, graph), &req, msg, sizeof(msg));
}

#define REFUSED(call, want)                                                                        \
	do {                                                                                       \
		events[0] = '\0';                                                                  \
		CHECK((call) == ENVIRONMENT_REFUSED);                                              \
		CHECK_STREQ(msg, (want));                                                          \
		CHECK_STREQ(events, "");                                                           \
	} while (0)

/* ---- The description (04 §1) -------------------------------------------------------- */

static void invalid_desc(HakkasEnvDescription d, const char *want)
{
	char why[ENV_DESCRIPTION_MESSAGE_MAX];
	CHECK(!env_description_validate(&d, why, sizeof(why)));
	CHECK_STREQ(why, want);
}

static void test_description_validate(void)
{
	HakkasEnvDescription d = description();
	char why[ENV_DESCRIPTION_MESSAGE_MAX] = "";
	CHECK(env_description_validate(&d, why, sizeof(why)));
	cJSON *as_json = env_description_to_json(&d);
	HakkasEnvDescription *back = env_description_from_json(as_json, why, sizeof(why));
	CHECK(back != NULL && env_description_equal(&d, back));
	env_description_free(back);
	cJSON_Delete(as_json);
	bool actuator;
	uint32_t i;
	CHECK(env_description_find(&d, "temp", &actuator, &i) && !actuator && i == 2);
	CHECK(env_description_find(&d, "relay", &actuator, &i) && actuator && i == 1);
	CHECK(!env_description_find(&d, "nope", &actuator, &i));
	CHECK(env_port_channels(&out_ports[0]) == 4 && env_port_channels(&out_ports[2]) == 1);
	CHECK(env_port_has_channel(&out_ports[0], 4) && !env_port_has_channel(&out_ports[0], 5));
	CHECK(!env_port_has_channel(&out_ports[0], 0) && env_port_has_channel(&out_ports[2], 0));
	CHECK(!env_port_has_channel(&out_ports[2], 1));

	/* Every rule. */
	CHECK(!env_description_validate(NULL, why, sizeof(why)));
	d.name = "";
	invalid_desc(d, "name: expected 1 to 64 bytes");
	HakkasEnvPort ports[2];
	d = description();
	ports[0] = out_ports[0];
	ports[1] = in_ports[2]; /* lamp: not addressable */
	ports[1].id = "ain"; /* unique across both directions */
	d.out_ports = &ports[0];
	d.out_port_count = 1;
	d.in_ports = &ports[1];
	d.in_port_count = 1;
	d.module_count = 0;
	invalid_desc(d, "duplicate port id \"ain\"");
	ports[1].id = "x";
	ports[0].count = 0;
	invalid_desc(d, "outPorts[0]: count: expected 1 to 1024");
	ports[0].count = 1025;
	invalid_desc(d, "outPorts[0]: count: expected 1 to 1024");
	ports[0].count = 1024;
	CHECK(env_description_validate(&d, NULL, 0));
	ports[1].count = 2; /* not addressable */
	invalid_desc(d, "inPorts[0]: count: only for an addressable port");
	ports[1].count = 0;
	ports[1].type = (enum hakkas_env_type)7;
	invalid_desc(d, "inPorts[0]: type: expected number or bool");
	ports[1].type = HAKKAS_ENV_BOOL;
	ports[1].has_min = ports[1].has_max = true;
	ports[1].min = 2;
	ports[1].max = 1;
	invalid_desc(d, "inPorts[0]: min, max: expected min <= max");
	ports[1].min = NAN;
	invalid_desc(d, "inPorts[0]: min, max: expected min <= max");
	ports[1].has_min = ports[1].has_max = false;
	ports[1].id = "";
	invalid_desc(d, "inPorts[0]: id: expected 1 to 64 bytes");
	char long_id[70];
	memset(long_id, 'x', 65);
	long_id[65] = '\0';
	ports[1].id = long_id;
	invalid_desc(d, "inPorts[0]: id: expected 1 to 64 bytes");
	long_id[64] = '\0';
	CHECK(env_description_validate(&d, NULL, 0));
	ports[1].id = "x";
	static const char *const bad_ports[] = {"ain", "zzz"};
	HakkasEnvModule m = {.ports = bad_ports, .port_count = 2};
	d.modules = &m;
	d.module_count = 1;
	invalid_desc(d, "modules[0]: ports[1]: no such port");
	m.port_count = 1;
	CHECK(env_description_validate(&d, NULL, 0));
	d.in_ports = NULL;
	invalid_desc(d, "a list with a count but no entries");

	/* The carrier (plan task 61), as a firmware builds it in C. */
	d = description(); /* its module is in slot 3 */
	static const uint32_t builtin[] = {0};
	HakkasEnvComm comm[2] = {{.slot = 1, .type = "colibri_ethernet"}, {.slot = 2}};
	HakkasEnvCarrier c = {.slots = 7, .builtin = builtin, .builtin_count = 1, .comm = comm,
			      .comm_count = 2};
	d.carrier = &c;
	CHECK(env_description_validate(&d, why, sizeof(why)));
	c.builtin = NULL;
	invalid_desc(d, "carrier: a list with a count but no entries");
	c.builtin = builtin;
	c.slots = 2;
	invalid_desc(d, "modules[0]: slot: expected 0 to 2");
	c.slots = 7;
	comm[1].slot = 1;
	invalid_desc(d, "carrier: comm[1]: slot 1 listed twice");
	comm[1].slot = 2;
	HakkasEnvModule mods[2] = {modules[0], {.has_slot = true, .slot = 0}};
	d.modules = mods;
	d.module_count = 2;
	invalid_desc(d, "modules[1]: builtin: slot 0 is a built-in slot");
	mods[1].builtin = true;
	CHECK(env_description_validate(&d, why, sizeof(why)));
	mods[1].has_slot = false;
	invalid_desc(d, "modules[1]: slot: expected 0 to 7");
	mods[1].has_slot = true;

	/* A name is at most HAKKAS_ENV_TEXT_MAX bytes (plan task 68). */
	static char long_name[HAKKAS_ENV_TEXT_MAX + 2];
	memset(long_name, 'n', HAKKAS_ENV_TEXT_MAX + 1);
	c.name = long_name;
	invalid_desc(d, "carrier: name, picture: longer than 1024 bytes");
	c.name = "Colibri 8";
	comm[0].name = long_name;
	invalid_desc(d, "carrier: comm[0]: name, picture: longer than 1024 bytes");
	comm[0].name = "Colibri Ethernet";
	HakkasEnvMcu mcu = {.type = "colibri_mcu6", .name = long_name};
	c.mcu = &mcu;
	invalid_desc(d, "carrier: mcu: name, picture: longer than 1024 bytes");
	mcu.name = "Colibri MCU6";
	CHECK(env_description_validate(&d, why, sizeof(why)));
	mods[1].name = long_name;
	invalid_desc(d, "modules[1]: text longer than 1024 bytes");
	mods[1].name = NULL;

	/* Pictures (plan task 75): links of at most HAKKAS_ENV_TEXT_MAX bytes, at
	 * most HAKKAS_ENV_POSITIONS_MAX positions in all, at most as many places
	 * as slots; what a position or place says is not checked. */
	c.picture = long_name;
	invalid_desc(d, "carrier: name, picture: longer than 1024 bytes");
	c.picture = NULL;
	mcu.picture = long_name;
	invalid_desc(d, "carrier: mcu: name, picture: longer than 1024 bytes");
	mcu.picture = NULL;
	mods[1].picture = long_name;
	invalid_desc(d, "modules[1]: text longer than 1024 bytes");
	mods[1].picture = "https://example.com/carrier.webp";
	static HakkasEnvPosition positions[HAKKAS_ENV_POSITIONS_MAX + 1];
	positions[0] = (HakkasEnvPosition){.port = "nope", .index = 9, .x = 7, .y = -1};
	mods[0].positions = positions;
	mods[0].position_count = 1;
	CHECK(env_description_validate(&d, why, sizeof(why)));
	mods[0].position_count = HAKKAS_ENV_POSITIONS_MAX;
	mods[1].positions = positions;
	mods[1].position_count = 1;
	invalid_desc(d, "more than 1024 positions");
	mods[1].position_count = 0;
	CHECK(env_description_validate(&d, why, sizeof(why)));
	mods[0].positions = NULL;
	invalid_desc(d, "modules[0]: positions: a count but no entries");
	mods[0].position_count = 0;
	static HakkasEnvPlace places[HAKKAS_ENV_IO_SLOTS_MAX + 1];
	c.io_places = places;
	c.io_place_count = HAKKAS_ENV_IO_SLOTS_MAX + 1;
	invalid_desc(d, "carrier: places: slots: more than 31 places");
	c.io_place_count = HAKKAS_ENV_IO_SLOTS_MAX;
	c.comm_places = places;
	c.comm_place_count = HAKKAS_ENV_COMM_SLOTS_MAX + 1;
	invalid_desc(d, "carrier: places: comm: more than 4 places");
	c.comm_place_count = HAKKAS_ENV_COMM_SLOTS_MAX;
	c.mcu_place = &places[0];
	CHECK(env_description_validate(&d, why, sizeof(why)));
	c.comm_places = NULL;
	invalid_desc(d, "carrier: a list with a count but no entries");
	c.comm_places = places;
	long_name[HAKKAS_ENV_TEXT_MAX] = '\0';
	c.picture = comm[0].picture = mcu.picture = long_name;
	CHECK(env_description_validate(&d, why, sizeof(why)));
}

static void test_description_json(void)
{
	/* The example of 04 §1, with a module. */
	const char *text =
		"{\"name\": \"emulator\", \"description\": \"Desktop simulator\","
		" \"icon\": \"chip\","
		" \"outPorts\": [{\"id\": \"analog-in\", \"type\": \"number\","
		" \"addressable\": true,"
		" \"count\": 8, \"unit\": \"V\", \"description\": \"0-10 V inputs\", \"min\": 0,"
		" \"max\": 10},"
		" {\"id\": \"digital-in\", \"type\": \"bool\", \"addressable\": true,"
		" \"count\": 16}],"
		" \"inPorts\": [{\"id\": \"analog-out\", \"type\": \"number\","
		" \"addressable\": true,"
		" \"count\": 4, \"unit\": \"V\"},"
		" {\"id\": \"relay\", \"type\": \"bool\", \"addressable\": true, \"count\": 8},"
		" {\"id\": \"buzzer\", \"type\": \"bool\", \"addressable\": null, \"future\": 1}],"
		" \"modules\": [{\"slot\": 3, \"type\": \"AIV\", \"serial\": \"77\","
		" \"documentation\": \"https://example.com\", \"ports\": [\"analog-in\"]}],"
		" \"later\": true}";
	cJSON *json = cJSON_Parse(text);
	char why[ENV_DESCRIPTION_MESSAGE_MAX];
	HakkasEnvDescription *d = env_description_from_json(json, why, sizeof(why));
	CHECK(d != NULL);
	if (d != NULL) {
		CHECK_STREQ(d->name, "emulator");
		CHECK_STREQ(d->icon, "chip");
		CHECK(d->out_port_count == 2 && d->in_port_count == 3 && d->module_count == 1);
		CHECK_STREQ(d->out_ports[0].unit, "V");
		CHECK(d->out_ports[0].addressable && d->out_ports[0].count == 8);
		CHECK(d->out_ports[0].has_min && d->out_ports[0].max == 10);
		CHECK(d->in_ports[1].type == HAKKAS_ENV_BOOL && d->in_ports[1].count == 8);
		CHECK(!d->in_ports[2].addressable && d->in_ports[2].count == 0);
		CHECK(d->in_ports[2].description == NULL);
		CHECK_STREQ(d->modules[0].serial, "77");
		CHECK(d->modules[0].vendor == NULL && d->modules[0].port_count == 1);
		CHECK_STREQ(d->modules[0].ports[0], "analog-in");

		/* Equality: a second parse is equal; any difference is not. */
		HakkasEnvDescription *e = env_description_from_json(json, why, sizeof(why));
		CHECK(e != NULL && env_description_equal(d, e));
		env_description_free(e);
		cJSON *module = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "modules"), 0);
		cJSON *port = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "outPorts"), 1);
		cJSON_ReplaceItemInObject(module, "serial", cJSON_CreateString("78"));
		e = env_description_from_json(json, why, sizeof(why));
		CHECK(e != NULL && !env_description_equal(d, e));
		env_description_free(e);
		cJSON_ReplaceItemInObject(port, "count", cJSON_CreateNumber(15));
		cJSON_ReplaceItemInObject(module, "serial", cJSON_CreateString("77"));
		e = env_description_from_json(json, why, sizeof(why));
		CHECK(e != NULL && !env_description_equal(d, e));
		env_description_free(e);
		CHECK(env_description_equal(d, d) && !env_description_equal(d, NULL));
	}
	env_description_free(d);
	cJSON_Delete(json);

	/* Invalid forms: the message names the problem. */
	static const struct {
		const char *json, *why;
	} bad[] = {
		{"[]", "expected an object"},
		{"{}", "name: expected a string"},
		{"{\"name\": 3}", "name: expected a string"},
		{"{\"name\": \"e\", \"outPorts\": {}}", "outPorts: expected an array"},
		{"{\"name\": \"e\", \"outPorts\": [3]}", "outPorts[0]: expected an object"},
		{"{\"name\": \"e\", \"inPorts\": [{\"type\": \"bool\"}]}",
		 "inPorts[0]: id: expected a string"},
		{"{\"name\": \"e\", \"inPorts\": [{\"id\": \"x\", \"type\": \"string\"}]}",
		 "inPorts[0]: type: expected \"number\" or \"bool\""},
		{"{\"name\": \"e\", \"inPorts\": [{\"id\": \"x\", \"type\": \"bool\", "
		 "\"addressable\": 1}]}",
		 "inPorts[0]: addressable: expected true or false"},
		{"{\"name\": \"e\", \"inPorts\": [{\"id\": \"x\", \"type\": \"bool\", "
		 "\"addressable\": true, \"count\": 1.5}]}",
		 "inPorts[0]: count: expected an integer"},
		{"{\"name\": \"e\", \"inPorts\": [{\"id\": \"x\", \"type\": \"bool\", "
		 "\"addressable\": true}]}",
		 "inPorts[0]: count: expected 1 to 1024"},
		{"{\"name\": \"e\", \"inPorts\": [{\"id\": \"x\", \"type\": \"bool\", \"min\": "
		 "\"0\"}]}",
		 "inPorts[0]: min, max: expected numbers"},
		{"{\"name\": \"e\", \"outPorts\": [{\"id\": \"x\", \"type\": \"bool\"}], "
		 "\"inPorts\": [{\"id\": \"x\", \"type\": \"bool\"}]}",
		 "duplicate port id \"x\""},
		{"{\"name\": \"e\", \"modules\": [{\"ports\": \"x\"}]}",
		 "modules[0]: ports: expected an array of port ids"},
		{"{\"name\": \"e\", \"modules\": [{\"ports\": [\"x\"]}]}",
		 "modules[0]: ports[0]: no such port"},
		{"{\"name\": \"e\", \"modules\": [{\"serial\": 42}]}",
		 "modules[0]: serial: expected a string"},
		{"{\"name\": \"e\", \"modules\": [{\"slot\": \"3\"}]}",
		 "modules[0]: slot: expected an integer of at least 0"},
		{"{\"name\": \"e\", \"modules\": [{\"slot\": -1}]}",
		 "modules[0]: slot: expected an integer of at least 0"},
		{"{\"name\": \"e\", \"modules\": [{\"builtin\": 1}]}",
		 "modules[0]: builtin: expected true or false"},
		{"{\"name\": \"e\", \"modules\": [{\"slot\": 1}, {\"slot\": 1}]}",
		 "modules[1]: slot 1 taken twice"},
		{"{\"name\": \"e\", \"modules\": [{\"slot\": 0, \"builtin\": true}]}",
		 "modules[0]: builtin: no carrier"},
		/* The carrier's structure (plan task 61). */
		{"{\"name\": \"e\", \"carrier\": 7}", "carrier: expected an object"},
		{"{\"name\": \"e\", \"carrier\": {}}",
		 "carrier: slots: expected an integer of at least 0"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 0}}",
		 "carrier: expected 1 to 31 I/O slots, built-in ones included"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 31, \"builtin\": [{\"slot\": 0}]}}",
		 "carrier: expected 1 to 31 I/O slots, built-in ones included"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2, \"builtin\": {}}}",
		 "carrier: builtin: expected an array"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2, \"builtin\": [3]}}",
		 "carrier: builtin[0]: slot: expected an integer of at least 0"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2, \"builtin\": [{\"slot\": 3}]}}",
		 "carrier: builtin[0]: slot: expected 0 to 2"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2, \"builtin\": [{\"slot\": 0},"
		 " {\"slot\": 0}]}}",
		 "carrier: builtin[1]: slot 0 listed twice"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2, \"comm\": [{}]}}",
		 "carrier: comm[0]: slot: expected an integer of at least 0"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2, \"comm\": [{\"slot\": 0}]}}",
		 "carrier: comm[0]: slot: expected 1 to 4"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2, \"comm\": [{\"slot\": 5}]}}",
		 "carrier: comm[0]: slot: expected 1 to 4"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2, \"comm\": [{\"slot\": 1},"
		 " {\"slot\": 1}]}}",
		 "carrier: comm[1]: slot 1 listed twice"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2, \"comm\": [{\"slot\": 1},"
		 " {\"slot\": 2}, {\"slot\": 3}, {\"slot\": 4}, {\"slot\": 1}]}}",
		 "carrier: comm: more than 4 communication slots"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2}, \"modules\": [{}]}",
		 "modules[0]: slot: expected 0 to 1"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2}, \"modules\": [{\"slot\": 2}]}",
		 "modules[0]: slot: expected 0 to 1"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2, \"builtin\": [{\"slot\": 0}]},"
		 " \"modules\": [{\"slot\": 0}]}",
		 "modules[0]: builtin: slot 0 is a built-in slot"},
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2, \"builtin\": [{\"slot\": 0}]},"
		 " \"modules\": [{\"slot\": 1, \"builtin\": true}]}",
		 "modules[0]: builtin: slot 1 is not a built-in slot"},
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		json = cJSON_Parse(bad[i].json);
		CHECK(json != NULL);
		why[0] = '\0';
		d = env_description_from_json(json, why, sizeof(why));
		CHECK(d == NULL);
		if (strcmp(why, bad[i].why) != 0)
			fprintf(stderr, "case %zu: %s\n", i, bad[i].json);
		CHECK_STREQ(why, bad[i].why);
		env_description_free(d);
		cJSON_Delete(json);
	}
}

/* The carrier board (04 §1, plan task 61): its view-only values are taken as
 * given, a wrongly typed one as absent; the communication parameters are not
 * part of the identical-description check. */
static void test_carrier_json(void)
{
	/* The example of 04 §1, with every communication kind. */
	const char *text =
		"{\"name\": \"colibri\","
		" \"carrier\": {\"type\": \"colibri_8\", \"name\": \"Colibri 8\","
		" \"vendor\": \"CurrentMakers\","
		" \"revision\": \"B\", \"serial\": \"C8-0042\", \"documentation\": \"https://x\","
		" \"slots\": 7, \"builtin\": [{\"slot\": 0}],"
		" \"power\": {\"voltage\": {\"min\": 12, \"max\": 30}, \"ac\": false, \"dc\": true,"
		" \"current\": 2.0},"
		" \"comm\": [{\"slot\": 1, \"type\": \"colibri_ethernet\","
		" \"name\": \"Colibri Ethernet\","
		" \"ip\": {\"mode\": \"dhcp\", \"address\": \"192.168.1.40\", \"prefix\": 24,"
		" \"gateway\": \"192.168.1.1\", \"dns\": [\"192.168.1.1\", 5, \"9.9.9.9\"],"
		" \"mac\": \"02:00:00:00:00:01\"}},"
		" {\"slot\": 2, \"type\": \"colibri_wifi\", \"ip\": {\"mode\": \"static\"},"
		" \"wifi\": {\"ssid\": \"plant\"}},"
		" {\"slot\": 3, \"type\": \"colibri_rs485\", \"rs485\": {\"bitrate\": 19200,"
		" \"parity\": \"mark\"}},"
		" {\"slot\": 4, \"type\": 9, \"lorawan\": {\"appEui\": \"0011\","
		" \"devEui\": \"not hex\", \"appKeySet\": true}}],"
		" \"mcu\": {\"type\": \"colibri_mcu6\", \"name\": \"Colibri MCU6\","
		" \"vendor\": \"CurrentMakers\", \"revision\": 3, \"serial\": \"M6-0007\"}},"
		" \"outPorts\": [{\"id\": \"slot3-ai\", \"type\": \"number\","
		" \"addressable\": true, \"count\": 2}],"
		" \"modules\": [{\"slot\": 0, \"builtin\": true,"
		" \"type\": \"carrier\"},"
		" {\"slot\": 3, \"type\": \"colibri_aiv\", \"name\": \"Colibri AIV\","
		" \"ports\": [\"slot3-ai\"]}]}";
	cJSON *json = cJSON_Parse(text);
	char why[ENV_DESCRIPTION_MESSAGE_MAX] = "";
	HakkasEnvDescription *d = env_description_from_json(json, why, sizeof(why));
	CHECK_STREQ(why, "");
	CHECK(d != NULL && d->carrier != NULL);
	if (d != NULL && d->carrier != NULL) {
		const HakkasEnvCarrier *c = d->carrier;
		CHECK_STREQ(c->type, "colibri_8");
		CHECK_STREQ(c->name, "Colibri 8");
		CHECK_STREQ(c->comm[0].name, "Colibri Ethernet");
		CHECK(c->comm[1].name == NULL);
		CHECK_STREQ(d->modules[1].name, "Colibri AIV");
		CHECK(d->modules[0].name == NULL);
		CHECK_STREQ(c->serial, "C8-0042");
		CHECK(c->slots == 7 && c->builtin_count == 1 && c->builtin[0] == 0);
		CHECK(c->power != NULL && c->power->has_voltage_min && c->power->voltage_max == 30);
		CHECK(!c->power->ac && c->power->dc && c->power->current == 2.0);
		CHECK(c->comm_count == 4);
		const HakkasEnvIp *ip = c->comm[0].ip;
		CHECK(ip != NULL && c->comm[0].rs485 == NULL && c->comm[0].ssid == NULL);
		CHECK_STREQ(ip->mode, "dhcp");
		CHECK(ip->has_prefix && ip->prefix == 24);
		CHECK(ip->dns_count == 2); /* a dns entry that is not a string is left out */
		CHECK_STREQ(ip->dns[1], "9.9.9.9");
		CHECK_STREQ(c->comm[1].ssid, "plant");
		CHECK(c->comm[1].ip != NULL && c->comm[1].ip->address == NULL);
		CHECK(c->comm[2].rs485 != NULL && c->comm[2].rs485->bitrate == 19200);
		CHECK_STREQ(c->comm[2].rs485->parity, "mark"); /* not validated */
		CHECK(c->comm[3].type == NULL); /* wrongly typed: absent */
		CHECK(c->comm[3].lorawan != NULL && c->comm[3].lorawan->app_key_set);
		CHECK_STREQ(c->comm[3].lorawan->dev_eui, "not hex");
		CHECK(d->modules[0].builtin && d->modules[0].has_slot && d->modules[0].slot == 0);
		CHECK(!d->modules[1].builtin && d->modules[1].slot == 3);
		const HakkasEnvMcu *mcu = c->mcu;
		CHECK(mcu != NULL);
		CHECK_STREQ(mcu->type, "colibri_mcu6");
		CHECK_STREQ(mcu->name, "Colibri MCU6");
		CHECK_STREQ(mcu->serial, "M6-0007");
		/* revision wrongly typed: absent */
		CHECK(mcu->revision == NULL && mcu->documentation == NULL);

		/* A new DHCP lease is the same environment; another module type in
		 * a communication slot, or another power supply, is not. */
		/* Its JSON form reads back as the same description. */
		cJSON *back = env_description_to_json(d);
		HakkasEnvDescription *e = env_description_from_json(back, why, sizeof(why));
		CHECK(e != NULL && env_description_equal(d, e));
		CHECK(e != NULL && e->carrier->comm[3].lorawan->app_key_set);
		CHECK(e != NULL && e->carrier->comm[0].ip->dns_count == 2);
		CHECK(e != NULL && e->carrier->mcu != NULL);
		CHECK_STREQ(e->carrier->mcu->vendor, "CurrentMakers");
		env_description_free(e);
		cJSON_Delete(back);
		cJSON *carrier = cJSON_GetObjectItem(json, "carrier");
		cJSON *comm0 = cJSON_GetArrayItem(cJSON_GetObjectItem(carrier, "comm"), 0);
		cJSON_ReplaceItemInObject(cJSON_GetObjectItem(comm0, "ip"), "address",
					  cJSON_CreateString("192.168.1.41"));
		e = env_description_from_json(json, why, sizeof(why));
		CHECK(e != NULL && env_description_equal(d, e));
		env_description_free(e);
		cJSON_ReplaceItemInObject(comm0, "type", cJSON_CreateString("colibri_wifi"));
		e = env_description_from_json(json, why, sizeof(why));
		CHECK(e != NULL && !env_description_equal(d, e));
		env_description_free(e);
		cJSON_ReplaceItemInObject(comm0, "type", cJSON_CreateString("colibri_ethernet"));
		/* A name is static: another one is another environment. */
		cJSON_ReplaceItemInObject(comm0, "name", cJSON_CreateString("Ethernet"));
		e = env_description_from_json(json, why, sizeof(why));
		CHECK(e != NULL && !env_description_equal(d, e));
		env_description_free(e);
		cJSON_ReplaceItemInObject(comm0, "name", cJSON_CreateString("Colibri Ethernet"));
		cJSON_ReplaceItemInObject(carrier, "name", cJSON_CreateString("Colibri Eight"));
		e = env_description_from_json(json, why, sizeof(why));
		CHECK(e != NULL && !env_description_equal(d, e));
		env_description_free(e);
		cJSON_ReplaceItemInObject(carrier, "name", cJSON_CreateString("Colibri 8"));
		/* The MCU module is static too: another one, or none, is another
		 * environment. */
		cJSON *mcu_json = cJSON_GetObjectItem(carrier, "mcu");
		cJSON_ReplaceItemInObject(mcu_json, "type", cJSON_CreateString("colibri_mcu2"));
		e = env_description_from_json(json, why, sizeof(why));
		CHECK(e != NULL && !env_description_equal(d, e));
		env_description_free(e);
		cJSON_ReplaceItemInObject(mcu_json, "type", cJSON_CreateString("colibri_mcu6"));
		cJSON *detached = cJSON_DetachItemFromObject(carrier, "mcu");
		e = env_description_from_json(json, why, sizeof(why));
		CHECK(e != NULL && e->carrier->mcu == NULL && !env_description_equal(d, e));
		env_description_free(e);
		cJSON_AddItemToObject(carrier, "mcu", detached);
		cJSON *aiv = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "modules"), 1);
		cJSON_DeleteItemFromObject(aiv, "name");
		e = env_description_from_json(json, why, sizeof(why));
		CHECK(e != NULL && !env_description_equal(d, e));
		env_description_free(e);
		cJSON_AddStringToObject(aiv, "name", "Colibri AIV");
		cJSON_ReplaceItemInObject(cJSON_GetObjectItem(carrier, "power"), "ac",
					  cJSON_CreateTrue());
		e = env_description_from_json(json, why, sizeof(why));
		CHECK(e != NULL && !env_description_equal(d, e));
		env_description_free(e);
		cJSON_DeleteItemFromObject(json, "carrier");
		cJSON *builtin_module =
			cJSON_GetArrayItem(cJSON_GetObjectItem(json, "modules"), 0);
		cJSON_DeleteItemFromObject(builtin_module, "builtin");
		e = env_description_from_json(json, why, sizeof(why));
		CHECK(e != NULL && e->carrier == NULL && !env_description_equal(d, e));
		env_description_free(e);
	}
	env_description_free(d);
	cJSON_Delete(json);

	/* The emulator's built-in slot may be empty (emulator spec §2). */
	json = cJSON_Parse("{\"name\": \"e\", \"carrier\": {\"slots\": 7, \"builtin\": "
			   "[{\"slot\": 0}], \"power\": \"12 V\", \"mcu\": \"MCU6\"}}");
	d = env_description_from_json(json, why, sizeof(why));
	CHECK(d != NULL && d->carrier->power == NULL && d->carrier->comm_count == 0);
	CHECK(d != NULL && d->carrier->mcu == NULL); /* not an object: absent */
	env_description_free(d);
	cJSON_Delete(json);
	/* 31 I/O slots in all is the most. */
	json = cJSON_Parse("{\"name\": \"e\", \"carrier\": {\"slots\": 30, \"builtin\": "
			   "[{\"slot\": 30}]}, \"modules\": [{\"slot\": 30, \"builtin\": true}]}");
	d = env_description_from_json(json, why, sizeof(why));
	CHECK(d != NULL);
	env_description_free(d);
	cJSON_Delete(json);
}

/* Pictures (04 §1, plan task 75): view only, an unusable position or place
 * left out; compared by the identical-description check. */
static void test_pictures_json(void)
{
	const char *text =
		"{\"name\": \"colibri\","
		" \"carrier\": {\"type\": \"colibri_8\", \"picture\": \"https://x/c8.webp\","
		" \"slots\": 7, \"builtin\": [{\"slot\": 0}],"
		" \"places\": {\"slots\": [{\"slot\": 1, \"x\": 0.37, \"y\": 0.435, \"w\": 0.048,"
		" \"h\": 0.165}, {\"slot\": 2, \"x\": 0.42}, 5,"
		" {\"slot\": 3, \"x\": 0.47, \"y\": 0.435, \"w\": 0.048, \"h\": 0.165}],"
		" \"comm\": [{\"slot\": 1, \"x\": 0.26, \"y\": 0.435, \"w\": 0.048,"
		" \"h\": \"tall\"}],"
		" \"mcu\": {\"x\": 0.32, \"y\": 0.435, \"w\": 0.048, \"h\": 0.165}},"
		" \"comm\": [{\"slot\": 1, \"type\": \"colibri_ethernet\","
		" \"picture\": \"https://x/e.webp\"}],"
		" \"mcu\": {\"type\": \"colibri_mcu6\", \"picture\": 6}},"
		" \"outPorts\": [{\"id\": \"slot3-ai\", \"type\": \"number\","
		" \"addressable\": true, \"count\": 2},"
		" {\"id\": \"slot0-di\", \"type\": \"bool\"}],"
		" \"modules\": [{\"slot\": 0, \"builtin\": true, \"type\": \"carrier\","
		" \"ports\": [\"slot0-di\"], \"positions\": [{\"port\": \"slot0-di\", \"x\": 0.74,"
		" \"y\": 0.65}]},"
		" {\"slot\": 3, \"type\": \"colibri_aiv\", \"picture\": \"https://x/aiv.webp\","
		" \"ports\": [\"slot3-ai\"], \"positions\": [{\"port\": \"slot3-ai\", \"index\": 1,"
		" \"x\": 0.27, \"y\": 0.43}, {\"port\": \"slot3-ai\", \"index\": 2, \"x\": 0.72},"
		" {\"port\": 4, \"x\": 0, \"y\": 0}, {\"port\": \"elsewhere\", \"index\": 9,"
		" \"x\": 1.5, \"y\": 0.43}]}]}";
	cJSON *json = cJSON_Parse(text);
	char why[ENV_DESCRIPTION_MESSAGE_MAX] = "";
	HakkasEnvDescription *d = env_description_from_json(json, why, sizeof(why));
	CHECK_STREQ(why, "");
	CHECK(d != NULL && d->carrier != NULL);
	if (d != NULL && d->carrier != NULL) {
		const HakkasEnvCarrier *c = d->carrier;
		CHECK_STREQ(c->picture, "https://x/c8.webp");
		CHECK_STREQ(c->comm[0].picture, "https://x/e.webp");
		CHECK(c->mcu != NULL && c->mcu->picture == NULL); /* wrongly typed: absent */
		/* Entries missing a member, or with one of another type, are left out. */
		CHECK(c->io_place_count == 2 && c->io_places[1].slot == 3);
		CHECK(c->io_places[0].x == 0.37 && c->io_places[0].h == 0.165);
		CHECK(c->comm_place_count == 0);
		CHECK(c->mcu_place != NULL && c->mcu_place->x == 0.32);
		CHECK(d->modules[0].picture == NULL && d->modules[0].position_count == 1);
		CHECK(d->modules[0].positions[0].index == 0); /* not addressable */
		const HakkasEnvModule *aiv = &d->modules[1];
		CHECK_STREQ(aiv->picture, "https://x/aiv.webp");
		/* What a position says is not checked: an unknown port and an x
		 * beyond the picture are kept, for the frontend to skip. */
		CHECK(aiv->position_count == 2);
		CHECK_STREQ(aiv->positions[0].port, "slot3-ai");
		CHECK(aiv->positions[0].index == 1 && aiv->positions[0].y == 0.43);
		CHECK_STREQ(aiv->positions[1].port, "elsewhere");
		CHECK(aiv->positions[1].index == 9 && aiv->positions[1].x == 1.5);

		/* Its JSON form reads back as the same description. */
		cJSON *back = env_description_to_json(d);
		HakkasEnvDescription *e = env_description_from_json(back, why, sizeof(why));
		CHECK(e != NULL && env_description_equal(d, e));
		CHECK(e != NULL && e->carrier->io_place_count == 2);
		CHECK(e != NULL && e->carrier->mcu_place != NULL);
		CHECK(e != NULL && e->modules[1].position_count == 2);
		CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetArrayItem(
						  cJSON_GetObjectItem(back, "modules"), 0),
						  "positions"), 0), "index") == NULL);
		env_description_free(e);
		cJSON_Delete(back);

		/* Pictures, positions and places are static: a change is another
		 * environment. */
		cJSON *carrier = cJSON_GetObjectItem(json, "carrier");
		cJSON *aiv_json = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "modules"), 1);
		cJSON *pos = cJSON_GetArrayItem(cJSON_GetObjectItem(aiv_json, "positions"), 0);
		cJSON *place = cJSON_GetObjectItem(cJSON_GetObjectItem(carrier, "places"), "mcu");
		cJSON *comm0 = cJSON_GetArrayItem(cJSON_GetObjectItem(carrier, "comm"), 0);
		struct {
			cJSON *obj;
			const char *key;
			cJSON *changed, *original;
		} changes[] = {
			{carrier, "picture", cJSON_CreateString("https://x/c8b.webp"),
			 cJSON_CreateString("https://x/c8.webp")},
			{comm0, "picture", cJSON_CreateString("https://x/e2.webp"),
			 cJSON_CreateString("https://x/e.webp")},
			{aiv_json, "picture", cJSON_CreateString("https://x/aiv2.webp"),
			 cJSON_CreateString("https://x/aiv.webp")},
			{pos, "x", cJSON_CreateNumber(0.28), cJSON_CreateNumber(0.27)},
			{pos, "index", cJSON_CreateNumber(2), cJSON_CreateNumber(1)},
			{place, "w", cJSON_CreateNumber(0.05), cJSON_CreateNumber(0.048)},
		};
		for (size_t i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
			cJSON *obj = changes[i].obj;
			cJSON_ReplaceItemInObject(obj, changes[i].key, changes[i].changed);
			e = env_description_from_json(json, why, sizeof(why));
			CHECK(e != NULL && !env_description_equal(d, e));
			env_description_free(e);
			cJSON_ReplaceItemInObject(obj, changes[i].key, changes[i].original);
			e = env_description_from_json(json, why, sizeof(why));
			CHECK(e != NULL && env_description_equal(d, e));
			env_description_free(e);
		}
	}
	env_description_free(d);
	cJSON_Delete(json);

	/* More positions or places than the limits refuse the description. */
	static const struct {
		const char *json, *why;
	} bad[] = {
		{"{\"name\": \"e\", \"carrier\": {\"slots\": 2,"
		 " \"places\": {\"comm\": [{}, {}, {}, {}, {}]}}}",
		 "carrier: places: comm: more than 4 places"},
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		json = cJSON_Parse(bad[i].json);
		CHECK(json != NULL);
		d = env_description_from_json(json, why, sizeof(why));
		CHECK(d == NULL);
		CHECK_STREQ(why, bad[i].why);
		cJSON_Delete(json);
	}
	cJSON *many = cJSON_CreateObject();
	cJSON_AddStringToObject(many, "name", "e");
	cJSON *mods = cJSON_AddArrayToObject(many, "modules");
	cJSON *carrier = cJSON_AddObjectToObject(many, "carrier");
	cJSON *slots = cJSON_AddArrayToObject(cJSON_AddObjectToObject(carrier, "places"), "slots");
	cJSON_AddNumberToObject(carrier, "slots", 31);
	for (int i = 0; i < 32; i++)
		cJSON_AddItemToArray(slots, cJSON_CreateObject());
	d = env_description_from_json(many, why, sizeof(why));
	CHECK(d == NULL);
	CHECK_STREQ(why, "carrier: places: slots: more than 31 places");
	cJSON_DeleteItemFromObject(many, "carrier");
	for (int k = 0; k < 2; k++) {
		cJSON *m = cJSON_CreateObject();
		cJSON *list = cJSON_AddArrayToObject(m, "positions");
		for (int i = 0; i < 600; i++) {
			cJSON *p = cJSON_CreateObject();
			cJSON_AddStringToObject(p, "port", "x");
			cJSON_AddNumberToObject(p, "x", 0.5);
			cJSON_AddNumberToObject(p, "y", 0.5);
			cJSON_AddItemToArray(list, p);
		}
		cJSON_AddItemToArray(mods, m);
	}
	d = env_description_from_json(many, why, sizeof(why));
	CHECK(d == NULL);
	CHECK_STREQ(why, "more than 1024 positions");
	cJSON_Delete(many);
}

/* ---- The bindings file (04 §4) ------------------------------------------------------ */

static void bad_file(const char *text, const char *want)
{
	BindingsFile f;
	char why[ENVIRONMENT_MESSAGE_MAX] = "";
	CHECK(bindings_file_parse(text, strlen(text), "b.json", &f, why, sizeof(why)) ==
	      BINDINGS_FILE_MALFORMED);
	CHECK(f.count == 0 && f.entries == NULL && f.environment == NULL);
	if (strcmp(why, want) != 0)
		fprintf(stderr, "file: %s\n", text);
	CHECK_STREQ(why, want);
}

static void test_bindings_file(void)
{
	const char *text = "{\"environment\": \"emulator\", \"bindings\": ["
			   "{\"port\": \"temperature\", \"env\": \"analog-in\", \"index\": 1},"
			   "{\"port\": \"lamp.1\", \"env\": \"lamp\", \"index\": null,"
			   " \"note\": \"x\"}],"
			   " \"version\": 2}\n";
	BindingsFile f;
	logs[0] = '\0';
	CHECK(bindings_file_parse(text, strlen(text), "b.json", &f, NULL, 0) == BINDINGS_FILE_OK);
	CHECK_STREQ(f.environment, "emulator");
	CHECK(f.count == 2);
	CHECK_STREQ(f.entries[0].port, "temperature");
	CHECK_STREQ(f.entries[0].env, "analog-in");
	CHECK(f.entries[0].index == 1);
	CHECK_STREQ(f.entries[1].port, "lamp.1");
	CHECK(f.entries[1].index == 0);
	CHECK_STREQ(logs, "warn b.json: unknown member \"version\" ignored\n"
			  "warn b.json: bindings[1]: unknown member \"note\" ignored\n");
	bindings_file_free(&f);
	CHECK(f.count == 0 && f.entries == NULL);
	text = "{\"environment\": \"e\", \"bindings\": []}";
	CHECK(bindings_file_parse(text, strlen(text), "b.json", &f, NULL, 0) == BINDINGS_FILE_OK);
	CHECK(f.count == 0);
	bindings_file_free(&f);

	bad_file("", "not a bindings file: expected a JSON object");
	bad_file("[]", "not a bindings file: expected a JSON object");
	bad_file("{\"environment\": \"e\", \"bindings\": []} x",
		 "not a bindings file: expected a JSON object");
	bad_file("{\"bindings\": []}", "environment: expected a non-empty string");
	bad_file("{\"environment\": \"\", \"bindings\": []}",
		 "environment: expected a non-empty string");
	bad_file("{\"environment\": \"e\"}", "bindings: expected an array");
	bad_file("{\"environment\": \"e\", \"bindings\": [1]}", "bindings[0]: expected an object");
	bad_file("{\"environment\": \"e\", \"bindings\": [{\"env\": \"x\"}]}",
		 "bindings[0]: port: expected a non-empty string");
	bad_file("{\"environment\": \"e\", \"bindings\": [{\"port\": \"\", \"env\": \"x\"}]}",
		 "bindings[0]: port: expected a non-empty string");
	bad_file("{\"environment\": \"e\", \"bindings\": [{\"port\": \"p\"}]}",
		 "bindings[0]: env: expected a non-empty string");
	/* The earlier form, which bound block ports, is not read (plan task 70). */
	bad_file("{\"environment\": \"e\", \"bindings\": [{\"port\": \"p\", \"env\": \"x\"},"
		 " {\"block\": \"b\", \"port\": \"p\", \"env\": \"x\"}]}",
		 "bindings[1]: block: block ports are not bound; bind a process port");
	static const char *const bad_index[] = {"0", "-1", "1.5", "\"1\"", "true", "1e10"};
	for (size_t i = 0; i < sizeof(bad_index) / sizeof(bad_index[0]); i++) {
		char file[256];
		snprintf(file, sizeof(file),
			 "{\"environment\": \"e\", \"bindings\": [{\"port\": \"p\","
			 " \"env\": \"x\"}, {\"port\": \"p\", \"env\": \"x\", \"index\": %s}]}",
			 bad_index[i]);
		bad_file(file, "bindings[1]: index: expected an integer of at least 1");
	}
}

/* ---- Opening ------------------------------------------------------------------------ */

static void test_open(void)
{
	build(false);
	CHECK(fake.starts == 1 && fake.sink != NULL);
	CHECK(environment_description(&env) == &fake.desc);
	CHECK(environment_wall_ms(&env) == 1700000000000);
	environment_close(&env);
	CHECK(fake.stops == 1);
	CHECK(environment_description(&env) == NULL);
	CHECK(environment_wall_ms(&env) == 777); /* the platform's clock */
	environment_close(&env);		 /* nothing more */
	CHECK(fake.stops == 1);

	/* No wall_ms: the platform's clock. */
	HakkasEnvironment spi = fake_spi();
	spi.wall_ms = NULL;
	CHECK(environment_open(&env, &rt, &spi, push_direct));
	CHECK(environment_wall_ms(&env) == 777);
	environment_close(&env);

	/* Refusals: nothing started, nothing left open. */
	log_set_threshold(LOG_LEVEL_FINE);
	logs[0] = '\0';
	fake.starts = 0;
	fake.describe_null = true;
	spi = fake_spi();
	CHECK(!environment_open(&env, &rt, &spi, push_direct));
	CHECK_STREQ(logs, "error the environment did not describe itself\n");
	fake.describe_null = false;
	HakkasEnvPort dup[2] = {out_ports[0], out_ports[0]};
	fake.desc.out_ports = dup;
	fake.desc.out_port_count = 2;
	fake.desc.module_count = 0;
	logs[0] = '\0';
	CHECK(!environment_open(&env, &rt, &spi, push_direct));
	CHECK_STREQ(logs, "error environment description: duplicate port id \"ain\"\n");
	fake.desc = description();
	fake.start_rc = -5;
	logs[0] = '\0';
	CHECK(!environment_open(&env, &rt, &spi, push_direct));
	CHECK(strstr(logs, "error the environment did not start (-5)\n") != NULL);
	CHECK(fake.starts == 1 && environment_description(&env) == NULL);
	spi.write = NULL;
	logs[0] = '\0';
	CHECK(!environment_open(&env, &rt, &spi, push_direct));
	CHECK_STREQ(logs, "error the environment's interface is incomplete\n");
	CHECK(fake.starts == 1);
	log_set_threshold(LOG_LEVEL_WARN);

	/* The sink refuses what the description does not have. */
	fake.start_rc = 0;
	spi = fake_spi();
	CHECK(environment_open(&env, &rt, &spi, push_direct));
	HakkasValue v = linkable_from_f64(1);
	CHECK(!hakkas_env_sink_push(fake.sink, 3, 0, &v)); /* no such port */
	CHECK(!hakkas_env_sink_push(fake.sink, -1, 0, &v));
	CHECK(!hakkas_env_sink_push(fake.sink, 0, 0, &v)); /* addressable: 1..4 */
	CHECK(!hakkas_env_sink_push(fake.sink, 0, 5, &v));
	CHECK(!hakkas_env_sink_push(fake.sink, 2, 1, &v)); /* not addressable: 0 */
	CHECK(!hakkas_env_sink_push(fake.sink, 0, 1, NULL));
	Linkable s;
	CHECK(linkable_from_string(&s, "1", 1));
	CHECK(!hakkas_env_sink_push(fake.sink, 0, 1, &s)); /* scalars only */
	linkable_clear(&s);
	CHECK(work_queue_len(&rt.queue) == 0);
	v = linkable_from_i64(3);
	CHECK(hakkas_env_sink_push(fake.sink, 2, 0, &v));
	CHECK(work_queue_len(&rt.queue) == 1);
	/* hakkas_env_sink_current only within start. */
	CHECK(!hakkas_env_sink_current(fake.sink, 2, 0, &v));
	CHECK(work_queue_len(&rt.queue) == 1);
	teardown();
}

/* ---- Sensors ------------------------------------------------------------------------ */

static void test_sensor_values(void)
{
	build(true); /* ain[1] = 1.5 and din[2] = true were delivered in start */
	CHECK(work_queue_len(&rt.queue) == 0); /* taken at once, not queued */
	CHECK(environment_sensor_value(&env, 0, 1) != NULL &&
	      linkable_to_f64(environment_sensor_value(&env, 0, 1)) == 1.5);
	CHECK(environment_sensor_value(&env, 0, 2) == NULL);
	CHECK(environment_sensor_value(&env, 1, 2) != NULL &&
	      environment_sensor_value(&env, 1, 2)->type == LINKABLE_BOOL);

	/* A binding delivers the current value at once, after its event. */
	CHECK(bind("P", "a.in1", "ain", 1) == ENVIRONMENT_OK);
	CHECK_STREQ(events, "P:+bind a.in1<ain[1];");
	CHECK(num("P", "a", "in1") == 1.5);
	CHECK(out("P", "a") == 0); /* P is stopped: stored only */
	/* A channel without a value yet leaves the input at its default. */
	CHECK(bind("P", "a.in2", "ain", 2) == ENVIRONMENT_OK);
	CHECK(num("P", "a", "in2") == 0);
	/* One channel feeds inputs in several processes. */
	CHECK(bind("Q", "c.in1", "ain", 1) == ENVIRONMENT_OK);
	CHECK(num("Q", "c", "in1") == 1.5);
	CHECK(runtime_binding_count(&rt) == 3);

	/* Changes go through the queue, to every bound input. */
	start("P");
	CHECK(out("P", "a") == 1.5);
	sense(0, 1, linkable_from_f64(2));
	CHECK(num("P", "a", "in1") == 1.5);
	drain();
	CHECK(num("P", "a", "in1") == 2 && num("Q", "c", "in1") == 2);
	CHECK(out("P", "a") == 2);
	CHECK(out("Q", "c") == 0); /* Q is stopped: stored only */
	start("Q");
	CHECK(out("Q", "c") == 2);

	/* The engine-wide change threshold (0.05, 04 §0 rule 4): against the
	 * last value delivered, so drift adds up. */
	sense(0, 1, linkable_from_f64(2.03));
	drain();
	CHECK(num("P", "a", "in1") == 2);
	sense(0, 1, linkable_from_f64(2.06));
	drain();
	CHECK(num("P", "a", "in1") == 2.06);
	/* The port's type: an i64 or a bool is a number on a number port. */
	sense(0, 1, linkable_from_i64(4));
	drain();
	CHECK(in("P", "a", "in1")->type == LINKABLE_F64 && num("P", "a", "in1") == 4);
	sense(0, 2, linkable_from_bool(true));
	drain();
	CHECK(num("P", "a", "in2") == 1 && out("P", "a") == 5);

	/* Bool ports: every change. A value for a channel nobody binds is its
	 * current value, which a later binding delivers. */
	sense(1, 1, linkable_from_f64(3));
	drain();
	CHECK(environment_sensor_value(&env, 1, 1)->type == LINKABLE_BOOL);
	CHECK(bind("P", "b.in1", "din", 1) == ENVIRONMENT_OK);
	CHECK(in("P", "b", "in1")->type == LINKABLE_BOOL && num("P", "b", "in1") == 1);
	CHECK(out("P", "b") == 1);
	sense(1, 1, linkable_from_bool(false));
	drain();
	CHECK(num("P", "b", "in1") == 0);

	/* A stopped process stores; its start computes from what is there. */
	stop("P");
	sense(0, 1, linkable_from_f64(10));
	drain();
	CHECK(num("P", "a", "in1") == 10 && out("P", "a") == 5);
	start("P");
	CHECK(out("P", "a") == 11);

	/* A value for no channel (from the queue directly) is discarded. */
	work_queue_push_io(&rt.queue, 9, 1, linkable_from_f64(1));
	work_queue_push_io(&rt.queue, 0, 9, linkable_from_f64(1));
	drain();
	CHECK(env.discarded == 2);
	teardown();
}

/* No fan-in, both ways (01 §2.2): "Input already connected". */
static void test_fan_in(void)
{
	build(true);
	init("P", "a", "in2", 3);
	CHECK(graphs_add_edge(&gs, "P", "b", "out", "a", "in1", NULL, NULL) == GRAPH_OK);
	REFUSED(bind("P", "a.in1", "ain", 1), "Input already connected: a.in1");
	REFUSED(bind("P", "a.in2", "ain", 1), "Input already connected: a.in2");
	CHECK(bind("P", "b.in1", "ain", 1) == ENVIRONMENT_OK);
	/* A second binding of the same input, even to the same channel. */
	REFUSED(bind("P", "b.in1", "ain", 1), "Input already connected: b.in1");
	REFUSED(bind("P", "b.in1", "ain", 2), "Input already connected: b.in1");
	/* An edge, an initial or a link onto the bound input. */
	GraphsMessage gm;
	CHECK(graphs_add_edge(&gs, "P", "a", "out", "b", "in1", NULL, &gm) ==
	      GRAPH_ERR_INPUT_CONNECTED);
	CHECK_STREQ(gm.text, "Input already connected: b.in1");
	Linkable one = linkable_from_f64(1);
	CHECK(graphs_add_initial(&gs, "P", "b", "in1", &one, &gm) == GRAPH_ERR_INPUT_CONNECTED);
	CHECK_STREQ(gm.text, "Input already connected: b.in1");
	CHECK(graphs_add_port(&gs, "P", GRAPH_INPORT, "pi", "b", "in1", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "Q", GRAPH_OUTPORT, "qo", "c", "out", NULL) == GRAPH_OK);
	CHECK(graphs_add_link(&gs, "Q", "qo", "P", "pi", &gm) == GRAPH_ERR_INPUT_CONNECTED);
	CHECK_STREQ(gm.text, "Input already connected: P.pi");
	/* And a sensor onto a linked input. */
	CHECK(graphs_add_port(&gs, "Q", GRAPH_INPORT, "qi", "c", "in2", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "P", GRAPH_OUTPORT, "po", "a", "out", NULL) == GRAPH_OK);
	CHECK(graphs_add_link(&gs, "P", "po", "Q", "qi", NULL) == GRAPH_OK);
	REFUSED(bind("Q", "c.in2", "temp", 0), "Input already connected: c.in2");
	teardown();
}

/* ---- Actuators ---------------------------------------------------------------------- */

static void test_actuator_writes(void)
{
	build(false);
	init("P", "a", "in1", 1);
	/* Bound while stopped: nothing written until the process starts. */
	CHECK(bind("P", "a.out", "aout", 2) == ENVIRONMENT_OK);
	CHECK(bind("P", "a.out", "relay", 1) == ENVIRONMENT_OK); /* one output, two */
	CHECK(bind("P", "b.out", "lamp", 0) == ENVIRONMENT_OK);
	CHECK_STREQ(events, "P:+bind a.out>aout[2];P:+bind a.out>relay[1];P:+bind b.out>lamp[0];");
	CHECK_STREQ(fake.writes, "");
	/* The start push writes every bound output, converted to the port's
	 * type. */
	start("P");
	CHECK_STREQ(fake.writes, "aout[2]=1;relay[1]=true;lamp=false;");
	fake.writes[0] = '\0';
	/* Every propagation; below the threshold, none. */
	init("P", "a", "in1", 2.5);
	CHECK_STREQ(fake.writes, "aout[2]=2.5;relay[1]=true;");
	fake.writes[0] = '\0';
	init("P", "a", "in1", 2.52);
	CHECK_STREQ(fake.writes, "");
	init("P", "a", "in1", 0);
	CHECK_STREQ(fake.writes, "aout[2]=0;relay[1]=false;");
	fake.writes[0] = '\0';
	/* Stopped: nothing is written; the actuator keeps its last value. */
	stop("P");
	init("P", "a", "in1", 7);
	CHECK_STREQ(fake.writes, "");
	start("P");
	CHECK_STREQ(fake.writes, "aout[2]=7;relay[1]=true;lamp=false;");
	fake.writes[0] = '\0';

	/* One driver per actuator channel, across processes. */
	REFUSED(bind("Q", "c.out", "aout", 2), "Actuator already driven: aout[2]");
	REFUSED(bind("P", "b.out", "lamp", 0), "Actuator already driven: lamp");
	/* A binding on a running process writes the output's value at once. */
	start("Q");
	init("Q", "c", "in1", 4);
	CHECK(bind("Q", "c.out", "aout", 1) == ENVIRONMENT_OK);
	CHECK_STREQ(fake.writes, "aout[1]=4;");
	fake.writes[0] = '\0';

	/* Unbinding: the actuator keeps its value (nothing written), the
	 * output no longer writes. */
	CHECK(unbind("P", "a.out", "relay", 1) == ENVIRONMENT_OK);
	CHECK_STREQ(fake.writes, "");
	init("P", "a", "in1", 8);
	CHECK_STREQ(fake.writes, "aout[2]=8;");
	/* The channel is free for another driver. */
	CHECK(bind("Q", "c.out", "relay", 1) == ENVIRONMENT_OK);
	teardown();
}

/* Every channel's current value (plan task 76): sensors first, then
 * actuators (the last value written), with change numbers, reported to the
 * watcher after the bound inputs have them. */
static char changes[256];

static void on_change(void *ctx, uint32_t ch)
{
	Environment *e = ctx;
	const char *id;
	uint32_t index;
	const Linkable *v;
	CHECK(environment_channel(e, ch, &id, &index, &v));
	append(changes, sizeof(changes), "%u:%s[%u]=%g@%u;", (unsigned)ch, id, (unsigned)index,
	       linkable_to_f64(v), (unsigned)environment_seq(e));
}

static void test_channel_values(void)
{
	build(true); /* ain[1] = 1.5 and din[2] = true, in start */
	changes[0] = '\0';
	environment_watch(&env, on_change, &env);
	/* ain 0-3, din 4-5, temp 6; aout 7-8, relay 9-11, lamp 12. */
	CHECK(environment_channel_count(&env) == 13);
	CHECK(environment_seq(&env) == 2);
	const char *id;
	uint32_t index;
	const Linkable *v;
	CHECK(environment_channel(&env, 0, &id, &index, &v));
	CHECK_STREQ(id, "ain");
	CHECK(index == 1 && linkable_to_f64(v) == 1.5);
	CHECK(environment_channel(&env, 5, &id, &index, &v));
	CHECK_STREQ(id, "din");
	CHECK(index == 2 && v->type == LINKABLE_BOOL);
	CHECK(!environment_channel(&env, 1, &id, &index, &v)); /* no value yet */
	CHECK(!environment_channel(&env, 13, &id, &index, &v));
	CHECK(environment_changed_after(&env, 5, 1) && !environment_changed_after(&env, 0, 1));
	CHECK(!environment_changed_after(&env, 1, 0)); /* never changed */

	/* A sensor's change, after the bound inputs have it; none below the
	 * threshold. */
	CHECK(bind("P", "a.in1", "ain", 1) == ENVIRONMENT_OK);
	sense(2, 0, linkable_from_f64(20));
	sense(0, 1, linkable_from_f64(1.52));
	drain();
	CHECK_STREQ(changes, "6:temp[0]=20@3;");
	changes[0] = '\0';

	/* An actuator's: what is written, filtered like a sensor's. */
	init("P", "a", "in2", 1); /* a.out = ain[1] + 1 = 2.5 */
	CHECK(bind("P", "a.out", "aout", 2) == ENVIRONMENT_OK);
	CHECK(bind("P", "b.out", "lamp", 0) == ENVIRONMENT_OK);
	CHECK(!environment_channel(&env, 8, &id, &index, &v)); /* not written yet */
	start("P");
	CHECK_STREQ(changes, "8:aout[2]=2.5@4;12:lamp[0]=0@5;");
	changes[0] = '\0';
	CHECK(environment_channel(&env, 8, &id, &index, &v));
	CHECK_STREQ(id, "aout");
	CHECK(index == 2 && linkable_to_f64(v) == 2.5);
	stop("P");
	start("P"); /* written again, unchanged: no change */
	CHECK_STREQ(changes, "");
	init("P", "a", "in2", 3);
	CHECK_STREQ(changes, "8:aout[2]=4.5@6;");
	/* Unbound, it keeps its last value. */
	CHECK(unbind("P", "a.out", "aout", 2) == ENVIRONMENT_OK);
	CHECK(environment_channel(&env, 8, &id, &index, &v) && linkable_to_f64(v) == 4.5);

	/* Change numbers wrap past 0, which stands for none. */
	env.seq = UINT32_MAX;
	changes[0] = '\0';
	sense(2, 0, linkable_from_f64(30));
	drain();
	CHECK_STREQ(changes, "6:temp[0]=30@1;");
	CHECK(environment_changed_after(&env, 6, UINT32_MAX));
	environment_watch(&env, NULL, NULL);
	teardown();
	CHECK(environment_channel_count(&env) == 0);
}

/* ---- Bindings files (04 §4) --------------------------------------------------------- */

static enum environment_status apply(const char *graph, const EnvBindingRequest *reqs, size_t n,
				     size_t *applied, size_t *skipped)
{
	return environment_bind_all(&env, graphs_find(&gs, graph), reqs, n, "b.json", applied,
				    skipped, msg, sizeof(msg));
}

/* A file whose entry `bad` is skipped with the warning want: the first
 * entry, a.in1 <- ain[1], is still made (warn and skip, 04 §4). */
static void skipped_file(const EnvBindingRequest *reqs, size_t n, size_t bad, const char *want)
{
	events[0] = '\0';
	logs[0] = '\0';
	size_t applied = 9, skipped = 9;
	CHECK(apply("P", reqs, n, &applied, &skipped) == ENVIRONMENT_OK);
	CHECK_STREQ(msg, "");
	CHECK(applied == n - 1 && skipped == 1);
	char line[256];
	snprintf(line, sizeof(line), "warn b.json: bindings[%zu]: %s: skipped\n", bad, want);
	CHECK_STREQ(logs, line);
	CHECK(strncmp(events, "P:+bind a.in1<ain[1];", 21) == 0);
	CHECK(runtime_binding_count(&rt) == n - 1);
	CHECK(unbind("P", "a.in1", "ain", 1) == ENVIRONMENT_OK);
	for (size_t i = 1; i < n; i++)
		if (i != bad)
			CHECK(unbind("P", reqs[i].port, reqs[i].env, reqs[i].index) ==
			      ENVIRONMENT_OK);
	CHECK(runtime_binding_count(&rt) == 0);
}

static void test_bindings_apply(void)
{
	build(true);
	drain();
	EnvBindingRequest ok[] = {
		{"a.in1", "ain", 1},
		{"a.out", "relay", 3},
		{"b.in1", "ain", 9},	 /* beyond the count: skipped */
		{"b.in2", "spare", 0}, /* not in this environment: skipped */
		{"b.out", "lamp", 0},
	};
	size_t applied, skipped;
	CHECK(apply("P", ok, 5, &applied, &skipped) == ENVIRONMENT_OK);
	CHECK(applied == 3 && skipped == 2);
	CHECK_STREQ(msg, "");
	CHECK_STREQ(events, "P:+bind a.in1<ain[1];P:+bind a.out>relay[3];P:+bind b.out>lamp[0];");
	CHECK_STREQ(logs, "warn b.json: bindings[2]: Index out of range: ain[9]: skipped\n"
			  "warn b.json: bindings[3]: Unknown environment port: spare: skipped\n");
	CHECK(num("P", "a", "in1") == 1.5);
	CHECK(num("P", "b", "in1") == 0); /* inert */
	teardown();

	/* Every other refusal skips its entry too; the rest are made. */
	build(false);
	EnvBindingRequest r1[] = {{"a.in1", "ain", 1}, {"x.in1", "ain", 2}};
	skipped_file(r1, 2, 1, "Unknown port: x.in1");
	EnvBindingRequest r2[] = {{"a.in1", "ain", 1}, {"a.in3", "ain", 2}};
	skipped_file(r2, 2, 1, "Unknown port: a.in3");
	EnvBindingRequest r3[] = {{"a.in1", "ain", 1}, {"a.out", "ain", 2}};
	skipped_file(r3, 2, 1, "Invalid binding direction");
	EnvBindingRequest r4[] = {{"a.in1", "ain", 1}, {"a.in2", "relay", 1}};
	skipped_file(r4, 2, 1, "Invalid binding direction");
	/* Against the file's earlier entries: one source per input, one
	 * driver per actuator channel. */
	EnvBindingRequest r5[] = {{"a.in1", "ain", 1}, {"a.in1", "din", 1}};
	skipped_file(r5, 2, 1, "Input already connected: a.in1");
	EnvBindingRequest r6[] = {
		{"a.in1", "ain", 1}, {"a.out", "relay", 2}, {"b.out", "relay", 2}};
	skipped_file(r6, 3, 2, "Actuator already driven: relay[2]");
	EnvBindingRequest r7[] = {{"a.in1", "ain", 1}, {"a.in2", "ain", 0}};
	skipped_file(r7, 2, 1, "Index required: ain");
	EnvBindingRequest r8[] = {{"a.in1", "ain", 1}, {"a.in2", "temp", 1}};
	skipped_file(r8, 2, 1, "No index expected: temp");
	EnvBindingRequest r9[] = {{"a.in1", "ain", 1}, {"", "temp", 0}};
	skipped_file(r9, 2, 1, "Invalid id");
	/* Against what is already there: an input with a source, a driven
	 * channel (in another process). */
	init("P", "a", "in2", 1);
	EnvBindingRequest r10[] = {{"a.in1", "ain", 1}, {"a.in2", "temp", 0}};
	skipped_file(r10, 2, 1, "Input already connected: a.in2");
	CHECK(bind("Q", "c.out", "aout", 1) == ENVIRONMENT_OK);
	EnvBindingRequest r11[] = {{"a.out", "aout", 1}};
	events[0] = '\0';
	logs[0] = '\0';
	CHECK(apply("P", r11, 1, &applied, &skipped) == ENVIRONMENT_OK);
	CHECK(applied == 0 && skipped == 1);
	CHECK_STREQ(logs, "warn b.json: bindings[0]: Actuator already driven: aout[1]: skipped\n");
	CHECK_STREQ(events, "");
	teardown();

	/* Without an environment, a file is refused: nothing to bind to. */
	build(false);
	environment_close(&env);
	CHECK(apply("P", ok, 1, NULL, NULL) == ENVIRONMENT_REFUSED);
	CHECK_STREQ(msg, "No environment");
	REFUSED(bind("P", "a.in1", "ain", 1), "No environment");
	teardown();
}

/* The single-binding refusals (the C API's) and unbinding. */
static void test_bind_refusals(void)
{
	build(false);
	REFUSED(bind("P", "a.in1", "nope", 0), "Unknown environment port: nope");
	REFUSED(bind("P", "a.in1", "ain", 5), "Index out of range: ain[5]");
	REFUSED(bind("P", "a.in1", "ain", 0), "Index required: ain");
	REFUSED(bind("P", "a.in1", "temp", 2), "No index expected: temp");
	REFUSED(bind("P", "", "temp", 0), "Invalid id");
	REFUSED(bind("P", "a.in1", NULL, 0), "Invalid id");
	REFUSED(bind("P", "zz.in1", "temp", 0), "Unknown port: zz.in1");
	REFUSED(bind("P", "a.zz", "temp", 0), "Unknown port: a.zz");
	REFUSED(bind("P", "a.out", "temp", 0), "Invalid binding direction");
	REFUSED(unbind("P", "a.in1", "temp", 0), "No such binding");
	CHECK(bind("P", "a.in1", "temp", 0) == ENVIRONMENT_OK);
	REFUSED(unbind("P", "a.in1", "ain", 1), "No such binding");
	REFUSED(unbind("P", "b.in1", "temp", 0), "No such binding");
	REFUSED(unbind("P", "a.in1", "nope", 0), "Unknown environment port: nope");
	teardown();
}

/* ---- Bindings ending ---------------------------------------------------------------- */

static void test_bindings_end(void)
{
	build(true);
	drain();
	start("P");
	init("P", "b", "in2", 3);
	CHECK(bind("P", "a.in1", "ain", 1) == ENVIRONMENT_OK);
	CHECK(bind("P", "a.out", "relay", 1) == ENVIRONMENT_OK);
	CHECK(graphs_add_edge(&gs, "P", "a", "out", "b", "in1", NULL, NULL) == GRAPH_OK);
	drain();
	CHECK(num("P", "a", "in1") == 1.5 && num("P", "b", "in1") == 1.5);
	fake.writes[0] = '\0';

	/* Unbinding a sensor: the input reverts to its default, which
	 * propagates (severing, 01 §2.2); a queued value reaches only the
	 * inputs bound when it is drained. */
	sense(0, 1, linkable_from_f64(9));
	events[0] = '\0';
	CHECK(unbind("P", "a.in1", "ain", 1) == ENVIRONMENT_OK);
	CHECK_STREQ(events, "P:-bind a.in1<ain[1];");
	CHECK(num("P", "a", "in1") == 0 && out("P", "a") == 0);
	CHECK_STREQ(fake.writes, "relay[1]=false;");
	drain();
	CHECK(num("P", "a", "in1") == 0 && num("P", "b", "in1") == 0);
	/* The input is free again. */
	init("P", "a", "in1", 2);
	CHECK(bind("P", "a.in1", "ain", 1) == ENVIRONMENT_REFUSED);

	/* Removing the block: its edges, then its process ports, each with its
	 * bindings (plan task 70); nothing reverts or is written. */
	CHECK(graphs_remove_initial(&gs, "P", "a", "in1", NULL) == GRAPH_OK);
	CHECK(bind("P", "a.in1", "ain", 1) == ENVIRONMENT_OK);
	fake.writes[0] = '\0';
	events[0] = '\0';
	CHECK(graphs_remove_node(&gs, "P", "a", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "P:-edge;P:-bind a.in1<ain[1];P:-bind a.out>relay[1];P:-node a;");
	CHECK_STREQ(fake.writes, "");
	CHECK(runtime_binding_count(&rt) == 0);
	sense(0, 1, linkable_from_f64(5)); /* to nobody */
	drain();

	/* Deleting a process: after its stop, before its deletion. */
	CHECK(bind("P", "b.in1", "ain", 1) == ENVIRONMENT_OK);
	CHECK(bind("P", "b.out", "lamp", 0) == ENVIRONMENT_OK);
	CHECK(bind("Q", "c.in1", "ain", 1) == ENVIRONMENT_OK);
	fake.writes[0] = '\0';
	events[0] = '\0';
	CHECK(graphs_delete_graph(&gs, "P", "P", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "P:stopped;P:-bind b.in1<ain[1];P:-bind b.out>lamp[0];P:-graph;");
	CHECK_STREQ(fake.writes, "");
	CHECK(runtime_binding_count(&rt) == 1);
	CHECK(runtime_binding_at(&rt, 0)->graph == graphs_find(&gs, "Q"));
	/* The lamp may be driven by another process now. */
	CHECK(bind("Q", "c.out", "lamp", 0) == ENVIRONMENT_OK);

	/* Renaming the block keeps its bindings. */
	CHECK(graphs_rename_node(&gs, "Q", "c", "c2", NULL) == GRAPH_OK);
	sense(0, 1, linkable_from_f64(6));
	drain();
	CHECK(num("Q", "c2", "in1") == 6);

	/* Renaming the process port keeps them too, under the new name, with
	 * no binding event (unlike a link, plan task 70). */
	events[0] = '\0';
	CHECK(graphs_rename_port(&gs, "Q", GRAPH_INPORT, "c.in1", "temperature", NULL) == GRAPH_OK);
	CHECK(strstr(events, "bind") == NULL);
	CHECK_STREQ(runtime_binding_at(&rt, 0)->name, "temperature");
	sense(0, 1, linkable_from_f64(7));
	drain();
	CHECK(num("Q", "c2", "in1") == 7);
	REFUSED(unbind("Q", "c.in1", "ain", 1), "No such binding");
	/* An outport of the same name is another port: the actuator's stays. */
	CHECK(graphs_rename_port(&gs, "Q", GRAPH_OUTPORT, "c.out", "temperature", NULL) ==
	      GRAPH_OK);
	CHECK_STREQ(runtime_binding_at(&rt, 1)->name, "temperature");
	CHECK(runtime_binding_at(&rt, 1)->actuator);
	CHECK(unbind("Q", "temperature", "lamp", 0) == ENVIRONMENT_OK);
	CHECK(runtime_binding_at(&rt, 0)->name != NULL && !runtime_binding_at(&rt, 0)->actuator);

	/* Removing the process port ends its binding, before the port's event;
	 * the input reverts to its default, like any severed input. */
	start("Q");
	events[0] = '\0';
	CHECK(graphs_remove_port(&gs, "Q", GRAPH_INPORT, "temperature", NULL) == GRAPH_OK);
	CHECK_STREQ(events, "Q:-bind temperature<ain[1];");
	CHECK(num("Q", "c2", "in1") == 0 && out("Q", "c2") == 0);
	CHECK(runtime_binding_count(&rt) == 0);
	/* Bindings left at shutdown go silently (asan). */
	CHECK(bind("Q", "c.in2", "ain", 2) == ENVIRONMENT_OK);
	teardown();
}

/* The environment port decides the direction: an inport and an outport may
 * share a name (04 §4, plan task 70). */
static void test_bind_direction(void)
{
	build(true);
	drain();
	CHECK(graphs_add_port(&gs, "P", GRAPH_INPORT, "x", "a", "in1", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "P", GRAPH_OUTPORT, "x", "a", "out", NULL) == GRAPH_OK);
	CHECK(bind("P", "x", "ain", 1) == ENVIRONMENT_OK);   /* the inport */
	CHECK(bind("P", "x", "relay", 2) == ENVIRONMENT_OK); /* the outport */
	CHECK_STREQ(events, "P:+bind x<ain[1];P:+bind x>relay[2];");
	CHECK(num("P", "a", "in1") == 1.5);
	/* Only the other direction has the name: the wrong direction. */
	CHECK(graphs_add_port(&gs, "P", GRAPH_INPORT, "only-in", "b", "in1", NULL) == GRAPH_OK);
	REFUSED(bind("P", "only-in", "relay", 3), "Invalid binding direction");
	REFUSED(bind("P", "nope", "relay", 3), "Unknown port: nope");
	/* A bound outport may also feed a link; a bound inport cannot be linked. */
	CHECK(graphs_add_port(&gs, "Q", GRAPH_INPORT, "qi", "c", "in1", NULL) == GRAPH_OK);
	CHECK(graphs_add_link(&gs, "P", "x", "Q", "qi", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "Q", GRAPH_OUTPORT, "qo", "c", "out", NULL) == GRAPH_OK);
	GraphsMessage gm;
	CHECK(graphs_add_link(&gs, "Q", "qo", "P", "x", &gm) == GRAPH_ERR_INPUT_CONNECTED);
	teardown();
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(log_add_sink(log_sink, NULL));
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	RUN(test_description_validate);
	RUN(test_description_json);
	RUN(test_carrier_json);
	RUN(test_pictures_json);
	RUN(test_bindings_file);
	RUN(test_open);
	RUN(test_sensor_values);
	RUN(test_fan_in);
	RUN(test_actuator_writes);
	RUN(test_channel_values);
	RUN(test_bindings_apply);
	RUN(test_bind_refusals);
	RUN(test_bindings_end);
	RUN(test_bind_direction);
	block_types_reset();
	return test_result();
}
