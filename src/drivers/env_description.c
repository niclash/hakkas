/*
 * The environment's description (04 §1). See drivers/env_description.h.
 */
#include "drivers/env_description.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "system/log.h"

static bool fail(char *msg, size_t cap, const char *fmt, ...) LOG_PRINTF_FORMAT(3, 4);

static bool fail(char *msg, size_t cap, const char *fmt, ...)
{
	if (msg != NULL && cap > 0) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(msg, cap, fmt, ap);
		va_end(ap);
	}
	return false;
}

/* The length of s if it is at most max bytes, else max + 1 (never reading
 * past max + 1 bytes or the NUL). */
static size_t bounded_len(const char *s, size_t max)
{
	size_t n = 0;
	while (n <= max && s[n] != '\0')
		n++;
	return n;
}

/* Whether s is NULL or at most max bytes. */
static bool text_ok(const char *s, size_t max)
{
	return s == NULL || bounded_len(s, max) <= max;
}

/* Whether s is 1..max bytes. */
static bool id_ok(const char *s, size_t max)
{
	if (s == NULL)
		return false;
	size_t n = bounded_len(s, max);
	return n >= 1 && n <= max;
}

static bool port_ok(const HakkasEnvPort *p, const char *list, size_t i, char *msg, size_t cap)
{
	if (!id_ok(p->id, HAKKAS_ENV_ID_MAX))
		return fail(msg, cap, "%s[%zu]: id: expected 1 to %d bytes", list, i,
			    HAKKAS_ENV_ID_MAX);
	if (p->type != HAKKAS_ENV_NUMBER && p->type != HAKKAS_ENV_BOOL)
		return fail(msg, cap, "%s[%zu]: type: expected number or bool", list, i);
	if (!text_ok(p->description, HAKKAS_ENV_TEXT_MAX) || !text_ok(p->unit, HAKKAS_ENV_TEXT_MAX))
		return fail(msg, cap, "%s[%zu]: text longer than %d bytes", list, i,
			    HAKKAS_ENV_TEXT_MAX);
	if ((p->has_min && isnan(p->min)) || (p->has_max && isnan(p->max)) ||
	    (p->has_min && p->has_max && p->min > p->max))
		return fail(msg, cap, "%s[%zu]: min, max: expected min <= max", list, i);
	if (p->addressable && (p->count < 1 || p->count > HAKKAS_ENV_COUNT_MAX))
		return fail(msg, cap, "%s[%zu]: count: expected 1 to %d", list, i,
			    HAKKAS_ENV_COUNT_MAX);
	if (!p->addressable && p->count != 0)
		return fail(msg, cap, "%s[%zu]: count: only for an addressable port", list, i);
	return true;
}

/* The ports of d in one sequence: out_ports, then in_ports. */
static const HakkasEnvPort *port_at(const HakkasEnvDescription *d, size_t i)
{
	return i < d->out_port_count ? &d->out_ports[i] : &d->in_ports[i - d->out_port_count];
}

bool env_description_find(const HakkasEnvDescription *d, const char *id, bool *actuator,
			  uint32_t *index)
{
	if (id == NULL)
		return false;
	for (size_t i = 0; i < d->out_port_count; i++) {
		if (strcmp(d->out_ports[i].id, id) == 0) {
			*actuator = false;
			*index = (uint32_t)i;
			return true;
		}
	}
	for (size_t i = 0; i < d->in_port_count; i++) {
		if (strcmp(d->in_ports[i].id, id) == 0) {
			*actuator = true;
			*index = (uint32_t)i;
			return true;
		}
	}
	return false;
}

bool env_description_validate(const HakkasEnvDescription *d, char *msg, size_t cap)
{
	if (d == NULL)
		return fail(msg, cap, "no description");
	if (!id_ok(d->name, HAKKAS_ENV_ID_MAX))
		return fail(msg, cap, "name: expected 1 to %d bytes", HAKKAS_ENV_ID_MAX);
	if (!text_ok(d->description, HAKKAS_ENV_TEXT_MAX) || !text_ok(d->icon, HAKKAS_ENV_TEXT_MAX))
		return fail(msg, cap, "description, icon: at most %d bytes", HAKKAS_ENV_TEXT_MAX);
	if ((d->in_port_count > 0 && d->in_ports == NULL) ||
	    (d->out_port_count > 0 && d->out_ports == NULL) ||
	    (d->module_count > 0 && d->modules == NULL))
		return fail(msg, cap, "a list with a count but no entries");
	size_t total = d->in_port_count + d->out_port_count;
	if (d->in_port_count > HAKKAS_ENV_PORTS_MAX || total > HAKKAS_ENV_PORTS_MAX)
		return fail(msg, cap, "more than %d ports", HAKKAS_ENV_PORTS_MAX);
	for (size_t i = 0; i < d->out_port_count; i++) {
		if (!port_ok(&d->out_ports[i], "outPorts", i, msg, cap))
			return false;
	}
	for (size_t i = 0; i < d->in_port_count; i++) {
		if (!port_ok(&d->in_ports[i], "inPorts", i, msg, cap))
			return false;
	}
	for (size_t i = 0; i < total; i++) {
		for (size_t j = 0; j < i; j++) {
			if (strcmp(port_at(d, i)->id, port_at(d, j)->id) == 0)
				return fail(msg, cap, "duplicate port id \"%s\"",
					    port_at(d, i)->id);
		}
	}
	if (d->module_count > HAKKAS_ENV_MODULES_MAX)
		return fail(msg, cap, "more than %d modules", HAKKAS_ENV_MODULES_MAX);
	for (size_t i = 0; i < d->module_count; i++) {
		const HakkasEnvModule *m = &d->modules[i];
		if (!text_ok(m->slot, HAKKAS_ENV_TEXT_MAX) ||
		    !text_ok(m->type, HAKKAS_ENV_TEXT_MAX) ||
		    !text_ok(m->vendor, HAKKAS_ENV_TEXT_MAX) ||
		    !text_ok(m->revision, HAKKAS_ENV_TEXT_MAX) ||
		    !text_ok(m->serial, HAKKAS_ENV_TEXT_MAX) ||
		    !text_ok(m->documentation, HAKKAS_ENV_TEXT_MAX))
			return fail(msg, cap, "modules[%zu]: text longer than %d bytes", i,
				    HAKKAS_ENV_TEXT_MAX);
		if (m->port_count > 0 && m->ports == NULL)
			return fail(msg, cap, "modules[%zu]: ports: a count but no entries", i);
		for (size_t k = 0; k < m->port_count; k++) {
			bool actuator;
			uint32_t index;
			if (!env_description_find(d, m->ports[k], &actuator, &index))
				return fail(msg, cap, "modules[%zu]: ports[%zu]: no such port", i,
					    k);
		}
	}
	return true;
}

/* ---- Equality --------------------------------------------------------------------------- */

static bool str_eq(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return strcmp(a, b) == 0;
}

static bool port_eq(const HakkasEnvPort *a, const HakkasEnvPort *b)
{
	return str_eq(a->id, b->id) && a->type == b->type &&
	       str_eq(a->description, b->description) && str_eq(a->unit, b->unit) &&
	       a->has_min == b->has_min && a->has_max == b->has_max &&
	       (!a->has_min || a->min == b->min) && (!a->has_max || a->max == b->max) &&
	       a->addressable == b->addressable && a->count == b->count;
}

static bool ports_eq(const HakkasEnvPort *a, size_t na, const HakkasEnvPort *b, size_t nb)
{
	if (na != nb)
		return false;
	for (size_t i = 0; i < na; i++) {
		if (!port_eq(&a[i], &b[i]))
			return false;
	}
	return true;
}

static bool module_eq(const HakkasEnvModule *a, const HakkasEnvModule *b)
{
	if (!str_eq(a->slot, b->slot) || !str_eq(a->type, b->type) ||
	    !str_eq(a->vendor, b->vendor) || !str_eq(a->revision, b->revision) ||
	    !str_eq(a->serial, b->serial) || !str_eq(a->documentation, b->documentation) ||
	    a->port_count != b->port_count)
		return false;
	for (size_t k = 0; k < a->port_count; k++) {
		if (!str_eq(a->ports[k], b->ports[k]))
			return false;
	}
	return true;
}

bool env_description_equal(const HakkasEnvDescription *a, const HakkasEnvDescription *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	if (!str_eq(a->name, b->name) || !str_eq(a->description, b->description) ||
	    !str_eq(a->icon, b->icon) ||
	    !ports_eq(a->in_ports, a->in_port_count, b->in_ports, b->in_port_count) ||
	    !ports_eq(a->out_ports, a->out_port_count, b->out_ports, b->out_port_count) ||
	    a->module_count != b->module_count)
		return false;
	for (size_t i = 0; i < a->module_count; i++) {
		if (!module_eq(&a->modules[i], &b->modules[i]))
			return false;
	}
	return true;
}

/* ---- JSON ------------------------------------------------------------------------------- */

/* The description made from JSON, with the arrays it owns (the public type
 * has const pointers). */
typedef struct {
	HakkasEnvDescription d; /* first: the public pointer is this struct's */
	HakkasEnvPort *in, *out;
	HakkasEnvModule *modules;
} Owned;

static void free_str(const char *s)
{
	free((void *)(uintptr_t)s);
}

static void free_port(const HakkasEnvPort *p)
{
	free_str(p->id);
	free_str(p->description);
	free_str(p->unit);
}

void env_description_free(HakkasEnvDescription *d)
{
	if (d == NULL)
		return;
	Owned *o = (Owned *)(void *)d;
	free_str(d->name);
	free_str(d->description);
	free_str(d->icon);
	for (size_t i = 0; i < d->in_port_count; i++)
		free_port(&o->in[i]);
	for (size_t i = 0; i < d->out_port_count; i++)
		free_port(&o->out[i]);
	for (size_t i = 0; i < d->module_count; i++) {
		HakkasEnvModule *m = &o->modules[i];
		free_str(m->slot);
		free_str(m->type);
		free_str(m->vendor);
		free_str(m->revision);
		free_str(m->serial);
		free_str(m->documentation);
		for (size_t k = 0; k < m->port_count; k++)
			free_str(m->ports[k]);
		free((void *)(uintptr_t)m->ports);
	}
	free(o->in);
	free(o->out);
	free(o->modules);
	free(o);
}

/* Parsing state: the first error wins. */
typedef struct {
	char *msg;
	size_t cap;
	bool ok;
} Parse;

static void error(Parse *p, const char *fmt, ...) LOG_PRINTF_FORMAT(2, 3);

static void error(Parse *p, const char *fmt, ...)
{
	if (!p->ok)
		return;
	p->ok = false;
	if (p->msg != NULL && p->cap > 0) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(p->msg, p->cap, fmt, ap);
		va_end(ap);
	}
}

/* Member key of obj, NULL if absent or null. */
static const cJSON *member(const cJSON *obj, const char *key)
{
	const cJSON *m = cJSON_GetObjectItemCaseSensitive(obj, key);
	return cJSON_IsNull(m) ? NULL : m;
}

/* An optional string member, copied (NULL if absent). */
static const char *opt_str(Parse *p, const cJSON *obj, const char *key, const char *where)
{
	const cJSON *m = member(obj, key);
	if (m == NULL)
		return NULL;
	if (!cJSON_IsString(m)) {
		error(p, "%s%s: expected a string", where, key);
		return NULL;
	}
	size_t len = strlen(m->valuestring) + 1;
	char *s = malloc(len);
	if (s == NULL) {
		error(p, "out of memory");
		return NULL;
	}
	memcpy(s, m->valuestring, len);
	return s;
}

/* An array member's size (0 if absent); -1 (an error) if not an array. */
static int array_size(Parse *p, const cJSON *obj, const char *key)
{
	const cJSON *m = member(obj, key);
	if (m == NULL)
		return 0;
	if (!cJSON_IsArray(m)) {
		error(p, "%s: expected an array", key);
		return -1;
	}
	return cJSON_GetArraySize(m);
}

static void parse_port(Parse *p, const cJSON *j, const char *list, int i, HakkasEnvPort *out)
{
	char where[48];
	snprintf(where, sizeof(where), "%s[%d]: ", list, i);
	if (!cJSON_IsObject(j)) {
		error(p, "%sexpected an object", where);
		return;
	}
	out->id = opt_str(p, j, "id", where);
	if (out->id == NULL)
		error(p, "%sid: expected a string", where);
	out->description = opt_str(p, j, "description", where);
	out->unit = opt_str(p, j, "unit", where);
	const cJSON *type = member(j, "type");
	if (cJSON_IsString(type) && strcmp(type->valuestring, "number") == 0)
		out->type = HAKKAS_ENV_NUMBER;
	else if (cJSON_IsString(type) && strcmp(type->valuestring, "bool") == 0)
		out->type = HAKKAS_ENV_BOOL;
	else
		error(p, "%stype: expected \"number\" or \"bool\"", where);
	const cJSON *min = member(j, "min"), *max = member(j, "max");
	if ((min != NULL && !cJSON_IsNumber(min)) || (max != NULL && !cJSON_IsNumber(max)))
		error(p, "%smin, max: expected numbers", where);
	out->has_min = cJSON_IsNumber(min);
	out->min = out->has_min ? min->valuedouble : 0;
	out->has_max = cJSON_IsNumber(max);
	out->max = out->has_max ? max->valuedouble : 0;
	const cJSON *addr = member(j, "addressable");
	if (addr != NULL && !cJSON_IsBool(addr))
		error(p, "%saddressable: expected true or false", where);
	out->addressable = cJSON_IsTrue(addr);
	const cJSON *count = member(j, "count");
	if (count != NULL) {
		double c = cJSON_IsNumber(count) ? count->valuedouble : -1;
		if (c < 0 || c > UINT32_MAX || c != floor(c))
			error(p, "%scount: expected an integer", where);
		else
			out->count = (uint32_t)c;
	}
}

static void parse_module(Parse *p, const cJSON *j, int i, HakkasEnvModule *out)
{
	char where[48];
	snprintf(where, sizeof(where), "modules[%d]: ", i);
	if (!cJSON_IsObject(j)) {
		error(p, "%sexpected an object", where);
		return;
	}
	out->slot = opt_str(p, j, "slot", where);
	out->type = opt_str(p, j, "type", where);
	out->vendor = opt_str(p, j, "vendor", where);
	out->revision = opt_str(p, j, "revision", where);
	out->serial = opt_str(p, j, "serial", where);
	out->documentation = opt_str(p, j, "documentation", where);
	const cJSON *ports = member(j, "ports");
	if (ports == NULL)
		return;
	if (!cJSON_IsArray(ports)) {
		error(p, "%sports: expected an array of port ids", where);
		return;
	}
	int n = cJSON_GetArraySize(ports);
	if (n == 0)
		return;
	const char **ids = calloc((size_t)n, sizeof(*ids));
	if (ids == NULL) {
		error(p, "out of memory");
		return;
	}
	out->ports = ids;
	const cJSON *e;
	cJSON_ArrayForEach(e, ports)
	{
		if (!cJSON_IsString(e)) {
			error(p, "%sports: expected an array of port ids", where);
			return;
		}
		size_t len = strlen(e->valuestring) + 1;
		char *s = malloc(len);
		if (s == NULL) {
			error(p, "out of memory");
			return;
		}
		memcpy(s, e->valuestring, len);
		ids[out->port_count++] = s;
	}
}

/* The ports of list key into *out (count *n). */
static void parse_ports(Parse *p, const cJSON *json, const char *key, HakkasEnvPort **out,
			size_t *n)
{
	int size = array_size(p, json, key);
	if (size <= 0)
		return;
	if (size > HAKKAS_ENV_PORTS_MAX) {
		error(p, "more than %d ports", HAKKAS_ENV_PORTS_MAX);
		return;
	}
	*out = calloc((size_t)size, sizeof(**out));
	if (*out == NULL) {
		error(p, "out of memory");
		return;
	}
	const cJSON *e;
	int i = 0;
	cJSON_ArrayForEach(e, member(json, key))
	{
		(*n)++; /* counted before parsing: what it holds is freed */
		parse_port(p, e, key, i++, &(*out)[*n - 1]);
	}
}

HakkasEnvDescription *env_description_from_json(const cJSON *json, char *msg, size_t cap)
{
	Parse p = {msg, cap, true};
	if (!cJSON_IsObject(json)) {
		error(&p, "expected an object");
		return NULL;
	}
	Owned *o = calloc(1, sizeof(*o));
	if (o == NULL) {
		error(&p, "out of memory");
		return NULL;
	}
	HakkasEnvDescription *d = &o->d;
	d->name = opt_str(&p, json, "name", "");
	if (d->name == NULL)
		error(&p, "name: expected a string");
	d->description = opt_str(&p, json, "description", "");
	d->icon = opt_str(&p, json, "icon", "");
	parse_ports(&p, json, "inPorts", &o->in, &d->in_port_count);
	parse_ports(&p, json, "outPorts", &o->out, &d->out_port_count);
	d->in_ports = o->in;
	d->out_ports = o->out;
	int n = array_size(&p, json, "modules");
	if (n > HAKKAS_ENV_MODULES_MAX) {
		error(&p, "more than %d modules", HAKKAS_ENV_MODULES_MAX);
	} else if (n > 0) {
		o->modules = calloc((size_t)n, sizeof(*o->modules));
		if (o->modules == NULL) {
			error(&p, "out of memory");
		} else {
			const cJSON *e;
			cJSON_ArrayForEach(e, member(json, "modules"))
			{
				d->module_count++;
				parse_module(&p, e, (int)d->module_count - 1,
					     &o->modules[d->module_count - 1]);
			}
		}
	}
	d->modules = o->modules;
	if (p.ok)
		p.ok = env_description_validate(d, msg, cap);
	if (!p.ok) {
		env_description_free(d);
		return NULL;
	}
	return d;
}
