/*
 * The credential store (02 §2.2). See protocol/users.h.
 */
#include "protocol/users.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "protocol/websocket.h"
#include "system/log.h"

const char *users_status_str(enum users_status st)
{
	switch (st) {
	case USERS_OK:
		return "ok";
	case USERS_ERR_MALFORMED:
		return "malformed users file";
	case USERS_ERR_STORAGE:
		return "storage error";
	case USERS_ERR_NOT_FOUND:
		return "not found";
	case USERS_ERR_EXISTS:
		return "user exists";
	case USERS_ERR_FULL:
		return "too many users";
	case USERS_ERR_INVALID:
		return "invalid value";
	case USERS_ERR_NO_MEMORY:
		return "out of memory";
	case USERS_ERR_UNAVAILABLE:
		return "random source or hash unavailable";
	}
	return "?";
}

static const char *const role_names[] = {"viewer", "operator", "editor", "admin"};

const char *users_role_name(enum user_role role)
{
	return (unsigned)role < 4 ? role_names[role] : "?";
}

bool users_role_parse(const char *name, enum user_role *out)
{
	for (unsigned i = 0; name != NULL && i < 4; i++) {
		if (strcmp(name, role_names[i]) == 0) {
			*out = (enum user_role)i;
			return true;
		}
	}
	return false;
}

bool users_name_valid(const char *name)
{
	if (name == NULL)
		return false;
	size_t len = strlen(name);
	if (len == 0 || len > USERS_NAME_MAX)
		return false;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)name[i];
		if (c < 0x20 || c == 0x7f)
			return false;
	}
	return ws_utf8_valid(name, len);
}

void users_init(UserStore *s)
{
	crypto_zeroize(s, sizeof(*s));
}

const UserRecord *users_find(const UserStore *s, const char *name)
{
	for (size_t i = 0; name != NULL && i < s->count; i++) {
		if (strcmp(s->users[i].name, name) == 0)
			return &s->users[i];
	}
	return NULL;
}

/* ---- Parsing ------------------------------------------------------------------ */

/* The members of a record, in file order. */
static const char *const members[] = {"user",	    "role",	  "salt",
				      "iterations", "stored_key", "server_key"};
#define MEMBER_COUNT 6

/* Decode the base64 string item into exactly size bytes. */
static bool decode_exact(const cJSON *item, uint8_t *out, size_t size)
{
	if (!cJSON_IsString(item))
		return false;
	const char *t = item->valuestring;
	size_t len = strlen(t);
	uint8_t buf[CRYPTO_BASE64_DECODED_MAX(CRYPTO_BASE64_LEN(USERS_KEY_SIZE))];
	size_t n = 0;
	if (len != CRYPTO_BASE64_LEN(size) || !crypto_base64_decode(t, len, buf, sizeof(buf), &n) ||
	    n != size)
		return false;
	memcpy(out, buf, size);
	crypto_zeroize(buf, sizeof(buf));
	return true;
}

/* One record from obj into r. False (logged, naming the record by its
 * 1-based index and the member, never a value) if it is not valid. */
static bool parse_record(const cJSON *obj, size_t index, UserRecord *r, const char *origin)
{
	if (!cJSON_IsObject(obj)) {
		log_error("%s: user %zu: expected an object", origin, index);
		return false;
	}
	const cJSON *item[MEMBER_COUNT] = {0};
	const cJSON *e;
	cJSON_ArrayForEach(e, obj)
	{
		size_t m = 0;
		while (m < MEMBER_COUNT && strcmp(e->string, members[m]) != 0)
			m++;
		if (m == MEMBER_COUNT) {
			log_error("%s: user %zu: unknown member", origin, index);
			return false;
		}
		if (item[m] != NULL) {
			log_error("%s: user %zu: duplicate member \"%s\"", origin, index,
				  members[m]);
			return false;
		}
		item[m] = e;
	}
	for (size_t m = 0; m < MEMBER_COUNT; m++) {
		if (item[m] == NULL) {
			log_error("%s: user %zu: missing \"%s\"", origin, index, members[m]);
			return false;
		}
	}

	const char *problem = NULL;
	const char *member = NULL;
	if (!cJSON_IsString(item[0]) || !users_name_valid(item[0]->valuestring)) {
		member = "user";
		problem = "expected 1-32 bytes of UTF-8 without control characters";
	} else if (!cJSON_IsString(item[1]) || !users_role_parse(item[1]->valuestring, &r->role)) {
		member = "role";
		problem = "expected \"admin\", \"editor\", \"operator\" or \"viewer\"";
	} else if (!decode_exact(item[2], r->salt, USERS_SALT_SIZE)) {
		member = "salt";
		problem = "expected the base64 of 16 bytes";
	} else if (!cJSON_IsNumber(item[3]) || !(item[3]->valuedouble >= 1.0) ||
		   !(item[3]->valuedouble <= 4294967295.0) ||
		   item[3]->valuedouble != floor(item[3]->valuedouble)) {
		member = "iterations";
		problem = "expected an integer 1-4294967295";
	} else if (!decode_exact(item[4], r->stored_key, USERS_KEY_SIZE)) {
		member = "stored_key";
		problem = "expected the base64 of 32 bytes";
	} else if (!decode_exact(item[5], r->server_key, USERS_KEY_SIZE)) {
		member = "server_key";
		problem = "expected the base64 of 32 bytes";
	}
	if (problem != NULL) {
		log_error("%s: user %zu: %s: %s", origin, index, member, problem);
		return false;
	}
	strcpy(r->name, item[0]->valuestring);
	r->iterations = (uint32_t)item[3]->valuedouble;
	return true;
}

/* Whether the len bytes at text are only JSON whitespace. */
static bool blank(const char *text, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (strchr(" \t\r\n", text[i]) == NULL || text[i] == '\0')
			return false;
	}
	return true;
}

enum users_status users_parse(UserStore *s, const char *text, size_t len, const char *origin)
{
	users_init(s);
	if (blank(text, len))
		return USERS_OK;
	if (len > USERS_FILE_MAX) {
		log_error("%s: more than %lu bytes", origin, (unsigned long)USERS_FILE_MAX);
		return USERS_ERR_MALFORMED;
	}
	cJSON *root = cJSON_ParseWithLength(text, len);
	if (root == NULL) {
		/* cJSON reports no difference between a syntax error and running
		 * out of memory; a syntax error is far more likely. */
		log_error("%s: not valid JSON", origin);
		return USERS_ERR_MALFORMED;
	}
	enum users_status st = USERS_OK;
	if (!cJSON_IsArray(root)) {
		log_error("%s: expected a JSON array of users", origin);
		st = USERS_ERR_MALFORMED;
	} else if (cJSON_GetArraySize(root) > HAKKAS_USERS_MAX) {
		log_error("%s: more than %d users", origin, HAKKAS_USERS_MAX);
		st = USERS_ERR_MALFORMED;
	} else {
		const cJSON *e;
		cJSON_ArrayForEach(e, root)
		{
			UserRecord *r = &s->users[s->count];
			if (!parse_record(e, s->count + 1, r, origin)) {
				st = USERS_ERR_MALFORMED;
				break;
			}
			if (users_find(s, r->name) != NULL) {
				log_error("%s: user %zu: duplicate user \"%s\"", origin,
					  s->count + 1, r->name);
				st = USERS_ERR_MALFORMED;
				break;
			}
			s->count++;
		}
	}
	cJSON_Delete(root);
	if (st != USERS_OK)
		users_init(s);
	return st;
}

/* ---- Printing ------------------------------------------------------------------ */

static bool add_base64(cJSON *obj, const char *name, const uint8_t *data, size_t len)
{
	char text[CRYPTO_BASE64_LEN(USERS_KEY_SIZE) + 1];
	if (crypto_base64_encode(data, len, text, sizeof(text)) == 0)
		return false;
	return cJSON_AddStringToObject(obj, name, text) != NULL;
}

char *users_serialize(const UserStore *s, size_t *len)
{
	cJSON *root = cJSON_CreateArray();
	bool ok = root != NULL;
	for (size_t i = 0; ok && i < s->count; i++) {
		const UserRecord *r = &s->users[i];
		cJSON *obj = cJSON_CreateObject();
		if (obj == NULL || !cJSON_AddItemToArray(root, obj)) {
			cJSON_Delete(obj);
			ok = false;
			break;
		}
		ok = cJSON_AddStringToObject(obj, "user", r->name) != NULL &&
		     cJSON_AddStringToObject(obj, "role", users_role_name(r->role)) != NULL &&
		     add_base64(obj, "salt", r->salt, USERS_SALT_SIZE) &&
		     cJSON_AddNumberToObject(obj, "iterations", (double)r->iterations) != NULL &&
		     add_base64(obj, "stored_key", r->stored_key, USERS_KEY_SIZE) &&
		     add_base64(obj, "server_key", r->server_key, USERS_KEY_SIZE);
	}
	char *text = ok ? cJSON_Print(root) : NULL;
	cJSON_Delete(root);
	if (text == NULL)
		return NULL;
	size_t n = strlen(text);
	char *out = realloc(text, n + 2);
	if (out == NULL) {
		free(text);
		return NULL;
	}
	out[n] = '\n';
	out[n + 1] = '\0';
	if (len != NULL)
		*len = n + 1;
	return out;
}

/* ---- Changes -------------------------------------------------------------------- */

enum users_status users_set_password(UserRecord *r, const char *password, size_t len,
				     uint32_t iterations, pal_random_fn random_bytes)
{
	if (password == NULL || len == 0 || len > USERS_PASSWORD_MAX || iterations == 0)
		return USERS_ERR_INVALID;
	uint8_t salt[USERS_SALT_SIZE];
	uint8_t salted[CRYPTO_SHA256_SIZE], client_key[CRYPTO_SHA256_SIZE];
	uint8_t stored_key[USERS_KEY_SIZE], server_key[USERS_KEY_SIZE];
	enum users_status st = USERS_ERR_UNAVAILABLE;
	if (random_bytes(salt, sizeof(salt)) == PAL_OK &&
	    crypto_pbkdf2_hmac_sha256(password, len, salt, sizeof(salt), iterations, salted,
				      sizeof(salted)) &&
	    crypto_hmac_sha256(salted, sizeof(salted), "Client Key", 10, client_key) &&
	    crypto_sha256(client_key, sizeof(client_key), stored_key) &&
	    crypto_hmac_sha256(salted, sizeof(salted), "Server Key", 10, server_key)) {
		memcpy(r->salt, salt, sizeof(salt));
		r->iterations = iterations;
		memcpy(r->stored_key, stored_key, sizeof(stored_key));
		memcpy(r->server_key, server_key, sizeof(server_key));
		st = USERS_OK;
	}
	crypto_zeroize(salted, sizeof(salted));
	crypto_zeroize(client_key, sizeof(client_key));
	crypto_zeroize(stored_key, sizeof(stored_key));
	crypto_zeroize(server_key, sizeof(server_key));
	return st;
}

enum users_status users_add(UserStore *s, const char *name, enum user_role role,
			    const char *password, size_t len, uint32_t iterations,
			    pal_random_fn random_bytes)
{
	if (!users_name_valid(name) || (unsigned)role > USER_ROLE_ADMIN)
		return USERS_ERR_INVALID;
	if (users_find(s, name) != NULL)
		return USERS_ERR_EXISTS;
	if (s->count >= HAKKAS_USERS_MAX)
		return USERS_ERR_FULL;
	UserRecord *r = &s->users[s->count];
	memset(r, 0, sizeof(*r));
	enum users_status st = users_set_password(r, password, len, iterations, random_bytes);
	if (st != USERS_OK)
		return st;
	strcpy(r->name, name);
	r->role = role;
	s->count++;
	return USERS_OK;
}

enum users_status users_passwd(UserStore *s, const char *name, const char *password, size_t len,
			       uint32_t iterations, pal_random_fn random_bytes)
{
	UserRecord *r = (UserRecord *)users_find(s, name);
	if (r == NULL)
		return USERS_ERR_NOT_FOUND;
	return users_set_password(r, password, len, iterations, random_bytes);
}

enum users_status users_remove(UserStore *s, const char *name)
{
	const UserRecord *r = users_find(s, name);
	if (r == NULL)
		return USERS_ERR_NOT_FOUND;
	size_t i = (size_t)(r - s->users);
	memmove(&s->users[i], &s->users[i + 1], (s->count - i - 1) * sizeof(UserRecord));
	s->count--;
	crypto_zeroize(&s->users[s->count], sizeof(UserRecord));
	return USERS_OK;
}
