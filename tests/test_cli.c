/*
 * The POSIX command line (platform/posix/cli.h, 05 §1.2), run in-process:
 * parsing every command and option, usage errors, the configuration's
 * precedence (defaults < file < options), and the output of list types,
 * describe type (the 02 §3.5 component:component payload) and describe
 * topology. Configuration and graph files live in a fresh temporary
 * directory that is removed afterwards.
 */
#define _XOPEN_SOURCE 700

#include "test.h"

#include <ftw.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

#include "blocktypes/block_types.h"
#include "platform/pal.h"
#include "platform/posix/cli.h"
#include "system/log.h"

static char root[PAL_PATH_MAX];

/* ---- Helpers ------------------------------------------------------------------ */

static void append(char *buf, size_t size, const char *fmt, ...) LOG_PRINTF_FORMAT(3, 4);

static void append(char *buf, size_t size, const char *fmt, ...)
{
	size_t len = strlen(buf);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + len, size - len, fmt, ap);
	va_end(ap);
}

/* Log lines, without the "<level> <file>:<line>: " prefix, one per line. */
static char logs[8192];

static void log_sink(void *ctx, enum log_level level, const char *line, size_t len)
{
	(void)ctx;
	(void)len;
	const char *msg = strstr(line, ": ");
	append(logs, sizeof(logs), "%s %s\n", log_level_name(level), msg != NULL ? msg + 2 : line);
}

/* root/name, made a directory. */
static const char *subdir(const char *name)
{
	static char buf[4][PAL_PATH_MAX];
	static int next;
	char *p = buf[next++ % 4];
	CHECK(snprintf(p, PAL_PATH_MAX, "%s/%s", root, name) < PAL_PATH_MAX);
	CHECK(pal_storage_mkdir(p) == PAL_OK);
	return p;
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

/* argv for "hakkas" followed by first and the rest of the NULL-terminated
 * arguments; returns argc. */
#define ARGV_MAX 24
static int build_argv(char **argv, const char *first, va_list ap)
{
	int argc = 0;
	argv[argc++] = "hakkas";
	for (const char *a = first; a != NULL && argc < ARGV_MAX - 1; a = va_arg(ap, const char *))
		argv[argc++] = (char *)a;
	argv[argc] = NULL;
	return argc;
}

static CliOptions opts;

/* cli_parse of the arguments (NULL-terminated), with the log cleared. */
static int parse(const char *first, ...)
{
	char *argv[ARGV_MAX];
	va_list ap;
	va_start(ap, first);
	int argc = build_argv(argv, first, ap);
	va_end(ap);
	logs[0] = '\0';
	return cli_parse(argc, argv, &opts);
}

/* What cli_main writes to out, and its exit code. */
static char output[16384];

static int run(const char *first, ...)
{
	char *argv[ARGV_MAX];
	va_list ap;
	va_start(ap, first);
	int argc = build_argv(argv, first, ap);
	va_end(ap);
	logs[0] = '\0';
	char *buf = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&buf, &size);
	CHECK(out != NULL);
	int rc = cli_main(argc, argv, out);
	fclose(out);
	snprintf(output, sizeof(output), "%s", buf != NULL ? buf : "");
	free(buf);
	log_set_threshold(LOG_LEVEL_WARN);
	return rc;
}

/* ---- Parsing --------------------------------------------------------------------- */

static void test_parse_commands(void)
{
	CHECK(parse("list", "types", NULL) == CLI_EXIT_OK);
	CHECK(opts.command == CLI_LIST_TYPES);
	CHECK_STREQ(opts.basedir, ".");
	CHECK_STREQ(opts.confdir, "/etc/hakkas");
	CHECK(!opts.level_set);

	CHECK(parse("describe", "type", "math/Add2", NULL) == CLI_EXIT_OK);
	CHECK(opts.command == CLI_DESCRIBE_TYPE);
	CHECK_STREQ(opts.arg, "math/Add2");
	CHECK(parse("describe", "topology", "g.json", NULL) == CLI_EXIT_OK);
	CHECK(opts.command == CLI_DESCRIBE_TOPOLOGY);
	CHECK_STREQ(opts.arg, "g.json");

	const char *subs[] = {"add", "passwd", "del", "list"};
	for (size_t i = 0; i < 4; i++) {
		CHECK(parse("user", subs[i], "alice", "--role", "editor", NULL) == CLI_EXIT_OK);
		CHECK(opts.command == CLI_USER);
		CHECK_STREQ(opts.arg, subs[i]);
	}

	CHECK(parse("run", "daemon", NULL) == CLI_EXIT_OK);
	CHECK(opts.command == CLI_RUN_DAEMON);
	CHECK(opts.id == NULL && opts.webdir == NULL && opts.host == NULL && opts.port == NULL &&
	      opts.users == NULL);
	CHECK(parse("run", "process", "a.json:b.json", NULL) == CLI_EXIT_OK);
	CHECK(opts.command == CLI_RUN_PROCESS);
	CHECK_STREQ(opts.arg, "a.json:b.json");
	CHECK(parse("run", "--id", "e1", "--webdir", "w", "--host", "127.0.0.1", "--port", "9000",
		    "--users", "u.json", "--port", "9001", "process", "g.json", NULL) ==
	      CLI_EXIT_OK);
	CHECK(opts.command == CLI_RUN_PROCESS);
	CHECK_STREQ(opts.id, "e1");
	CHECK_STREQ(opts.webdir, "w");
	CHECK_STREQ(opts.host, "127.0.0.1");
	CHECK_STREQ(opts.port, "9001"); /* the last one wins */
	CHECK_STREQ(opts.users, "u.json");

	CHECK(parse("--help", NULL) == CLI_EXIT_OK && opts.command == CLI_HELP);
	CHECK(parse("-h", "bogus", NULL) == CLI_EXIT_OK && opts.command == CLI_HELP);
	CHECK(parse("--fine", "--help", NULL) == CLI_EXIT_OK && opts.command == CLI_HELP);
	CHECK(parse("--version", NULL) == CLI_EXIT_OK && opts.command == CLI_VERSION);
	CHECK_STREQ(logs, "");
}

static void test_parse_global_options(void)
{
	CHECK(parse("--fine", "list", "types", NULL) == CLI_EXIT_OK);
	CHECK(opts.level_set && opts.level == LOG_LEVEL_FINE);
	CHECK(parse("--info", "list", "types", NULL) == CLI_EXIT_OK);
	CHECK(opts.level_set && opts.level == LOG_LEVEL_INFO);
	CHECK(parse("--fine", "--warn", "list", "types", NULL) == CLI_EXIT_OK);
	CHECK(opts.level_set && opts.level == LOG_LEVEL_WARN);
	CHECK(parse("--basedir", "/var/lib/hakkas", "--confdir", "conf", "run", "daemon", NULL) ==
	      CLI_EXIT_OK);
	CHECK_STREQ(opts.basedir, "/var/lib/hakkas");
	CHECK_STREQ(opts.confdir, "conf");
	CHECK(opts.command == CLI_RUN_DAEMON);
}

/* The arguments are a usage error whose log contains want. */
static void usage_error(const char *want, const char *first, ...)
{
	char *argv[ARGV_MAX];
	va_list ap;
	va_start(ap, first);
	int argc = build_argv(argv, first, ap);
	va_end(ap);
	logs[0] = '\0';
	CHECK(cli_parse(argc, argv, &opts) == CLI_EXIT_USAGE);
	if (strstr(logs, want) == NULL || strstr(logs, "hakkas --help") == NULL)
		fprintf(stderr, "usage_error: log \"%s\", expected \"%s\"\n", logs, want);
	CHECK(strstr(logs, want) != NULL);
	CHECK(strncmp(logs, "error ", 6) == 0);
}

static void test_parse_usage_errors(void)
{
	usage_error("no command given", NULL);
	usage_error("no command given", "--fine", NULL);
	usage_error("unknown option '--error'", "--error", "list", "types", NULL);
	usage_error("unknown option '-x'", "-x", NULL);
	usage_error("option --basedir needs a value", "--basedir", NULL);
	usage_error("option --confdir needs a value", "--confdir", "", "list", "types", NULL);
	usage_error("unknown command 'lists'", "lists", "types", NULL);
	usage_error("unknown option '--port'", "--port", "1", "run", "daemon", NULL);
	usage_error("list: expected 'types'", "list", NULL);
	usage_error("list: expected 'types'", "list", "graphs", NULL);
	usage_error("list types: unexpected argument 'x'", "list", "types", "x", NULL);
	usage_error("describe: expected", "describe", NULL);
	usage_error("describe: expected", "describe", "graph", "x", NULL);
	usage_error("describe type: missing the type name", "describe", "type", NULL);
	usage_error("describe type: unexpected argument 'b'", "describe", "type", "a", "b", NULL);
	usage_error("describe topology: missing the file name", "describe", "topology", "",
		    NULL);
	usage_error("user: expected 'add', 'passwd', 'del' or 'list'", "user", NULL);
	usage_error("user: expected", "user", "remove", "bob", NULL);
	usage_error("run: expected 'process <files>' or 'daemon'", "run", NULL);
	usage_error("run: expected", "run", "--port", "1", NULL);
	usage_error("run: expected", "run", "start", NULL);
	usage_error("run: unknown option '--fine'", "run", "--fine", "daemon", NULL);
	usage_error("option --port needs a value", "run", "--port", NULL);
	usage_error("option --id needs a value", "run", "--id", "", "daemon", NULL);
	usage_error("run process: missing the graph file list", "run", "process", NULL);
	usage_error("run process: unexpected argument 'b'", "run", "process", "a", "b", NULL);
	usage_error("run daemon: unexpected argument 'x'", "run", "daemon", "x", NULL);
	usage_error("list types: unexpected argument '--fine'", "list", "types", "--fine", NULL);
}

/* ---- Configuration ------------------------------------------------------------------ */

static void test_config_defaults(void)
{
	const char *conf = subdir("empty-conf");
	Config cfg;
	CHECK(parse("--confdir", conf, "list", "types", NULL) == CLI_EXIT_OK);
	CHECK(cli_config(&opts, &cfg) == CLI_EXIT_OK);
	char cwd[PAL_PATH_MAX];
	CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
	CHECK_STREQ(cfg.webdir, join(cwd, "frontend/src"));
	CHECK_STREQ(cfg.datadir, ".");
	CHECK_STREQ(cfg.confdir, conf);
	CHECK_STREQ(cfg.file, join(conf, "hakkas.json"));
	CHECK_STREQ(cfg.auth.users_file, join(conf, "users.json"));
	CHECK(cfg.port == 8080);
	CHECK(cfg.log.level == LOG_LEVEL_WARN && log_threshold() == LOG_LEVEL_WARN);

	CHECK(parse("--confdir", conf, "--basedir", "/data", "list", "types", NULL) ==
	      CLI_EXIT_OK);
	CHECK(cli_config(&opts, &cfg) == CLI_EXIT_OK);
	CHECK_STREQ(cfg.datadir, "/data");
}

static void test_config_precedence(void)
{
	const char *conf = subdir("conf");
	put(join(conf, "hakkas.json"),
	    "{\"engine_id\":\"file-id\",\"label\":\"From file\",\"host\":\"10.0.0.1\","
	    "\"port\":9000,\"webdir\":\"web\",\"datadir\":\"/file/data\","
	    "\"log\":{\"level\":\"error\"},\"auth\":{\"users_file\":\"/file/users.json\"}}");
	Config cfg;

	/* The file over the defaults; its relative webdir is relative to
	 * confdir; --basedir does not override a configured datadir. */
	CHECK(parse("--confdir", conf, "--basedir", "/base", "run", "daemon", NULL) ==
	      CLI_EXIT_OK);
	CHECK(cli_config(&opts, &cfg) == CLI_EXIT_OK);
	CHECK_STREQ(cfg.engine_id, "file-id");
	CHECK_STREQ(cfg.label, "From file");
	CHECK_STREQ(cfg.host, "10.0.0.1");
	CHECK(cfg.port == 9000);
	CHECK_STREQ(cfg.webdir, join(conf, "web"));
	CHECK_STREQ(cfg.datadir, "/file/data");
	CHECK_STREQ(cfg.auth.users_file, "/file/users.json");
	CHECK(cfg.log.level == LOG_LEVEL_ERROR && log_threshold() == LOG_LEVEL_ERROR);

	/* The options over the file. */
	CHECK(parse("--info", "--confdir", conf, "run", "--id", "opt-id", "--webdir", "opt/web",
		    "--host", "::", "--port", "9100", "--users", "opt-users.json", "process",
		    "x.json", NULL) == CLI_EXIT_OK);
	CHECK(cli_config(&opts, &cfg) == CLI_EXIT_OK);
	CHECK_STREQ(cfg.engine_id, "opt-id");
	CHECK_STREQ(cfg.label, "From file");
	CHECK_STREQ(cfg.host, "::");
	CHECK(cfg.port == 9100);
	CHECK_STREQ(cfg.webdir, "opt/web"); /* as given: relative to the working directory */
	CHECK_STREQ(cfg.auth.users_file, "opt-users.json");
	CHECK(cfg.log.level == LOG_LEVEL_INFO && log_threshold() == LOG_LEVEL_INFO);
	log_set_threshold(LOG_LEVEL_WARN);

	/* An invalid option value is a usage error naming the option. */
	CHECK(parse("--confdir", conf, "run", "--port", "65535", "daemon", NULL) == CLI_EXIT_OK);
	logs[0] = '\0';
	CHECK(cli_config(&opts, &cfg) == CLI_EXIT_USAGE);
	CHECK(strstr(logs, "error --port: port: expected an integer from 1 to 65534") != NULL);
	CHECK(parse("--confdir", conf, "run", "--id",
		    "12345678901234567890123456789012345678901234567890123456789012345", "daemon",
		    NULL) == CLI_EXIT_OK);
	CHECK(cli_config(&opts, &cfg) == CLI_EXIT_USAGE);
	log_set_threshold(LOG_LEVEL_WARN);

	/* An invalid or malformed file is a runtime failure, for every command. */
	const char *bad = subdir("bad-conf");
	put(join(bad, "hakkas.json"), "{\"port\": 0}");
	CHECK(run("--confdir", bad, "list", "types", NULL) == CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "port: expected an integer") != NULL);
	CHECK_STREQ(output, "");
	put(join(bad, "hakkas.json"), "{");
	CHECK(run("--confdir", bad, "list", "types", NULL) == CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "not a valid configuration file") != NULL);
	/* --fine applies while the file is read. */
	const char *none = subdir("no-conf");
	CHECK(run("--fine", "--confdir", none, "list", "types", NULL) == CLI_EXIT_OK);
	CHECK(strstr(logs, "info") != NULL && strstr(logs, "not found") != NULL);
}

/* ---- Commands ----------------------------------------------------------------------- */

static void test_list_types(void)
{
	const char *conf = subdir("list-conf");
	char want[8192] = "";
	CHECK(block_types_register_builtins() == BLOCK_TYPES_OK);
	for (size_t i = 0; i < block_types_count(); i++)
		append(want, sizeof(want), "%s\n", block_types_at(i)->name);
	block_types_reset();
	CHECK(run("--confdir", conf, "list", "types", NULL) == CLI_EXIT_OK);
	CHECK_STREQ(output, want);
	CHECK(strncmp(output, "math/Add4\nmath/Add2\n", 20) == 0);
	CHECK(strstr(output, "timing/Interval\n") != NULL);
	CHECK_STREQ(logs, "");
	/* The registry is left empty. */
	CHECK(block_types_count() == 0);
}

static void test_describe_type(void)
{
	const char *conf = subdir("type-conf");
	CHECK(run("--confdir", conf, "describe", "type", "math/Add2", NULL) == CLI_EXIT_OK);
	/* The component:component payload of 02 §3.5, exactly. */
	CHECK_STREQ(output,
		    "{\"name\":\"math/Add2\",\"description\":\"out = in1+in2\",\"subgraph\":false,"
		    "\"icon\":\"add\",\"inPorts\":["
		    "{\"id\":\"in1\",\"description\":\"Operand 1\",\"type\":\"number\","
		    "\"addressable\":false},"
		    "{\"id\":\"in2\",\"description\":\"Operand 2\",\"type\":\"number\","
		    "\"addressable\":false}],\"outPorts\":["
		    "{\"id\":\"out\",\"description\":\"Result\",\"type\":\"number\","
		    "\"addressable\":false}]}\n");
	CHECK_STREQ(logs, "");

	CHECK(run("--confdir", conf, "describe", "type", "math/Nope", NULL) == CLI_EXIT_FAILURE);
	CHECK_STREQ(output, "");
	CHECK(strstr(logs, "error unknown block type: math/Nope") != NULL);
}

static void test_describe_topology(void)
{
	const char *conf = subdir("topo-conf");
	const char *file = join(root, "topology.json");
	put(file, "{\n  \"id\": \"demo\",\n  \"name\": \"Demo\",\n  \"started\": true,\n"
		  "  \"blocks\": {\n"
		  "    \"add\": {\"component\": \"math/Add2\", "
		  "\"metadata\": {\"x\": 10, \"y\": 20}},\n"
		  "    \"old\": {\"component\": \"vendor/Gone\"}\n  },\n"
		  "  \"connections\": [\n"
		  "    {\"src\": {\"data\": 2.5}, "
		  "\"tgt\": {\"node\": \"add\", \"port\": \"in1\"}},\n"
		  "    {\"src\": {\"node\": \"old\", \"port\": \"out\"}, "
		  "\"tgt\": {\"node\": \"add\", \"port\": \"in2\"}}\n  ],\n"
		  "  \"outports\": [{\"name\": \"sum\", \"node\": \"add\", "
		  "\"port\": \"out\"}]\n}\n");
	CHECK(run("--confdir", conf, "describe", "topology", file, NULL) == CLI_EXIT_OK);
	CHECK_STREQ(output,
		    "{\"id\":\"demo\",\"name\":\"Demo\",\"description\":\"\",\"icon\":\"\","
		    "\"started\":true,\"blocks\":{"
		    "\"add\":{\"component\":\"math/Add2\",\"metadata\":{\"x\":10,\"y\":20}},"
		    "\"old\":{\"component\":\"vendor/Gone\",\"metadata\":{\"x\":50,\"y\":50}}},"
		    "\"connections\":["
		    "{\"src\":{\"data\":2.5},\"tgt\":{\"node\":\"add\",\"port\":\"in1\"}},"
		    "{\"src\":{\"node\":\"old\",\"port\":\"out\"},"
		    "\"tgt\":{\"node\":\"add\",\"port\":\"in2\"}}],"
		    "\"inports\":[],"
		    "\"outports\":[{\"name\":\"sum\",\"node\":\"add\",\"port\":\"out\"}]}"
		    "\n");
	/* The placeholder is reported on the log, not the output. */
	CHECK(strstr(logs, "warn") != NULL && strstr(logs, "vendor/Gone") != NULL);

	CHECK(run("--confdir", conf, "describe", "topology", join(root, "missing.json"), NULL) ==
	      CLI_EXIT_FAILURE);
	CHECK_STREQ(output, "");
	CHECK(strstr(logs, "missing.json") != NULL);
	put(file, "{\"name\": \"no id\"}");
	CHECK(run("--confdir", conf, "describe", "topology", file, NULL) == CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "not a valid graph file") != NULL);
}

static void test_not_implemented(void)
{
	const char *conf = subdir("ni-conf");
	CHECK(run("--confdir", conf, "user", "add", "alice", NULL) == CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "error user add: not implemented yet") != NULL);
	CHECK(run("--confdir", conf, "run", "--port", "9000", "daemon", NULL) == CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "error run: not implemented yet") != NULL);
	CHECK(run("--confdir", conf, "run", "process", "a.json", NULL) == CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "error run: not implemented yet") != NULL);
	/* run validates its options before that. */
	CHECK(run("--confdir", conf, "run", "--port", "0", "daemon", NULL) == CLI_EXIT_USAGE);
	CHECK(strstr(logs, "not implemented") == NULL);
	CHECK_STREQ(output, "");
}

static void test_help_and_version(void)
{
	CHECK(run("--help", NULL) == CLI_EXIT_OK);
	CHECK(strncmp(output, "usage: hakkas", 13) == 0);
	CHECK(strstr(output, "describe topology <filename>") != NULL);
	CHECK(strstr(output, "--users <file>") != NULL);
	CHECK_STREQ(logs, "");
	CHECK(run("--version", NULL) == CLI_EXIT_OK);
	CHECK(strncmp(output, "hakkas ", 7) == 0);
	CHECK(run(NULL) == CLI_EXIT_USAGE);
	CHECK_STREQ(output, "");
	CHECK(strstr(logs, "no command given") != NULL);
}

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
	(void)sb;
	(void)flag;
	(void)ftw;
	return remove(path);
}

int main(void)
{
	CHECK(pal_init() == PAL_OK);
	CHECK(log_add_sink(log_sink, NULL));
	const char *base = getenv("TMPDIR");
	snprintf(root, sizeof(root), "%s/hakkas-cli-XXXXXX", base && *base ? base : "/tmp");
	if (mkdtemp(root) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	RUN(test_parse_commands);
	RUN(test_parse_global_options);
	RUN(test_parse_usage_errors);
	RUN(test_config_defaults);
	RUN(test_config_precedence);
	RUN(test_list_types);
	RUN(test_describe_type);
	RUN(test_describe_topology);
	RUN(test_not_implemented);
	RUN(test_help_and_version);

	log_remove_sink(log_sink, NULL);
	if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
	return test_result();
}
