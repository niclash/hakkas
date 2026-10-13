/*
 * The bindings file (04 §4). See drivers/bindings_file.h.
 */
#include "drivers/bindings_file.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "system/log.h"

typedef struct {
	const char *origin;
	char *msg;
	size_t cap;
	bool ok;
	bool no_memory;
} Parser;

static void fail(Parser *p, const char *fmt, ...) LOG_PRINTF_FORMAT(2, 3);

static void fail(Parser *p, const char *fmt, ...)
{
	if (!p->ok)
		return; /* the first problem wins */
	p->ok = false;
	if (p->msg != NULL && p->cap > 0) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(p->msg, p->cap, fmt, ap);
		va_end(ap);
	}
}

static void free_str(const char *s)
{
	free((void *)(uintptr_t)s);
}

static char *dup_str(Parser *p, const char *s)
{
	size_t len = strlen(s) + 1;
	char *d = malloc(len);
	if (d == NULL) {
		p->no_memory = true;
		fail(p, "%s", "Out of memory");
		return NULL;
	}
	memcpy(d, s, len);
	return d;
}

/* Warn about every member of obj not named in known (NULL-terminated). */
static void unknown_members(const Parser *p, const cJSON *obj, const char *where,
			    const char *const *known)
{
	const cJSON *m;
	cJSON_ArrayForEach(m, obj)
	{
		bool found = false;
		for (const char *const *k = known; *k != NULL && !found; k++)
			found = strcmp(m->string, *k) == 0;
		if (!found)
			log_warn("%s: %s%sunknown member \"%s\" ignored", p->origin, where,
				 where[0] != '\0' ? ": " : "", m->string);
	}
}

/* The non-empty string member key of obj, copied; NULL (failed) if absent
 * or not one. */
static const char *name_member(Parser *p, const cJSON *obj, const char *key, size_t i)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (!cJSON_IsString(v) || v->valuestring[0] == '\0') {
		fail(p, "bindings[%zu]: %s: expected a non-empty string", i, key);
		return NULL;
	}
	return dup_str(p, v->valuestring);
}

static void parse_entry(Parser *p, const cJSON *e, size_t i, EnvBindingRequest *out)
{
	static const char *const known[] = {"block", "port", "env", "index", NULL};
	if (!cJSON_IsObject(e)) {
		fail(p, "bindings[%zu]: expected an object", i);
		return;
	}
	char where[32];
	snprintf(where, sizeof(where), "bindings[%zu]", i);
	unknown_members(p, e, where, known);
	out->block = name_member(p, e, "block", i);
	out->port = name_member(p, e, "port", i);
	out->env = name_member(p, e, "env", i);
	const cJSON *index = cJSON_GetObjectItemCaseSensitive(e, "index");
	if (index == NULL || cJSON_IsNull(index))
		return; /* no index (0) */
	double d = cJSON_IsNumber(index) ? index->valuedouble : NAN;
	if (!(d >= 1 && d <= UINT32_MAX && d == floor(d))) {
		fail(p, "bindings[%zu]: index: expected an integer of at least 1", i);
		return;
	}
	out->index = (uint32_t)d;
}

static bool only_whitespace(const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
			return false;
	}
	return true;
}

enum bindings_file_status bindings_file_parse(const char *text, size_t len, const char *origin,
					      BindingsFile *out, char *msg, size_t cap)
{
	static const char *const known[] = {"environment", "bindings", NULL};
	memset(out, 0, sizeof(*out));
	if (msg != NULL && cap > 0)
		msg[0] = '\0';
	Parser p = {origin, msg, cap, true, false};
	const char *end = NULL;
	cJSON *root = cJSON_ParseWithLengthOpts(text, len, &end, false);
	if (root != NULL && !only_whitespace(end, (size_t)(text + len - end))) {
		cJSON_Delete(root);
		root = NULL;
	}
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		fail(&p, "%s", "not a bindings file: expected a JSON object");
		return BINDINGS_FILE_MALFORMED;
	}
	unknown_members(&p, root, "", known);
	const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "environment");
	const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "bindings");
	if (!cJSON_IsString(name) || name->valuestring[0] == '\0')
		fail(&p, "%s", "environment: expected a non-empty string");
	else if (!cJSON_IsArray(list))
		fail(&p, "%s", "bindings: expected an array");
	else
		out->environment = dup_str(&p, name->valuestring);
	size_t n = p.ok ? (size_t)cJSON_GetArraySize(list) : 0;
	if (n > 0 && (out->entries = calloc(n, sizeof(*out->entries))) == NULL) {
		p.no_memory = true;
		fail(&p, "%s", "Out of memory");
	}
	const cJSON *e;
	if (p.ok) {
		cJSON_ArrayForEach(e, list)
		{
			/* Counted first: what an entry holds is freed with it. */
			out->count++;
			parse_entry(&p, e, out->count - 1, &out->entries[out->count - 1]);
		}
	}
	cJSON_Delete(root);
	if (p.ok)
		return BINDINGS_FILE_OK;
	bindings_file_free(out);
	return p.no_memory ? BINDINGS_FILE_NO_MEMORY : BINDINGS_FILE_MALFORMED;
}

void bindings_file_free(BindingsFile *f)
{
	free(f->environment);
	for (size_t i = 0; i < f->count; i++) {
		free_str(f->entries[i].block);
		free_str(f->entries[i].port);
		free_str(f->entries[i].env);
	}
	free(f->entries);
	memset(f, 0, sizeof(*f));
}
