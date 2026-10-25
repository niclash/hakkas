/*
 * The POSIX command line (05 §1.2). See platform/posix/cli.h.
 */
#include "platform/posix/cli.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/graph_files.h"
#include "blocktypes/block_types.h"
#include "engine/version.h"
#include "graphs/graph_json.h"
#include "graphs/graphs.h"
#include "platform/pal.h"
#include "platform/posix/host.h"
#include "platform/posix/password.h"
#include "protocol/users.h"
#include "system/crypto.h"

#define HINT " (see hakkas --help)"

void cli_usage(FILE *out)
{
	fputs("usage: hakkas [global options] <command>\n"
	      "\n"
	      "global options:\n"
	      "  --fine | --info | --warn   log verbosity (overrides log.level)\n"
	      "  --basedir <dir>            base directory, the default datadir (default .)\n"
	      "  --confdir <dir>            configuration directory (default /etc/hakkas)\n"
	      "  --help, -h                 print this help\n"
	      "  --version                  print the version\n"
	      "\n"
	      "commands:\n"
	      "  list types                     print every registered block type name\n"
	      "  describe type <typename>       print a block type's JSON descriptor\n"
	      "  describe topology <filename>   load a graph file and print its JSON\n"
	      "  user add <name> --role <role>  add a user (role: admin, editor, operator,\n"
	      "                                 viewer); the password is asked for\n"
	      "  user passwd <name>             set a user's password\n"
	      "  user del <name>                remove a user\n"
	      "  user list                      print every user's name and role\n"
	      "  run [run options]              run the engine with every process saved in\n"
	      "                                 <datadir> (graphs/, bindings/, links.json)\n"
	      "\n"
	      "run options (override the configuration file):\n"
	      "  --id <id>        engine_id\n"
	      "  --webdir <dir>   webdir\n"
	      "  --host <addr>    host\n"
	      "  --port <n>       port (the WebSocket uses port + 1)\n"
	      "  --users <file>   auth.users_file\n",
	      out);
}

/* ---- Parsing ------------------------------------------------------------------ */

typedef struct {
	int argc;
	char *const *argv;
	int i; /* the next argument */
} Args;

static const char *next(Args *a)
{
	return a->i < a->argc ? a->argv[a->i++] : NULL;
}

/* The value of option opt, the next argument: NULL (logged) if there is none
 * or it is empty. */
static const char *option_value(Args *a, const char *opt)
{
	const char *v = next(a);
	if (v == NULL || v[0] == '\0') {
		log_error("option %s needs a value" HINT, opt);
		return NULL;
	}
	return v;
}

/* The command's arguments are complete: anything more is a usage error. */
static int no_more(Args *a, const char *command)
{
	if (a->i < a->argc) {
		log_error("%s: unexpected argument '%s'" HINT, command, a->argv[a->i]);
		return CLI_EXIT_USAGE;
	}
	return CLI_EXIT_OK;
}

/* The one argument of command (e.g. a type name), then nothing more. */
static int one_argument(Args *a, const char *command, const char *what, const char **out)
{
	*out = next(a);
	if (*out == NULL || (*out)[0] == '\0') {
		log_error("%s: missing %s" HINT, command, what);
		return CLI_EXIT_USAGE;
	}
	return no_more(a, command);
}

static int parse_run(Args *a, CliOptions *o)
{
	o->command = CLI_RUN;
	const char *arg;
	while ((arg = next(a)) != NULL) {
		const char **dst = strcmp(arg, "--id") == 0	 ? &o->id
				   : strcmp(arg, "--webdir") == 0 ? &o->webdir
				   : strcmp(arg, "--host") == 0	 ? &o->host
				   : strcmp(arg, "--port") == 0	 ? &o->port
				   : strcmp(arg, "--users") == 0  ? &o->users
								  : NULL;
		if (dst == NULL && arg[0] != '-') {
			/* Only the run options: what it runs is <datadir> (plan
			 * task 62; `run process`, `processes` and `daemon` are
			 * gone). */
			log_error("run: unexpected argument '%s'" HINT, arg);
			return CLI_EXIT_USAGE;
		}
		if (dst == NULL) {
			log_error("run: unknown option '%s'" HINT, arg);
			return CLI_EXIT_USAGE;
		}
		if ((*dst = option_value(a, arg)) == NULL)
			return CLI_EXIT_USAGE;
	}
	return CLI_EXIT_OK;
}

/* user add <name> --role <role> (the option before or after the name),
 * user passwd|del <name>, user list (02 §2.2). */
static int parse_user(Args *a, CliOptions *o)
{
	const char *sub = next(a);
	if (sub == NULL || (strcmp(sub, "add") != 0 && strcmp(sub, "passwd") != 0 &&
			    strcmp(sub, "del") != 0 && strcmp(sub, "list") != 0)) {
		log_error("user: expected 'add', 'passwd', 'del' or 'list'" HINT);
		return CLI_EXIT_USAGE;
	}
	o->command = CLI_USER;
	o->arg = sub;
	if (strcmp(sub, "list") == 0)
		return no_more(a, "user list");

	char command[16];
	snprintf(command, sizeof(command), "user %s", sub);
	bool add = strcmp(sub, "add") == 0;
	const char *arg;
	while ((arg = next(a)) != NULL) {
		if (add && strcmp(arg, "--role") == 0) {
			if ((o->role = option_value(a, arg)) == NULL)
				return CLI_EXIT_USAGE;
		} else if (arg[0] == '-') {
			log_error("%s: unknown option '%s'" HINT, command, arg);
			return CLI_EXIT_USAGE;
		} else if (o->user == NULL) {
			o->user = arg;
		} else {
			log_error("%s: unexpected argument '%s'" HINT, command, arg);
			return CLI_EXIT_USAGE;
		}
	}
	if (o->user == NULL || o->user[0] == '\0') {
		log_error("%s: missing the user name" HINT, command);
		return CLI_EXIT_USAGE;
	}
	if (!add)
		return CLI_EXIT_OK;
	if (!users_name_valid(o->user)) {
		log_error("user add: invalid user name: 1-32 bytes of UTF-8 without control "
			  "characters" HINT);
		return CLI_EXIT_USAGE;
	}
	enum user_role role;
	if (o->role == NULL) {
		log_error("user add: missing --role <role>" HINT);
		return CLI_EXIT_USAGE;
	}
	if (!users_role_parse(o->role, &role)) {
		log_error("user add: invalid role '%s': expected admin, editor, operator or "
			  "viewer" HINT,
			  o->role);
		return CLI_EXIT_USAGE;
	}
	return CLI_EXIT_OK;
}

int cli_parse(int argc, char *const argv[], CliOptions *o)
{
	memset(o, 0, sizeof(*o));
	o->basedir = CLI_DEFAULT_BASEDIR;
	o->confdir = CLI_DEFAULT_CONFDIR;
	Args a = {argc, argv, 1};

	const char *arg;
	while ((arg = next(&a)) != NULL && arg[0] == '-') {
		if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
			o->command = CLI_HELP;
			return CLI_EXIT_OK;
		} else if (strcmp(arg, "--version") == 0) {
			o->command = CLI_VERSION;
			return CLI_EXIT_OK;
		} else if (strcmp(arg, "--fine") == 0 || strcmp(arg, "--info") == 0 ||
			   strcmp(arg, "--warn") == 0) {
			o->level_set = true;
			log_level_parse(arg + 2, &o->level);
		} else if (strcmp(arg, "--basedir") == 0) {
			if ((o->basedir = option_value(&a, arg)) == NULL)
				return CLI_EXIT_USAGE;
		} else if (strcmp(arg, "--confdir") == 0) {
			if ((o->confdir = option_value(&a, arg)) == NULL)
				return CLI_EXIT_USAGE;
		} else {
			log_error("unknown option '%s'" HINT, arg);
			return CLI_EXIT_USAGE;
		}
	}
	if (arg == NULL) {
		log_error("no command given" HINT);
		return CLI_EXIT_USAGE;
	}

	if (strcmp(arg, "list") == 0) {
		const char *what = next(&a);
		if (what == NULL || strcmp(what, "types") != 0) {
			log_error("list: expected 'types'" HINT);
			return CLI_EXIT_USAGE;
		}
		o->command = CLI_LIST_TYPES;
		return no_more(&a, "list types");
	}
	if (strcmp(arg, "describe") == 0) {
		const char *what = next(&a);
		if (what != NULL && strcmp(what, "type") == 0) {
			o->command = CLI_DESCRIBE_TYPE;
			return one_argument(&a, "describe type", "the type name", &o->arg);
		}
		if (what != NULL && strcmp(what, "topology") == 0) {
			o->command = CLI_DESCRIBE_TOPOLOGY;
			return one_argument(&a, "describe topology", "the file name", &o->arg);
		}
		log_error("describe: expected 'type <typename>' or 'topology <filename>'" HINT);
		return CLI_EXIT_USAGE;
	}
	if (strcmp(arg, "user") == 0)
		return parse_user(&a, o);
	if (strcmp(arg, "run") == 0)
		return parse_run(&a, o);
	log_error("unknown command '%s'" HINT, arg);
	return CLI_EXIT_USAGE;
}

/* ---- Configuration --------------------------------------------------------------- */

int cli_config(const CliOptions *o, Config *cfg)
{
	/* The defaults (05 §1.1): webdir the build's, datadir <basedir>. */
	ConfigDefaults d = {
		.confdir = o->confdir, .webdir = HAKKAS_DEFAULT_WEBDIR, .datadir = o->basedir};
	if (config_init(cfg, &d) != CONFIG_OK)
		return CLI_EXIT_USAGE;

	if (o->level_set)
		log_set_threshold(o->level);
	if (config_load(cfg) != CONFIG_OK)
		return CLI_EXIT_FAILURE;

	/* The options override the file (05 §1.2). */
	struct {
		const char *value;
		const char *key;
		const char *option;
	} overrides[] = {
		{o->id, "engine_id", "--id"},	   {o->webdir, "webdir", "--webdir"},
		{o->host, "host", "--host"},	   {o->port, "port", "--port"},
		{o->users, "auth.users_file", "--users"},
	};
	for (size_t i = 0; i < sizeof(overrides) / sizeof(overrides[0]); i++) {
		if (overrides[i].value != NULL &&
		    config_set(cfg, overrides[i].key, overrides[i].value, overrides[i].option) !=
			    CONFIG_OK)
			return CLI_EXIT_USAGE;
	}
	if (o->level_set)
		cfg->log.level = o->level;
	log_set_threshold(cfg->log.level);
	return CLI_EXIT_OK;
}

/* ---- Commands ------------------------------------------------------------------- */

/* Print json compactly (graph_json_print) on its own line and free it. */
static int print_json(cJSON *json, FILE *out)
{
	char *text = graph_json_print(json);
	cJSON_Delete(json);
	if (text == NULL) {
		log_error("out of memory");
		return CLI_EXIT_FAILURE;
	}
	fprintf(out, "%s\n", text);
	free(text);
	return CLI_EXIT_OK;
}

static int describe_type(const char *name, FILE *out)
{
	const BlockTypeDef *def = block_types_get(name);
	if (def == NULL) {
		log_error("unknown block type: %s", name);
		return CLI_EXIT_FAILURE;
	}
	return print_json(graph_json_component(def), out);
}

/* The engine's state for describe topology: static, the runtime's work queue
 * is large. */
static Runtime topo_rt;
static Graphs topo_gs;

static int describe_topology(const Config *cfg, const char *path, FILE *out)
{
	runtime_init(&topo_rt);
	runtime_set_change_threshold(&topo_rt, cfg->change_threshold);
	TimeZone tz;
	if (tz_parse(cfg->time_zone, &tz)) /* checked when the configuration was read */
		runtime_set_time_zone(&topo_rt, &tz);
	graphs_init(&topo_gs, &topo_rt, block_types_get, pal_random_bytes);
	GraphFiles gf;
	int rc = CLI_EXIT_FAILURE;
	Graph *g = NULL;
	bool started = false;
	if (!graph_files_init(&gf, &topo_gs, cfg->datadir))
		log_error("datadir too long: %s", cfg->datadir);
	else if (graph_files_load(&gf, path, &g, &started) == GRAPH_FILES_OK) {
		/* As the file says, so that its canonical form is printed. */
		if (started)
			graphs_start(&topo_gs, g->id, NULL);
		size_t len = 0;
		char *text = graph_files_serialize(g, &len);
		if (text != NULL) {
			fprintf(out, "%s\n", text);
			free(text);
			rc = CLI_EXIT_OK;
		} else {
			log_error("out of memory");
		}
	}
	graphs_deinit(&topo_gs);
	runtime_deinit(&topo_rt);
	return rc;
}

/* The users file, edited by the user commands: static, it is a few KiB. */
static UserStore user_store;

/* Read the password for user add/passwd from in into buf (zeroized by the
 * caller). */
static bool read_password(const char *command, FILE *in, char *buf, size_t cap, size_t *len)
{
	enum posix_password_status st = posix_read_password(in, stderr, true, buf, cap, len);
	if (st != POSIX_PASSWORD_OK) {
		log_error("%s: %s", command, posix_password_status_str(st));
		return false;
	}
	return true;
}

/* user add|passwd|del|list (02 §2.2): load the users file, change it, write
 * it back atomically. */
static int user_command(const CliOptions *o, const Config *cfg, FILE *in, FILE *out)
{
	const char *path = cfg->auth.users_file;
	char command[16];
	snprintf(command, sizeof(command), "user %s", o->arg);
	enum users_status st = users_load(&user_store, path);
	if (st == USERS_ERR_NOT_FOUND)
		st = USERS_OK; /* no users yet */
	if (st != USERS_OK) {
		log_error("%s: %s: %s, left unchanged", command, path, users_status_str(st));
		return CLI_EXIT_FAILURE;
	}

	int rc = CLI_EXIT_FAILURE;
	char password[USERS_PASSWORD_MAX + 1];
	size_t len = 0;
	if (strcmp(o->arg, "list") == 0) {
		for (size_t i = 0; i < user_store.count; i++)
			fprintf(out, "%s %s\n", user_store.users[i].name,
				users_role_name(user_store.users[i].role));
		rc = CLI_EXIT_OK;
	} else if (strcmp(o->arg, "del") == 0) {
		if (users_remove(&user_store, o->user) != USERS_OK)
			log_error("%s: no user '%s'", command, o->user);
		else if (users_save(&user_store, path) == USERS_OK)
			rc = CLI_EXIT_OK;
	} else if (strcmp(o->arg, "add") == 0) {
		enum user_role role = USER_ROLE_VIEWER;
		users_role_parse(o->role, &role); /* checked by cli_parse */
		if (users_find(&user_store, o->user) != NULL)
			log_error("%s: user '%s' exists", command, o->user);
		else if (user_store.count >= HAKKAS_USERS_MAX)
			log_error("%s: %s already holds %d users", command, path, HAKKAS_USERS_MAX);
		else if (read_password(command, in, password, sizeof(password), &len)) {
			st = users_add(&user_store, o->user, role, password, len,
				       USERS_DEFAULT_ITERATIONS, pal_random_bytes);
			if (st != USERS_OK)
				log_error("%s: %s", command, users_status_str(st));
			else if (users_save(&user_store, path) == USERS_OK)
				rc = CLI_EXIT_OK;
		}
	} else { /* passwd */
		if (users_find(&user_store, o->user) == NULL)
			log_error("%s: no user '%s'", command, o->user);
		else if (read_password(command, in, password, sizeof(password), &len)) {
			st = users_passwd(&user_store, o->user, password, len,
					  USERS_DEFAULT_ITERATIONS, pal_random_bytes);
			if (st != USERS_OK)
				log_error("%s: %s", command, users_status_str(st));
			else if (users_save(&user_store, path) == USERS_OK)
				rc = CLI_EXIT_OK;
		}
	}
	crypto_zeroize(password, sizeof(password));
	users_init(&user_store);
	return rc;
}

int cli_execute(const CliOptions *o, const Config *cfg, FILE *in, FILE *out)
{
	switch (o->command) {
	case CLI_HELP:
		cli_usage(out);
		return CLI_EXIT_OK;
	case CLI_VERSION:
		fprintf(out, "hakkas %s\n", hakkas_version());
		return CLI_EXIT_OK;
	case CLI_LIST_TYPES:
		for (size_t i = 0; i < block_types_count(); i++)
			fprintf(out, "%s\n", block_types_at(i)->name);
		return CLI_EXIT_OK;
	case CLI_DESCRIBE_TYPE:
		return describe_type(o->arg, out);
	case CLI_DESCRIBE_TOPOLOGY:
		return describe_topology(cfg, o->arg, out);
	case CLI_USER:
		return user_command(o, cfg, in, out);
	case CLI_RUN:
		return posix_host_run(cfg);
	}
	return CLI_EXIT_FAILURE;
}

int cli_main(int argc, char *const argv[], FILE *in, FILE *out)
{
	CliOptions o;
	int rc = cli_parse(argc, argv, &o);
	if (rc != CLI_EXIT_OK)
		return rc;
	if (o.command == CLI_HELP || o.command == CLI_VERSION)
		return cli_execute(&o, NULL, in, out);

	/* Config is a few KiB; keep it off the stack. */
	static Config cfg;
	rc = cli_config(&o, &cfg);
	if (rc != CLI_EXIT_OK)
		return rc;
	/* run's engine registers them itself (05 §1.4 step 4). */
	if (o.command == CLI_RUN)
		return cli_execute(&o, &cfg, in, out);
	if (block_types_register_builtins() != BLOCK_TYPES_OK) {
		log_error("cannot register the built-in block types");
		block_types_reset();
		return CLI_EXIT_FAILURE;
	}
	rc = cli_execute(&o, &cfg, in, out);
	block_types_reset();
	return rc;
}
