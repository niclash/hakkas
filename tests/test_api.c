/*
 * The local C API (hakkas.h, 02 §6) over the whole engine in-process (the
 * end-to-end helpers, e2e.h): every function's success path and its
 * refusals with the exact wire messages (02 §3.4.1, §1.2); the typed events
 * delivered to the host subscriber in order; and that they are the wire's
 * events — each typed event is rendered here, from its fields alone, into
 * the 02 §3 message, and the sequence must equal what a logged-in WebSocket
 * client (unscoped) receives for the same commands, issued through the C
 * API or by a client over the wire. Also: commands refused during an event
 * delivery (HAKKAS_ERR_BUSY), subscribing and unsubscribing (also from a
 * callback), the file-backed commands (load, load_saved, persist, delete).
 */
#include "e2e.h"

#include <stdlib.h>

#include "app/process_list.h"
#include "cJSON.h"
#include "graphs/graph_json.h"
#include "graphs/linkable.h"
#include "platform/pal.h"
#include "protocol/messages.h"

/* ---- The host's events, rendered as wire messages ------------------------------- */

static cJSON *endpoint(const char *block, const char *port)
{
	cJSON *e = cJSON_CreateObject();
	cJSON_AddStringToObject(e, "node", block);
	cJSON_AddStringToObject(e, "port", port);
	return e;
}

static cJSON *data_object(const HakkasValue *v)
{
	char buf[LINKABLE_NUMBER_TEXT_MAX];
	cJSON *d = cJSON_CreateObject();
	if (v->type == LINKABLE_STRING)
		cJSON_AddStringToObject(d, "data", v->as.s);
	else {
		linkable_stringify(v, buf, sizeof(buf));
		cJSON_AddStringToObject(d, "data", buf);
	}
	return d;
}

/* A block's metadata: x, y, then the other keys of the JSON text. */
static cJSON *node_metadata(const HakkasEvent *ev)
{
	cJSON *m = cJSON_CreateObject();
	cJSON_AddNumberToObject(m, "x", ev->x);
	cJSON_AddNumberToObject(m, "y", ev->y);
	cJSON *extra = ev->metadata != NULL ? cJSON_Parse(ev->metadata) : NULL;
	CHECK(ev->metadata == NULL || cJSON_IsObject(extra));
	while (extra != NULL && extra->child != NULL) {
		cJSON *item = cJSON_DetachItemViaPointer(extra, extra->child);
		cJSON_AddItemToObject(m, item->string, item);
	}
	cJSON_Delete(extra);
	return m;
}

static cJSON *group_metadata(const HakkasEvent *ev)
{
	cJSON *m = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "description", ev->description);
	cJSON_AddStringToObject(m, "icon", ev->icon);
	return m;
}

static void add_time(cJSON *p, int64_t ms)
{
	char t[PROTO_TIME_SIZE];
	proto_msg_time(ms, t);
	cJSON_AddStringToObject(p, "time", t);
}

/* The 02 §3 message of ev, from its typed fields only. */
static char *render(const HakkasEvent *ev)
{
	const char *protocol = "graph", *command = "?";
	cJSON *p = cJSON_CreateObject();
	bool started = ev->type == HAKKAS_EVENT_PROCESS_STARTED;
	switch (ev->type) {
	case HAKKAS_EVENT_PROCESS_CREATED:
		protocol = "runtime", command = "new_graph";
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddStringToObject(p, "name", ev->name);
		cJSON_AddStringToObject(p, "description", ev->description);
		cJSON_AddStringToObject(p, "icon", ev->icon);
		break;
	case HAKKAS_EVENT_PROCESS_DELETED:
		protocol = "runtime", command = "delete_graph";
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddStringToObject(p, "name", ev->name);
		break;
	case HAKKAS_EVENT_PROCESS_RENAMED:
		command = "rename";
		cJSON_AddStringToObject(p, "id", ev->process);
		cJSON_AddStringToObject(p, "from", ev->old_name);
		cJSON_AddStringToObject(p, "to", ev->name);
		break;
	case HAKKAS_EVENT_PROCESS_STARTED:
	case HAKKAS_EVENT_PROCESS_STOPPED:
		protocol = "network", command = started ? "started" : "stopped";
		CHECK(ev->started == started);
		cJSON_AddStringToObject(p, "graph", ev->process);
		add_time(p, ev->wall_ms); /* the wall clock (02 §3.3) */
		if (!started)
			cJSON_AddNumberToObject(p, "uptime", (double)(ev->uptime_ms / 1000));
		cJSON_AddBoolToObject(p, "started", started);
		cJSON_AddBoolToObject(p, "running", started);
		cJSON_AddBoolToObject(p, "debug", false);
		break;
	case HAKKAS_EVENT_PROCESS_STATUS:
		protocol = "network", command = "status";
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddStringToObject(p, "name", ev->name);
		cJSON_AddStringToObject(p, "description", ev->description);
		cJSON_AddNumberToObject(p, "uptime", (double)(ev->uptime_ms / 1000));
		cJSON_AddBoolToObject(p, "running", ev->started);
		cJSON_AddBoolToObject(p, "started", ev->started);
		cJSON_AddBoolToObject(p, "debug", false);
		break;
	case HAKKAS_EVENT_BLOCK_ADDED:
		command = "addnode";
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddStringToObject(p, "component", ev->component);
		cJSON_AddStringToObject(p, "id", ev->block);
		cJSON_AddItemToObject(p, "metadata", node_metadata(ev));
		break;
	case HAKKAS_EVENT_BLOCK_REMOVED:
		command = "removenode";
		cJSON_AddStringToObject(p, "id", ev->block);
		cJSON_AddStringToObject(p, "graph", ev->process);
		break;
	case HAKKAS_EVENT_BLOCK_RENAMED:
		command = "renamenode";
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddStringToObject(p, "from", ev->old_name);
		cJSON_AddStringToObject(p, "to", ev->block);
		break;
	case HAKKAS_EVENT_BLOCK_CHANGED:
		command = "changenode";
		cJSON_AddStringToObject(p, "id", ev->block);
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddItemToObject(p, "metadata", node_metadata(ev));
		break;
	case HAKKAS_EVENT_CONNECTION_ADDED:
	case HAKKAS_EVENT_CONNECTION_REMOVED:
	case HAKKAS_EVENT_CONNECTION_CHANGED:
		command = ev->type == HAKKAS_EVENT_CONNECTION_ADDED	? "addedge"
			  : ev->type == HAKKAS_EVENT_CONNECTION_REMOVED ? "removeedge"
									: "changeedge";
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddItemToObject(p, "src", endpoint(ev->src_block, ev->src_port));
		cJSON_AddItemToObject(p, "tgt", endpoint(ev->block, ev->port));
		if (ev->metadata != NULL)
			cJSON_AddItemToObject(p, "metadata", cJSON_Parse(ev->metadata));
		else if (ev->type == HAKKAS_EVENT_CONNECTION_CHANGED)
			cJSON_AddItemToObject(p, "metadata", cJSON_CreateObject());
		break;
	case HAKKAS_EVENT_INITIAL_ADDED:
	case HAKKAS_EVENT_INITIAL_REMOVED:
		command = ev->type == HAKKAS_EVENT_INITIAL_ADDED ? "addinitial" : "removeinitial";
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddItemToObject(p, "src", data_object(ev->value));
		cJSON_AddItemToObject(p, "tgt", endpoint(ev->block, ev->port));
		break;
	case HAKKAS_EVENT_INPORT_ADDED:
	case HAKKAS_EVENT_OUTPORT_ADDED:
		command = ev->type == HAKKAS_EVENT_INPORT_ADDED ? "addinport" : "addoutport";
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddStringToObject(p, "name", ev->name);
		cJSON_AddStringToObject(p, "node", ev->block);
		cJSON_AddStringToObject(p, "port", ev->port);
		break;
	case HAKKAS_EVENT_INPORT_REMOVED:
	case HAKKAS_EVENT_OUTPORT_REMOVED:
	case HAKKAS_EVENT_GROUP_REMOVED:
		command = ev->type == HAKKAS_EVENT_INPORT_REMOVED    ? "removeinport"
			  : ev->type == HAKKAS_EVENT_OUTPORT_REMOVED ? "removeoutport"
								     : "removegroup";
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddStringToObject(p, "name", ev->name);
		break;
	case HAKKAS_EVENT_INPORT_RENAMED:
	case HAKKAS_EVENT_OUTPORT_RENAMED:
	case HAKKAS_EVENT_GROUP_RENAMED:
		command = ev->type == HAKKAS_EVENT_INPORT_RENAMED    ? "renameinport"
			  : ev->type == HAKKAS_EVENT_OUTPORT_RENAMED ? "renameoutport"
								     : "renamegroup";
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddStringToObject(p, "from", ev->old_name);
		cJSON_AddStringToObject(p, "to", ev->name);
		break;
	case HAKKAS_EVENT_GROUP_ADDED: {
		command = "addgroup";
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddStringToObject(p, "name", ev->name);
		cJSON *nodes = cJSON_AddArrayToObject(p, "nodes");
		const CompositeDef *c = ev->block_type->composite;
		for (uint32_t i = 0; i < c->block_count; i++)
			cJSON_AddItemToArray(nodes, cJSON_CreateString(c->blocks[i].name));
		cJSON_AddItemToObject(p, "metadata", group_metadata(ev));
		break;
	}
	case HAKKAS_EVENT_GROUP_CHANGED:
		command = "changegroup";
		cJSON_AddStringToObject(p, "graph", ev->process);
		cJSON_AddStringToObject(p, "name", ev->name);
		cJSON_AddItemToObject(p, "metadata", group_metadata(ev));
		break;
	case HAKKAS_EVENT_COMPONENT:
		protocol = "component", command = "component";
		CHECK(ev->process == NULL);
		CHECK_STREQ(ev->block_type->name, ev->name);
		cJSON_Delete(p);
		p = graph_json_component(ev->block_type);
		break;
	case HAKKAS_EVENT_ERROR:
		protocol = "network", command = "error";
		cJSON_AddStringToObject(p, "stack", "");
		cJSON_AddStringToObject(p, "message", ev->message);
		if (ev->process != NULL)
			cJSON_AddStringToObject(p, "graph", ev->process);
		break;
	case HAKKAS_EVENT_LINK_ADDED:
	case HAKKAS_EVENT_LINK_REMOVED:
	case HAKKAS_EVENT_BINDING_ADDED:
	case HAKKAS_EVENT_BINDING_REMOVED:
		cJSON_Delete(p); /* no wire message (02 §6): see on_host_event */
		return NULL;
	}
	cJSON *m = cJSON_CreateObject();
	cJSON_AddStringToObject(m, "protocol", protocol);
	cJSON_AddStringToObject(m, "command", command);
	cJSON_AddItemToObject(m, "payload", p);
	char *text = cJSON_PrintUnformatted(m);
	cJSON_Delete(m);
	return text;
}

/* What the host subscriber received since the last wire_same/clear. */
#define EVENTS_MAX 256
static char *events[EVENTS_MAX];
static int nevents;

static void clear_events(void)
{
	for (int i = 0; i < nevents; i++)
		free(events[i]);
	nevents = 0;
}

/* The links between processes the host saw (they have no wire form):
 * "+<from>.<outport>><to>.<inport>;" or "-...;" each. */
static char link_log[1024];

static void on_host_event(void *ctx, const HakkasEvent *ev)
{
	(void)ctx;
	if (ev->type == HAKKAS_EVENT_LINK_ADDED || ev->type == HAKKAS_EVENT_LINK_REMOVED) {
		size_t len = strlen(link_log);
		snprintf(link_log + len, sizeof(link_log) - len, "%c%s.%s>%s.%s;",
			 ev->type == HAKKAS_EVENT_LINK_ADDED ? '+' : '-', ev->process, ev->name,
			 ev->to_process, ev->to_port);
		CHECK(ev->block == NULL && ev->value == NULL);
		return;
	}
	CHECK(nevents < EVENTS_MAX);
	if (nevents < EVENTS_MAX)
		events[nevents++] = render(ev);
}

/* The host's i-th event is protocol:command with payload (' for ", '*' for
 * any value). */
#define EV(i, protocol, command, payload)                                                          \
	CHECK(nevents > (i) && e2e_match(events[i], protocol, command, payload))

/* The host got exactly n events (since the last clear). */
#define NEV(n)                                                                                     \
	do {                                                                                       \
		if (nevents != (n))                                                                \
			fprintf(stderr, "%d host events, expected %d\n", nevents, n);              \
		CHECK(nevents == (n));                                                             \
	} while (0)

/* What client c received equals the host's events, in order, as JSON;
 * then the host's list is cleared. */
static void wire_same(E2eClient *c)
{
	char **msgs;
	int n = e2e_collect(c, &msgs);
	if (n != nevents)
		fprintf(stderr, "wire: %d messages, host: %d events\n", n, nevents);
	CHECK(n == nevents);
	for (int i = 0; i < n && i < nevents; i++) {
		cJSON *a = cJSON_Parse(msgs[i]), *b = cJSON_Parse(events[i]);
		bool same = a != NULL && b != NULL && cJSON_Compare(a, b, true);
		if (!same)
			fprintf(stderr, "  wire: %s\n  host: %s\n", msgs[i], events[i]);
		CHECK(same);
		cJSON_Delete(a);
		cJSON_Delete(b);
	}
	e2e_free_msgs(msgs, n);
	clear_events();
}

/* The last command's message. */
static HakkasMessage m;
#define OK(call) CHECK((call) == HAKKAS_OK)
#define REFUSED(call, want)                                                                        \
	do {                                                                                       \
		int before_ = nevents;                                                             \
		CHECK((call) == HAKKAS_REFUSED);                                                   \
		CHECK_STREQ(m.text, want);                                                         \
		CHECK(nevents == before_); /* no event */                                          \
	} while (0)

static E2eClient wire;
static Hakkas *h;

static const char *graph_file(const char *name)
{
	return e2e_path(e2e_path(e2e_basedir, "graphs"), name);
}

/* ---- Process lifetime ----------------------------------------------------------- */

static void test_processes(void)
{
	char id[HAKKAS_ID_MAX + 1];
	OK(hakkas_process_create(h, "p1", "One", NULL, NULL, id, &m));
	CHECK_STREQ(id, "p1");
	CHECK_STREQ(m.text, "");
	NEV(1);
	EV(0, "runtime", "new_graph", "{'graph':'p1','name':'One','description':'','icon':''}");
	REFUSED(hakkas_process_create(h, "p1", NULL, NULL, NULL, id, &m), "Graph exists: p1");
	CHECK_STREQ(id, "");
	REFUSED(hakkas_process_create(h, "", NULL, NULL, NULL, NULL, &m), "Invalid id");
	OK(hakkas_process_create(h, NULL, NULL, "d", "cog", id, &m));
	CHECK(strlen(id) == 36);
	EV(1, "runtime", "new_graph", "{'graph':'*','name':'<<new graph>>','description':'d',"
	      "'icon':'cog'}");
	wire_same(&wire);

	/* Listing and status, read without events. */
	CHECK(hakkas_process_count(h) == 2);
	HakkasProcessStatus st;
	CHECK(hakkas_process_at(h, 0, &st) && strcmp(st.id, "p1") == 0 && !st.started);
	CHECK(hakkas_process_at(h, 1, &st) && strcmp(st.id, id) == 0 &&
	      strcmp(st.icon, "cog") == 0);
	CHECK(!hakkas_process_at(h, 2, &st));
	OK(hakkas_process_status(h, "p1", &st, &m));
	CHECK(!st.started && st.uptime_ms == 0 && strcmp(st.name, "One") == 0);
	REFUSED(hakkas_process_status(h, "zz", &st, &m), "Unknown graph: zz");
	REFUSED(hakkas_process_status(h, "", &st, &m), "Invalid id");
	REFUSED(hakkas_process_status(h, NULL, &st, &m), "Invalid field: graph");

	/* Start, stop, again (the event is reported either way). */
	OK(hakkas_process_start(h, "p1", &m));
	OK(hakkas_process_start(h, "p1", &m));
	OK(hakkas_process_status(h, "p1", &st, &m));
	CHECK(st.started && st.time_started_ms > 0);
	OK(hakkas_process_stop(h, "p1", &m));
	NEV(3);
	EV(0, "network", "started", "{'graph':'p1','time':'*','started':true,'running':true,"
	      "'debug':false}");
	EV(1, "network", "started", NULL);
	EV(2, "network", "stopped", "{'graph':'p1','time':'*','uptime':0,'started':false,"
	      "'running':false,'debug':false}");
	wire_same(&wire);
	REFUSED(hakkas_process_start(h, "zz", &m), "Unknown graph: zz");
	REFUSED(hakkas_process_stop(h, NULL, &m), "Invalid field: graph");
	REFUSED(hakkas_process_stop(h, "", &m), "Invalid id");

	/* Rename. */
	OK(hakkas_process_rename(h, "p1", "Uno", &m));
	EV(0, "graph", "rename", "{'id':'p1','from':'One','to':'Uno'}");
	REFUSED(hakkas_process_rename(h, NULL, "x", &m), "Invalid field: id");
	REFUSED(hakkas_process_rename(h, "p1", NULL, &m), "Invalid field: to");
	REFUSED(hakkas_process_rename(h, "zz", "x", &m), "Unknown graph: zz");
	wire_same(&wire);

	/* Persist: the file, no event; a failed save is an ERROR event. */
	OK(hakkas_process_start(h, "p1", &m));
	OK(hakkas_process_persist(h, "p1", &m));
	CHECK(pal_storage_exists(graph_file("p1.json")));
	REFUSED(hakkas_process_persist(h, "zz", &m), "Unknown graph: zz");
	REFUSED(hakkas_process_persist(h, NULL, &m), "Invalid field: graph");
	char graphs[PAL_PATH_MAX], moved[PAL_PATH_MAX];
	snprintf(graphs, sizeof(graphs), "%s", e2e_path(e2e_basedir, "graphs"));
	snprintf(moved, sizeof(moved), "%s", e2e_path(e2e_basedir, "graphs.moved"));
	CHECK(rename(graphs, moved) == 0);
	e2e_put(graphs, "not a directory");
	CHECK(hakkas_process_persist(h, "p1", &m) == HAKKAS_ERR_STORAGE);
	CHECK_STREQ(m.text, "Unable to save graph: Uno (p1)");
	CHECK(pal_storage_remove(graphs) == PAL_OK);
	CHECK(rename(moved, graphs) == 0);
	NEV(2);
	EV(1, "network", "error", "{'stack':'','message':'Unable to save graph: Uno (p1)',"
	      "'graph':'p1'}");
	wire_same(&wire);

	/* Delete: a started process is stopped first; its file goes. */
	REFUSED(hakkas_process_delete(h, "p1", "One", &m), "Graph name mismatch: p1");
	REFUSED(hakkas_process_delete(h, NULL, "Uno", &m), "Invalid field: id");
	REFUSED(hakkas_process_delete(h, "p1", NULL, &m), "Invalid field: name");
	OK(hakkas_process_delete(h, "p1", "Uno", &m));
	NEV(2);
	EV(0, "network", "stopped", NULL);
	EV(1, "runtime", "delete_graph", "{'graph':'p1','name':'Uno'}");
	CHECK(!pal_storage_exists(graph_file("p1.json")));
	OK(hakkas_process_delete(h, id, "<<new graph>>", &m));
	wire_same(&wire);
	CHECK(hakkas_process_count(h) == 0);
}

/* Loading from files: the flag is reported, nothing is started. */
static void test_load(void)
{
	char path[PAL_PATH_MAX];
	snprintf(path, sizeof(path), "%s", e2e_path(e2e_root, "l1.json"));
	e2e_put(path, "{\"id\":\"l1\",\"name\":\"L\",\"started\":true,\"blocks\":{\"a\":"
		      "{\"component\":\"math/Add2\"}},\"connections\":[{\"src\":{\"data\":2},"
		      "\"tgt\":{\"block\":\"a\",\"port\":\"in1\"}}]}");
	HakkasLoaded l;
	OK(hakkas_process_load(h, path, &l, &m));
	CHECK_STREQ(l.id, "l1");
	CHECK(l.started);
	NEV(3);
	EV(0, "runtime", "new_graph", "{'graph':'l1','name':'L','description':'','icon':''}");
	EV(1, "graph", "addnode", "{'graph':'l1','component':'math/Add2','id':'a',"
	      "'metadata':{'x':50,'y':50}}");
	EV(2, "graph", "addinitial", "{'graph':'l1','src':{'data':'2'},'tgt':{'node':'a',"
	      "'port':'in1'}}");
	wire_same(&wire);
	HakkasProcessStatus st;
	OK(hakkas_process_status(h, "l1", &st, &m));
	CHECK(!st.started);
	CHECK(hakkas_process_load(h, path, &l, &m) == HAKKAS_ERR_EXISTS);
	CHECK_STREQ(m.text, "graph exists");
	CHECK(hakkas_process_load(h, e2e_path(e2e_root, "none.json"), &l, &m) ==
	      HAKKAS_ERR_NOT_FOUND);
	CHECK(hakkas_process_load(h, NULL, &l, &m) == HAKKAS_ERR_INVALID);
	e2e_put(path, "{\"id\":");
	CHECK(hakkas_process_load(h, path, NULL, &m) == HAKKAS_ERR_MALFORMED);
	NEV(0);
	OK(hakkas_process_delete(h, "l1", "L", &m));
	wire_same(&wire);
}

static char saved[256];

static void on_saved(void *ctx, const HakkasLoaded *l)
{
	(void)ctx;
	size_t len = strlen(saved);
	snprintf(saved + len, sizeof(saved) - len, "%s %d;", l->id, l->started);
	/* Commands may be called from here. */
	if (l->started)
		CHECK(hakkas_process_start(h, l->id, NULL) == HAKKAS_OK);
}

static void test_load_saved(void)
{
	e2e_put(graph_file("s2.json"), "{\"id\":\"s2\"}");
	e2e_put(graph_file("s1.json"), "{\"id\":\"s1\",\"started\":true}");
	size_t n = 0;
	saved[0] = '\0';
	OK(hakkas_process_load_saved(h, on_saved, NULL, &n));
	CHECK(n == 2);
	CHECK_STREQ(saved, "s1 1;s2 0;");
	NEV(3);
	EV(0, "runtime", "new_graph", "{'graph':'s1','name':'<<new graph>>','description':'',"
	      "'icon':''}");
	EV(1, "network", "started", NULL);
	EV(2, "runtime", "new_graph", NULL);
	wire_same(&wire);
	/* Loaded already: every file fails, the first failure is returned. */
	saved[0] = '\0';
	CHECK(hakkas_process_load_saved(h, on_saved, NULL, &n) == HAKKAS_ERR_EXISTS);
	CHECK(n == 0 && saved[0] == '\0');
	OK(hakkas_process_delete(h, "s1", "<<new graph>>", &m));
	OK(hakkas_process_delete(h, "s2", "<<new graph>>", &m));
	CHECK(!pal_storage_exists(graph_file("s1.json")));
	wire_same(&wire);
}

/* ---- Editing -------------------------------------------------------------------- */

static void test_blocks(void)
{
	OK(hakkas_process_create(h, "e", "E", NULL, NULL, NULL, &m));
	OK(hakkas_block_add(h, "e", "a", "math/Add2", "{\"x\":1,\"y\":2,\"k\":\"v\"}", &m));
	OK(hakkas_block_add(h, "e", "b", "math/Add2", NULL, &m));
	NEV(3);
	EV(1, "graph", "addnode", "{'graph':'e','component':'math/Add2','id':'a',"
	      "'metadata':{'x':1,'y':2,'k':'v'}}");
	EV(2, "graph", "addnode", "{'graph':'e','component':'math/Add2','id':'b',"
	      "'metadata':{'x':50,'y':50}}");
	wire_same(&wire);
	REFUSED(hakkas_block_add(h, "e", NULL, "math/Add2", NULL, &m), "Invalid field: id");
	REFUSED(hakkas_block_add(h, NULL, "c", "math/Add2", NULL, &m), "Invalid field: graph");
	REFUSED(hakkas_block_add(h, "e", "c", NULL, NULL, &m), "Invalid field: component");
	REFUSED(hakkas_block_add(h, "e", "c", "math/Add2", "{\"x\":\"no\"}", &m),
		"Invalid field: metadata");
	REFUSED(hakkas_block_add(h, "e", "c", "math/Add2", "[1]", &m), "Invalid field: metadata");
	REFUSED(hakkas_block_add(h, "e", "c", "math/Add2", "{", &m), "Invalid field: metadata");
	REFUSED(hakkas_block_add(h, "e", "a", "math/Add2", NULL, &m), "Node exists: a");
	REFUSED(hakkas_block_add(h, "e", "c", "no/Such", NULL, &m), "Unknown component: no/Such");
	REFUSED(hakkas_block_add(h, "zz", "c", "math/Add2", NULL, &m), "Unknown graph: zz");
	REFUSED(hakkas_block_add(h, "e", "", "math/Add2", NULL, &m), "Invalid id");
	OK(hakkas_block_add(h, "e", "n", "math/Add2", "null", &m)); /* null: none */

	OK(hakkas_block_rename(h, "e", "b", "c", &m));
	REFUSED(hakkas_block_rename(h, "e", "c", "a", &m), "Node exists: a");
	REFUSED(hakkas_block_rename(h, "e", "zz", "y", &m), "Unknown node: zz");
	REFUSED(hakkas_block_rename(h, "e", NULL, "y", &m), "Invalid field: from");
	REFUSED(hakkas_block_rename(h, "e", "c", NULL, &m), "Invalid field: to");
	REFUSED(hakkas_block_rename(h, NULL, "c", "y", &m), "Invalid field: graph");

	OK(hakkas_block_change(h, "e", "a", "{\"x\":5,\"k\":null,\"j\":[1]}", &m));
	NEV(3);
	EV(1, "graph", "renamenode", "{'graph':'e','from':'b','to':'c'}");
	EV(2, "graph", "changenode", "{'id':'a','graph':'e','metadata':{'x':5,'y':2,'j':[1]}}");
	wire_same(&wire);
	REFUSED(hakkas_block_change(h, "e", "a", NULL, &m), "Invalid field: metadata");
	REFUSED(hakkas_block_change(h, "e", "a", "[1]", &m), "Invalid field: metadata");
	REFUSED(hakkas_block_change(h, "e", NULL, "{}", &m), "Invalid field: id");
	REFUSED(hakkas_block_change(h, NULL, "a", "{}", &m), "Invalid field: graph");
	/* The patch is checked before the process and the block (02 §1.2). */
	REFUSED(hakkas_block_change(h, "zz", "a", "{\"y\":true}", &m), "Invalid field: metadata");
	REFUSED(hakkas_block_change(h, "zz", "a", "{}", &m), "Unknown graph: zz");
	REFUSED(hakkas_block_change(h, "e", "zz", "{}", &m), "Unknown node: zz");

	OK(hakkas_block_remove(h, "e", "n", &m));
	EV(0, "graph", "removenode", "{'id':'n','graph':'e'}");
	REFUSED(hakkas_block_remove(h, "e", "n", &m), "Unknown node: n");
	REFUSED(hakkas_block_remove(h, "e", NULL, &m), "Invalid field: id");
	REFUSED(hakkas_block_remove(h, NULL, "a", &m), "Invalid field: graph");
	wire_same(&wire);
}

static void test_connections(void)
{
	OK(hakkas_connection_add(h, "e", "a", "out", "c", "in1", "{\"route\":1}", &m));
	EV(0, "graph", "addedge", "{'graph':'e','src':{'node':'a','port':'out'},"
	      "'tgt':{'node':'c','port':'in1'},'metadata':{'route':1}}");
	REFUSED(hakkas_connection_add(h, "e", NULL, "out", "c", "in2", NULL, &m),
		"Invalid field: src");
	REFUSED(hakkas_connection_add(h, "e", "a", NULL, "c", "in2", NULL, &m),
		"Invalid field: src");
	REFUSED(hakkas_connection_add(h, "e", "a", "out", "c", NULL, NULL, &m),
		"Invalid field: tgt");
	REFUSED(hakkas_connection_add(h, NULL, "a", "out", "c", "in2", NULL, &m),
		"Invalid field: graph");
	REFUSED(hakkas_connection_add(h, "e", "a", "out", "c", "in2", "3", &m),
		"Invalid field: metadata");
	REFUSED(hakkas_connection_add(h, "e", "a", "out", "c", "in1", NULL, &m),
		"Input already connected: c.in1");
	REFUSED(hakkas_connection_add(h, "e", "a", "in1", "c", "in2", NULL, &m),
		"Invalid edge direction");
	REFUSED(hakkas_connection_add(h, "e", "a", "nope", "c", "in2", NULL, &m),
		"Unknown port: a.nope");
	REFUSED(hakkas_connection_add(h, "e", "zz", "out", "c", "in2", NULL, &m),
		"Unknown node: zz");

	OK(hakkas_connection_change(h, "e", "a", "out", "c", "in1", "{\"route\":2}", &m));
	OK(hakkas_connection_change(h, "e", "a", "out", "c", "in1", NULL, &m));
	EV(1, "graph", "changeedge", "{'graph':'e','src':{'node':'a','port':'out'},"
	      "'tgt':{'node':'c','port':'in1'},'metadata':{'route':2}}");
	EV(2, "graph", "changeedge", "{'graph':'e','src':{'node':'a','port':'out'},"
	      "'tgt':{'node':'c','port':'in1'},'metadata':{}}");
	REFUSED(hakkas_connection_change(h, "e", "a", "out", "c", "in2", NULL, &m),
		"No such connection");
	REFUSED(hakkas_connection_change(h, "e", "a", "out", "c", "in1", "x", &m),
		"Invalid field: metadata");
	REFUSED(hakkas_connection_change(h, NULL, "a", "out", "c", "in1", NULL, &m),
		"Invalid field: graph");
	wire_same(&wire);
}

static void test_initials(void)
{
	/* A string is parsed for the port's type; a number is taken as is (an
	 * i64 only from here, 05 §2.1). */
	HakkasValue s;
	CHECK(linkable_from_string(&s, "2", 1));
	OK(hakkas_initial_add(h, "e", "a", "in1", &s, &m));
	linkable_clear(&s);
	HakkasValue i = linkable_from_i64(7);
	OK(hakkas_initial_add(h, "e", "a", "in2", &i, &m));
	NEV(2);
	EV(0, "graph", "addinitial", "{'graph':'e','src':{'data':'2'},'tgt':{'node':'a',"
	      "'port':'in1'}}");
	EV(1, "graph", "addinitial", "{'graph':'e','src':{'data':'7'},'tgt':{'node':'a',"
	      "'port':'in2'}}");
	REFUSED(hakkas_initial_add(h, "e", "a", "in1", NULL, &m), "Invalid field: src");
	HakkasValue unset = {0};
	REFUSED(hakkas_initial_add(h, "e", "a", "in1", &unset, &m), "Invalid field: src");
	REFUSED(hakkas_initial_add(h, "e", NULL, "in1", &i, &m), "Invalid field: tgt");
	REFUSED(hakkas_initial_add(h, NULL, "a", "in1", &i, &m), "Invalid field: graph");
	REFUSED(hakkas_initial_add(h, "e", "c", "in1", &i, &m), "Input already connected: c.in1");
	REFUSED(hakkas_initial_add(h, "e", "a", "out", &i, &m), "Invalid edge direction");
	REFUSED(hakkas_initial_add(h, "e", "a", "zz", &i, &m), "Unknown port: a.zz");
	OK(hakkas_initial_remove(h, "e", "a", "in2", &m));
	EV(2, "graph", "removeinitial", "{'src':{'data':'7'},'tgt':{'node':'a','port':'in2'},"
	      "'graph':'e'}");
	REFUSED(hakkas_initial_remove(h, "e", "a", "in2", &m), "No such connection");
	REFUSED(hakkas_initial_remove(h, "e", "a", NULL, &m), "Invalid field: tgt");
	REFUSED(hakkas_initial_remove(h, NULL, "a", "in2", &m), "Invalid field: graph");
	wire_same(&wire);
}

static void test_ports(void)
{
	OK(hakkas_port_add(h, "e", HAKKAS_INPORT, "x", "a", "in2", &m));
	OK(hakkas_port_add(h, "e", HAKKAS_OUTPORT, "y", "c", "out", &m));
	EV(0, "graph", "addinport", "{'graph':'e','name':'x','node':'a','port':'in2'}");
	EV(1, "graph", "addoutport", "{'graph':'e','name':'y','node':'c','port':'out'}");
	REFUSED(hakkas_port_add(h, "e", HAKKAS_INPORT, "x", "a", "in1", &m), "Port exists: x");
	REFUSED(hakkas_port_add(h, "e", HAKKAS_INPORT, "z", "a", "out", &m),
		"Unknown port: a.out");
	REFUSED(hakkas_port_add(h, NULL, HAKKAS_INPORT, "z", "a", "in1", &m),
		"Invalid field: graph");
	REFUSED(hakkas_port_add(h, "e", HAKKAS_INPORT, "z", NULL, "in1", &m),
		"Invalid field: node");
	REFUSED(hakkas_port_add(h, "e", HAKKAS_INPORT, "z", "a", NULL, &m),
		"Invalid field: port");
	REFUSED(hakkas_port_add(h, "e", HAKKAS_INPORT, NULL, "a", "in1", &m),
		"Invalid field: public");
	OK(hakkas_port_rename(h, "e", HAKKAS_INPORT, "x", "x2", &m));
	EV(2, "graph", "renameinport", "{'graph':'e','from':'x','to':'x2'}");
	REFUSED(hakkas_port_rename(h, "e", HAKKAS_INPORT, "nope", "q", &m), "Unknown port: nope");
	REFUSED(hakkas_port_rename(h, "e", HAKKAS_OUTPORT, "y", "y", &m), "Port exists: y");
	REFUSED(hakkas_port_rename(h, "e", HAKKAS_OUTPORT, NULL, "q", &m), "Invalid field: from");
	REFUSED(hakkas_port_rename(h, "e", HAKKAS_OUTPORT, "y", NULL, &m), "Invalid field: to");
	OK(hakkas_port_remove(h, "e", HAKKAS_OUTPORT, "y", &m));
	EV(3, "graph", "removeoutport", "{'graph':'e','name':'y'}");
	REFUSED(hakkas_port_remove(h, "e", HAKKAS_OUTPORT, "y", &m), "Unknown port: y");
	REFUSED(hakkas_port_remove(h, "e", HAKKAS_OUTPORT, NULL, &m), "Invalid field: public");
	wire_same(&wire);
}

static void test_groups(void)
{
	const char *sel[] = {"c"};
	OK(hakkas_group_add(h, "e", "t/G", sel, 1, "A group", NULL, &m));
	NEV(2);
	EV(0, "graph", "addgroup", "{'graph':'e','name':'t/G','nodes':['c'],"
	      "'metadata':{'description':'A group','icon':''}}");
	EV(1, "component", "component", "{'name':'t/G','description':'A group','icon':'',"
	      "'subgraph':true,'inPorts':'*','outPorts':'*'}");
	CHECK(pal_storage_exists(e2e_path(e2e_path(e2e_basedir, "types"), "t%2FG.json")));
	REFUSED(hakkas_group_add(h, "e", "t/G", sel, 1, "d", NULL, &m), "Component exists: t/G");
	REFUSED(hakkas_group_add(h, "e", "t/H", sel, 0, "d", NULL, &m), "Invalid field: nodes");
	REFUSED(hakkas_group_add(h, "e", "t/H", NULL, 1, "d", NULL, &m), "Invalid field: nodes");
	REFUSED(hakkas_group_add(h, "e", "t/H", sel, 1, NULL, NULL, &m),
		"Invalid field: metadata");
	REFUSED(hakkas_group_add(h, "e", NULL, sel, 1, "d", NULL, &m), "Invalid field: name");
	REFUSED(hakkas_group_add(h, NULL, "t/H", sel, 1, "d", NULL, &m), "Invalid field: graph");
	const char *none[] = {"zz"};
	REFUSED(hakkas_group_add(h, "e", "t/H", none, 1, "d", NULL, &m), "Unknown node: zz");
	REFUSED(hakkas_group_add(h, "zz", "t/H", sel, 1, "d", NULL, &m), "Unknown graph: zz");

	OK(hakkas_group_change(h, "e", "t/G", "Changed", "cog", &m));
	EV(2, "graph", "changegroup", "{'graph':'e','name':'t/G',"
	      "'metadata':{'description':'Changed','icon':'cog'}}");
	EV(3, "component", "component", NULL);
	REFUSED(hakkas_group_change(h, "e", "t/X", "d", NULL, &m), "Unknown component: t/X");
	REFUSED(hakkas_group_change(h, "e", "t/G", NULL, NULL, &m), "Invalid field: metadata");
	OK(hakkas_group_rename(h, "e", "t/G", "t/G2", &m));
	EV(4, "graph", "renamegroup", "{'graph':'e','from':'t/G','to':'t/G2'}");
	EV(5, "component", "component", NULL);
	REFUSED(hakkas_group_rename(h, "e", "t/G2", "math/Add2", &m),
		"Component exists: math/Add2");
	REFUSED(hakkas_group_rename(h, "e", "t/G2", NULL, &m), "Invalid field: to");
	OK(hakkas_block_add(h, "e", "inst", "t/G2", NULL, &m));
	REFUSED(hakkas_group_remove(h, "e", "t/G2", &m), "Component in use: t/G2");
	OK(hakkas_block_remove(h, "e", "inst", &m));
	OK(hakkas_group_remove(h, "e", "t/G2", &m));
	EV(8, "graph", "removegroup", "{'graph':'e','name':'t/G2'}");
	REFUSED(hakkas_group_remove(h, "e", "t/G2", &m), "Unknown component: t/G2");
	REFUSED(hakkas_group_remove(h, "e", NULL, &m), "Invalid field: name");
	wire_same(&wire);
}

/* A compound removal reports its parts first (02 §1.1), exactly as on the
 * wire; then the same commands from a client over the wire reach the host
 * as the same events. */
static bool twelve_events(void *arg)
{
	(void)arg;
	return nevents >= 12;
}

static void test_compound_and_wire(void)
{
	OK(hakkas_block_remove(h, "e", "a", &m));
	NEV(4);
	EV(0, "graph", "removeinitial", "{'src':{'data':'2'},'tgt':{'node':'a','port':'in1'},"
	      "'graph':'e'}");
	EV(1, "graph", "removeedge", "{'graph':'e','src':{'node':'a','port':'out'},"
	      "'tgt':{'node':'c','port':'in1'}}");
	EV(2, "graph", "removeinport", "{'graph':'e','name':'x2'}");
	EV(3, "graph", "removenode", "{'id':'a','graph':'e'}");
	wire_same(&wire);

	/* Commands over the wire: the host sees what the clients see. */
	e2e_send(&wire, "graph", "addnode",
		 "{'id':'w','graph':'e','component':'math/Add2','metadata':{'x':3,'y':4,'q':1}}");
	e2e_send(&wire, "graph", "addinitial",
		 "{'graph':'e','src':{'data':'4.5'},'tgt':{'node':'w','port':'in1'}}");
	e2e_send(&wire, "graph", "addedge",
		 "{'graph':'e','src':{'node':'w','port':'out'},'tgt':{'node':'c','port':'in1'}}");
	e2e_send(&wire, "network", "start", "{'graph':'e'}");
	e2e_send(&wire, "graph", "list", "{}");
	e2e_send(&wire, "runtime", "new_graph", "{'id':'w2','name':'W2'}");
	e2e_send(&wire, "graph", "rename", "{'id':'w2','to':'Wire'}");
	e2e_send(&wire, "graph", "removenode", "{'id':'w','graph':'e'}");
	e2e_send(&wire, "network", "stop", "{'graph':'e'}");
	e2e_send(&wire, "runtime", "delete_graph", "{'id':'w2','name':'Wire'}");
	/* addnode, addinitial, addedge, started, a status per process, new_graph,
	 * rename, the removal's initial, edge and node, stopped, delete_graph. */
	CHECK(e2e_run_until(twelve_events, NULL));
	NEV(12);
	EV(0, "graph", "addnode", "{'graph':'e','component':'math/Add2','id':'w',"
	      "'metadata':{'x':3,'y':4,'q':1}}");
	EV(1, "graph", "addinitial", "{'graph':'e','src':{'data':'4.5'},'tgt':{'node':'w',"
	      "'port':'in1'}}");
	EV(3, "network", "started", NULL);
	EV(4, "network", "status", NULL);
	wire_same(&wire);
}

/* ---- Subscribers and the engine thread rule ------------------------------------- */

static int busy_calls;
static enum hakkas_status busy_status;
static bool status_ok;

static void busy_callback(void *ctx, const HakkasEvent *ev)
{
	(void)ctx;
	(void)ev;
	busy_calls++;
	HakkasMessage bm;
	busy_status = hakkas_block_add(h, "e", "zz", "math/Add2", NULL, &bm);
	CHECK_STREQ(bm.text, "not allowed during event delivery");
	HakkasProcessStatus st;
	status_ok = hakkas_process_status(h, "e", &st, NULL) == HAKKAS_OK; /* read-only */
}

static int once_calls, late_calls;

static void late_callback(void *ctx, const HakkasEvent *ev)
{
	(void)ctx;
	(void)ev;
	late_calls++;
}

static void once_callback(void *ctx, const HakkasEvent *ev)
{
	(void)ev;
	once_calls++;
	hakkas_unsubscribe(h, once_callback, ctx);
	CHECK(hakkas_subscribe(h, late_callback, NULL));
}

static void test_subscribers(void)
{
	CHECK(hakkas_subscribe(h, busy_callback, NULL));
	CHECK(!hakkas_subscribe(h, busy_callback, NULL)); /* already */
	OK(hakkas_block_add(h, "e", "b1", "math/Add2", NULL, &m));
	CHECK(busy_calls == 1 && busy_status == HAKKAS_ERR_BUSY && status_ok);
	hakkas_unsubscribe(h, busy_callback, NULL);
	hakkas_unsubscribe(h, busy_callback, NULL); /* not subscribed: nothing */

	/* Unsubscribing itself and subscribing another from a callback: the
	 * new one gets the next event, not the current one. */
	CHECK(hakkas_subscribe(h, once_callback, NULL));
	OK(hakkas_block_add(h, "e", "b2", "math/Add2", NULL, &m));
	CHECK(once_calls == 1 && late_calls == 0);
	OK(hakkas_block_add(h, "e", "b3", "math/Add2", NULL, &m));
	CHECK(once_calls == 1 && late_calls == 1);
	hakkas_unsubscribe(h, late_callback, NULL);

	/* At most HAKKAS_SUBSCRIBERS_MAX. */
	static int ctxs[HAKKAS_SUBSCRIBERS_MAX];
	int added = 0;
	for (int i = 0; i < HAKKAS_SUBSCRIBERS_MAX; i++)
		added += hakkas_subscribe(h, late_callback, &ctxs[i]);
	CHECK(added == HAKKAS_SUBSCRIBERS_MAX - 1); /* the host's own is one */
	for (int i = 0; i < HAKKAS_SUBSCRIBERS_MAX; i++)
		hakkas_unsubscribe(h, late_callback, &ctxs[i]);
	CHECK(hakkas_subscribe(h, late_callback, NULL));
	hakkas_unsubscribe(h, late_callback, NULL);
	wire_same(&wire);
}

/* A command from a tick hook while the loop runs. */
static bool ticked;

static uint64_t tick(void *ctx, uint64_t now_ms)
{
	(void)ctx;
	if (!ticked) {
		ticked = true;
		CHECK(hakkas_block_add(h, "e", "t1", "math/Add2", NULL, NULL) == HAKKAS_OK);
	}
	return now_ms + 1000;
}

static void test_tick(void)
{
	CHECK(hakkas_add_tick(h, tick, NULL));
	e2e_iterate(1);
	CHECK(ticked);
	CHECK(hakkas_remove_tick(h, tick, NULL));
	NEV(1);
	EV(0, "graph", "addnode", NULL);
	wire_same(&wire);
	OK(hakkas_process_delete(h, "e", "E", &m));
	wire_same(&wire);
}

/* ---- Links between processes (plan task 21f) ------------------------------------ */

/* The next message to the wire client is the network:data of the watch on
 * q's t.in1 with data. */
static void expect_t_in1(const char *data)
{
	char want[256];
	snprintf(want, sizeof(want),
		 "{'id':'*','graph':'q','src':{'node':'w','port':'out'},'tgt':{'node':'t',"
		 "'port':'in1'},'data':'%s'}",
		 data);
	EXPECT(&wire, "network", "data", want);
}

static void test_links(void)
{
	/* p: s (s.in1 = 2), outport o = s.out, inport pi = s.in2;
	 * q: t, inports i = t.in1, i2 = t.in2 (t.in2 = 1), outport qo. */
	OK(hakkas_process_create(h, "p", "P", NULL, NULL, NULL, &m));
	OK(hakkas_process_create(h, "q", "Q", NULL, NULL, NULL, &m));
	OK(hakkas_block_add(h, "p", "s", "math/Add2", NULL, &m));
	OK(hakkas_block_add(h, "q", "t", "math/Add2", NULL, &m));
	OK(hakkas_port_add(h, "p", HAKKAS_OUTPORT, "o", "s", "out", &m));
	OK(hakkas_port_add(h, "p", HAKKAS_INPORT, "pi", "s", "in2", &m));
	OK(hakkas_port_add(h, "q", HAKKAS_INPORT, "i", "t", "in1", &m));
	OK(hakkas_port_add(h, "q", HAKKAS_INPORT, "i2", "t", "in2", &m));
	OK(hakkas_port_add(h, "q", HAKKAS_OUTPORT, "qo", "t", "out", &m));
	HakkasValue two = linkable_from_f64(2), one = linkable_from_f64(1);
	OK(hakkas_initial_add(h, "p", "s", "in1", &two, &m));
	OK(hakkas_initial_add(h, "q", "t", "in2", &one, &m));
	wire_same(&wire);

	/* Made: the host's event, nothing on the wire. */
	link_log[0] = '\0';
	OK(hakkas_link_add(h, "p", "o", "q", "i", &m));
	CHECK_STREQ(m.text, "");
	CHECK_STREQ(link_log, "+p.o>q.i;");
	NEV(0);
	wire_same(&wire);
	CHECK(hakkas_link_count(h) == 1);
	HakkasLink l;
	CHECK(hakkas_link_at(h, 0, &l));
	CHECK_STREQ(l.from_process, "p");
	CHECK_STREQ(l.from_outport, "o");
	CHECK_STREQ(l.to_process, "q");
	CHECK_STREQ(l.to_inport, "i");
	CHECK(!hakkas_link_at(h, 1, &l) && l.from_process == NULL);

	/* Refusals: no event, no link event either. */
	link_log[0] = '\0';
	REFUSED(hakkas_link_add(h, NULL, "o", "q", "i", &m), "Invalid field: from");
	REFUSED(hakkas_link_add(h, "p", NULL, "q", "i", &m), "Invalid field: from");
	REFUSED(hakkas_link_add(h, "p", "o", NULL, "i", &m), "Invalid field: to");
	REFUSED(hakkas_link_add(h, "p", "o", "q", NULL, &m), "Invalid field: to");
	REFUSED(hakkas_link_add(h, "p", "", "q", "i", &m), "Invalid id");
	REFUSED(hakkas_link_add(h, "zz", "o", "q", "i", &m), "Unknown graph: zz");
	REFUSED(hakkas_link_add(h, "p", "o", "zz", "i", &m), "Unknown graph: zz");
	REFUSED(hakkas_link_add(h, "p", "nope", "q", "i", &m), "Unknown port: p.nope");
	REFUSED(hakkas_link_add(h, "p", "o", "q", "nope", &m), "Unknown port: q.nope");
	REFUSED(hakkas_link_add(h, "p", "pi", "q", "i", &m), "Invalid link direction");
	REFUSED(hakkas_link_add(h, "p", "o", "q", "qo", &m), "Invalid link direction");
	REFUSED(hakkas_link_add(h, "p", "o", "q", "i", &m), "Link exists");
	REFUSED(hakkas_link_add(h, "p", "o", "q", "i2", &m), "Input already connected: q.i2");
	REFUSED(hakkas_initial_add(h, "q", "t", "in1", &two, &m),
		"Input already connected: t.in1");
	REFUSED(hakkas_connection_add(h, "q", "t", "out", "t", "in1", NULL, &m),
		"Input already connected: t.in1");
	REFUSED(hakkas_link_remove(h, "p", "o", "q", "i2", &m), "No such link");
	REFUSED(hakkas_link_remove(h, "p", "o", NULL, "i", &m), "Invalid field: to");
	REFUSED(hakkas_link_remove(h, "zz", "o", "q", "i", &m), "Unknown graph: zz");
	CHECK_STREQ(link_log, "");
	wire_same(&wire);

	/* Values over the link, watched by the wire client on q's t.in1 (02
	 * §4.2): the same values a connection would carry. */
	OK(hakkas_process_start(h, "q", &m));
	OK(hakkas_process_start(h, "p", &m));
	wire_same(&wire);
	static const char watch[] = "{'graph':'q','edges':[{'src':{'node':'w','port':'out'},"
				    "'tgt':{'node':'t','port':'in1'}}]}";
	e2e_send(&wire, "network", "edges", watch);
	EXPECT(&wire, "network", "edges", watch);
	expect_t_in1("2");
	HakkasValue five = linkable_from_f64(5);
	OK(hakkas_initial_add(h, "p", "s", "in1", &five, &m));
	EXPECT(&wire, "graph", "addinitial", NULL);
	expect_t_in1("5");
	QUIET(&wire);
	clear_events();

	/* The source stopped: the value is kept, nothing reverts. */
	OK(hakkas_process_stop(h, "p", &m));
	OK(hakkas_initial_add(h, "p", "s", "in1", &two, &m));
	EXPECT(&wire, "network", "stopped", NULL);
	EXPECT(&wire, "graph", "addinitial", NULL);
	QUIET(&wire);
	clear_events();

	/* Removed: the input reverts at once (its watch sees the default),
	 * and that is all the wire sees. */
	OK(hakkas_link_remove(h, "p", "o", "q", "i", &m));
	CHECK_STREQ(link_log, "-p.o>q.i;");
	NEV(0);
	expect_t_in1("0");
	QUIET(&wire);
	CHECK(hakkas_link_count(h) == 0);

	/* Made again from the stopped source: nothing until it starts. */
	OK(hakkas_link_add(h, "p", "o", "q", "i", &m));
	QUIET(&wire);
	OK(hakkas_process_start(h, "p", &m));
	EXPECT(&wire, "network", "started", NULL);
	expect_t_in1("2");
	QUIET(&wire);
	clear_events();
	e2e_send(&wire, "network", "edges", "{'graph':'q','edges':[]}");
	EXPECT(&wire, "network", "edges", NULL);

	/* Deleting the source: stopped, the link removed, deleted — and the
	 * wire sees exactly what it saw before links existed. */
	link_log[0] = '\0';
	OK(hakkas_process_delete(h, "p", "P", &m));
	CHECK_STREQ(link_log, "-p.o>q.i;");
	NEV(2);
	EV(0, "network", "stopped", NULL);
	EV(1, "runtime", "delete_graph", "{'graph':'p','name':'P'}");
	wire_same(&wire);
	CHECK(hakkas_link_count(h) == 0);
	OK(hakkas_process_delete(h, "q", "Q", &m));
	wire_same(&wire);
}

/* Two processes chained from their files by a process list's link, made
 * as the POSIX host makes it (platform/posix/host.c): the value crosses. */
static void test_links_from_files(void)
{
	char src[PAL_PATH_MAX], tgt[PAL_PATH_MAX], list[PAL_PATH_MAX];
	snprintf(src, sizeof(src), "%s", e2e_path(e2e_root, "c1.json"));
	snprintf(tgt, sizeof(tgt), "%s", e2e_path(e2e_root, "c2.json"));
	snprintf(list, sizeof(list), "%s", e2e_path(e2e_root, "chain.json"));
	e2e_put(src, "{\"id\":\"c1\",\"blocks\":{\"a\":{\"component\":\"math/Add2\"}},"
		     "\"connections\":[{\"src\":{\"data\":3},\"tgt\":{\"block\":\"a\","
		     "\"port\":\"in1\"}}],\"outports\":[{\"name\":\"out\",\"block\":\"a\","
		     "\"port\":\"out\"}]}");
	e2e_put(tgt, "{\"id\":\"c2\",\"blocks\":{\"b\":{\"component\":\"math/Add2\"}},"
		     "\"inports\":[{\"name\":\"in\",\"block\":\"b\",\"port\":\"in2\"}]}");
	e2e_put(list, "{\"processes\":[{\"graph\":\"c1.json\"},{\"graph\":\"c2.json\"}],"
		      "\"links\":[{\"from\":\"c1.out\",\"to\":\"c2.in\"}]}");
	ProcessList pl;
	CHECK(process_list_load(list, &pl) == GRAPH_FILES_OK && pl.link_count == 1);
	for (size_t i = 0; i < pl.count; i++)
		OK(hakkas_process_load(h, pl.entries[i].graph, NULL, &m));
	link_log[0] = '\0';
	for (size_t i = 0; i < pl.link_count; i++)
		OK(hakkas_link_add(h, pl.links[i].from_process, pl.links[i].from_port,
				   pl.links[i].to_process, pl.links[i].to_port, &m));
	process_list_free(&pl);
	CHECK_STREQ(link_log, "+c1.out>c2.in;");
	OK(hakkas_process_start(h, "c1", &m));
	OK(hakkas_process_start(h, "c2", &m));
	wire_same(&wire);
	static const char watch[] = "{'graph':'c2','edges':[{'src':{'node':'w','port':'out'},"
				    "'tgt':{'node':'b','port':'in2'}}]}";
	e2e_send(&wire, "network", "edges", watch);
	EXPECT(&wire, "network", "edges", watch);
	EXPECT(&wire, "network", "data",
	       "{'id':'*','graph':'c2','src':{'node':'w','port':'out'},'tgt':{'node':'b',"
	       "'port':'in2'},'data':'3'}");
	QUIET(&wire);
	e2e_send(&wire, "network", "edges", "{'graph':'c2','edges':[]}");
	EXPECT(&wire, "network", "edges", NULL);
	OK(hakkas_process_delete(h, "c2", "<<new graph>>", &m));
	OK(hakkas_process_delete(h, "c1", "<<new graph>>", &m));
	CHECK_STREQ(link_log, "+c1.out>c2.in;-c1.out>c2.in;");
	wire_same(&wire);
}

int main(void)
{
	e2e_setup("api");
	if (!e2e_start()) {
		e2e_teardown();
		return 1;
	}
	h = e2e_hakkas;
	CHECK(hakkas_subscribe(h, on_host_event, NULL));
	CHECK(e2e_connect(&wire) && e2e_login(&wire, "alice"));
	CHECK(hakkas_config(h) != NULL && hakkas_web_port(h) != 0);
	CHECK_STREQ(hakkas_status_str(HAKKAS_REFUSED), "refused");

	RUN(test_processes);
	RUN(test_load);
	RUN(test_load_saved);
	RUN(test_blocks);
	RUN(test_connections);
	RUN(test_initials);
	RUN(test_ports);
	RUN(test_groups);
	RUN(test_compound_and_wire);
	RUN(test_subscribers);
	RUN(test_tick);
	RUN(test_links);
	RUN(test_links_from_files);

	e2e_close(&wire);
	hakkas_unsubscribe(h, on_host_event, NULL);
	clear_events();
	e2e_stop();
	e2e_teardown();
	return test_result();
}
