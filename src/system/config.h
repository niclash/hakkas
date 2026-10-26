/*
 * The configuration (05 §1.1): every key of hakkas.json with its default,
 * parsing the file, command-line overrides, the generated engine_id and
 * auth.server_secret.
 *
 * Naming: functions are prefixed config_, the status enum CONFIG_ /
 * config_status. Keys are named as in the 05 §1.1 table: a nested key is
 * "<section>.<key>" ("log.level"), written in the file as a nested object
 * ({"log": {"level": "info"}}).
 *
 * Precedence (05 §1.2): defaults < configuration file < command-line options.
 * The platform supplies the defaults that depend on it (ConfigDefaults:
 * confdir, webdir, datadir); config_init sets them and every other default,
 * config_load applies the file, config_set applies one command-line option.
 *
 * The file (decided, plan task 11; 05 §1.1 "Implementation decisions"):
 *  - a missing file means all defaults; every key is optional, and a key whose
 *    value is null is treated as absent;
 *  - a file that is not a JSON object, nested deeper than CJSON_NESTING_LIMIT,
 *    larger than HAKKAS_CONFIG_FILE_MAX, or with a duplicate key (at any level
 *    the configuration reads) is malformed: CONFIG_ERR_MALFORMED, logged as an
 *    error; the engine refuses to start;
 *  - an unknown key (also within a section) is logged as a warning and
 *    ignored (forward compatibility); a section ("log", "auth",
 *    "environment", "tls", "net") whose value is not an object is an invalid
 *    value;
 *  - a key with the wrong type or an out-of-range value is logged as an error
 *    naming the key; every key is checked (so every problem is reported at
 *    once) and then CONFIG_ERR_INVALID is returned; the engine refuses to
 *    start;
 *  - a relative path in the file (webdir, datadir, auth.users_file,
 *    environment.socket, environment.pictures, tls.cert, tls.key) is
 *    relative to confdir, the
 *    file's own directory; a path given
 *    as a command-line option is used as given (relative to the working
 *    directory).
 *
 * Memory: Config is plain fixed-size data (no heap), a few KiB; parsing builds
 * a temporary cJSON tree.
 *
 * Secrets: auth.server_secret is never logged (02 §2.5 item 8).
 *
 * Storage: config.c is pure; config_load and config_ensure_server_secret
 * (system/config_file.c) read and write through the PAL (platform/pal.h).
 *
 * Threading: engine-thread only (startup).
 */
#ifndef HAKKAS_SYSTEM_CONFIG_H
#define HAKKAS_SYSTEM_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform/pal.h"
#include "system/log.h"
#include "system/tz.h"

/* The file names in confdir (05 §1.1, 02 §2.2). */
#define CONFIG_FILE_NAME "hakkas.json"
#define CONFIG_USERS_FILE_NAME "users.json"

/* The largest configuration file that is read, in bytes; a larger one is
 * malformed. A port's build may define it. */
#ifndef HAKKAS_CONFIG_FILE_MAX
#define HAKKAS_CONFIG_FILE_MAX (16 * 1024)
#endif

/* The most allowed_origins entries; more is an invalid value. A port's build
 * may define it. */
#ifndef HAKKAS_CONFIG_ORIGINS_MAX
#define HAKKAS_CONFIG_ORIGINS_MAX 8
#endif

/* Longest values in bytes (without the NUL). */
#define CONFIG_ID_MAX 64	/* engine_id, like a graph id (02 §3.4.1) */
#define CONFIG_LABEL_MAX 64	/* label */
#define CONFIG_HOST_MAX 63	/* host */
#define CONFIG_ORIGIN_MAX 127	/* one allowed_origins entry */
#define CONFIG_SECRET_MIN 16	/* auth.server_secret */
#define CONFIG_SECRET_MAX 128	/* auth.server_secret */
#define CONFIG_HOSTNAME_MAX 63	/* net.hostname (one DNS label) */
#define CONFIG_IPV4_MAX 15	/* net.address etc., dotted quad */
#define CONFIG_TIME_ZONE_MAX TZ_TEXT_MAX /* time_zone */

/* The generated server secret: this many random bytes, base64 (RFC 4648 §4,
 * padded: 44 characters). */
#define CONFIG_SECRET_BYTES 32

/* Defaults (05 §1.1). */
#define CONFIG_DEFAULT_LABEL "Hakkas"
#define CONFIG_DEFAULT_HOST "0.0.0.0"
#define CONFIG_DEFAULT_PORT 8080
#define CONFIG_DEFAULT_MAX_CONNECTIONS 4
#define CONFIG_DEFAULT_MAX_MESSAGE_BYTES 16384
#define CONFIG_DEFAULT_CHANGE_THRESHOLD 0.05
#define CONFIG_DEFAULT_TIME_ZONE "UTC0"
#define CONFIG_DEFAULT_LOG_LEVEL LOG_LEVEL_WARN
#define CONFIG_DEFAULT_IDLE_TIMEOUT_S 1800
#define CONFIG_DEFAULT_MAX_LIFETIME_S 43200

/* Ranges of the integer keys, inclusive. port + 1 is the WebSocket port
 * (02 §0), so port is at most 65534. */
#define CONFIG_PORT_MIN 1
#define CONFIG_PORT_MAX 65534
#define CONFIG_MAX_CONNECTIONS_MIN 1
#define CONFIG_MAX_CONNECTIONS_MAX 1024
#define CONFIG_MAX_MESSAGE_BYTES_MIN 1024
#define CONFIG_MAX_MESSAGE_BYTES_MAX (16 * 1024 * 1024)
#define CONFIG_SECONDS_MIN 1
#define CONFIG_SECONDS_MAX 2147483647

enum config_status {
	CONFIG_OK = 0,
	CONFIG_ERR_INVALID = -1,     /* a key's value has the wrong type or is out of range */
	CONFIG_ERR_MALFORMED = -2,   /* the file is not a valid configuration document */
	CONFIG_ERR_STORAGE = -3,     /* reading or writing the file failed */
	CONFIG_ERR_NO_MEMORY = -4,
	CONFIG_ERR_UNAVAILABLE = -5, /* the random source failed */
};

/* A short, static description of st, for log lines. */
const char *config_status_str(enum config_status st);

/* Every key of 05 §1.1. Strings are NUL-terminated; "" means unset where the
 * key has no default. */
typedef struct {
	/* File locations: confdir, and the configuration file in it. */
	char confdir[PAL_PATH_MAX];
	char file[PAL_PATH_MAX]; /* <confdir>/hakkas.json */

	char engine_id[CONFIG_ID_MAX + 1]; /* "": unset, see config_generate_engine_id */
	char label[CONFIG_LABEL_MAX + 1];
	char host[CONFIG_HOST_MAX + 1];
	uint32_t port; /* the web server; the FBP WebSocket is port + 1 */
	char webdir[PAL_PATH_MAX];
	char datadir[PAL_PATH_MAX];
	uint32_t max_connections;
	uint32_t max_message_bytes;
	char allowed_origins[HAKKAS_CONFIG_ORIGINS_MAX][CONFIG_ORIGIN_MAX + 1];
	uint32_t allowed_origin_count;
	double change_threshold; /* >= 0, +Infinity included (01 §3.3) */
	/* A POSIX TZ string valid for tz_parse (system/tz.h): local time for
	 * the calendar blocks (01 §3.4, 03 §6.2). */
	char time_zone[CONFIG_TIME_ZONE_MAX + 1];
	struct {
		enum log_level level;
		bool mirror;
	} log;
	struct {
		char users_file[PAL_PATH_MAX];
		uint32_t idle_timeout_s;
		uint32_t max_lifetime_s;
		/* "": absent from the file, see config_ensure_server_secret. */
		char server_secret[CONFIG_SECRET_MAX + 1];
	} auth;
	struct {
		/* POSIX: the Unix socket of the external environment program
		 * (04 §0.1); "": no environment. */
		char socket[PAL_PATH_MAX];
		/* The directory of the environment's own copies of its
		 * pictures, served at /environment/pictures/ (04 §1, 05 §3,
		 * plan task 77); "": none. */
		char pictures[PAL_PATH_MAX];
	} environment;
	struct {
		bool enabled;
		char cert[PAL_PATH_MAX]; /* "": none */
		char key[PAL_PATH_MAX];	 /* "": none */
	} tls;
	struct {
		/* "": the platform's default, "hakkas-<last 3 MAC bytes in hex>"
		 * (Zephyr; 04 §6). */
		char hostname[CONFIG_HOSTNAME_MAX + 1];
		bool dhcp;
		char address[CONFIG_IPV4_MAX + 1]; /* "": none */
		char netmask[CONFIG_IPV4_MAX + 1];
		char gateway[CONFIG_IPV4_MAX + 1];
	} net;
} Config;

/* The defaults that depend on the platform (05 §1.1). POSIX: confdir
 * (--confdir, default /etc/hakkas), webdir chosen by the build (the source
 * tree's frontend/dist, or a package's installed frontend), datadir <basedir>;
 * Zephyr: 05 §1.3. */
typedef struct {
	const char *confdir;
	const char *webdir;
	const char *datadir;
} ConfigDefaults;

/* Make cfg the defaults: the keys' defaults of 05 §1.1 and d's directories;
 * file is <confdir>/hakkas.json and auth.users_file <confdir>/users.json.
 * CONFIG_ERR_INVALID (logged) if a directory is NULL, empty or too long. */
enum config_status config_init(Config *cfg, const ConfigDefaults *d);

/* Apply the configuration document text (len bytes, need not be
 * NUL-terminated) over cfg; origin names it in log lines (e.g. its path).
 * CONFIG_OK, CONFIG_ERR_MALFORMED, CONFIG_ERR_INVALID (see above; every
 * problem logged), CONFIG_ERR_NO_MEMORY. On failure cfg may be partly
 * changed; the engine does not start. */
enum config_status config_parse(Config *cfg, const char *text, size_t len, const char *origin);

/* Read cfg->file through the PAL and apply it (config_parse). A missing file
 * is CONFIG_OK with cfg unchanged (logged at info). CONFIG_ERR_STORAGE for
 * another read failure, CONFIG_ERR_MALFORMED for a file larger than
 * HAKKAS_CONFIG_FILE_MAX. (system/config_file.c) */
enum config_status config_load(Config *cfg);

/* Apply one command-line override (05 §1.2): key (a 05 §1.1 key name, e.g.
 * "port", "auth.users_file", "log.level") set from its text: a string or path
 * as given, an integer in decimal digits, a boolean "true"/"false", a log
 * level by name. Checked like the file's value; origin (e.g. "--port") names
 * it in the error. CONFIG_ERR_INVALID (logged) for a bad value or an unknown
 * key, CONFIG_ERR_NO_MEMORY. allowed_origins cannot be set this way. */
enum config_status config_set(Config *cfg, const char *key, const char *text, const char *origin);

/* engine_id unset: a fresh random UUID v4 for this boot (05 §1.1), from
 * random_bytes (pal_random_bytes). CONFIG_ERR_UNAVAILABLE if that fails.
 * Called at startup after the overrides (plan task 19). */
enum config_status config_generate_engine_id(Config *cfg, pal_random_fn random_bytes);

/*
 * auth.server_secret absent (05 §1.1, 02 §2.3): generate CONFIG_SECRET_BYTES
 * from random_bytes, base64-encode them (RFC 4648 §4, 44 characters) and add
 * the key to cfg->file, written atomically (pal_storage_write; mode 0600 on
 * POSIX). The rest of the file is kept byte for byte: the member is inserted
 * as text into the "auth" object (created if absent; a null "auth" or
 * "server_secret" is replaced). A missing file is created holding only the
 * secret. CONFIG_OK (also if cfg already has a secret, nothing written),
 * CONFIG_ERR_UNAVAILABLE, CONFIG_ERR_STORAGE (cannot read or write the file,
 * e.g. confdir missing or read-only: the engine does not start),
 * CONFIG_ERR_MALFORMED (the file changed since config_load), _NO_MEMORY. The
 * secret is never logged. Only startup (`run`, plan task 19) calls it, so
 * the other commands never write the file. (system/config_file.c)
 */
enum config_status config_ensure_server_secret(Config *cfg, pal_random_fn random_bytes);

#endif
