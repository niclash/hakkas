/*
 * The configuration (system/config.h, 05 §1.1): defaults, a full and a
 * partial file, a missing file, malformed files, unknown keys, wrong types
 * and out-of-range values, nested keys, null, relative paths, the
 * allowed_origins bound, command-line overrides, the generated engine_id and
 * auth.server_secret with its write-back; and the UUIDs it uses
 * (system/uuid.h; its base64 encoder is tested in test_crypto.c). Files live
 * in a fresh temporary directory that is removed afterwards.
 */
#define _XOPEN_SOURCE 700

#include "test.h"

#include <ftw.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/pal.h"
#include "system/config.h"
#include "system/log.h"
#include "system/uuid.h"

static char root[PAL_PATH_MAX];
static Config cfg;

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

/* JSON written with ' for ": a copy with every ' replaced. */
static const char *jq(const char *s)
{
	static char buf[4][4096];
	static int next;
	char *b = buf[next++ % 4];
	size_t n = strlen(s);
	CHECK(n < sizeof(buf[0]));
	for (size_t i = 0; i <= n; i++)
		b[i] = s[i] == '\'' ? '"' : s[i];
	return b;
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

/* dir + "/" + name, in one of a few static buffers. */
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

/* The whole file at path as a heap string, or NULL. */
static char *slurp(const char *path)
{
	struct pal_stat st;
	if (pal_storage_stat(path, &st) != PAL_OK)
		return NULL;
	char *buf = malloc((size_t)st.size + 1);
	size_t len = 0;
	if (buf == NULL || pal_storage_read(path, buf, (size_t)st.size, &len) != PAL_OK) {
		free(buf);
		return NULL;
	}
	buf[len] = '\0';
	return buf;
}

static const ConfigDefaults defaults = {
	.confdir = "/etc/hakkas", .webdir = "/srv/web", .datadir = "."};

/* cfg reset to the defaults with confdir dir, and the log cleared. */
static void reset(const char *confdir)
{
	ConfigDefaults d = defaults;
	if (confdir != NULL)
		d.confdir = confdir;
	CHECK(config_init(&cfg, &d) == CONFIG_OK);
	logs[0] = '\0';
}

/* Parse text (JSON with ' for ") over the defaults, expecting st. */
static void parse(const char *text, enum config_status st)
{
	reset(NULL);
	const char *t = jq(text);
	enum config_status got = config_parse(&cfg, t, strlen(t), "f");
	if (got != st)
		fprintf(stderr, "parse(%s): got %d, expected %d\n", text, got, st);
	CHECK(got == st);
}

/* A fake random source: bytes 0, 1, 2, ... */
static enum pal_err counting_random(void *buf, size_t len)
{
	unsigned char *b = buf;
	for (size_t i = 0; i < len; i++)
		b[i] = (unsigned char)i;
	return PAL_OK;
}

static enum pal_err zero_random(void *buf, size_t len)
{
	memset(buf, 0, len);
	return PAL_OK;
}

static enum pal_err failing_random(void *buf, size_t len)
{
	(void)buf;
	(void)len;
	return PAL_ERR_UNAVAILABLE;
}

/* ---- Helpers of the configuration ------------------------------------------------ */

static void test_uuid(void)
{
	char id[UUID_TEXT_SIZE];
	CHECK(uuid_v4(counting_random, id));
	/* Bytes 0..15 with the version and variant bits set. */
	CHECK_STREQ(id, "00010203-0405-4607-8809-0a0b0c0d0e0f");
	CHECK(!uuid_v4(failing_random, id));
	CHECK(uuid_v4(pal_random_bytes, id) && strlen(id) == 36 && id[14] == '4');
}

/* ---- Defaults and files ------------------------------------------------------------ */

static void test_defaults(void)
{
	reset(NULL);
	CHECK_STREQ(cfg.confdir, "/etc/hakkas");
	CHECK_STREQ(cfg.file, "/etc/hakkas/hakkas.json");
	CHECK_STREQ(cfg.engine_id, "");
	CHECK_STREQ(cfg.label, "Hakkas");
	CHECK_STREQ(cfg.host, "0.0.0.0");
	CHECK(cfg.port == 8080);
	CHECK_STREQ(cfg.webdir, "/srv/web");
	CHECK_STREQ(cfg.datadir, ".");
	CHECK(cfg.max_connections == 4);
	CHECK(cfg.max_message_bytes == 16384);
	CHECK(cfg.allowed_origin_count == 0);
	CHECK(cfg.change_threshold == 0.05);
	CHECK_STREQ(cfg.time_zone, "UTC0");
	CHECK(cfg.log.level == LOG_LEVEL_WARN);
	CHECK(!cfg.log.mirror);
	CHECK_STREQ(cfg.auth.users_file, "/etc/hakkas/users.json");
	CHECK(cfg.auth.idle_timeout_s == 1800);
	CHECK(cfg.auth.max_lifetime_s == 43200);
	CHECK_STREQ(cfg.auth.server_secret, "");
	CHECK(!cfg.tls.enabled);
	CHECK_STREQ(cfg.environment.socket, ""); /* no environment */
	CHECK_STREQ(cfg.tls.cert, "");
	CHECK_STREQ(cfg.tls.key, "");
	CHECK_STREQ(cfg.net.hostname, "");
	CHECK(cfg.net.dhcp);
	CHECK_STREQ(cfg.net.address, "");
	CHECK_STREQ(cfg.net.netmask, "");
	CHECK_STREQ(cfg.net.gateway, "");

	/* A trailing '/' of confdir is not doubled. */
	reset("/cfg/");
	CHECK_STREQ(cfg.file, "/cfg/hakkas.json");
	CHECK_STREQ(cfg.auth.users_file, "/cfg/users.json");
	reset("/");
	CHECK_STREQ(cfg.file, "/hakkas.json");

	/* The platform's directories must be given and fit. */
	ConfigDefaults d = defaults;
	d.confdir = "";
	CHECK(config_init(&cfg, &d) == CONFIG_ERR_INVALID);
	d = defaults;
	d.webdir = NULL;
	CHECK(config_init(&cfg, &d) == CONFIG_ERR_INVALID);
	char long_dir[PAL_PATH_MAX];
	memset(long_dir, 'a', sizeof(long_dir) - 1);
	long_dir[0] = '/';
	long_dir[sizeof(long_dir) - 1] = '\0';
	d = defaults;
	d.datadir = long_dir;
	CHECK(config_init(&cfg, &d) == CONFIG_ERR_INVALID);
	/* A confdir that fits alone but not with "/hakkas.json.tmp". */
	long_dir[PAL_PATH_MAX - 10] = '\0';
	d = defaults;
	d.confdir = long_dir;
	CHECK(config_init(&cfg, &d) == CONFIG_ERR_INVALID);
}

static void test_full_file(void)
{
	parse("{'engine_id':'dev-1','label':'Lab','host':'127.0.0.1','port':9000,"
	      "'webdir':'/w','datadir':'/d','max_connections':8,'max_message_bytes':65536,"
	      "'allowed_origins':['http://a:1','http://b'],'change_threshold':0.25,"
	      "'log':{'level':'fine','mirror':true},"
	      "'auth':{'users_file':'/u.json','idle_timeout_s':60,'max_lifetime_s':120,"
	      "'server_secret':'0123456789abcdef0123'},"
	      "'tls':{'enabled':true,'cert':'/c.pem','key':'/k.pem'},"
	      "'net':{'hostname':'box-1','dhcp':false,'address':'192.168.1.10',"
	      "'netmask':'255.255.255.0','gateway':'192.168.1.1'}}",
	      CONFIG_OK);
	CHECK_STREQ(cfg.engine_id, "dev-1");
	CHECK_STREQ(cfg.label, "Lab");
	CHECK_STREQ(cfg.host, "127.0.0.1");
	CHECK(cfg.port == 9000);
	CHECK_STREQ(cfg.webdir, "/w");
	CHECK_STREQ(cfg.datadir, "/d");
	CHECK(cfg.max_connections == 8);
	CHECK(cfg.max_message_bytes == 65536);
	CHECK(cfg.allowed_origin_count == 2);
	CHECK_STREQ(cfg.allowed_origins[0], "http://a:1");
	CHECK_STREQ(cfg.allowed_origins[1], "http://b");
	CHECK(cfg.change_threshold == 0.25);
	CHECK(cfg.log.level == LOG_LEVEL_FINE);
	CHECK(cfg.log.mirror);
	CHECK_STREQ(cfg.auth.users_file, "/u.json");
	CHECK(cfg.auth.idle_timeout_s == 60);
	CHECK(cfg.auth.max_lifetime_s == 120);
	CHECK_STREQ(cfg.auth.server_secret, "0123456789abcdef0123");
	CHECK(cfg.tls.enabled);
	CHECK_STREQ(cfg.tls.cert, "/c.pem");
	CHECK_STREQ(cfg.tls.key, "/k.pem");
	CHECK_STREQ(cfg.net.hostname, "box-1");
	CHECK(!cfg.net.dhcp);
	CHECK_STREQ(cfg.net.address, "192.168.1.10");
	CHECK_STREQ(cfg.net.netmask, "255.255.255.0");
	CHECK_STREQ(cfg.net.gateway, "192.168.1.1");
	CHECK_STREQ(logs, "");
	/* The secret is never logged. */
	CHECK(strstr(logs, "0123456789abcdef") == NULL);
}

static void test_partial_file(void)
{
	/* Only the keys present change; a section may hold some of its keys. */
	parse(" {'port': 1, 'log': {'mirror': true}, 'auth': {}} \n", CONFIG_OK);
	CHECK(cfg.port == 1);
	CHECK(cfg.log.mirror);
	CHECK(cfg.log.level == LOG_LEVEL_WARN);
	CHECK(cfg.max_connections == 4);
	CHECK_STREQ(cfg.auth.users_file, "/etc/hakkas/users.json");
	parse("{}", CONFIG_OK);
	CHECK(cfg.port == 8080);

	/* null is the same as absent, for a key and for a section. */
	parse("{'port':null,'engine_id':null,'log':null,'auth':{'server_secret':null}}",
	      CONFIG_OK);
	CHECK(cfg.port == 8080);
	CHECK_STREQ(cfg.engine_id, "");
	CHECK(cfg.log.level == LOG_LEVEL_WARN);
	CHECK_STREQ(cfg.auth.server_secret, "");

	/* Range ends and special values. */
	parse("{'port':65534,'max_connections':1,'max_message_bytes':1024,"
	      "'change_threshold':0,'label':'','net':{'address':''}}",
	      CONFIG_OK);
	CHECK(cfg.port == 65534 && cfg.max_connections == 1 && cfg.max_message_bytes == 1024);
	CHECK(cfg.change_threshold == 0);
	CHECK_STREQ(cfg.label, "");
	parse("{'change_threshold':1e999}", CONFIG_OK);
	CHECK(isinf(cfg.change_threshold) && cfg.change_threshold > 0);
	parse("{'port':8.0e3}", CONFIG_OK);
	CHECK(cfg.port == 8000);
	/* time_zone (plan task 24): any valid POSIX TZ string. */
	parse("{'time_zone':'CET-1CEST,M3.5.0,M10.5.0/3'}", CONFIG_OK);
	CHECK_STREQ(cfg.time_zone, "CET-1CEST,M3.5.0,M10.5.0/3");
	parse("{'time_zone':'<+0330>-3:30'}", CONFIG_OK);
	CHECK_STREQ(cfg.time_zone, "<+0330>-3:30");
	parse("{'time_zone':null}", CONFIG_OK); /* absent: the default */
	CHECK_STREQ(cfg.time_zone, "UTC0");
}

static void test_relative_paths(void)
{
	/* Relative paths in the file are relative to confdir; absolute ones
	 * are kept; an empty tls path means none. */
	parse("{'webdir':'web','datadir':'./data','auth':{'users_file':'u.json'},"
	      "'tls':{'cert':'tls/c.pem','key':''},'environment':{'socket':'sim.sock'}}",
	      CONFIG_OK);
	CHECK_STREQ(cfg.environment.socket, "/etc/hakkas/sim.sock");
	CHECK_STREQ(cfg.webdir, "/etc/hakkas/web");
	CHECK_STREQ(cfg.datadir, "/etc/hakkas/./data");
	CHECK_STREQ(cfg.auth.users_file, "/etc/hakkas/u.json");
	CHECK_STREQ(cfg.tls.cert, "/etc/hakkas/tls/c.pem");
	CHECK_STREQ(cfg.tls.key, "");
	/* A relative confdir gives paths relative to the working directory. */
	reset("conf");
	const char *t = "{\"datadir\":\"d\"}";
	CHECK(config_parse(&cfg, t, strlen(t), "f") == CONFIG_OK);
	CHECK_STREQ(cfg.datadir, "conf/d");

	/* Too long once joined. */
	char text[PAL_PATH_MAX + 64];
	char name[PAL_PATH_MAX];
	memset(name, 'a', PAL_PATH_MAX - 12);
	name[PAL_PATH_MAX - 12] = '\0';
	snprintf(text, sizeof(text), "{\"datadir\":\"%s\"}", name);
	reset(NULL);
	CHECK(config_parse(&cfg, text, strlen(text), "f") == CONFIG_ERR_INVALID);
	CHECK(strstr(logs, "error f: datadir: path too long") != NULL);
	/* The same length is fine as an absolute path. */
	name[0] = '/';
	snprintf(text, sizeof(text), "{\"datadir\":\"%s\"}", name);
	CHECK(config_parse(&cfg, text, strlen(text), "f") == CONFIG_OK);
}

static void test_load(void)
{
	const char *dir = subdir("load");
	reset(dir);
	/* Missing file: all defaults, noted at info. */
	enum log_level saved = log_threshold();
	log_set_threshold(LOG_LEVEL_FINE);
	CHECK(config_load(&cfg) == CONFIG_OK);
	log_set_threshold(saved);
	CHECK(cfg.port == 8080);
	CHECK(strstr(logs, "info") != NULL && strstr(logs, "not found") != NULL);

	put(join(dir, "hakkas.json"),
	    "{\n\t\"port\": 9100,\n\t\"log\": {\"level\": \"info\"}\n}\n");
	reset(dir);
	CHECK(config_load(&cfg) == CONFIG_OK);
	CHECK(cfg.port == 9100);
	CHECK(cfg.log.level == LOG_LEVEL_INFO);

	/* Larger than HAKKAS_CONFIG_FILE_MAX: malformed. */
	size_t big = HAKKAS_CONFIG_FILE_MAX + 1;
	char *text = malloc(big + 1);
	CHECK(text != NULL);
	if (text != NULL) {
		memset(text, ' ', big);
		text[0] = '{';
		text[big - 1] = '}';
		CHECK(pal_storage_write(cfg.file, text, big) == PAL_OK);
		free(text);
		reset(dir);
		CHECK(config_load(&cfg) == CONFIG_ERR_MALFORMED);
	}

	/* Malformed text through the file. */
	put(cfg.file, "{port: 1}");
	reset(dir);
	CHECK(config_load(&cfg) == CONFIG_ERR_MALFORMED);
	CHECK(strstr(logs, "not a valid configuration file") != NULL);

	/* A directory where the file should be. */
	CHECK(pal_storage_remove(cfg.file) == PAL_OK);
	CHECK(pal_storage_mkdir(cfg.file) == PAL_OK);
	reset(dir);
	CHECK(config_load(&cfg) == CONFIG_ERR_STORAGE);
}

static void test_malformed(void)
{
	static const char *const bad[] = {
		"",
		"   ",
		"not json",
		"[]",
		"'string'",
		"42",
		"null",
		"{'port':1",
		"{'port':1} x",
		"{'port':1}{}",
		"{port:1}",
		"{'port':1,}",
		/* Duplicate keys, at the top and in a section. */
		"{'port':1,'port':2}",
		"{'log':{'level':'info','level':'fine'}}",
		"{'auth':{},'auth':{}}",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		parse(bad[i], CONFIG_ERR_MALFORMED);
	parse("{'port':1,'port':2}", CONFIG_ERR_MALFORMED);
	CHECK(strstr(logs, "error f: duplicate key \"port\"") != NULL);
	parse("{'log':{'level':'info','level':'fine'}}", CONFIG_ERR_MALFORMED);
	CHECK(strstr(logs, "duplicate key \"log.level\"") != NULL);

	/* Deeper than cJSON's nesting limit. */
	char deep[200] = "{'x':";
	for (int i = 0; i < 40; i++)
		strcat(deep, "[");
	for (int i = 0; i < 40; i++)
		strcat(deep, "]");
	strcat(deep, "}");
	parse(deep, CONFIG_ERR_MALFORMED);
}

static void test_unknown_keys(void)
{
	/* Ignored with a warning; the rest still applies. */
	parse("{'future':{'a':1},'port':9001,'log':{'colour':true,'level':'info'},"
	      "'log.level':'fine','net':{'ipv6':[]}}",
	      CONFIG_OK);
	CHECK(cfg.port == 9001);
	CHECK(cfg.log.level == LOG_LEVEL_INFO);
	CHECK_STREQ(logs, "warn f: unknown key \"future\" ignored\n"
			  "warn f: unknown key \"log.colour\" ignored\n"
			  "warn f: unknown key \"log.level\" ignored\n"
			  "warn f: unknown key \"net.ipv6\" ignored\n");
	/* A section inside a section is not a section. */
	parse("{'log':{'auth':{}}}", CONFIG_OK);
	CHECK_STREQ(logs, "warn f: unknown key \"log.auth\" ignored\n");
}

/* text has exactly one invalid key, whose error starts with want. */
static void invalid(const char *text, const char *want)
{
	parse(text, CONFIG_ERR_INVALID);
	char expect[160];
	snprintf(expect, sizeof(expect), "error f: %s", want);
	if (strncmp(logs, expect, strlen(expect)) != 0 || strchr(logs, '\n') != strrchr(logs, '\n'))
		fprintf(stderr, "invalid(%s): log \"%s\", expected \"%s...\"\n", text, logs,
			expect);
	CHECK(strncmp(logs, expect, strlen(expect)) == 0);
}

static void test_invalid_values(void)
{
	invalid("{'port':0}", "port: expected an integer from 1 to 65534");
	invalid("{'port':65535}", "port: expected an integer from 1 to 65534");
	invalid("{'port':-1}", "port: expected an integer");
	invalid("{'port':80.5}", "port: expected an integer");
	invalid("{'port':'8080'}", "port: expected an integer");
	invalid("{'port':true}", "port: expected an integer");
	invalid("{'max_connections':0}", "max_connections: expected an integer from 1 to 1024");
	invalid("{'max_connections':1025}", "max_connections: expected an integer");
	invalid("{'max_message_bytes':1023}", "max_message_bytes: expected an integer");
	invalid("{'max_message_bytes':16777217}", "max_message_bytes: expected an integer");
	invalid("{'change_threshold':-0.1}", "change_threshold: expected a number >= 0");
	invalid("{'change_threshold':-1e999}", "change_threshold: expected a number >= 0");
	invalid("{'change_threshold':'0.1'}", "change_threshold: expected a number >= 0");
	invalid("{'time_zone':'Europe/Stockholm'}",
		"time_zone: expected a POSIX TZ string of at most 64 bytes, e.g. "
		"\"CET-1CEST,M3.5.0,M10.5.0/3\"");
	invalid("{'time_zone':''}", "time_zone: expected a POSIX TZ string");
	invalid("{'time_zone':'CET-1CEST'}", "time_zone: expected a POSIX TZ string");
	invalid("{'time_zone':1}", "time_zone: expected a POSIX TZ string");
	invalid("{'time_zone':'"
		"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA0'}",
		"time_zone: expected a POSIX TZ string");
	invalid("{'engine_id':''}", "engine_id: expected a string of 1 to 64 bytes");
	invalid("{'engine_id':42}", "engine_id: expected a string of 1 to 64 bytes");
	invalid("{'engine_id':'"
		"12345678901234567890123456789012345678901234567890123456789012345'}",
		"engine_id: expected a string of 1 to 64 bytes");
	invalid("{'host':''}", "host: expected a string of 1 to 63 bytes");
	invalid("{'webdir':''}", "webdir: expected a path of 1 to");
	invalid("{'datadir':[]}", "datadir: expected a path");
	invalid("{'allowed_origins':'http://a'}",
		"allowed_origins: expected an array of at most 8");
	invalid("{'allowed_origins':['']}", "allowed_origins: expected an array");
	invalid("{'allowed_origins':[1]}", "allowed_origins: expected an array");
	invalid("{'log':'info'}", "log: expected an object");
	invalid("{'log':{'level':'debug'}}",
		"log.level: expected \"fine\", \"info\", \"warn\" or \"error\"");
	invalid("{'log':{'level':'INFO'}}", "log.level: expected");
	invalid("{'log':{'mirror':1}}", "log.mirror: expected true or false");
	invalid("{'auth':[]}", "auth: expected an object");
	invalid("{'auth':{'idle_timeout_s':0}}", "auth.idle_timeout_s: expected an integer from 1");
	invalid("{'auth':{'max_lifetime_s':2147483648}}", "auth.max_lifetime_s: expected");
	invalid("{'auth':{'server_secret':'short'}}",
		"auth.server_secret: expected a string of 16 to 128 bytes");
	invalid("{'auth':{'users_file':''}}", "auth.users_file: expected a path");
	invalid("{'tls':{'enabled':'yes'}}", "tls.enabled: expected true or false");
	invalid("{'tls':{'cert':5}}", "tls.cert: expected a path of 0 to");
	invalid("{'environment':{'socket':5}}", "environment.socket: expected a path of 0 to");
	invalid("{'environment':5}", "environment: expected an object");
	invalid("{'net':{'hostname':''}}", "net.hostname: expected a host name");
	invalid("{'net':{'hostname':'-box'}}", "net.hostname: expected a host name");
	invalid("{'net':{'hostname':'a.b'}}", "net.hostname: expected a host name");
	invalid("{'net':{'dhcp':null,'address':'1.2.3'}}", "net.address: expected an IPv4");
	invalid("{'net':{'netmask':'256.0.0.0'}}", "net.netmask: expected an IPv4");
	invalid("{'net':{'gateway':'1.2.3.4.5'}}", "net.gateway: expected an IPv4");
	invalid("{'net':{'gateway':'1..3.4'}}", "net.gateway: expected an IPv4");
	invalid("{'net':{'gateway':'0001.2.3.4'}}", "net.gateway: expected an IPv4");

	/* Every invalid key is reported, not only the first; valid keys
	 * still apply. */
	parse("{'port':0,'label':'ok','max_connections':0,'log':{'level':1}}",
	      CONFIG_ERR_INVALID);
	CHECK_STREQ(logs, "error f: port: expected an integer from 1 to 65534\n"
			  "error f: max_connections: expected an integer from 1 to 1024\n"
			  "error f: log.level: expected \"fine\", \"info\", \"warn\" or "
			  "\"error\"\n");
	CHECK_STREQ(cfg.label, "ok");

	/* The secret's value never appears in an error. */
	parse("{'auth':{'server_secret':'tiny-secret'}}", CONFIG_ERR_INVALID);
	CHECK(strstr(logs, "tiny-secret") == NULL);
}

static void test_allowed_origins_bound(void)
{
	parse("{'allowed_origins':['a','b','c','d','e','f','g','h']}", CONFIG_OK);
	CHECK(cfg.allowed_origin_count == HAKKAS_CONFIG_ORIGINS_MAX);
	CHECK_STREQ(cfg.allowed_origins[7], "h");
	parse("{'allowed_origins':['a','b','c','d','e','f','g','h','i']}", CONFIG_ERR_INVALID);
	parse("{'allowed_origins':[]}", CONFIG_OK);
	CHECK(cfg.allowed_origin_count == 0);
	/* Longest entry. */
	char text[CONFIG_ORIGIN_MAX + 64];
	char origin[CONFIG_ORIGIN_MAX + 2];
	memset(origin, 'o', CONFIG_ORIGIN_MAX);
	origin[CONFIG_ORIGIN_MAX] = '\0';
	snprintf(text, sizeof(text), "{\"allowed_origins\":[\"%s\"]}", origin);
	reset(NULL);
	CHECK(config_parse(&cfg, text, strlen(text), "f") == CONFIG_OK);
	CHECK_STREQ(cfg.allowed_origins[0], origin);
	strcat(origin, "o");
	snprintf(text, sizeof(text), "{\"allowed_origins\":[\"%s\"]}", origin);
	CHECK(config_parse(&cfg, text, strlen(text), "f") == CONFIG_ERR_INVALID);
	/* A later valid list replaces an earlier one. */
	parse("{'allowed_origins':['a','b']}", CONFIG_OK);
	const char *t = "{\"allowed_origins\":[\"c\"]}";
	CHECK(config_parse(&cfg, t, strlen(t), "g") == CONFIG_OK);
	CHECK(cfg.allowed_origin_count == 1);
	CHECK_STREQ(cfg.allowed_origins[0], "c");
}

static void test_set(void)
{
	reset(NULL);
	CHECK(config_set(&cfg, "port", "9002", "--port") == CONFIG_OK);
	CHECK(cfg.port == 9002);
	CHECK(config_set(&cfg, "engine_id", "my-engine", "--id") == CONFIG_OK);
	CHECK_STREQ(cfg.engine_id, "my-engine");
	CHECK(config_set(&cfg, "host", "::1", "--host") == CONFIG_OK);
	CHECK_STREQ(cfg.host, "::1");
	/* Option paths are used as given, not relative to confdir. */
	CHECK(config_set(&cfg, "webdir", "web", "--webdir") == CONFIG_OK);
	CHECK_STREQ(cfg.webdir, "web");
	CHECK(config_set(&cfg, "auth.users_file", "u.json", "--users") == CONFIG_OK);
	CHECK_STREQ(cfg.auth.users_file, "u.json");
	CHECK(config_set(&cfg, "log.level", "fine", "--fine") == CONFIG_OK);
	CHECK(cfg.log.level == LOG_LEVEL_FINE);
	CHECK(config_set(&cfg, "log.mirror", "true", "x") == CONFIG_OK && cfg.log.mirror);
	CHECK(config_set(&cfg, "time_zone", "EST5EDT,M3.2.0,M11.1.0", "x") == CONFIG_OK);
	CHECK_STREQ(cfg.time_zone, "EST5EDT,M3.2.0,M11.1.0");
	CHECK(config_set(&cfg, "change_threshold", "0.5", "x") == CONFIG_OK);
	CHECK(cfg.change_threshold == 0.5);
	CHECK_STREQ(logs, "");

	static const char *const bad_ports[] = {"0", "65535", "-1", "+1", "1e3", "80x",
						"",  " 80",  "99999999999"};
	for (size_t i = 0; i < sizeof(bad_ports) / sizeof(bad_ports[0]); i++) {
		logs[0] = '\0';
		CHECK(config_set(&cfg, "port", bad_ports[i], "--port") == CONFIG_ERR_INVALID);
		CHECK_STREQ(logs, "error --port: port: expected an integer from 1 to 65534\n");
	}
	CHECK(cfg.port == 9002);
	CHECK(config_set(&cfg, "engine_id", "", "--id") == CONFIG_ERR_INVALID);
	CHECK(config_set(&cfg, "log.mirror", "yes", "x") == CONFIG_ERR_INVALID);
	CHECK(config_set(&cfg, "change_threshold", "-1", "x") == CONFIG_ERR_INVALID);
	CHECK(config_set(&cfg, "change_threshold", "nan", "x") == CONFIG_ERR_INVALID);
	CHECK(config_set(&cfg, "time_zone", "EST", "x") == CONFIG_ERR_INVALID);
	CHECK_STREQ(cfg.time_zone, "EST5EDT,M3.2.0,M11.1.0");
	CHECK(config_set(&cfg, "allowed_origins", "a", "x") == CONFIG_ERR_INVALID);
	CHECK(config_set(&cfg, "nope", "1", "x") == CONFIG_ERR_INVALID);
	CHECK(config_set(&cfg, "log", "1", "x") == CONFIG_ERR_INVALID);
}

static void test_engine_id(void)
{
	reset(NULL);
	CHECK(config_generate_engine_id(&cfg, counting_random) == CONFIG_OK);
	CHECK_STREQ(cfg.engine_id, "00010203-0405-4607-8809-0a0b0c0d0e0f");
	/* Already set (configured or generated): kept. */
	CHECK(config_generate_engine_id(&cfg, zero_random) == CONFIG_OK);
	CHECK_STREQ(cfg.engine_id, "00010203-0405-4607-8809-0a0b0c0d0e0f");
	parse("{'engine_id':'fixed'}", CONFIG_OK);
	CHECK(config_generate_engine_id(&cfg, counting_random) == CONFIG_OK);
	CHECK_STREQ(cfg.engine_id, "fixed");
	/* A fresh one each boot. */
	char first[UUID_TEXT_SIZE];
	reset(NULL);
	CHECK(config_generate_engine_id(&cfg, pal_random_bytes) == CONFIG_OK);
	strcpy(first, cfg.engine_id);
	reset(NULL);
	CHECK(config_generate_engine_id(&cfg, pal_random_bytes) == CONFIG_OK);
	CHECK(strlen(cfg.engine_id) == 36 && strcmp(first, cfg.engine_id) != 0);
	reset(NULL);
	CHECK(config_generate_engine_id(&cfg, failing_random) == CONFIG_ERR_UNAVAILABLE);
	CHECK_STREQ(cfg.engine_id, "");
}

/* ---- auth.server_secret ------------------------------------------------------------ */

/* The secret zero_random generates: 32 zero bytes, base64. */
#define ZERO_SECRET "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="

/* In a fresh confdir dir with hakkas.json holding before (NULL: no file),
 * load, generate the secret, and expect the file to become after. */
static void secret_case(const char *dir, const char *before, const char *after)
{
	reset(dir);
	if (before != NULL)
		put(cfg.file, before);
	else
		pal_storage_remove(cfg.file);
	CHECK(config_load(&cfg) == CONFIG_OK);
	logs[0] = '\0';
	CHECK(config_ensure_server_secret(&cfg, zero_random) == CONFIG_OK);
	CHECK_STREQ(cfg.auth.server_secret, ZERO_SECRET);
	char *text = slurp(cfg.file);
	CHECK_STREQ(text, after);
	free(text);
	/* Never logged. */
	CHECK(strstr(logs, "AAAA") == NULL);
	/* The written file loads back with the same secret and nothing else
	 * changed. */
	Config again;
	ConfigDefaults d = defaults;
	d.confdir = dir;
	CHECK(config_init(&again, &d) == CONFIG_OK);
	CHECK(config_load(&again) == CONFIG_OK);
	CHECK(memcmp(&again, &cfg, sizeof(cfg)) == 0);
}

static void test_server_secret(void)
{
	const char *dir = subdir("secret");

	/* No file: created with only the secret, mode 0600. */
	secret_case(dir, NULL, "{\"auth\": {\"server_secret\": \"" ZERO_SECRET "\"}}\n");
	struct stat st;
	CHECK(stat(join(dir, "hakkas.json"), &st) == 0 && (st.st_mode & 0777) == 0600);

	/* No auth: added as the first member, in the file's own layout; every
	 * other byte is kept (numbers, key order, unknown keys, spacing). */
	secret_case(dir,
		    "{\n  \"port\": 9000,\n  \"change_threshold\": 0.1000000000000000055,\n"
		    "  \"future\": {\"x\": [1, 2]}\n}\n",
		    "{\n  \"auth\": {\"server_secret\": \"" ZERO_SECRET "\"},\n  \"port\": 9000,\n"
		    "  \"change_threshold\": 0.1000000000000000055,\n"
		    "  \"future\": {\"x\": [1, 2]}\n}\n");
	CHECK(cfg.port == 9000);
	secret_case(dir, "{\"port\":9000}", "{\"auth\": {\"server_secret\": \"" ZERO_SECRET
					    "\"},\"port\":9000}");
	secret_case(dir, "{ }", "{\"auth\": {\"server_secret\": \"" ZERO_SECRET "\"} }");

	/* An auth object: the secret becomes its first member. Keys and
	 * strings that look like "auth" elsewhere are not it. */
	secret_case(dir,
		    "{\n\t\"label\": \"auth\",\n\t\"x\": {\"auth\": {}},\n\t\"auth\": {\n"
		    "\t\t\"idle_timeout_s\": 60\n\t}\n}\n",
		    "{\n\t\"label\": \"auth\",\n\t\"x\": {\"auth\": {}},\n\t\"auth\": {\n"
		    "\t\t\"server_secret\": \"" ZERO_SECRET "\",\n"
		    "\t\t\"idle_timeout_s\": 60\n\t}\n}\n");
	CHECK(cfg.auth.idle_timeout_s == 60);
	secret_case(dir, "{\"auth\":{}}", "{\"auth\":{\"server_secret\": \"" ZERO_SECRET "\"}}");
	/* An escaped spelling of a key, and strings with quotes and braces. */
	secret_case(dir, "{\"l\\\"}\":\"}{\\\\\",\"\\u0061uth\":{}}",
		    "{\"l\\\"}\":\"}{\\\\\",\"\\u0061uth\":{\"server_secret\": \"" ZERO_SECRET
		    "\"}}");

	/* null auth or null secret: replaced, never duplicated. */
	secret_case(dir, "{\"auth\": null, \"port\": 1}",
		    "{\"auth\": {\"server_secret\": \"" ZERO_SECRET "\"}, \"port\": 1}");
	secret_case(dir, "{\"auth\": {\"server_secret\" : null, \"idle_timeout_s\": 5}}",
		    "{\"auth\": {\"server_secret\" : \"" ZERO_SECRET
		    "\", \"idle_timeout_s\": 5}}");

	/* Present: not regenerated, the file is not written. */
	const char *with = "{\"auth\": {\"server_secret\": \"configured-secret-123\"}}";
	reset(dir);
	put(cfg.file, with);
	CHECK(config_load(&cfg) == CONFIG_OK);
	CHECK(config_ensure_server_secret(&cfg, counting_random) == CONFIG_OK);
	CHECK_STREQ(cfg.auth.server_secret, "configured-secret-123");
	char *text = slurp(cfg.file);
	CHECK_STREQ(text, with);
	free(text);

	/* A real random secret: 44 base64 characters, different each time. */
	reset(dir);
	pal_storage_remove(cfg.file);
	CHECK(config_ensure_server_secret(&cfg, pal_random_bytes) == CONFIG_OK);
	CHECK(strlen(cfg.auth.server_secret) == 44 && cfg.auth.server_secret[43] == '=');
	char first[CONFIG_SECRET_MAX + 1];
	strcpy(first, cfg.auth.server_secret);
	reset(dir);
	pal_storage_remove(cfg.file);
	CHECK(config_ensure_server_secret(&cfg, pal_random_bytes) == CONFIG_OK);
	CHECK(strcmp(first, cfg.auth.server_secret) != 0);

	/* The random source fails: nothing written. */
	reset(dir);
	put(cfg.file, "{}");
	CHECK(config_ensure_server_secret(&cfg, failing_random) == CONFIG_ERR_UNAVAILABLE);
	CHECK_STREQ(cfg.auth.server_secret, "");
	text = slurp(cfg.file);
	CHECK_STREQ(text, "{}");
	free(text);

	/* The file changed since it was loaded. */
	reset(dir);
	put(cfg.file, "[1]");
	CHECK(config_ensure_server_secret(&cfg, zero_random) == CONFIG_ERR_MALFORMED);
	put(cfg.file, "{\"auth\": 5}");
	CHECK(config_ensure_server_secret(&cfg, zero_random) == CONFIG_ERR_MALFORMED);
	put(cfg.file, "{\"auth\": {\"server_secret\": \"configured-secret-123\"}}");
	CHECK(config_ensure_server_secret(&cfg, zero_random) == CONFIG_ERR_MALFORMED);
	CHECK_STREQ(cfg.auth.server_secret, "");

	/* confdir does not exist: cannot be written. */
	reset(join(dir, "missing"));
	CHECK(config_load(&cfg) == CONFIG_OK);
	CHECK(config_ensure_server_secret(&cfg, zero_random) == CONFIG_ERR_STORAGE);
	CHECK_STREQ(cfg.auth.server_secret, "");
	CHECK(strstr(logs, "cannot write") != NULL && strstr(logs, "AAAA") == NULL);
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
	snprintf(root, sizeof(root), "%s/hakkas-config-XXXXXX", base && *base ? base : "/tmp");
	if (mkdtemp(root) == NULL) {
		perror("mkdtemp");
		return 1;
	}

	RUN(test_uuid);
	RUN(test_defaults);
	RUN(test_full_file);
	RUN(test_partial_file);
	RUN(test_relative_paths);
	RUN(test_load);
	RUN(test_malformed);
	RUN(test_unknown_keys);
	RUN(test_invalid_values);
	RUN(test_allowed_origins_bound);
	RUN(test_set);
	RUN(test_engine_id);
	RUN(test_server_secret);

	log_remove_sink(log_sink, NULL);
	if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
		perror("removing the test directory");
	return test_result();
}
