/*
 * End-to-end live dataflow (plan task 20, 05 §5; 03 §2-§3.1, 02 §4.2): a
 * small graph built over the wire on the whole engine started in-process
 * (e2e.h), its link values watched with network:edges while initials
 * change, the graph stopped and started again; a timing/Interval observed
 * toggling on the engine's real clock (bounded waits, generous bounds);
 * then network:persist, the engine stopped and started again in daemon mode
 * on the same basedir, and the graph is back: the same graph:connect
 * snapshot, started, its values flowing.
 */
#include "e2e.h"

#include <stdlib.h>

#include "platform/pal.h"

static E2eClient e;
static char **before;
static int nbefore;

static void cmd(const char *protocol, const char *command, const char *payload)
{
	e2e_send(&e, protocol, command, payload);
	CHECK(e2e_recv(&e) != NULL);
}

/* The next message is network:data for the watched input node.port with
 * value data (src as subscribed: w.out). */
static void expect_data(const char *node, const char *port, const char *data)
{
	char want[256];
	snprintf(want, sizeof(want),
		 "{'id':'unknown','graph':'flow','src':{'node':'w','port':'out'},'tgt':{'node':"
		 "'%s','port':'%s'},'data':'%s'}",
		 node, port, data);
	EXPECT(&e, "network", "data", want);
}

static void test_live_values(void)
{
	CHECK(e2e_connect(&e) && e2e_login(&e, "alice"));
	cmd("runtime", "new_graph", "{'id':'flow','name':'Flow'}");
	/* a1 -> a2 -> a3, all math/Add2 (03 §3). */
	cmd("graph", "addnode", "{'id':'a1','graph':'flow','component':'math/Add2'}");
	cmd("graph", "addnode", "{'id':'a2','graph':'flow','component':'math/Add2'}");
	cmd("graph", "addnode", "{'id':'a3','graph':'flow','component':'math/Add2'}");
	cmd("graph", "addedge",
	    "{'src':{'node':'a1','port':'out'},'tgt':{'node':'a2','port':'in1'},'graph':'flow'}");
	cmd("graph", "addedge",
	    "{'src':{'node':'a2','port':'out'},'tgt':{'node':'a3','port':'in1'},'graph':'flow'}");
	cmd("graph", "addinitial",
	    "{'tgt':{'node':'a1','port':'in1'},'src':{'data':2},'graph':'flow'}");
	cmd("graph", "addinitial",
	    "{'tgt':{'node':'a1','port':'in2'},'src':{'data':3},'graph':'flow'}");
	cmd("graph", "addinitial",
	    "{'tgt':{'node':'a2','port':'in2'},'src':{'data':'10'},'graph':'flow'}");
	e2e_send(&e, "network", "start", "{'graph':'flow'}");
	EXPECT(&e, "network", "started", NULL);
	QUIET(&e);

	/* The watch: the echo, then each input's current value. */
	static const char edges[] =
		"{'graph':'flow','edges':[{'src':{'node':'w','port':'out'},'tgt':{'node':'a2',"
		"'port':'in1'}},{'src':{'node':'w','port':'out'},'tgt':{'node':'a3','port':"
		"'in1'}}]}";
	e2e_send(&e, "network", "edges", edges);
	EXPECT(&e, "network", "edges", edges);
	expect_data("a2", "in1", "5");
	expect_data("a3", "in1", "15");
	/* A changed initial: its event, then the values downstream. */
	e2e_send(&e, "graph", "addinitial",
		 "{'tgt':{'node':'a1','port':'in1'},'src':{'data':7},'graph':'flow'}");
	EXPECT(&e, "graph", "addinitial",
	       "{'graph':'flow','src':{'data':'7'},'tgt':{'node':'a1','port':'in1'}}");
	expect_data("a2", "in1", "10");
	expect_data("a3", "in1", "20");
	/* The same value again changes nothing downstream (01 §3.3). */
	e2e_send(&e, "graph", "addinitial",
		 "{'tgt':{'node':'a1','port':'in1'},'src':{'data':'7'},'graph':'flow'}");
	EXPECT(&e, "graph", "addinitial", NULL);
	QUIET(&e);
	/* A stopped graph propagates nothing; a start pushes every output. */
	e2e_send(&e, "network", "stop", "{'graph':'flow'}");
	EXPECT(&e, "network", "stopped", NULL);
	e2e_send(&e, "graph", "addinitial",
		 "{'tgt':{'node':'a1','port':'in1'},'src':{'data':1},'graph':'flow'}");
	EXPECT(&e, "graph", "addinitial", NULL);
	QUIET(&e);
	e2e_send(&e, "network", "start", "{'graph':'flow'}");
	EXPECT(&e, "network", "started", NULL);
	/* Every block's start pushes its output (03 §2), in creation order,
	 * delivered breadth-first: a1's 4 reaches a2.in1 and a2's 10 + 10 of
	 * its inputs kept while stopped reaches a3.in1, then a2's new 14. */
	expect_data("a2", "in1", "4");
	expect_data("a3", "in1", "20");
	expect_data("a3", "in1", "14");
	/* A removed edge: the input reverts to its default at once. */
	static const char a2_a3[] = "{'src':{'node':'a2','port':'out'},'tgt':{'node':'a3',"
				    "'port':'in1'},'graph':'flow'}";
	e2e_send(&e, "graph", "removeedge", a2_a3);
	EXPECT(&e, "graph", "removeedge", NULL);
	expect_data("a3", "in1", "0");
	e2e_send(&e, "graph", "addedge", a2_a3);
	EXPECT(&e, "graph", "addedge", NULL);
	expect_data("a3", "in1", "14");
	QUIET(&e);
}

/* Waits for count network:data messages on t's sink; true if they
 * alternate between "true" and "false". *elapsed_ms is the time from the
 * first to the last. */
static bool toggles(int count, uint64_t *elapsed_ms)
{
	uint64_t first = 0;
	char prev[8] = "";
	bool alternate = true;
	for (int i = 0; i < count; i++) {
		const char *m = e2e_recv(&e);
		if (!e2e_match(m, "network", "data", NULL))
			return false;
		const char *data = e2e_payload_str(m, "data");
		CHECK(strcmp(data, "true") == 0 || strcmp(data, "false") == 0);
		alternate = alternate && strcmp(data, prev) != 0;
		snprintf(prev, sizeof(prev), "%s", data);
		if (i == 0)
			first = pal_uptime_ms();
	}
	*elapsed_ms = pal_uptime_ms() - first;
	return alternate;
}

static void test_interval(void)
{
	/* A 20 ms, 50 % pulse train into s.in1 (03 §3.1), added while the
	 * graph is stopped: a block added to a started graph starts at once
	 * with its default period (1 s), and an initial changes the period
	 * only from the next cycle. */
	e2e_send(&e, "network", "stop", "{'graph':'flow'}");
	EXPECT(&e, "network", "stopped", NULL);
	cmd("graph", "addnode", "{'id':'t','graph':'flow','component':'timing/Interval'}");
	cmd("graph", "addnode", "{'id':'s','graph':'flow','component':'math/Add2'}");
	cmd("graph", "addinitial",
	    "{'tgt':{'node':'t','port':'milliseconds'},'src':{'data':20},'graph':'flow'}");
	cmd("graph", "addinitial",
	    "{'tgt':{'node':'t','port':'duty-cycle'},'src':{'data':50},'graph':'flow'}");
	cmd("graph", "addedge",
	    "{'src':{'node':'t','port':'out'},'tgt':{'node':'s','port':'in1'},'graph':'flow'}");
	QUIET(&e);
	/* A new set replaces the watch of a2 and a3. */
	static const char edges[] = "{'graph':'flow','edges':[{'src':{'node':'w','port':'out'},"
				    "'tgt':{'node':'s','port':'in1'}}]}";
	e2e_send(&e, "network", "edges", edges);
	EXPECT(&e, "network", "edges", edges);
	expect_data("s", "in1", "0");
	e2e_send(&e, "network", "start", "{'graph':'flow'}");
	EXPECT(&e, "network", "started", NULL);
	/* Six edges, nominally 50 ms (a transition every 10 ms on the cycle's
	 * grid, 03 §3.1): generously, at least 10 ms (a late timer skips edges,
	 * it never adds any) and at most two seconds. */
	uint64_t elapsed = 0;
	CHECK(toggles(6, &elapsed));
	if (elapsed < 10 || elapsed > 2000)
		fprintf(stderr, "  six toggles in %llu ms\n", (unsigned long long)elapsed);
	CHECK(elapsed >= 10 && elapsed <= 2000);
	/* The watch ends (its values in flight may still arrive first). */
	e2e_send(&e, "network", "edges", "{'graph':'flow','edges':[]}");
	for (;;) {
		const char *m = e2e_recv(&e);
		CHECK(m != NULL);
		if (m == NULL || !e2e_json_eq(m, "{'protocol':'network','command':'data','payload':"
						 "'*'}"))
			break;
	}
	CHECK(e2e_match(e.text, "network", "edges", "{'graph':'flow','edges':[]}"));
	QUIET(&e);
}

static void test_persist_restart(void)
{
	e2e_send(&e, "network", "persist", "{'graph':'flow'}");
	QUIET(&e);
	CHECK(pal_storage_exists(e2e_path(e2e_path(e2e_basedir, "graphs"), "flow.json")));
	e2e_send(&e, "graph", "connect", "{'id':'flow'}");
	nbefore = e2e_collect(&e, &before);
	CHECK(nbefore == 1 + 5 + 5 + 3); /* clear, blocks, initials, edges */
	e2e_close(&e);

	/* The engine stopped and started again, `run daemon`. */
	e2e_stop();
	CHECK(e2e_start());
	if (e2e_engine == NULL)
		return;
	CHECK(strstr(e2e_logs(), "1 graph loaded") != NULL);
	CHECK(e2e_connect(&e) && e2e_login(&e, "bob"));
	e2e_send(&e, "graph", "connect", "{'id':'flow'}");
	char **after;
	int nafter = e2e_collect(&e, &after);
	CHECK(nafter == nbefore);
	for (int i = 0; i < nafter && i < nbefore; i++) {
		if (strcmp(after[i], before[i]) != 0)
			fprintf(stderr, "  before: %s\n  after:  %s\n", before[i], after[i]);
		CHECK_STREQ(after[i], before[i]);
	}
	e2e_free_msgs(after, nafter);
	e2e_free_msgs(before, nbefore);
	/* Started, as it was persisted; its values flow. */
	e2e_send(&e, "network", "getstatus", "{'graph':'flow'}");
	EXPECT(&e, "network", "status",
	       "{'graph':'flow','name':'Flow','description':'','uptime':'*','running':true,"
	       "'started':true,'debug':false}");
	static const char edges[] =
		"{'graph':'flow','edges':[{'src':{'node':'w','port':'out'},'tgt':{'node':'a3',"
		"'port':'in1'}},{'src':{'node':'w','port':'out'},'tgt':{'node':'s',"
		"'port':'in1'}}]}";
	e2e_send(&e, "network", "edges", edges);
	EXPECT(&e, "network", "edges", edges);
	expect_data("a3", "in1", "14");
	EXPECT(&e, "network", "data", NULL); /* s.in1 now */
	uint64_t elapsed = 0;
	CHECK(toggles(3, &elapsed));
	e2e_send(&e, "runtime", "delete_graph", "{'id':'flow','name':'Flow'}");
	for (;;) {
		const char *m = e2e_recv(&e);
		CHECK(m != NULL);
		if (m == NULL || e2e_json_eq(m, "{'protocol':'runtime','command':'delete_graph',"
						"'payload':{'graph':'flow','name':'Flow'}}"))
			break;
	}
	CHECK(!pal_storage_exists(e2e_path(e2e_path(e2e_basedir, "graphs"), "flow.json")));
	e2e_close(&e);
}

int main(void)
{
	e2e_setup("dataflow");
	if (!e2e_start()) {
		e2e_teardown();
		return 1;
	}
	RUN(test_live_values);
	RUN(test_interval);
	RUN(test_persist_restart);
	e2e_stop();
	e2e_teardown();
	return test_result();
}
