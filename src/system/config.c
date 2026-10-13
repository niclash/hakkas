/*
 * The configuration (05 §1.1): defaults, parsing, overrides. See
 * system/config.h. Reading and writing the file is system/config_file.c.
 */
#include "system/config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "system/uuid.h"

const char *config_status_str(enum config_status st)
{
	switch (st) {
	case CONFIG_OK:
		return "ok";
	case CONFIG_ERR_INVALID:
		return "invalid value";
	case CONFIG_ERR_MALFORMED:
		return "malformed configuration file";
	case CONFIG_ERR_STORAGE:
		return "storage error";
	case CONFIG_ERR_NO_MEMORY:
		return "out of memory";
	case CONFIG_ERR_UNAVAILABLE:
		return "random source unavailable";
	}
	return "?";
}

/* ---- The keys (05 §1.1) ------------------------------------------------------ */

enum kind {
	K_STRING,    /* a string of min..size-1 bytes */
	K_PATH,	     /* a path of min..CONFIG_PATH_LEN_MAX bytes; relative: see resolve */
	K_UINT,	     /* an integer in min..max */
	K_THRESHOLD, /* a number >= 0 (01 §3.3), +Infinity included */
	K_BOOL,
	K_LEVEL,     /* a log level name (05 §4) */
	K_ORIGINS,   /* an array of origin strings */
	K_IPV4,	     /* a dotted-quad IPv4 address, or "" */
	K_HOSTNAME,  /* a DNS label: letters, digits, '-' inside */
	K_TIME_ZONE, /* a POSIX TZ string (system/tz.h) */
};

typedef struct {
	const char *name;
	enum kind kind;
	size_t offset;
	size_t size;  /* string kinds: the buffer's size */
	uint32_t min; /* K_UINT: the range; string kinds: the shortest length */
	uint32_t max;
} KeyDef;

#define FIELD_SIZE(f) sizeof(((Config *)0)->f)
#define KEY(n, k, f, lo, hi) {n, k, offsetof(Config, f), FIELD_SIZE(f), lo, hi}

/* The longest path: room is left for an atomic write's ".tmp" suffix (05 §2.3)
 * and the NUL, so any configured path can be written to. */
#define CONFIG_PATH_LEN_MAX (PAL_PATH_MAX - sizeof(PAL_TMP_SUFFIX))

static const KeyDef keys[] = {
	KEY("engine_id", K_STRING, engine_id, 1, 0),
	KEY("label", K_STRING, label, 0, 0),
	KEY("host", K_STRING, host, 1, 0),
	KEY("port", K_UINT, port, CONFIG_PORT_MIN, CONFIG_PORT_MAX),
	KEY("webdir", K_PATH, webdir, 1, 0),
	KEY("datadir", K_PATH, datadir, 1, 0),
	KEY("max_connections", K_UINT, max_connections, CONFIG_MAX_CONNECTIONS_MIN,
	    CONFIG_MAX_CONNECTIONS_MAX),
	KEY("max_message_bytes", K_UINT, max_message_bytes, CONFIG_MAX_MESSAGE_BYTES_MIN,
	    CONFIG_MAX_MESSAGE_BYTES_MAX),
	KEY("allowed_origins", K_ORIGINS, allowed_origins, 1, HAKKAS_CONFIG_ORIGINS_MAX),
	KEY("change_threshold", K_THRESHOLD, change_threshold, 0, 0),
	KEY("time_zone", K_TIME_ZONE, time_zone, 1, 0),
	KEY("log.level", K_LEVEL, log.level, 0, 0),
	KEY("log.mirror", K_BOOL, log.mirror, 0, 0),
	KEY("auth.users_file", K_PATH, auth.users_file, 1, 0),
	KEY("auth.idle_timeout_s", K_UINT, auth.idle_timeout_s, CONFIG_SECONDS_MIN,
	    CONFIG_SECONDS_MAX),
	KEY("auth.max_lifetime_s", K_UINT, auth.max_lifetime_s, CONFIG_SECONDS_MIN,
	    CONFIG_SECONDS_MAX),
	KEY("auth.server_secret", K_STRING, auth.server_secret, CONFIG_SECRET_MIN, 0),
	KEY("environment.socket", K_PATH, environment.socket, 0, 0),
	KEY("tls.enabled", K_BOOL, tls.enabled, 0, 0),
	KEY("tls.cert", K_PATH, tls.cert, 0, 0),
	KEY("tls.key", K_PATH, tls.key, 0, 0),
	KEY("net.hostname", K_HOSTNAME, net.hostname, 1, 0),
	KEY("net.dhcp", K_BOOL, net.dhcp, 0, 0),
	KEY("net.address", K_IPV4, net.address, 0, 0),
	KEY("net.netmask", K_IPV4, net.netmask, 0, 0),
	KEY("net.gateway", K_IPV4, net.gateway, 0, 0),
};

#define KEY_COUNT (sizeof(keys) / sizeof(keys[0]))

/* The sections: top-level keys whose value is an object of nested keys. */
static const char *const sections[] = {"log", "auth", "environment", "tls", "net"};

#define SECTION_COUNT (sizeof(sections) / sizeof(sections[0]))

/* Room for "<section>.<key>" of any known key. */
#define KEY_NAME_MAX 32

static const KeyDef *find_key(const char *name)
{
	for (size_t i = 0; i < KEY_COUNT; i++) {
		if (strcmp(keys[i].name, name) == 0)
			return &keys[i];
	}
	return NULL;
}

static bool is_section(const char *name)
{
	for (size_t i = 0; i < SECTION_COUNT; i++) {
		if (strcmp(sections[i], name) == 0)
			return true;
	}
	return false;
}

/* ---- Defaults ---------------------------------------------------------------- */

/* dir + "/" + name into out (size bytes), without doubling a trailing '/' of
 * dir. False if dir is empty or the result does not fit len_max bytes. */
static bool join(const char *dir, const char *name, char *out, size_t size, size_t len_max)
{
	size_t len = strlen(dir);
	if (len == 0)
		return false;
	int n = snprintf(out, size, "%s%s%s", dir, dir[len - 1] == '/' ? "" : "/", name);
	return n >= 0 && (size_t)n < size && (size_t)n <= len_max;
}

/* Copy the path src into dst (a PAL_PATH_MAX buffer); false if it is NULL,
 * empty or longer than CONFIG_PATH_LEN_MAX. */
static bool copy_path(char *dst, const char *src)
{
	if (src == NULL || src[0] == '\0' || strlen(src) > CONFIG_PATH_LEN_MAX)
		return false;
	strcpy(dst, src);
	return true;
}

enum config_status config_init(Config *cfg, const ConfigDefaults *d)
{
	memset(cfg, 0, sizeof(*cfg));
	strcpy(cfg->label, CONFIG_DEFAULT_LABEL);
	strcpy(cfg->host, CONFIG_DEFAULT_HOST);
	cfg->port = CONFIG_DEFAULT_PORT;
	cfg->max_connections = CONFIG_DEFAULT_MAX_CONNECTIONS;
	cfg->max_message_bytes = CONFIG_DEFAULT_MAX_MESSAGE_BYTES;
	cfg->change_threshold = CONFIG_DEFAULT_CHANGE_THRESHOLD;
	strcpy(cfg->time_zone, CONFIG_DEFAULT_TIME_ZONE);
	cfg->log.level = CONFIG_DEFAULT_LOG_LEVEL;
	cfg->auth.idle_timeout_s = CONFIG_DEFAULT_IDLE_TIMEOUT_S;
	cfg->auth.max_lifetime_s = CONFIG_DEFAULT_MAX_LIFETIME_S;
	cfg->net.dhcp = true;

	if (!copy_path(cfg->confdir, d->confdir) ||
	    !join(cfg->confdir, CONFIG_FILE_NAME, cfg->file, sizeof(cfg->file),
		  CONFIG_PATH_LEN_MAX) ||
	    !join(cfg->confdir, CONFIG_USERS_FILE_NAME, cfg->auth.users_file,
		  sizeof(cfg->auth.users_file), CONFIG_PATH_LEN_MAX)) {
		log_error("configuration directory: expected a path of 1 to %u bytes",
			  (unsigned)(CONFIG_PATH_LEN_MAX - (sizeof("/" CONFIG_FILE_NAME) - 1)));
		return CONFIG_ERR_INVALID;
	}
	if (!copy_path(cfg->webdir, d->webdir)) {
		log_error("webdir: expected a path of 1 to %u bytes",
			  (unsigned)CONFIG_PATH_LEN_MAX);
		return CONFIG_ERR_INVALID;
	}
	if (!copy_path(cfg->datadir, d->datadir)) {
		log_error("datadir: expected a path of 1 to %u bytes",
			  (unsigned)CONFIG_PATH_LEN_MAX);
		return CONFIG_ERR_INVALID;
	}
	return CONFIG_OK;
}

/* ---- Values ------------------------------------------------------------------- */

static bool ipv4_valid(const char *s)
{
	for (int part = 0; part < 4; part++) {
		if (part > 0 && *s++ != '.')
			return false;
		unsigned v = 0;
		int digits = 0;
		while (*s >= '0' && *s <= '9' && digits < 4) {
			v = v * 10 + (unsigned)(*s++ - '0');
			digits++;
		}
		if (digits == 0 || digits > 3 || v > 255)
			return false;
	}
	return *s == '\0';
}

static bool hostname_valid(const char *s)
{
	size_t len = strlen(s);
	if (s[0] == '-' || (len > 0 && s[len - 1] == '-'))
		return false;
	for (; *s != '\0'; s++) {
		char c = *s;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		      c == '-'))
			return false;
	}
	return true;
}

static void problem(const char *origin, const char *key, const char *what)
{
	log_error("%s: %s: %s", origin, key, what);
}

/* A string value of def's kind into its field (or, for a path from the file,
 * resolved against confdir). False (logged) if v is not one. Values are never
 * logged: one may be the server secret. */
static bool apply_string(Config *cfg, const KeyDef *def, const cJSON *v, const char *origin,
			 bool from_file)
{
	char *field = (char *)cfg + def->offset;
	size_t max = def->kind == K_PATH ? CONFIG_PATH_LEN_MAX : def->size - 1;
	char what[96];
	size_t len = cJSON_IsString(v) ? strlen(v->valuestring) : 0;
	bool ok = cJSON_IsString(v) && len >= def->min && len <= max;
	if (ok && def->kind == K_IPV4 && len > 0)
		ok = ipv4_valid(v->valuestring);
	if (ok && def->kind == K_HOSTNAME)
		ok = hostname_valid(v->valuestring);
	if (ok && def->kind == K_TIME_ZONE) {
		TimeZone tz;
		ok = tz_parse(v->valuestring, &tz);
	}
	if (!ok) {
		if (def->kind == K_TIME_ZONE)
			snprintf(what, sizeof(what),
				 "expected a POSIX TZ string of at most %zu bytes, e.g. "
				 "\"CET-1CEST,M3.5.0,M10.5.0/3\"",
				 max);
		else if (def->kind == K_IPV4)
			snprintf(what, sizeof(what), "expected an IPv4 address (a.b.c.d) or \"\"");
		else if (def->kind == K_HOSTNAME)
			snprintf(what, sizeof(what),
				 "expected a host name of 1 to %zu letters, digits or inner '-'",
				 max);
		else
			snprintf(what, sizeof(what), "expected a %s of %u to %zu bytes",
				 def->kind == K_PATH ? "path" : "string", (unsigned)def->min, max);
		problem(origin, def->name, what);
		return false;
	}
	/* A relative path in the file is relative to confdir, the file's own
	 * directory (05 §1.1); a command-line path is used as given. */
	if (def->kind == K_PATH && from_file && len > 0 && v->valuestring[0] != '/') {
		char buf[PAL_PATH_MAX];
		if (!join(cfg->confdir, v->valuestring, buf, sizeof(buf), CONFIG_PATH_LEN_MAX)) {
			log_error("%s: %s: path too long when made relative to %s (at most %zu "
				  "bytes)",
				  origin, def->name, cfg->confdir, max);
			return false;
		}
		strcpy(field, buf);
		return true;
	}
	memcpy(field, v->valuestring, len + 1);
	return true;
}

static bool apply_origins(Config *cfg, const KeyDef *def, const cJSON *v, const char *origin)
{
	bool ok = cJSON_IsArray(v) && cJSON_GetArraySize(v) <= (int)def->max;
	const cJSON *e;
	if (ok) {
		cJSON_ArrayForEach(e, v)
		{
			if (!cJSON_IsString(e) || e->valuestring[0] == '\0' ||
			    strlen(e->valuestring) > CONFIG_ORIGIN_MAX)
				ok = false;
		}
	}
	if (!ok) {
		char what[96];
		snprintf(what, sizeof(what),
			 "expected an array of at most %u strings of 1 to %d bytes",
			 (unsigned)def->max, CONFIG_ORIGIN_MAX);
		problem(origin, def->name, what);
		return false;
	}
	cfg->allowed_origin_count = 0;
	cJSON_ArrayForEach(e, v)
	{
		strcpy(cfg->allowed_origins[cfg->allowed_origin_count++], e->valuestring);
	}
	return true;
}

/* Apply the value v of key def. null is absent: nothing changes. False
 * (logged) for a wrong type or an out-of-range value. */
static bool apply_value(Config *cfg, const KeyDef *def, const cJSON *v, const char *origin,
			bool from_file)
{
	if (cJSON_IsNull(v))
		return true;
	void *field = (char *)cfg + def->offset;
	char what[96];
	switch (def->kind) {
	case K_STRING:
	case K_PATH:
	case K_IPV4:
	case K_HOSTNAME:
	case K_TIME_ZONE:
		return apply_string(cfg, def, v, origin, from_file);
	case K_ORIGINS:
		return apply_origins(cfg, def, v, origin);
	case K_UINT: {
		double d = cJSON_IsNumber(v) ? v->valuedouble : NAN;
		if (!(d >= def->min && d <= def->max && d == floor(d))) {
			snprintf(what, sizeof(what), "expected an integer from %u to %u",
				 (unsigned)def->min, (unsigned)def->max);
			problem(origin, def->name, what);
			return false;
		}
		*(uint32_t *)field = (uint32_t)d;
		return true;
	}
	case K_THRESHOLD:
		/* Valid thresholds are >= 0, +Infinity (1e999) included (01 §3.3). */
		if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0)) {
			problem(origin, def->name, "expected a number >= 0");
			return false;
		}
		*(double *)field = v->valuedouble;
		return true;
	case K_BOOL:
		if (!cJSON_IsBool(v)) {
			problem(origin, def->name, "expected true or false");
			return false;
		}
		*(bool *)field = cJSON_IsTrue(v);
		return true;
	case K_LEVEL: {
		enum log_level level;
		if (!cJSON_IsString(v) || !log_level_parse(v->valuestring, &level)) {
			problem(origin, def->name,
				"expected \"fine\", \"info\", \"warn\" or \"error\"");
			return false;
		}
		*(enum log_level *)field = level;
		return true;
	}
	}
	return false;
}

/* ---- The file ------------------------------------------------------------------ */

/* Whether an earlier member of item's object has item's key. */
static bool duplicate(const cJSON *obj, const cJSON *item)
{
	for (const cJSON *e = obj->child; e != item; e = e->next) {
		if (strcmp(e->string, item->string) == 0)
			return true;
	}
	return false;
}

/* Apply the members of obj, the root (section NULL) or a section's object.
 * Every member is tried, so that every problem is logged. */
static enum config_status apply_object(Config *cfg, const cJSON *obj, const char *section,
				       const char *origin)
{
	enum config_status st = CONFIG_OK;
	const cJSON *e;
	cJSON_ArrayForEach(e, obj)
	{
		char name[KEY_NAME_MAX];
		if (duplicate(obj, e)) {
			log_error("%s: duplicate key \"%s%s%s\"", origin, section ? section : "",
				  section ? "." : "", e->string);
			return CONFIG_ERR_MALFORMED;
		}
		int n = snprintf(name, sizeof(name), "%s%s%s", section ? section : "",
				 section ? "." : "", e->string);
		/* A dotted name is not a nested key: "log.level" is {"log":
		 * {"level": ...}} only. */
		const KeyDef *def = NULL;
		if (n >= 0 && (size_t)n < sizeof(name) && strchr(e->string, '.') == NULL)
			def = find_key(name);
		if (def != NULL) {
			if (!apply_value(cfg, def, e, origin, true))
				st = CONFIG_ERR_INVALID;
		} else if (section == NULL && is_section(e->string)) {
			if (cJSON_IsNull(e))
				continue;
			if (!cJSON_IsObject(e)) {
				problem(origin, e->string, "expected an object");
				st = CONFIG_ERR_INVALID;
				continue;
			}
			enum config_status sst = apply_object(cfg, e, e->string, origin);
			if (sst == CONFIG_ERR_MALFORMED)
				return sst;
			if (sst != CONFIG_OK)
				st = sst;
		} else {
			/* Forward compatibility: a newer key is ignored (05 §1.1). */
			log_warn("%s: unknown key \"%s%s%s\" ignored", origin,
				 section ? section : "", section ? "." : "", e->string);
		}
	}
	return st;
}

/* Whether the len bytes at s are JSON whitespace (RFC 8259). */
static bool only_whitespace(const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
			return false;
	}
	return true;
}

enum config_status config_parse(Config *cfg, const char *text, size_t len, const char *origin)
{
	const char *end = NULL;
	cJSON *root = cJSON_ParseWithLengthOpts(text, len, &end, false);
	if (root != NULL && !only_whitespace(end, (size_t)(text + len - end))) {
		cJSON_Delete(root); /* trailing text after the document */
		root = NULL;
	}
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		log_error("%s: not a valid configuration file (a JSON object)", origin);
		return CONFIG_ERR_MALFORMED;
	}
	enum config_status st = apply_object(cfg, root, NULL, origin);
	cJSON_Delete(root);
	return st;
}

/* ---- Command-line overrides --------------------------------------------------- */

/* text as the JSON value of a key of kind k. Text that is not a valid value
 * of the kind becomes a string item, which the kind's check then refuses with
 * its usual message. */
static cJSON *value_from_text(enum kind k, const char *text)
{
	switch (k) {
	case K_UINT: {
		size_t n = strlen(text);
		if (n == 0 || n > 10 || strspn(text, "0123456789") != n)
			break;
		return cJSON_CreateNumber(strtod(text, NULL));
	}
	case K_THRESHOLD: {
		char *end = NULL;
		double d = strtod(text, &end);
		if (text[0] == '\0' || *end != '\0' || isnan(d))
			break;
		return cJSON_CreateNumber(d);
	}
	case K_BOOL:
		if (strcmp(text, "true") == 0)
			return cJSON_CreateTrue();
		if (strcmp(text, "false") == 0)
			return cJSON_CreateFalse();
		break;
	default:
		break;
	}
	return cJSON_CreateString(text);
}

enum config_status config_set(Config *cfg, const char *key, const char *text, const char *origin)
{
	const KeyDef *def = key != NULL ? find_key(key) : NULL;
	if (def == NULL || def->kind == K_ORIGINS || text == NULL) {
		log_error("%s: %s cannot be set this way", origin, key ? key : "(null)");
		return CONFIG_ERR_INVALID;
	}
	cJSON *v = value_from_text(def->kind, text);
	if (v == NULL)
		return CONFIG_ERR_NO_MEMORY;
	bool ok = apply_value(cfg, def, v, origin, false);
	cJSON_Delete(v);
	return ok ? CONFIG_OK : CONFIG_ERR_INVALID;
}

/* ---- Generated values ---------------------------------------------------------- */

enum config_status config_generate_engine_id(Config *cfg, pal_random_fn random_bytes)
{
	if (cfg->engine_id[0] != '\0')
		return CONFIG_OK;
	char uuid[UUID_TEXT_SIZE];
	if (!uuid_v4(random_bytes, uuid)) {
		log_error("cannot generate the engine id: random source unavailable");
		return CONFIG_ERR_UNAVAILABLE;
	}
	strcpy(cfg->engine_id, uuid);
	return CONFIG_OK;
}
