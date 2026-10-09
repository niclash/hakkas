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
 *			user add|passwd|del|list ...
 *			run [run options] process <file>[:<file>...]
 *			run [run options] daemon
 *	run options:	--id <id>, --webdir <dir>, --host <addr>, --port <n>,
 *			--users <file>
 *
 * Global options come before the command, run options between "run" and
 * "process"/"daemon"; an option repeated takes its last value. Values follow
 * as the next argument (no "--opt=value"). Option paths are used as given,
 * i.e. relative to the working directory.
 *
 * Every command except --help and --version first builds the configuration
 * (05 §1.4 step 1): the defaults (confdir from --confdir, default /etc/hakkas;
 * webdir <cwd>/frontend/src; datadir <basedir>, default "."), then
 * <confdir>/hakkas.json, then the options; the log threshold is the
 * resulting log.level (a --fine/--info/--warn already applies while the file
 * is read).
 *
 * Output (a type list, a JSON descriptor) goes to out (stdout); diagnostics
 * go through the log, whose POSIX sink is stderr. JSON is printed compact, one
 * document per line.
 *
 * Exit codes: CLI_EXIT_OK (0) success, CLI_EXIT_FAILURE (1) a runtime failure
 * (unreadable or invalid configuration file, unknown type, a graph file that
 * does not load, a command not implemented yet), CLI_EXIT_USAGE (2) a usage
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
/* webdir's default, relative to the working directory (05 §1.1). */
#define CLI_DEFAULT_WEBDIR "frontend/src"

enum cli_command {
	CLI_HELP,
	CLI_VERSION,
	CLI_LIST_TYPES,
	CLI_DESCRIBE_TYPE,
	CLI_DESCRIBE_TOPOLOGY,
	CLI_USER,
	CLI_RUN_PROCESS,
	CLI_RUN_DAEMON,
};

/* The parsed arguments; the strings point into argv. */
typedef struct {
	enum cli_command command;
	bool level_set; /* --fine/--info/--warn given */
	enum log_level level;
	const char *basedir;
	const char *confdir;
	/* describe type: the type name; describe topology: the file; user: the
	 * subcommand; run process: the colon-separated file list. */
	const char *arg;
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
 * types must be registered (block_types_register_builtins). */
int cli_execute(const CliOptions *opts, const Config *cfg, FILE *out);

/* The usage text. */
void cli_usage(FILE *out);

/* The whole command line: parse, configure (setting the log threshold),
 * register the built-in block types, execute, unregister them. Returns the
 * exit code. */
int cli_main(int argc, char *const argv[], FILE *out);

#endif
