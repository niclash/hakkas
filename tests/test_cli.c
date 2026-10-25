/*
 * The POSIX command line (platform/posix/cli.h, 05 §1.2), run in-process:
 * parsing every command and option, usage errors, the configuration's
 * precedence (defaults < file < options), and the output of list types,
 * describe type (the 02 §3.5 component:component payload) and describe
 * topology, and run as the POSIX host (platform/posix/host.h): which saved
 * processes it loads and starts, with their saved bindings, and its
 * failures. Configuration and
 * graph files live in a fresh temporary directory that is removed
 * afterwards.
 */
#define _XOPEN_SOURCE 700

#include "test.h"

#include <ftw.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "app/graph_files.h"
#include "blocktypes/block_types.h"
#include "platform/pal.h"
#include "platform/posix/cli.h"
#include "protocol/auth.h"
#include "protocol/users.h"
#include "system/crypto.h"
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
static char logs[32768];

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

/* What cli_main reads from in (not a terminal): the password lines. */
static const char *input = "";

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
	FILE *in = input[0] != '\0' ? fmemopen((void *)input, strlen(input), "r")
				    : fopen("/dev/null", "r");
	CHECK(in != NULL);
	int rc = cli_main(argc, argv, in, out);
	fclose(in);
	input = "";
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

	CHECK(parse("user", "add", "alice", "--role", "editor", NULL) == CLI_EXIT_OK);
	CHECK(opts.command == CLI_USER);
	CHECK_STREQ(opts.arg, "add");
	CHECK_STREQ(opts.user, "alice");
	CHECK_STREQ(opts.role, "editor");
	CHECK(parse("user", "add", "--role", "viewer", "bob", NULL) == CLI_EXIT_OK);
	CHECK_STREQ(opts.user, "bob");
	CHECK_STREQ(opts.role, "viewer");
	CHECK(parse("user", "passwd", "alice", NULL) == CLI_EXIT_OK);
	CHECK(opts.command == CLI_USER);
	CHECK_STREQ(opts.arg, "passwd");
	CHECK_STREQ(opts.user, "alice");
	CHECK(opts.role == NULL);
	CHECK(parse("user", "del", "alice", NULL) == CLI_EXIT_OK);
	CHECK_STREQ(opts.arg, "del");
	CHECK_STREQ(opts.user, "alice");
	CHECK(parse("user", "list", NULL) == CLI_EXIT_OK);
	CHECK_STREQ(opts.arg, "list");
	CHECK(opts.user == NULL);

	CHECK(parse("run", NULL) == CLI_EXIT_OK);
	CHECK(opts.command == CLI_RUN);
	CHECK(opts.id == NULL && opts.webdir == NULL && opts.host == NULL && opts.port == NULL &&
	      opts.users == NULL);
	CHECK(parse("run", "--id", "e1", "--webdir", "w", "--host", "127.0.0.1", "--port", "9000",
		    "--users", "u.json", "--port", "9001", NULL) == CLI_EXIT_OK);
	CHECK(opts.command == CLI_RUN);
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
	CHECK(parse("--basedir", "/var/lib/hakkas", "--confdir", "conf", "run", NULL) ==
	      CLI_EXIT_OK);
	CHECK_STREQ(opts.basedir, "/var/lib/hakkas");
	CHECK_STREQ(opts.confdir, "conf");
	CHECK(opts.command == CLI_RUN);
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
	usage_error("unknown option '--port'", "--port", "1", "run", NULL);
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
	usage_error("user add: missing the user name", "user", "add", "--role", "admin", NULL);
	usage_error("user add: missing --role <role>", "user", "add", "alice", NULL);
	usage_error("user add: invalid role 'root'", "user", "add", "alice", "--role", "root",
		    NULL);
	usage_error("option --role needs a value", "user", "add", "alice", "--role", NULL);
	usage_error("user add: invalid user name", "user", "add",
		    "a-name-that-is-longer-than-32-bytes", "--role", "viewer", NULL);
	usage_error("user add: invalid user name", "user", "add", "tab\there", "--role",
		    "viewer", NULL);
	usage_error("user add: unexpected argument 'bob'", "user", "add", "alice", "bob", NULL);
	usage_error("user passwd: missing the user name", "user", "passwd", NULL);
	usage_error("user passwd: unknown option '--role'", "user", "passwd", "alice", "--role",
		    "admin", NULL);
	usage_error("user del: unexpected argument 'b'", "user", "del", "a", "b", NULL);
	usage_error("user list: unexpected argument 'x'", "user", "list", "x", NULL);
	/* What run runs is <datadir>: the old modes are gone (plan task 62). */
	usage_error("run: unexpected argument 'daemon'", "run", "daemon", NULL);
	usage_error("run: unexpected argument 'process'", "run", "process", "a.json", NULL);
	usage_error("run: unexpected argument 'processes'", "run", "--port", "1", "processes",
		    "p.json", NULL);
	usage_error("run: unknown option '--fine'", "run", "--fine", NULL);
	usage_error("option --port needs a value", "run", "--port", NULL);
	usage_error("option --id needs a value", "run", "--id", "", NULL);
	usage_error("list types: unexpected argument '--fine'", "list", "types", "--fine", NULL);
}

/* ---- Configuration ------------------------------------------------------------------ */

static void test_config_defaults(void)
{
	const char *conf = subdir("empty-conf");
	Config cfg;
	CHECK(parse("--confdir", conf, "list", "types", NULL) == CLI_EXIT_OK);
	CHECK(cli_config(&opts, &cfg) == CLI_EXIT_OK);
	CHECK_STREQ(cfg.webdir, HAKKAS_DEFAULT_WEBDIR);
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
	CHECK(parse("--confdir", conf, "--basedir", "/base", "run", NULL) == CLI_EXIT_OK);
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
		    "--host", "::", "--port", "9100", "--users", "opt-users.json", NULL) ==
	      CLI_EXIT_OK);
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
	CHECK(parse("--confdir", conf, "run", "--port", "65535", NULL) == CLI_EXIT_OK);
	logs[0] = '\0';
	CHECK(cli_config(&opts, &cfg) == CLI_EXIT_USAGE);
	CHECK(strstr(logs, "error --port: port: expected an integer from 1 to 65534") != NULL);
	CHECK(parse("--confdir", conf, "run", "--id",
		    "12345678901234567890123456789012345678901234567890123456789012345", NULL) ==
	      CLI_EXIT_OK);
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
		  "\"tgt\": {\"block\": \"add\", \"port\": \"in1\"}},\n"
		  "    {\"src\": {\"process\": \"old\", \"port\": \"out\"}, "
		  "\"tgt\": {\"block\": \"add\", \"port\": \"in2\"}}\n  ],\n"
		  "  \"outports\": [{\"name\": \"sum\", \"block\": \"add\", "
		  "\"port\": \"out\"}]\n}\n");
	CHECK(run("--confdir", conf, "describe", "topology", file, NULL) == CLI_EXIT_OK);
	CHECK_STREQ(output,
		    "{\"id\":\"demo\",\"name\":\"Demo\",\"description\":\"\",\"icon\":\"\","
		    "\"started\":true,\"blocks\":{"
		    "\"add\":{\"component\":\"math/Add2\",\"metadata\":{\"x\":10,\"y\":20}},"
		    "\"old\":{\"component\":\"vendor/Gone\",\"metadata\":{\"x\":50,\"y\":50}}},"
		    "\"connections\":["
		    "{\"src\":{\"data\":2.5},\"tgt\":{\"block\":\"add\",\"port\":\"in1\"}},"
		    "{\"src\":{\"block\":\"old\",\"port\":\"out\"},"
		    "\"tgt\":{\"block\":\"add\",\"port\":\"in2\"}}],"
		    "\"inports\":[],"
		    "\"outports\":[{\"name\":\"sum\",\"block\":\"add\",\"port\":\"out\"}]}"
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

/* A free loopback port whose successor is free too (the WebSocket's,
 * port + 1); 0 if none was found. */
static uint16_t free_port_pair(void)
{
	for (int i = 0; i < 50; i++) {
		int a = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		int b = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		struct sockaddr_in sa = {.sin_family = AF_INET};
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		socklen_t len = sizeof(sa);
		uint16_t port = 0;
		if (a >= 0 && b >= 0 && bind(a, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
		    getsockname(a, (struct sockaddr *)&sa, &len) == 0) {
			port = ntohs(sa.sin_port);
			sa.sin_port = htons((uint16_t)(port + 1));
			if (port >= 65534 || bind(b, (struct sockaddr *)&sa, sizeof(sa)) != 0)
				port = 0;
		}
		if (a >= 0)
			close(a);
		if (b >= 0)
			close(b);
		if (port != 0)
			return port;
	}
	return 0;
}

/* run (05 §1.2, §1.4): the engine through the command line. Its startup
 * and shutdown are tested in test_engine.c; here the exit codes. */
static void test_run(void)
{
	const char *conf = subdir("run-conf");
	const char *base = subdir("run-base");
	/* run validates its options first. */
	CHECK(run("--confdir", conf, "run", "--port", "0", NULL) == CLI_EXIT_USAGE);
	CHECK(!pal_storage_exists(join(conf, "hakkas.json")));

	/* A clean stop: requested before the loop runs, it stops it at once
	 * (01 §0.2), and run exits 0. */
	uint16_t port = free_port_pair();
	CHECK(port != 0);
	char ports[16];
	snprintf(ports, sizeof(ports), "%u", (unsigned)port);
	pal_request_stop();
	CHECK(run("--info", "--confdir", conf, "--basedir", base, "run", "--host", "127.0.0.1",
		  "--port", ports, NULL) == CLI_EXIT_OK);
	CHECK(strstr(logs, " ready: web server on 127.0.0.1 port ") != NULL);
	CHECK(strstr(logs, "info engine stopped") != NULL);
	CHECK(pal_storage_exists(join(base, "graphs")) && pal_storage_exists(join(base, "types")) &&
	      pal_storage_exists(join(base, "bindings")));
	/* Step 1 has generated the server secret (05 §1.1). */
	CHECK(pal_storage_exists(join(conf, "hakkas.json")));
	CHECK_STREQ(output, "");

	/* A malformed configuration file refuses every command, run too. */
	put(join(conf, "hakkas.json"), "{\"port\": 8080,}");
	CHECK(run("--confdir", conf, "run", NULL) == CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "not a valid configuration file") != NULL);
	CHECK(strstr(logs, "ready") == NULL);
}

/* ---- run as the host (05 §1.2, platform/posix/host.h) ------------------------------ */

#define G_STARTED                                                                                  \
	"{\"id\":\"g1\",\"started\":true,\"blocks\":{\"a\":{\"component\":\"math/Add2\"}}}"
#define G_STOPPED "{\"id\":\"g2\",\"started\":false}"
#define G_NOFLAG "{\"id\":\"g3\"}"
/* Two processes to link: l1's outport out (a.out, a.in1 = 2) to l2's inport
 * in (b.in1). */
#define G_LINK_SRC                                                                                 \
	"{\"id\":\"l1\",\"blocks\":{\"a\":{\"component\":\"math/Add2\"}},\"connections\":"    \
	"[{\"src\":{\"data\":2},\"tgt\":{\"block\":\"a\",\"port\":\"in1\"}}],\"outports\":"     \
	"[{\"name\":\"out\",\"block\":\"a\",\"port\":\"out\"}]}"
#define G_LINK_TGT                                                                                 \
	"{\"id\":\"l2\",\"blocks\":{\"b\":{\"component\":\"math/Add2\"}},\"inports\":"        \
	"[{\"name\":\"in\",\"block\":\"b\",\"port\":\"in1\"}]}"

static const char *host_conf, *host_base;

/* run --info ... run --host 127.0.0.1 --port <free>, stopped before the
 * loop runs (01 §0.2): the processes are loaded and started, then the
 * engine stops at once. */
static int run_host(void)
{
	uint16_t port = free_port_pair();
	CHECK(port != 0);
	char ports[16];
	snprintf(ports, sizeof(ports), "%u", (unsigned)port);
	pal_request_stop();
	/* A failed startup clears the request with the loop (pal_loop_close). */
	return run("--info", "--confdir", host_conf, "--basedir", host_base, "run", "--host",
		   "127.0.0.1", "--port", ports, NULL);
}

static bool logged(const char *text)
{
	if (strstr(logs, text) != NULL)
		return true;
	fprintf(stderr, "expected \"%s\" in:\n%s", text, logs);
	return false;
}

/* A fresh basedir (its own per test) with graphs/ and bindings/. */
static void host_setup(const char *base)
{
	host_conf = subdir("host-conf");
	host_base = subdir(base);
	char dir[PAL_PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", join(host_base, "graphs"));
	CHECK(pal_storage_mkdir(dir) == PAL_OK);
	snprintf(dir, sizeof(dir), "%s", join(host_base, "bindings"));
	CHECK(pal_storage_mkdir(dir) == PAL_OK);
}

/* run: every saved process, the flag decides; a file that does not load
 * is skipped; the saved links are made once every process is loaded and
 * before any starts, a link that cannot be made is skipped (plan task 62). */
static void test_run_saved(void)
{
	host_setup("host-saved");
	put(join(host_base, "graphs/g1.json"), G_STARTED);
	put(join(host_base, "graphs/g2.json"), G_STOPPED);
	put(join(host_base, "graphs/bad.json"), "nonsense");
	CHECK(run_host() == CLI_EXIT_OK);
	CHECK(logged("graphs/bad.json: not a valid graph file"));
	CHECK(logged("warn not every process in "));
	CHECK(logged("info process g1 started\n"));
	CHECK(strstr(logs, "process g2 started") == NULL);
	CHECK(logged("info 2 processes loaded, 1 started\n"));
	CHECK(pal_storage_remove(join(host_base, "graphs/bad.json")) == PAL_OK);

	/* Links (plan task 21f, saved since plan task 62). */
	host_setup("host-links");
	put(join(host_base, "graphs/l1.json"), G_LINK_SRC);
	put(join(host_base, "graphs/l2.json"), G_LINK_TGT);
	put(join(host_base, "links.json"), "{\"links\":[{\"from\":\"l1.out\",\"to\":\"l2.in\"},"
					   "{\"from\":\"l1.out\",\"to\":\"l2.nope\"}]}");
	CHECK(run_host() == CLI_EXIT_OK);
	CHECK(logged("info link l1.out -> l2.in made\n"));
	CHECK(logged("links.json: links[1]: l1.out -> l2.nope: Unknown port: l2.nope: skipped\n"));
	const char *made = strstr(logs, "link l1.out -> l2.in made");
	const char *loaded = strstr(logs, "2 processes loaded");
	CHECK(made != NULL && loaded != NULL && made < loaded);
	/* A malformed links file is not applied; startup goes on. */
	put(join(host_base, "links.json"), "{\"links\":3}");
	CHECK(run_host() == CLI_EXIT_OK);
	CHECK(logged("links.json: links: expected an array\n"));
	CHECK(strstr(logs, "made") == NULL);
	CHECK(logged("info 2 processes loaded, 0 started\n"));

	/* The sister project's example graph, saved. */
	host_setup("host-example");
	char *text;
	size_t len;
	CHECK(graph_files_read(HAKKAS_EXAMPLES_DIR "/example.json", 1 << 20, &text, &len) ==
	      GRAPH_FILES_OK);
	CHECK(pal_storage_write(join(host_base, "graphs/example.json"), text, len) == PAL_OK);
	free(text);
	CHECK(run_host() == CLI_EXIT_OK);
	CHECK(logged("info 1 process loaded, "));
}

/* ---- run with saved environment bindings (04 §0.1, §4) -------------------------- */

/* A fake environment program (04 §0.1): serves a Unix socket, describes
 * itself and sends its current values on each connection, and keeps what
 * hakkas sends. */
static struct {
	pthread_mutex_t lock;
	pthread_t thread;
	int listen_fd;
	bool stop;	      /* guarded by lock */
	char received[8192]; /* guarded by lock */
} env_srv = {.lock = PTHREAD_MUTEX_INITIALIZER, .listen_fd = -1};

#define ENV_DESCRIBE                                                                               \
	"{\"describe\":{\"name\":\"sim\",\"outPorts\":[{\"id\":\"ain\",\"type\":\"number\","       \
	"\"addressable\":true,\"count\":2}],"                                                      \
	"\"inPorts\":[{\"id\":\"aout\",\"type\":\"number\"}]}}\n"                                  \
	"{\"value\":{\"port\":\"ain\",\"index\":1,\"value\":2}}\n"                                 \
	"{\"value\":{\"port\":\"ain\",\"index\":2,\"value\":0}}\n"

static void *env_server(void *arg)
{
	(void)arg;
	int conn = -1;
	for (;;) {
		pthread_mutex_lock(&env_srv.lock);
		bool stop = env_srv.stop;
		pthread_mutex_unlock(&env_srv.lock);
		if (stop)
			break;
		struct pollfd p = {conn >= 0 ? conn : env_srv.listen_fd, POLLIN, 0};
		if (poll(&p, 1, 5) <= 0)
			continue;
		if (conn < 0) {
			conn = accept(env_srv.listen_fd, NULL, NULL);
			if (conn >= 0 && send(conn, ENV_DESCRIBE, strlen(ENV_DESCRIBE),
					      MSG_NOSIGNAL) != (ssize_t)strlen(ENV_DESCRIBE)) {
				close(conn);
				conn = -1;
			}
			continue;
		}
		char buf[512];
		ssize_t n = recv(conn, buf, sizeof(buf) - 1, 0);
		if (n <= 0) {
			close(conn);
			conn = -1;
			continue;
		}
		buf[n] = '\0';
		pthread_mutex_lock(&env_srv.lock);
		append(env_srv.received, sizeof(env_srv.received), "%s", buf);
		pthread_mutex_unlock(&env_srv.lock);
	}
	if (conn >= 0)
		close(conn);
	return NULL;
}

/* Whether the program received text, waiting for it up to 5 s. */
static bool env_received(const char *text)
{
	uint64_t end = pal_uptime_ms() + 5000;
	for (;;) {
		pthread_mutex_lock(&env_srv.lock);
		bool found = strstr(env_srv.received, text) != NULL;
		pthread_mutex_unlock(&env_srv.lock);
		if (found || pal_uptime_ms() > end)
			return found;
		struct timespec ts = {0, 1000000};
		nanosleep(&ts, NULL);
	}
}

/* run with a saved process's bindings file, <basedir>/bindings/<id>.json
 * (plan task 62): applied after the process loads and before anything
 * starts (05 §1.2); a channel the
 * environment lacks is skipped; bindings never refuse startup (04 §4): a
 * graph-side error skips its entry, a mismatched environment is a warning,
 * and a bindings file that does not load, or has no environment, is not
 * applied. */
static void test_run_bindings(void)
{
	host_setup("host-bindings");
	char sock[108];
	snprintf(sock, sizeof(sock), "/tmp/hakkas-cli-env-%d.sock", (int)getpid());
	unlink(sock);
	env_srv.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
	CHECK(bind(env_srv.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	CHECK(listen(env_srv.listen_fd, 4) == 0);
	CHECK(pthread_create(&env_srv.thread, NULL, env_server, NULL) == 0);

	/* The configuration names the program's socket (05 §1.1). */
	const char *conf = subdir("env-conf");
	char text[256];
	snprintf(text, sizeof(text), "{\"environment\": {\"socket\": \"%s\"}}", sock);
	put(join(conf, "hakkas.json"), text);
	put(join(host_base, "graphs/g1.json"), G_STARTED);
	char path[PAL_PATH_MAX]; /* join's buffers rotate */
	snprintf(path, sizeof(path), "%s", join(host_base, "bindings/g1.json"));
	put(path, "{\"environment\":\"sim\",\"bindings\":["
		  "{\"block\":\"a\",\"port\":\"in2\",\"env\":\"ain\",\"index\":1},"
		  "{\"block\":\"a\",\"port\":\"out\",\"env\":\"aout\"},"
		  "{\"block\":\"a\",\"port\":\"in1\",\"env\":\"ain\",\"index\":3}]}");
	const char *saved_conf = host_conf;
	host_conf = conf;
	CHECK(run_host() == CLI_EXIT_OK);
	CHECK(logged("info environment sim at "));
	CHECK(logged("warn "));
	CHECK(logged("/bindings/g1.json: bindings[2]: Index out of range: ain[3]: skipped\n"));
	char want[2 * PAL_PATH_MAX];
	snprintf(want, sizeof(want), "info process g1: 2 bindings applied from %s, 1 skipped\n",
		 path);
	CHECK(logged(want));
	CHECK(logged("info process g1 started\n"));
	const char *applied = strstr(logs, "bindings applied");
	const char *started = strstr(logs, "process g1 started");
	CHECK(applied != NULL && started != NULL && applied < started);
	/* g1's a.out = a.in1 + ain[1] = 2, written at the start push. */
	CHECK(env_received("{\"hello\":{\"version\":1}}\n"));
	CHECK(env_received("{\"write\":{\"port\":\"aout\",\"value\":2}}\n"));

	/* A graph-side error skips its entry; the process still starts. */
	put(path, "{\"environment\":\"sim\",\"bindings\":["
		  "{\"block\":\"zz\",\"port\":\"in2\",\"env\":\"ain\",\"index\":1},"
		  "{\"block\":\"a\",\"port\":\"in2\",\"env\":\"ain\",\"index\":1}]}");
	CHECK(run_host() == CLI_EXIT_OK);
	snprintf(want, sizeof(want), "warn %s: bindings[0]: Unknown node: zz: skipped\n", path);
	CHECK(logged(want));
	snprintf(want, sizeof(want), "info process g1: 1 binding applied from %s, 1 skipped\n",
		 path);
	CHECK(logged(want));
	CHECK(logged("info process g1 started\n"));
	/* Another environment's file: a warning, and applied. */
	put(path, "{\"environment\":\"colibri\",\"bindings\":[]}");
	CHECK(run_host() == CLI_EXIT_OK);
	CHECK(logged("g1.json: Environment mismatch: colibri (running: sim)\n"));
	CHECK(logged("bindings applied from "));
	CHECK(logged("info process g1 started\n"));
	/* A malformed file is not applied. */
	put(path, "{\"environment\":\"sim\",\"bindings\":{}}");
	CHECK(run_host() == CLI_EXIT_OK);
	CHECK(logged("g1.json: bindings: expected an array: not applied\n"));
	CHECK(strstr(logs, "bindings applied") == NULL);
	CHECK(logged("info process g1 started\n"));

	/* Without an environment, a bindings file is not applied (04 §0.1). */
	host_conf = saved_conf;
	put(path, "{\"environment\":\"sim\",\"bindings\":[]}");
	CHECK(run_host() == CLI_EXIT_OK);
	CHECK(logged("g1.json: No environment: not applied\n"));
	CHECK(logged("info process g1 started\n"));

	pthread_mutex_lock(&env_srv.lock);
	env_srv.stop = true;
	pthread_mutex_unlock(&env_srv.lock);
	pthread_join(env_srv.thread, NULL);
	close(env_srv.listen_fd);
	unlink(sock);
}

/* ---- user add|passwd|del|list (02 §2.2) ------------------------------------------- */

static uint64_t fixed_clock(void *ctx)
{
	(void)ctx;
	return 1000;
}

/* Whether user logs in with password against the users file at path: the
 * client's side of 02 §2.3 computed here, against the real authorizer. */
static bool can_login(const char *path, const char *user, const char *password)
{
	static UserStore store;
	if (users_load(&store, path) != USERS_OK)
		return false;
	AuthConfig cfg = {
		.users = &store,
		.server_secret = "a test server secret",
		.idle_timeout_s = 60,
		.max_lifetime_s = 60,
		.clock = fixed_clock,
		.random_bytes = pal_random_bytes,
	};
	Auth *auth;
	CHECK(auth_create(&cfg, &auth));
	Authorizer az = auth_authorizer(auth);
	AuthConn *c = az.ops->open(az.self, "127.0.0.1", NULL, NULL);
	AuthChallenge ch;
	bool ok = az.ops->challenge(az.self, c, user, "AAECAwQFBgcICQoLDA0ODw==", NULL, &ch) ==
		  AUTH_OK;

	uint8_t salt[16], salted[32], client_key[32], stored[32], client_sig[32], proof[32];
	size_t n = 0;
	ok = ok && crypto_base64_decode(ch.salt, strlen(ch.salt), salt, sizeof(salt), &n) &&
	     crypto_pbkdf2_hmac_sha256(password, strlen(password), salt, n, ch.iterations,
				       salted, 32) &&
	     crypto_hmac_sha256(salted, 32, "Client Key", 10, client_key) &&
	     crypto_sha256(client_key, 32, stored);
	char msg[256], proof64[64];
	snprintf(msg, sizeof(msg), "%s,%s,%s,%u", user, ch.nonce, ch.salt,
		 (unsigned)ch.iterations);
	ok = ok && crypto_hmac_sha256(stored, 32, msg, strlen(msg), client_sig);
	for (int i = 0; i < 32; i++)
		proof[i] = client_key[i] ^ client_sig[i];
	crypto_base64_encode(proof, 32, proof64, sizeof(proof64));
	AuthProof p;
	ok = ok && az.ops->verify_proof(az.self, c, user, ch.nonce, NULL, proof64, &p) == AUTH_OK;
	char secret[AUTH_SECRET_SIZE], rid[AUTH_RESUME_SIZE];
	ok = ok && az.ops->issue_secret(az.self, c, secret, rid);
	az.ops->close(az.self, c);
	auth_destroy(auth);
	return ok;
}

/* The file at path as text (empty if none). */
static const char *contents(const char *path)
{
	static char buf[USERS_FILE_MAX + 1];
	size_t len = 0;
	if (pal_storage_read(path, buf, sizeof(buf) - 1, &len) != PAL_OK)
		len = 0;
	buf[len] = '\0';
	return buf;
}

static void test_user_commands(void)
{
	const char *conf = subdir("users-conf");
	const char *users = join(conf, "users.json");

	/* No file yet: no users. */
	CHECK(run("--confdir", conf, "user", "list", NULL) == CLI_EXIT_OK);
	CHECK_STREQ(output, "");
	CHECK(!pal_storage_exists(users));

	/* add: the password from in (not a terminal: one line). */
	input = "correct horse\n";
	CHECK(run("--confdir", conf, "user", "add", "alice", "--role", "editor", NULL) ==
	      CLI_EXIT_OK);
	CHECK_STREQ(output, "");
	CHECK(pal_storage_exists(users));
	CHECK(strstr(contents(users), "\"alice\"") != NULL);
	CHECK(strstr(contents(users), "100000") != NULL);
	CHECK(strstr(contents(users), "correct") == NULL); /* only derived keys */
	CHECK(strstr(logs, "correct") == NULL);
	CHECK(can_login(users, "alice", "correct horse"));
	CHECK(!can_login(users, "alice", "correct horse\n"));

	input = "pw2\r\n";
	CHECK(run("--confdir", conf, "user", "add", "--role", "viewer", "bob", NULL) ==
	      CLI_EXIT_OK);
	CHECK(run("--confdir", conf, "user", "list", NULL) == CLI_EXIT_OK);
	CHECK_STREQ(output, "alice editor\nbob viewer\n");

	/* Refusals leave the file as it was. */
	char before[USERS_FILE_MAX + 1];
	snprintf(before, sizeof(before), "%s", contents(users));
	input = "x\n";
	CHECK(run("--confdir", conf, "user", "add", "alice", "--role", "admin", NULL) ==
	      CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "user add: user 'alice' exists") != NULL);
	input = "\n";
	CHECK(run("--confdir", conf, "user", "add", "carol", "--role", "admin", NULL) ==
	      CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "user add: empty password") != NULL);
	CHECK(run("--confdir", conf, "user", "add", "carol", "--role", "admin", NULL) ==
	      CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "user add: no password given") != NULL);
	static char long_input[USERS_PASSWORD_MAX + 8];
	memset(long_input, 'p', USERS_PASSWORD_MAX + 1);
	long_input[USERS_PASSWORD_MAX + 1] = '\n';
	input = long_input;
	CHECK(run("--confdir", conf, "user", "add", "carol", "--role", "admin", NULL) ==
	      CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "user add: password too long") != NULL);
	input = "x\n";
	CHECK(run("--confdir", conf, "user", "passwd", "nobody", NULL) == CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "user passwd: no user 'nobody'") != NULL);
	CHECK(run("--confdir", conf, "user", "del", "nobody", NULL) == CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "user del: no user 'nobody'") != NULL);
	CHECK_STREQ(contents(users), before);

	/* passwd: a new password, the same role. */
	input = "battery staple\n";
	CHECK(run("--confdir", conf, "user", "passwd", "alice", NULL) == CLI_EXIT_OK);
	CHECK(can_login(users, "alice", "battery staple"));
	CHECK(!can_login(users, "alice", "correct horse"));
	CHECK(run("--confdir", conf, "user", "list", NULL) == CLI_EXIT_OK);
	CHECK_STREQ(output, "alice editor\nbob viewer\n");

	/* del. */
	CHECK(run("--confdir", conf, "user", "del", "bob", NULL) == CLI_EXIT_OK);
	CHECK(run("--confdir", conf, "user", "list", NULL) == CLI_EXIT_OK);
	CHECK_STREQ(output, "alice editor\n");
	CHECK(run("--confdir", conf, "user", "del", "alice", NULL) == CLI_EXIT_OK);
	CHECK(run("--confdir", conf, "user", "list", NULL) == CLI_EXIT_OK);
	CHECK_STREQ(output, "");
	CHECK_STREQ(contents(users), "[]\n");

	/* A malformed file is never overwritten. */
	put(users, "[{\"user\": \"x\"}]");
	input = "pw\n";
	CHECK(run("--confdir", conf, "user", "add", "dave", "--role", "viewer", NULL) ==
	      CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "malformed users file, left unchanged") != NULL);
	CHECK(run("--confdir", conf, "user", "list", NULL) == CLI_EXIT_FAILURE);
	CHECK_STREQ(contents(users), "[{\"user\": \"x\"}]");

	/* auth.users_file from the configuration file, relative to confdir. */
	const char *conf2 = subdir("users-conf2");
	put(join(conf2, "hakkas.json"), "{\"auth\": {\"users_file\": \"people.json\"}}");
	input = "pw\n";
	CHECK(run("--confdir", conf2, "user", "add", "erin", "--role", "operator", NULL) ==
	      CLI_EXIT_OK);
	CHECK(pal_storage_exists(join(conf2, "people.json")));
	CHECK(!pal_storage_exists(join(conf2, "users.json")));
	CHECK(run("--confdir", conf2, "user", "list", NULL) == CLI_EXIT_OK);
	CHECK_STREQ(output, "erin operator\n");

	/* A missing confdir cannot be written. */
	input = "pw\n";
	CHECK(run("--confdir", join(root, "no-such-dir"), "user", "add", "x", "--role", "viewer",
		  NULL) == CLI_EXIT_FAILURE);
	CHECK(strstr(logs, "cannot write") != NULL);
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
	RUN(test_run);
	RUN(test_run_saved);
	RUN(test_run_bindings);
	RUN(test_user_commands);
	RUN(test_help_and_version);

	log_remove_sink(log_sink, NULL);
	if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
	return test_result();
}
