/*
 * The POSIX command line (05 §1.2): parsing the arguments, building the
 * configuration from defaults < configuration file < options, and running
 * the command. main() is cli_main; the steps are separate so that tests can
 * run them in-process.
 *
 * Grammar (decided, plan task 11):
 *
 *	hakkas [global options] <command>
 *	global options: --fine | --info | --warn, --basedir <dir>, --confdir <dir>,
 *			--help | -h, --version
 *	commands:	list types
 *			describe type <typename>
 *			describe topology <filename>
 *			user add <name> --role <role>
 *			user passwd <name>
 *			user del <name>
 *			user list
 *			run [run options]
 *	run options:	--id <id>, --webdir <dir>, --host <addr>, --port <n>,
 *			--users <file>
 *
 * Global options come before the command, run options after "run" (decided
 * with the user 2026-10-25: `run process`, `run processes` and `run daemon`
 * are gone; `run` is what `run daemon` was); an option repeated takes its
 * last value. Values follow
 * as the next argument (no "--opt=value"). Option paths are used as given,
 * i.e. relative to the working directory.
 *
 * Every command except --help and --version first builds the configuration
 * (05 §1.4 step 1): the defaults (confdir from --confdir, default /etc/hakkas;
 * webdir HAKKAS_DEFAULT_WEBDIR; datadir <basedir>, default "."), then
 * <confdir>/hakkas.json, then the options; the log threshold is the
 * resulting log.level (a --fine/--info/--warn already applies while the file
 * is read).
 *
 * The user commands (02 §2.2; decided in plan task 16, recorded in 05
 * §1.2) edit the users file auth.users_file (default <confdir>/users.json;
 * a missing file is no users): add creates a user with a role (admin,
 * editor, operator, viewer; a name that exists is refused), passwd sets an
 * existing user's password (the role stays), del removes a user, list prints
 * "<name> <role>" per user, nothing else. add and passwd read the password
 * from in (platform/posix/password.h: a terminal with echo off and asked
 * twice, else one line), never from argv, and store only the derived keys
 * (a fresh random salt, USERS_DEFAULT_ITERATIONS). A malformed users file
 * is never overwritten: the command fails. The file is written atomically;
 * a running engine re-reads it at every login challenge (02 §2.2).
 *
 * Output (a type list, a JSON descriptor) goes to out (stdout); diagnostics
 * go through the log, whose POSIX sink is stderr. JSON is printed compact, one
 * document per line.
 *
 * run (05 §1.2, §1.4; plan tasks 19, 21e, 62) is the POSIX host
 * (platform/posix/host.h): it creates the runtime through the local C API
 * (hakkas.h) with SIGINT/SIGTERM as its stop request
 * (platform/posix/signals.h), loads and starts every saved process with its
 * bindings and the saved links (hakkas_process_load_saved) and runs the
 * engine until a stop request; it exits 0 after that clean stop and 1 if
 * the engine refuses to start.
 *
 * Exit codes: CLI_EXIT_OK (0) success, CLI_EXIT_FAILURE (1) a runtime failure
 * (unreadable or invalid configuration file, unknown type, a graph file that
 * does not load, an unknown or existing user, a refused password, a
 * malformed or unwritable users file, an engine that refuses to start),
 * CLI_EXIT_USAGE (2) a usage
 * error (unknown command or option, a missing or extra argument, an invalid
 * option value).
 */
#ifndef HAKKAS_PLATFORM_POSIX_CLI_H
#define HAKKAS_PLATFORM_POSIX_CLI_H

#include <stdbool.h>
#include <stdio.h>

#include "system/config.h"
#include "system/log.h"

#define CLI_EXIT_OK 0
#define CLI_EXIT_FAILURE 1
#define CLI_EXIT_USAGE 2

#define CLI_DEFAULT_BASEDIR "."
#define CLI_DEFAULT_CONFDIR "/etc/hakkas"
/* webdir's default (05 §1.1): an absolute path chosen by the build,
 * HAKKAS_DEFAULT_WEBDIR — the source tree's frontend/dist, or the installed
 * frontend of a package (HAKKAS_PACKAGE, CMakeLists.txt). */
#ifndef HAKKAS_DEFAULT_WEBDIR
#error "HAKKAS_DEFAULT_WEBDIR is set by CMakeLists.txt"
#endif

enum cli_command {
	CLI_HELP,
	CLI_VERSION,
	CLI_LIST_TYPES,
	CLI_DESCRIBE_TYPE,
	CLI_DESCRIBE_TOPOLOGY,
	CLI_USER,
	CLI_RUN,
};

/* The parsed arguments; the strings point into argv. */
typedef struct {
	enum cli_command command;
	bool level_set; /* --fine/--info/--warn given */
	enum log_level level;
	const char *basedir;
	const char *confdir;
	/* describe type: the type name; describe topology: the file; user: the
	 * subcommand. */
	const char *arg;
	/* user add, passwd, del: the user name; user add: the role. */
	const char *user;
	const char *role;
	/* run options, NULL when not given */
	const char *id;
	const char *webdir;
	const char *host;
	const char *port;
	const char *users;
} CliOptions;

/* Parse argv[1..argc-1] into *opts. CLI_EXIT_OK, or CLI_EXIT_USAGE with the
 * problem logged as an error. */
int cli_parse(int argc, char *const argv[], CliOptions *opts);

/* Build the configuration for opts: defaults < file < options (05 §1.2).
 * CLI_EXIT_OK; CLI_EXIT_FAILURE for a configuration file that cannot be read
 * or is invalid; CLI_EXIT_USAGE for an invalid option value. Logged. */
int cli_config(const CliOptions *opts, Config *cfg);

/* Run the parsed command with cfg (unused for --help/--version). The block
 * types must be registered (block_types_register_builtins), except for run,
 * whose engine registers them (05 §1.4 step 4) and must not find them
 * registered. in is where passwords are read from (stdin). */
int cli_execute(const CliOptions *opts, const Config *cfg, FILE *in, FILE *out);

/* The usage text. */
void cli_usage(FILE *out);

/* The whole command line: parse, configure (setting the log threshold),
 * register the built-in block types (not for run), execute, unregister them.
 * Returns the exit code. */
int cli_main(int argc, char *const argv[], FILE *in, FILE *out);

#endif
