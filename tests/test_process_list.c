/*
 * The process list (app/process_list.h, 05 §1.2 "Process list"): both
 * forms, paths resolved against the list file's directory, the "start"
 * values, bindings and links, unknown members warned about, every malformed
 * shape refused with its log line, and reading the file (missing, too big).
 */
#define _XOPEN_SOURCE 700

#include "test.h"

#include <ftw.h>
#include <stdarg.h>
#include <stdlib.h>

#include "app/process_list.h"
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

/* text with ' for ", parsed as if read from path. */
static enum graph_files_status parse(const char *text, const char *path, ProcessList *l)
{
	static char buf[2048];
	size_t n = strlen(text);
	CHECK(n < sizeof(buf));
	for (size_t i = 0; i <= n; i++)
		buf[i] = text[i] == '\'' ? '"' : text[i];
	logs[0] = '\0';
	return process_list_parse(buf, n, path, l);
}

static void test_array(void)
{
	ProcessList l;
	CHECK(parse("['a.json', './sub/b.json', '/abs/c.json']", "/etc/hakkas/processes.json",
		    &l) == GRAPH_FILES_OK);
	CHECK(l.count == 3 && l.link_count == 0);
	if (l.count == 3) {
		CHECK_STREQ(l.entries[0].graph, "/etc/hakkas/a.json");
		CHECK_STREQ(l.entries[1].graph, "/etc/hakkas/./sub/b.json");
		CHECK_STREQ(l.entries[2].graph, "/abs/c.json");
		CHECK(l.entries[0].start == PROCESS_LIST_START_FILE);
		CHECK(l.entries[0].bindings == NULL);
	}
	process_list_free(&l);
	CHECK(l.count == 0 && l.entries == NULL);
	/* A list in the working directory: paths as written. */
	CHECK(parse("['a.json']", "processes.json", &l) == GRAPH_FILES_OK);
	CHECK(l.count == 1 && strcmp(l.entries[0].graph, "a.json") == 0);
	process_list_free(&l);
	CHECK(parse(" [ ] \n", "p.json", &l) == GRAPH_FILES_OK && l.count == 0);
	CHECK_STREQ(logs, "");
	process_list_free(&l);
	/* The sister project's file. */
	CHECK(parse("[\n  './docs/example.json'\n]\n", "dir/processes.json", &l) ==
	      GRAPH_FILES_OK);
	CHECK(l.count == 1 && strcmp(l.entries[0].graph, "dir/./docs/example.json") == 0);
	process_list_free(&l);
}

static void test_object(void)
{
	ProcessList l;
	CHECK(parse("{'processes':[{'graph':'a.json','start':true,'bindings':'emu.json'},"
		    "{'graph':'/x/b.json','start':false},{'graph':'c.json','start':null,"
		    "'bindings':null,'extra':1}],'links':[{'from':'a.out','to':'b.in'},"
		    "{'from':'b.o','to':'a.i'}],'version':1}",
		    "/d/p.json", &l) == GRAPH_FILES_OK);
	CHECK(l.count == 3 && l.link_count == 2);
	if (l.count == 3) {
		CHECK_STREQ(l.entries[0].graph, "/d/a.json");
		CHECK_STREQ(l.entries[0].bindings, "/d/emu.json");
		CHECK(l.entries[0].start == PROCESS_LIST_START_YES);
		CHECK_STREQ(l.entries[1].graph, "/x/b.json");
		CHECK(l.entries[1].start == PROCESS_LIST_START_NO && l.entries[1].bindings == NULL);
		CHECK(l.entries[2].start == PROCESS_LIST_START_FILE);
		CHECK(l.entries[2].bindings == NULL);
	}
	CHECK_STREQ(logs, "warn /d/p.json: unknown member \"version\" ignored\n"
			  "warn /d/p.json: processes[2]: unknown member \"extra\" ignored\n");
	process_list_free(&l);
	CHECK(parse("{'processes':[],'links':null}", "p.json", &l) == GRAPH_FILES_OK);
	CHECK(l.count == 0 && l.link_count == 0);
	process_list_free(&l);
}

/* text is refused as malformed with this error line. */
static void malformed(const char *text, const char *line)
{
	ProcessList l;
	CHECK(parse(text, "p.json", &l) == GRAPH_FILES_ERR_MALFORMED);
	CHECK(l.count == 0 && l.entries == NULL);
	char want[512];
	snprintf(want, sizeof(want), "error p.json: %s\n", line);
	if (strstr(logs, want) == NULL)
		fprintf(stderr, "expected \"%s\" in \"%s\"\n", want, logs);
	CHECK(strstr(logs, want) != NULL);
}

static void test_malformed(void)
{
	const char *not_list = "not a valid process list: expected a JSON array of graph file "
			       "paths or an object with \"processes\"";
	malformed("", not_list);
	malformed("nonsense", not_list);
	malformed("['a.json'] x", not_list);
	malformed("3", not_list);
	malformed("'a.json'", not_list);
	malformed("['a.json', 3]", "[1]: expected a graph file path (a non-empty string)");
	malformed("['']", "[0]: expected a graph file path (a non-empty string)");
	malformed("{}", "processes: expected an array");
	malformed("{'processes':{}}", "processes: expected an array");
	malformed("{'processes':['a.json']}",
		  "processes[0]: expected an object with a graph file path \"graph\"");
	malformed("{'processes':[{'graph':''}]}",
		  "processes[0]: expected an object with a graph file path \"graph\"");
	malformed("{'processes':[{'graph':'a','bindings':3}]}",
		  "processes[0]: bindings: expected a file path (a non-empty string)");
	malformed("{'processes':[{'graph':'a','start':'yes'}]}",
		  "processes[0]: start: expected true or false");
	malformed("{'processes':[],'links':{}}", "links: expected an array");
	malformed("{'processes':[],'links':[{'from':'a.b'}]}",
		  "links[0]: expected an object with \"from\" and \"to\"");
	char text[PAL_PATH_MAX + 16];
	snprintf(text, sizeof(text), "['%0*d']", PAL_PATH_MAX, 0);
	malformed(text, "[0]: path too long");
}

static char root[PAL_PATH_MAX];

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb;
	(void)flag;
	(void)ftw;
	return remove(path);
}

static void test_load(void)
{
	char path[PAL_PATH_MAX + 16];
	snprintf(path, sizeof(path), "%s/processes.json", root);
	ProcessList l;
	logs[0] = '\0';
	CHECK(process_list_load(path, &l) == GRAPH_FILES_ERR_NOT_FOUND);
	CHECK(strstr(logs, "processes.json") != NULL);
	const char *text = "{\"processes\":[{\"graph\":\"g.json\"}]}";
	CHECK(pal_storage_write(path, text, strlen(text)) == PAL_OK);
	CHECK(process_list_load(path, &l) == GRAPH_FILES_OK);
	char want[PAL_PATH_MAX + 16];
	snprintf(want, sizeof(want), "%s/g.json", root);
	CHECK(l.count == 1 && strcmp(l.entries[0].graph, want) == 0);
	process_list_free(&l);
	static char big[HAKKAS_PROCESS_LIST_MAX + 2];
	memset(big, ' ', sizeof(big) - 1);
	big[0] = '[';
	big[sizeof(big) - 2] = ']';
	CHECK(pal_storage_write(path, big, strlen(big)) == PAL_OK);
	CHECK(process_list_load(path, &l) == GRAPH_FILES_ERR_TOO_BIG);
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(log_add_sink(log_sink, NULL));
	const char *base = getenv("TMPDIR");
	snprintf(root, sizeof(root), "%s/hakkas-plist-XXXXXX", base && *base ? base : "/tmp");
	if (mkdtemp(root) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	RUN(test_array);
	RUN(test_object);
	RUN(test_malformed);
	RUN(test_load);
	log_remove_sink(log_sink, NULL);
	if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
	return test_result();
}
