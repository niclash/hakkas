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

/* The carrier's I/O slots, built-in ones included. */
static size_t io_slots(const HakkasEnvCarrier *c)
{
	return c->builtin_count + c->slots;
}

static bool is_builtin_slot(const HakkasEnvCarrier *c, uint32_t slot)
{
	for (size_t i = 0; i < c->builtin_count; i++) {
		if (c->builtin[i] == slot)
			return true;
	}
	return false;
}

/* The carrier's structure (04 §1): its slots. Its view-only values are not
 * checked (decided with the user 2026-10-25). */
static bool carrier_ok(const HakkasEnvCarrier *c, char *msg, size_t cap)
{
	if ((c->builtin_count > 0 && c->builtin == NULL) || (c->comm_count > 0 && c->comm == NULL))
		return fail(msg, cap, "carrier: a list with a count but no entries");
	size_t n = io_slots(c);
	if (c->slots > HAKKAS_ENV_IO_SLOTS_MAX || c->builtin_count > HAKKAS_ENV_IO_SLOTS_MAX ||
	    n < 1 || n > HAKKAS_ENV_IO_SLOTS_MAX)
		return fail(msg, cap, "carrier: expected 1 to %d I/O slots, built-in ones included",
			    HAKKAS_ENV_IO_SLOTS_MAX);
	for (size_t i = 0; i < c->builtin_count; i++) {
		if (c->builtin[i] >= n)
			return fail(msg, cap, "carrier: builtin[%zu]: slot: expected 0 to %zu", i,
				    n - 1);
		for (size_t j = 0; j < i; j++) {
			if (c->builtin[j] == c->builtin[i])
				return fail(msg, cap, "carrier: builtin[%zu]: slot %u listed twice",
					    i, (unsigned)c->builtin[i]);
		}
	}
	if (c->comm_count > HAKKAS_ENV_COMM_SLOTS_MAX)
		return fail(msg, cap, "carrier: comm: more than %d communication slots",
			    HAKKAS_ENV_COMM_SLOTS_MAX);
	for (size_t i = 0; i < c->comm_count; i++) {
		uint32_t s = c->comm[i].slot;
		if (s < 1 || s > HAKKAS_ENV_COMM_SLOTS_MAX)
			return fail(msg, cap, "carrier: comm[%zu]: slot: expected 1 to %d", i,
				    HAKKAS_ENV_COMM_SLOTS_MAX);
		for (size_t j = 0; j < i; j++) {
			if (c->comm[j].slot == s)
				return fail(msg, cap, "carrier: comm[%zu]: slot %u listed twice", i,
					    (unsigned)s);
		}
	}
	return true;
}

/* Module i's slot (04 §1): with a carrier one of its I/O slots, built-in
 * exactly when the carrier lists that slot as built-in; no slot twice. */
static bool module_slot_ok(const HakkasEnvDescription *d, size_t i, char *msg, size_t cap)
{
	const HakkasEnvModule *m = &d->modules[i];
	const HakkasEnvCarrier *c = d->carrier;
	if (c == NULL) {
		if (m->builtin)
			return fail(msg, cap, "modules[%zu]: builtin: no carrier", i);
	} else {
		if (!m->has_slot || m->slot >= io_slots(c))
			return fail(msg, cap, "modules[%zu]: slot: expected 0 to %zu", i,
				    io_slots(c) - 1);
		bool builtin = is_builtin_slot(c, m->slot);
		if (m->builtin != builtin)
			return fail(msg, cap, "modules[%zu]: builtin: slot %u is %sa built-in slot",
				    i, (unsigned)m->slot, builtin ? "" : "not ");
	}
	for (size_t j = 0; m->has_slot && j < i; j++) {
		if (d->modules[j].has_slot && d->modules[j].slot == m->slot)
			return fail(msg, cap, "modules[%zu]: slot %u taken twice", i,
				    (unsigned)m->slot);
	}
	return true;
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
	if (d->carrier != NULL && !carrier_ok(d->carrier, msg, cap))
		return false;
	for (size_t i = 0; i < d->module_count; i++) {
		const HakkasEnvModule *m = &d->modules[i];
		if (!module_slot_ok(d, i, msg, cap))
			return false;
		if (!text_ok(m->type, HAKKAS_ENV_TEXT_MAX) ||
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
	if (a->has_slot != b->has_slot || (a->has_slot && a->slot != b->slot) ||
	    a->builtin != b->builtin || !str_eq(a->type, b->type) ||
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

static bool power_eq(const HakkasEnvPower *a, const HakkasEnvPower *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return a->has_voltage_min == b->has_voltage_min &&
	       (!a->has_voltage_min || a->voltage_min == b->voltage_min) &&
	       a->has_voltage_max == b->has_voltage_max &&
	       (!a->has_voltage_max || a->voltage_max == b->voltage_max) && a->ac == b->ac &&
	       a->dc == b->dc && a->has_current == b->has_current &&
	       (!a->has_current || a->current == b->current);
}

/* The communication slots' parameters are left out: they are live values
 * (a DHCP lease changes the address) and view only (decided with the user
 * 2026-10-25, 04 §0.1). */
static bool carrier_eq(const HakkasEnvCarrier *a, const HakkasEnvCarrier *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	if (!str_eq(a->type, b->type) || !str_eq(a->vendor, b->vendor) ||
	    !str_eq(a->revision, b->revision) || !str_eq(a->serial, b->serial) ||
	    !str_eq(a->documentation, b->documentation) || a->slots != b->slots ||
	    a->builtin_count != b->builtin_count || !power_eq(a->power, b->power) ||
	    a->comm_count != b->comm_count)
		return false;
	for (size_t i = 0; i < a->builtin_count; i++) {
		if (a->builtin[i] != b->builtin[i])
			return false;
	}
	for (size_t i = 0; i < a->comm_count; i++) {
		if (a->comm[i].slot != b->comm[i].slot || !str_eq(a->comm[i].type, b->comm[i].type))
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
	    a->module_count != b->module_count || !carrier_eq(a->carrier, b->carrier))
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
	HakkasEnvCarrier carrier;
	uint32_t *builtin;
	HakkasEnvPower power;
	HakkasEnvComm *comm;
	/* What comm[i]'s parameter pointers point at. */
	struct {
		HakkasEnvIp ip;
		HakkasEnvRs485 rs485;
		HakkasEnvLoRaWan lorawan;
	} *params;
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
		free_str(m->type);
		free_str(m->vendor);
		free_str(m->revision);
		free_str(m->serial);
		free_str(m->documentation);
		for (size_t k = 0; k < m->port_count; k++)
			free_str(m->ports[k]);
		free((void *)(uintptr_t)m->ports);
	}
	free_str(o->carrier.type);
	free_str(o->carrier.vendor);
	free_str(o->carrier.revision);
	free_str(o->carrier.serial);
	free_str(o->carrier.documentation);
	for (size_t i = 0; i < o->carrier.comm_count; i++) {
		free_str(o->comm[i].type);
		free_str(o->comm[i].ssid);
		const HakkasEnvIp *ip = &o->params[i].ip;
		free_str(ip->mode);
		free_str(ip->address);
		free_str(ip->gateway);
		free_str(ip->mac);
		for (size_t k = 0; k < ip->dns_count; k++)
			free_str(ip->dns[k]);
		free((void *)(uintptr_t)ip->dns);
		free_str(o->params[i].rs485.parity);
		free_str(o->params[i].lorawan.app_eui);
		free_str(o->params[i].lorawan.dev_eui);
	}
	free(o->builtin);
	free(o->comm);
	free(o->params);
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

/* A copy of s (NULL, an error, if memory runs out). */
static const char *copy_str(Parse *p, const char *s)
{
	size_t len = strlen(s) + 1;
	char *c = malloc(len);
	if (c == NULL) {
		error(p, "out of memory");
		return NULL;
	}
	memcpy(c, s, len);
	return c;
}

/* A view-only member (04 §1): taken if it has its type, else absent — never
 * an error (decided with the user 2026-10-25). */
static const char *view_str(Parse *p, const cJSON *obj, const char *key)
{
	const cJSON *m = member(obj, key);
	return cJSON_IsString(m) ? copy_str(p, m->valuestring) : NULL;
}

static bool view_num(const cJSON *obj, const char *key, double *out)
{
	const cJSON *m = member(obj, key);
	if (!cJSON_IsNumber(m))
		return false;
	*out = m->valuedouble;
	return true;
}

static bool view_uint(const cJSON *obj, const char *key, uint32_t *out)
{
	double v;
	if (!view_num(obj, key, &v) || v < 0 || v > UINT32_MAX || v != floor(v))
		return false;
	*out = (uint32_t)v;
	return true;
}

static bool view_bool(const cJSON *obj, const char *key, bool *out)
{
	const cJSON *m = member(obj, key);
	if (!cJSON_IsBool(m))
		return false;
	*out = cJSON_IsTrue(m);
	return true;
}

/* A structural integer member, 0..UINT32_MAX: false if absent; an error if
 * it is not such an integer. */
static bool opt_uint(Parse *p, const cJSON *obj, const char *key, const char *where,
		     uint32_t *out)
{
	const cJSON *m = member(obj, key);
	if (m == NULL)
		return false;
	if (!view_uint(obj, key, out)) {
		error(p, "%s%s: expected an integer of at least 0", where, key);
		return false;
	}
	return true;
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
	out->has_slot = opt_uint(p, j, "slot", where, &out->slot);
	const cJSON *builtin = member(j, "builtin");
	if (builtin != NULL && !cJSON_IsBool(builtin))
		error(p, "%sbuiltin: expected true or false", where);
	out->builtin = cJSON_IsTrue(builtin);
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

static void parse_ip(Parse *p, const cJSON *j, HakkasEnvIp *ip)
{
	ip->mode = view_str(p, j, "mode");
	ip->address = view_str(p, j, "address");
	ip->has_prefix = view_uint(j, "prefix", &ip->prefix);
	ip->gateway = view_str(p, j, "gateway");
	ip->mac = view_str(p, j, "mac");
	const cJSON *dns = member(j, "dns");
	int n = cJSON_IsArray(dns) ? cJSON_GetArraySize(dns) : 0;
	if (n == 0)
		return;
	const char **list = calloc((size_t)n, sizeof(*list));
	if (list == NULL) {
		error(p, "out of memory");
		return;
	}
	ip->dns = list;
	const cJSON *e;
	cJSON_ArrayForEach(e, dns)
	{
		if (!cJSON_IsString(e))
			continue;
		list[ip->dns_count] = copy_str(p, e->valuestring);
		if (list[ip->dns_count] != NULL)
			ip->dns_count++;
	}
}

/* Communication slot i (04 §1): its slot number is structure, the rest is
 * view only. */
static void parse_comm(Parse *p, Owned *o, const cJSON *j, int i)
{
	char where[48];
	snprintf(where, sizeof(where), "carrier: comm[%d]: ", i);
	HakkasEnvComm *c = &o->comm[i];
	if (!cJSON_IsObject(j)) {
		error(p, "%sexpected an object", where);
		return;
	}
	if (!opt_uint(p, j, "slot", where, &c->slot))
		error(p, "%sslot: expected an integer of at least 0", where);
	c->type = view_str(p, j, "type");
	const cJSON *ip = member(j, "ip");
	if (cJSON_IsObject(ip)) {
		parse_ip(p, ip, &o->params[i].ip);
		c->ip = &o->params[i].ip;
	}
	const cJSON *wifi = member(j, "wifi");
	if (cJSON_IsObject(wifi))
		c->ssid = view_str(p, wifi, "ssid");
	const cJSON *rs485 = member(j, "rs485");
	if (cJSON_IsObject(rs485)) {
		HakkasEnvRs485 *r = &o->params[i].rs485;
		r->has_bitrate = view_uint(rs485, "bitrate", &r->bitrate);
		r->parity = view_str(p, rs485, "parity");
		c->rs485 = r;
	}
	const cJSON *lora = member(j, "lorawan");
	if (cJSON_IsObject(lora)) {
		HakkasEnvLoRaWan *l = &o->params[i].lorawan;
		l->app_eui = view_str(p, lora, "appEui");
		l->dev_eui = view_str(p, lora, "devEui");
		l->has_app_key_set = view_bool(lora, "appKeySet", &l->app_key_set);
		c->lorawan = l;
	}
}

static void parse_carrier(Parse *p, Owned *o, const cJSON *j)
{
	HakkasEnvCarrier *c = &o->carrier;
	if (!cJSON_IsObject(j)) {
		error(p, "carrier: expected an object");
		return;
	}
	o->d.carrier = c;
	c->type = view_str(p, j, "type");
	c->vendor = view_str(p, j, "vendor");
	c->revision = view_str(p, j, "revision");
	c->serial = view_str(p, j, "serial");
	c->documentation = view_str(p, j, "documentation");
	if (!opt_uint(p, j, "slots", "carrier: ", &c->slots))
		error(p, "carrier: slots: expected an integer of at least 0");
	const cJSON *power = member(j, "power");
	if (cJSON_IsObject(power)) {
		HakkasEnvPower *w = &o->power;
		const cJSON *v = member(power, "voltage");
		w->has_voltage_min = view_num(v, "min", &w->voltage_min);
		w->has_voltage_max = view_num(v, "max", &w->voltage_max);
		view_bool(power, "ac", &w->ac);
		view_bool(power, "dc", &w->dc);
		w->has_current = view_num(power, "current", &w->current);
		c->power = w;
	}
	const cJSON *list = member(j, "builtin");
	int n = cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0;
	if (list != NULL && !cJSON_IsArray(list)) {
		error(p, "carrier: builtin: expected an array");
	} else if (n > HAKKAS_ENV_IO_SLOTS_MAX) {
		error(p, "carrier: expected 1 to %d I/O slots, built-in ones included",
		      HAKKAS_ENV_IO_SLOTS_MAX);
	} else if (n > 0) {
		o->builtin = calloc((size_t)n, sizeof(*o->builtin));
		if (o->builtin == NULL) {
			error(p, "out of memory");
			return;
		}
		c->builtin = o->builtin;
		const cJSON *e;
		cJSON_ArrayForEach(e, list)
		{
			char where[48];
			snprintf(where, sizeof(where), "carrier: builtin[%zu]: ", c->builtin_count);
			if (!cJSON_IsObject(e) ||
			    !opt_uint(p, e, "slot", where, &o->builtin[c->builtin_count]))
				error(p, "%sslot: expected an integer of at least 0", where);
			c->builtin_count++;
		}
	}
	list = member(j, "comm");
	n = cJSON_IsArray(list) ? cJSON_GetArraySize(list) : 0;
	if (list != NULL && !cJSON_IsArray(list)) {
		error(p, "carrier: comm: expected an array");
	} else if (n > HAKKAS_ENV_COMM_SLOTS_MAX) {
		error(p, "carrier: comm: more than %d communication slots",
		      HAKKAS_ENV_COMM_SLOTS_MAX);
	} else if (n > 0) {
		o->comm = calloc((size_t)n, sizeof(*o->comm));
		o->params = calloc((size_t)n, sizeof(*o->params));
		if (o->comm == NULL || o->params == NULL) {
			error(p, "out of memory");
			return;
		}
		c->comm = o->comm;
		const cJSON *e;
		cJSON_ArrayForEach(e, list)
		{
			c->comm_count++; /* counted before parsing: what it holds is freed */
			parse_comm(p, o, e, (int)c->comm_count - 1);
		}
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
	const cJSON *carrier = member(json, "carrier");
	if (carrier != NULL)
		parse_carrier(&p, o, carrier);
	if (p.ok)
		p.ok = env_description_validate(d, msg, cap);
	if (!p.ok) {
		env_description_free(d);
		return NULL;
	}
	return d;
}
