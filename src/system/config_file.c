/*
 * The configuration file through the PAL (05 §1.1): reading it, and writing
 * the generated auth.server_secret back. See system/config.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "system/base64.h"
#include "system/config.h"

/* Read the file at path into a heap buffer with a NUL after its *len bytes.
 * CONFIG_ERR_STORAGE with *missing set for no file. */
static enum config_status read_file(const char *path, char **out, size_t *len, bool *missing)
{
	*out = NULL;
	*len = 0;
	*missing = false;
	struct pal_stat info;
	enum pal_err err = pal_storage_stat(path, &info);
	if (err == PAL_ERR_NOT_FOUND) {
		*missing = true;
		return CONFIG_ERR_STORAGE;
	}
	if (err == PAL_OK && info.is_dir)
		err = PAL_ERR_INVALID;
	if (err != PAL_OK) {
		log_error("cannot read %s: %s", path, pal_err_str(err));
		return CONFIG_ERR_STORAGE;
	}
	if (info.size > HAKKAS_CONFIG_FILE_MAX) {
		log_error("%s: %llu bytes, more than the limit of %lu", path,
			  (unsigned long long)info.size, (unsigned long)HAKKAS_CONFIG_FILE_MAX);
		return CONFIG_ERR_MALFORMED;
	}
	size_t cap = (size_t)info.size;
	char *buf = malloc(cap + 1);
	if (buf == NULL)
		return CONFIG_ERR_NO_MEMORY;
	err = pal_storage_read(path, buf, cap, len);
	if (err != PAL_OK) {
		free(buf);
		log_error("cannot read %s: %s", path, pal_err_str(err));
		return CONFIG_ERR_STORAGE;
	}
	buf[*len] = '\0';
	*out = buf;
	return CONFIG_OK;
}

enum config_status config_load(Config *cfg)
{
	char *text;
	size_t len;
	bool missing;
	enum config_status st = read_file(cfg->file, &text, &len, &missing);
	if (missing) {
		log_info("%s not found: using the defaults", cfg->file);
		return CONFIG_OK;
	}
	if (st != CONFIG_OK)
		return st;
	st = config_parse(cfg, text, len, cfg->file);
	free(text);
	return st;
}

/* ---- Inserting the secret as text --------------------------------------------- */

/*
 * A minimal scanner over a document cJSON has already accepted, so it only
 * needs to find where things are: skip whitespace, strings and values, and
 * find an object's member by key. Every index stays below len.
 */

static size_t skip_ws(const char *t, size_t len, size_t i)
{
	while (i < len && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r'))
		i++;
	return i;
}

/* t[i] is '"': the index after the closing quote. */
static size_t skip_string(const char *t, size_t len, size_t i)
{
	for (i++; i < len && t[i] != '"'; i++) {
		if (t[i] == '\\')
			i++;
	}
	return i < len ? i + 1 : len;
}

/* The index after the value starting at t[i]. */
static size_t skip_value(const char *t, size_t len, size_t i)
{
	if (i >= len)
		return len;
	if (t[i] == '"')
		return skip_string(t, len, i);
	if (t[i] == '{' || t[i] == '[') {
		int depth = 0;
		while (i < len) {
			if (t[i] == '"') {
				i = skip_string(t, len, i);
				continue;
			}
			if (t[i] == '{' || t[i] == '[')
				depth++;
			else if ((t[i] == '}' || t[i] == ']') && --depth == 0)
				return i + 1;
			i++;
		}
		return len;
	}
	while (i < len && strchr(",}] \t\r\n", t[i]) == NULL)
		i++;
	return i;
}

/* Whether the JSON string text t[from..to) decodes to key. */
static bool key_is(const char *t, size_t from, size_t to, const char *key, bool *no_memory)
{
	cJSON *k = cJSON_ParseWithLength(t + from, to - from);
	if (k == NULL) {
		*no_memory = true; /* the text was valid, so only memory can fail */
		return false;
	}
	bool same = cJSON_IsString(k) && strcmp(k->valuestring, key) == 0;
	cJSON_Delete(k);
	return same;
}

/* The start of the value of obj's first member called key (obj: the index of
 * the object's '{'), or len if there is none. *value_end is its end. Matches
 * cJSON, which also takes the first member (duplicates are refused anyway). */
static size_t find_member(const char *t, size_t len, size_t obj, const char *key,
			  size_t *value_end, bool *no_memory)
{
	size_t i = skip_ws(t, len, obj + 1);
	while (i < len && t[i] == '"') {
		size_t key_end = skip_string(t, len, i);
		bool match = key_is(t, i, key_end, key, no_memory);
		i = skip_ws(t, len, key_end);
		i = skip_ws(t, len, i + 1); /* past ':' */
		size_t end = skip_value(t, len, i);
		if (match) {
			*value_end = end;
			return i;
		}
		i = skip_ws(t, len, end);
		if (i >= len || t[i] != ',')
			break;
		i = skip_ws(t, len, i + 1);
	}
	return len;
}

/* t with [from, to) replaced by ins: a new heap string, or NULL. */
static char *splice(const char *t, size_t len, size_t from, size_t to, const char *ins)
{
	size_t n = strlen(ins);
	char *out = malloc(len - (to - from) + n + 1);
	if (out == NULL)
		return NULL;
	memcpy(out, t, from);
	memcpy(out + from, ins, n);
	memcpy(out + from + n, t + to, len - to);
	out[len - (to - from) + n] = '\0';
	return out;
}

/* t with member inserted as the first member of the object at t[obj]. In a
 * non-empty object it is followed by a ',' and the whitespace that preceded
 * the first member, so a formatted file keeps its layout. */
static char *insert_member(const char *t, size_t len, size_t obj, const char *member)
{
	size_t first = skip_ws(t, len, obj + 1);
	if (first < len && t[first] == '}')
		return splice(t, len, obj + 1, obj + 1, member);
	size_t ws = first - (obj + 1);
	size_t n = ws + strlen(member) + 1 + ws + 1;
	char *ins = malloc(n);
	if (ins == NULL)
		return NULL;
	snprintf(ins, n, "%.*s%s,%.*s", (int)ws, t + obj + 1, member, (int)ws, t + obj + 1);
	/* The original whitespace before the first member now follows the ','. */
	char *out = splice(t, len, obj + 1, first, ins);
	free(ins);
	return out;
}

/* t (a configuration document) with "auth.server_secret": secret added. */
static char *with_secret(const char *t, size_t len, const char *secret, bool *no_memory)
{
	char member[sizeof("\"auth\": {\"server_secret\": \"\"}") + CONFIG_SECRET_MAX];
	size_t root = skip_ws(t, len, 0);
	size_t auth_end = 0;
	size_t auth = find_member(t, len, root, "auth", &auth_end, no_memory);
	if (*no_memory)
		return NULL;
	if (auth == len) {
		snprintf(member, sizeof(member), "\"auth\": {\"server_secret\": \"%s\"}", secret);
		return insert_member(t, len, root, member);
	}
	if (t[auth] != '{') {
		/* "auth": null, the same as absent. */
		snprintf(member, sizeof(member), "{\"server_secret\": \"%s\"}", secret);
		return splice(t, len, auth, auth_end, member);
	}
	size_t secret_end = 0;
	size_t old = find_member(t, len, auth, "server_secret", &secret_end, no_memory);
	if (*no_memory)
		return NULL;
	if (old != len) {
		/* "server_secret": null; replace the value, or the key would be
		 * duplicated. */
		snprintf(member, sizeof(member), "\"%s\"", secret);
		return splice(t, len, old, secret_end, member);
	}
	snprintf(member, sizeof(member), "\"server_secret\": \"%s\"", secret);
	return insert_member(t, len, auth, member);
}

enum config_status config_ensure_server_secret(Config *cfg, pal_random_fn random_bytes)
{
	if (cfg->auth.server_secret[0] != '\0')
		return CONFIG_OK;

	char *text;
	size_t len;
	bool missing;
	enum config_status st = read_file(cfg->file, &text, &len, &missing);
	if (missing) {
		/* No file: create one holding only the secret. */
		static const char empty[] = "{}\n";
		text = malloc(sizeof(empty));
		if (text == NULL)
			return CONFIG_ERR_NO_MEMORY;
		memcpy(text, empty, sizeof(empty));
		len = sizeof(empty) - 1;
	} else if (st != CONFIG_OK) {
		return st;
	} else {
		/* Check the file still is a configuration object without a
		 * secret (it could have changed since config_load). */
		cJSON *root = cJSON_ParseWithLength(text, len);
		const cJSON *auth = cJSON_GetObjectItemCaseSensitive(root, "auth");
		const cJSON *old = cJSON_GetObjectItemCaseSensitive(auth, "server_secret");
		/* Then with_secret replaces only a null "auth" or "server_secret". */
		bool ok = cJSON_IsObject(root) && (auth == NULL || cJSON_IsNull(auth) ||
						   cJSON_IsObject(auth)) &&
			  (old == NULL || cJSON_IsNull(old));
		cJSON_Delete(root);
		if (!ok) {
			free(text);
			log_error("%s: not a valid configuration file (a JSON object)", cfg->file);
			return CONFIG_ERR_MALFORMED;
		}
	}

	unsigned char raw[CONFIG_SECRET_BYTES];
	char secret[BASE64_ENCODED_LEN(CONFIG_SECRET_BYTES) + 1];
	if (random_bytes(raw, sizeof(raw)) != PAL_OK) {
		free(text);
		log_error("cannot generate auth.server_secret: random source unavailable");
		return CONFIG_ERR_UNAVAILABLE;
	}
	base64_encode(raw, sizeof(raw), secret, sizeof(secret));
	memset(raw, 0, sizeof(raw));

	bool no_memory = false;
	char *out = with_secret(text, len, secret, &no_memory);
	free(text);
	if (out == NULL)
		return CONFIG_ERR_NO_MEMORY;
	enum pal_err err = pal_storage_write(cfg->file, out, strlen(out));
	free(out);
	if (err != PAL_OK) {
		log_error("cannot write %s with the generated auth.server_secret: %s", cfg->file,
			  pal_err_str(err));
		return CONFIG_ERR_STORAGE;
	}
	strcpy(cfg->auth.server_secret, secret);
	log_info("%s: generated auth.server_secret", cfg->file);
	return CONFIG_OK;
}
