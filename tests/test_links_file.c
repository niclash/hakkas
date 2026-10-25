/*
 * The saved links between processes (app/links_file.h, 05 §2.3; plan task
 * 62): the form, each reference split on its last '.', unknown members
 * warned about, every malformed shape refused with a warning, and the
 * serialized form of a runtime's links.
 */
#include "test.h"

#include <stdlib.h>

#include "app/links_file.h"
#include "blocktypes/block_types.h"
#include "graphs/graphs.h"
#include "platform/pal.h"
#include "system/log.h"

static char logs[4096];

static void log_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)len;
	const char *msg = strstr(line, ": ");
	size_t n = strlen(logs);
	snprintf(logs + n, sizeof(logs) - n, "%s %s\n", log_level_name(level),
		 msg != NULL ? msg + 2 : line);
}

/* text with ' for ". */
static enum graph_files_status parse(const char *text, LinksFile *f)
{
	static char buf[2048];
	size_t n = strlen(text);
	CHECK(n < sizeof(buf));
	for (size_t i = 0; i <= n; i++)
		buf[i] = text[i] == '\'' ? '"' : text[i];
	logs[0] = '\0';
	return links_file_parse(buf, n, "links.json", f);
}

#define NOT_LINKS "warn links.json: not a links file: expected a JSON object with \"links\"\n"

static void test_parse(void)
{
	LinksFile f;
	CHECK(parse("{'links':[{'from':'p.out','to':'q.in'},{'from':'a.b.c','to':'d.e'}]}", &f) ==
	      GRAPH_FILES_OK);
	CHECK(f.count == 2);
	if (f.count == 2) {
		CHECK_STREQ(f.links[0].from_process, "p");
		CHECK_STREQ(f.links[0].from_port, "out");
		CHECK_STREQ(f.links[0].to_process, "q");
		CHECK_STREQ(f.links[0].to_port, "in");
		CHECK_STREQ(f.links[1].from_process, "a.b"); /* on the last '.' */
		CHECK_STREQ(f.links[1].from_port, "c");
	}
	CHECK_STREQ(logs, "");
	links_file_free(&f);
	CHECK(f.count == 0 && f.links == NULL);
	CHECK(parse("{}", &f) == GRAPH_FILES_OK && f.count == 0);
	CHECK(parse("{'links':null}", &f) == GRAPH_FILES_OK && f.count == 0);
	CHECK(parse(" {'links':[],'later':1} \n", &f) == GRAPH_FILES_OK && f.count == 0);
	CHECK_STREQ(logs, "warn links.json: unknown member \"later\" ignored\n");

	static const struct {
		const char *text, *log;
	} bad[] = {
		{"[]", NOT_LINKS},
		{"{} x", NOT_LINKS},
		{"{'links':{}}", "warn links.json: links: expected an array\n"},
		{"{'links':[3]}",
		 "warn links.json: links[0]: expected an object with \"from\" and \"to\"\n"},
		{"{'links':[{'from':'p.o','to':''}]}",
		 "warn links.json: links[0]: expected an object with \"from\" and \"to\"\n"},
		{"{'links':[{'from':'po','to':'q.i'}]}",
		 "warn links.json: links[0]: from: expected \"<process>.<outport>\"\n"},
		{"{'links':[{'from':'p.o','to':'q.'}]}",
		 "warn links.json: links[0]: to: expected \"<process>.<inport>\"\n"},
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		CHECK(parse(bad[i].text, &f) == GRAPH_FILES_ERR_MALFORMED);
		CHECK(f.count == 0 && f.links == NULL);
		if (strcmp(logs, bad[i].log) != 0)
			fprintf(stderr, "case %zu: %s\n", i, bad[i].text);
		CHECK_STREQ(logs, bad[i].log);
	}
}

static void test_serialize(void)
{
	Runtime rt;
	Graphs gs;
	runtime_init(&rt);
	graphs_init(&gs, &rt, block_types_get, pal_random_bytes);
	size_t len = 0;
	char *text = links_file_serialize(&rt, &len);
	CHECK_STREQ(text, "{\"links\":[]}");
	CHECK(len == strlen("{\"links\":[]}"));
	free(text);
	CHECK(graphs_new_graph(&gs, "p", "P", NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_new_graph(&gs, "q.r", "Q", NULL, NULL, NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "p", "a", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_node(&gs, "q.r", "b", "math/Add2", NULL, NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "p", GRAPH_OUTPORT, "o", "a", "out", NULL) == GRAPH_OK);
	CHECK(graphs_add_port(&gs, "q.r", GRAPH_INPORT, "i", "b", "in1", NULL) == GRAPH_OK);
	CHECK(graphs_add_link(&gs, "p", "o", "q.r", "i", NULL) == GRAPH_OK);
	text = links_file_serialize(&rt, &len);
	CHECK_STREQ(text, "{\"links\":[{\"from\":\"p.o\",\"to\":\"q.r.i\"}]}");
	/* What it writes reads back. */
	LinksFile f;
	CHECK(links_file_parse(text, len, "x", &f) == GRAPH_FILES_OK && f.count == 1);
	CHECK(f.count == 1 && strcmp(f.links[0].to_process, "q.r") == 0);
	links_file_free(&f);
	free(text);
	graphs_deinit(&gs);
	runtime_deinit(&rt);
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(log_add_sink(log_sink, NULL));
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	RUN(test_parse);
	RUN(test_serialize);
	block_types_reset();
	log_remove_sink(log_sink, NULL);
	return test_result();
}
