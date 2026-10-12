/*
 * Composite block types (03 §5). See blocktypes/composite_types.h.
 */
#include "blocktypes/composite_types.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blocktypes/block_types.h"
#include "graphs/graph_json.h"
#include "system/log.h"

/* What a port entry gave explicitly (03 §5.1: type and description are
 * optional, taken from the inner port otherwise), kept for the canonical
 * document. */
typedef struct {
	char *type;	   /* owned, or NULL */
	char *description; /* owned, or NULL */
	int addressable;   /* 0 or 1, or -1 if not given */
} PortGiven;

/* One registered composite type and everything it owns. def must stay the
 * first member: the registry and every instance refer to &def. */
typedef struct {
	BlockTypeDef def; /* name, icon, description owned */
	CompositeDef cdef;
	PortDescriptor *ports; /* inputs, then outputs; every string owned */
	PortGiven *given;      /* one per port */
	CompositeRef *refs;    /* cdef.inports, then cdef.outports */
	CompositeBlockDef *blocks; /* names owned */
	CompositeEdge *edges;
	CompositeInitial *initials; /* values owned */
} CompositeType;

static CompositeType *owned[HAKKAS_BLOCK_TYPES_MAX];
static size_t owned_count;

/* ---- Helpers ------------------------------------------------------------------ */

static char *dup_str(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);
	if (d != NULL)
		memcpy(d, s, n);
	return d;
}

static enum composite_status fail(CompositeMessage *msg, enum composite_status st,
				  const char *fmt, ...) LOG_PRINTF_FORMAT(3, 4);

static enum composite_status fail(CompositeMessage *msg, enum composite_status st,
				  const char *fmt, ...)
{
	if (msg != NULL) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(msg->text, sizeof(msg->text), fmt, ap);
		va_end(ap);
	}
	return st;
}

static enum composite_status no_memory(CompositeMessage *msg)
{
	return fail(msg, COMPOSITE_ERR_NO_MEMORY, "Out of memory");
}

static void ok(CompositeMessage *msg)
{
	if (msg != NULL)
		msg->text[0] = '\0';
}

static const cJSON *member(const cJSON *obj, const char *key)
{
	return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static const char *string_of(const cJSON *obj, const char *key)
{
	const cJSON *v = member(obj, key);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool absent_or_string(const cJSON *obj, const char *key)
{
	const cJSON *v = member(obj, key);
	return v == NULL || cJSON_IsString(v);
}

static uint32_t array_size(const cJSON *obj, const char *key)
{
	const cJSON *v = member(obj, key);
	return cJSON_IsArray(v) ? (uint32_t)cJSON_GetArraySize(v) : 0;
}

/* A "Block.port" reference (01 §2.4): a string that splits on its last '.'
 * into a block name of 1..64 bytes and a non-empty port name. */
static bool ref_valid(const char *ref)
{
	size_t block_len;
	const char *port;
	return ref != NULL && port_ref_split(ref, &block_len, &port) &&
	       block_len <= GRAPH_ID_MAX;
}

/* A public port name: 1..64 bytes, no '.' (a parent composite could not
 * reference it, 01 §2.4). */
static bool port_name_valid(const char *name)
{
	return graph_id_valid(name) && strchr(name, '.') == NULL;
}

static bool port_type_valid(const char *type)
{
	return strcmp(type, "bool") == 0 || strcmp(type, "number") == 0 ||
	       strcmp(type, "string") == 0;
}

/* Whether string member key of the entries of arr before entry is the same
 * as entry's. */
static bool duplicate_before(const cJSON *arr, const cJSON *entry, const char *key)
{
	const char *name = string_of(entry, key);
	for (const cJSON *e = arr->child; e != entry; e = e->next) {
		if (strcmp(string_of(e, key), name) == 0)
			return true;
	}
	return false;
}

/* "Block.port" as a heap string. */
static char *ref_text(const char *block, const char *port)
{
	size_t a = strlen(block), b = strlen(port);
	char *s = malloc(a + b + 2);
	if (s != NULL) {
		memcpy(s, block, a);
		s[a] = '.';
		memcpy(s + a + 1, port, b + 1);
	}
	return s;
}

/* ---- Freeing ------------------------------------------------------------------ */

static void type_free(CompositeType *t)
{
	if (t == NULL)
		return;
	free((char *)t->def.name);
	free((char *)t->def.icon);
	free((char *)t->def.description);
	uint32_t nports = t->def.input_count + t->def.output_count;
	for (uint32_t i = 0; t->ports != NULL && i < nports; i++) {
		PortDescriptor *p = &t->ports[i];
		free((char *)p->name);
		free((char *)p->description);
		free((char *)p->type);
		free((char *)p->inner);
		linkable_clear(&p->default_value);
	}
	for (uint32_t i = 0; t->given != NULL && i < nports; i++) {
		free(t->given[i].type);
		free(t->given[i].description);
	}
	free(t->ports);
	free(t->given);
	free(t->refs);
	for (uint32_t j = 0; t->blocks != NULL && j < t->cdef.block_count; j++)
		free((char *)t->blocks[j].name);
	free(t->blocks);
	free(t->edges);
	for (uint32_t k = 0; t->initials != NULL && k < t->cdef.initial_count; k++)
		linkable_clear(&t->initials[k].value);
	free(t->initials);
	free(t);
}

/* ---- Shape (check 1) ---------------------------------------------------------- */

/* Checks entry e of array arr. */
typedef bool (*entry_check_fn)(const cJSON *arr, const cJSON *e);

/* Each entry of array key is an object whose members pass check. */
static bool entries_valid(const cJSON *doc, const char *key, entry_check_fn check)
{
	const cJSON *arr = member(doc, key);
	if (arr == NULL)
		return true;
	if (!cJSON_IsArray(arr))
		return false;
	for (const cJSON *e = arr->child; e != NULL; e = e->next) {
		if (!cJSON_IsObject(e) || !check(arr, e))
			return false;
	}
	return true;
}

static bool block_entry_valid(const cJSON *arr, const cJSON *e)
{
	return graph_id_valid(string_of(e, "name")) && string_of(e, "type") != NULL &&
	       !duplicate_before(arr, e, "name");
}

static bool edge_entry_valid(const cJSON *arr, const cJSON *e)
{
	(void)arr;
	return ref_valid(string_of(e, "src")) && ref_valid(string_of(e, "tgt"));
}

static bool initial_entry_valid(const cJSON *arr, const cJSON *e)
{
	(void)arr;
	const cJSON *src = member(e, "src");
	return (cJSON_IsString(src) || cJSON_IsNumber(src) || cJSON_IsBool(src)) &&
	       ref_valid(string_of(e, "tgt"));
}

static bool port_entry_valid(const cJSON *arr, const cJSON *e, const char *ref_key)
{
	const char *type = string_of(e, "type");
	const cJSON *addr = member(e, "addressable");
	return port_name_valid(string_of(e, "name")) && !duplicate_before(arr, e, "name") &&
	       ref_valid(string_of(e, ref_key)) && absent_or_string(e, "type") &&
	       (type == NULL || port_type_valid(type)) && absent_or_string(e, "description") &&
	       (addr == NULL || cJSON_IsBool(addr));
}

static bool inport_entry_valid(const cJSON *arr, const cJSON *e)
{
	return port_entry_valid(arr, e, "tgt");
}

static bool outport_entry_valid(const cJSON *arr, const cJSON *e)
{
	return port_entry_valid(arr, e, "src");
}

static enum composite_status check_shape(const cJSON *doc, CompositeMessage *msg)
{
	const char *bad = NULL;
	if (!graph_id_valid(string_of(doc, "name")))
		bad = "name";
	else if (!absent_or_string(doc, "icon"))
		bad = "icon";
	else if (!absent_or_string(doc, "description"))
		bad = "description";
	else if (!cJSON_IsArray(member(doc, "blocks")) ||
		 !entries_valid(doc, "blocks", block_entry_valid))
		bad = "blocks";
	else if (!entries_valid(doc, "edges", edge_entry_valid))
		bad = "edges";
	else if (!entries_valid(doc, "initials", initial_entry_valid))
		bad = "initials";
	else if (!entries_valid(doc, "inports", inport_entry_valid))
		bad = "inports";
	else if (!entries_valid(doc, "outports", outport_entry_valid))
		bad = "outports";
	if (bad != NULL)
		return fail(msg, COMPOSITE_ERR_INVALID, "Invalid field: %s", bad);
	return COMPOSITE_OK;
}

/* ---- Building (checks 2-5) ---------------------------------------------------- */

typedef struct {
	CompositeType *t;
	CompositeMessage *msg;
	bool **fed; /* per inner block, per input: fed by an edge, initial or inport */
} Build;

/* Whether type t is name or contains a composite that is (03 §5.1: no
 * recursion, directly or through another composite). */
static bool contains(const BlockTypeDef *t, const char *name)
{
	if (strcmp(t->name, name) == 0)
		return true;
	if (!t->subgraph)
		return false;
	for (uint32_t j = 0; j < t->composite->block_count; j++) {
		if (contains(t->composite->blocks[j].type, name))
			return true;
	}
	return false;
}

static int32_t inner_index(const CompositeType *t, const char *name, size_t len)
{
	for (uint32_t j = 0; j < t->cdef.block_count; j++) {
		const char *n = t->blocks[j].name;
		if (strncmp(n, name, len) == 0 && n[len] == '\0')
			return (int32_t)j;
	}
	return -1;
}

static int32_t port_index(const PortDescriptor *ports, uint32_t count, const char *name)
{
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(ports[i].name, name) == 0)
			return (int32_t)i;
	}
	return -1;
}

/* Resolve ref to an inner input (input) or output. A port of the other
 * direction is "Invalid edge direction" if edge_direction, else "Unknown
 * port". */
static enum composite_status resolve(Build *bd, const char *ref, bool input, bool edge_direction,
				     CompositeRef *out)
{
	size_t len;
	const char *port;
	port_ref_split(ref, &len, &port); /* checked in check_shape */
	int32_t j = inner_index(bd->t, ref, len);
	if (j < 0)
		return fail(bd->msg, COMPOSITE_ERR_UNKNOWN_PORT, "Unknown node: %.*s", (int)len,
			    ref);
	const BlockTypeDef *type = bd->t->blocks[j].type;
	int32_t in = port_index(type->inputs, type->input_count, port);
	int32_t outp = port_index(type->outputs, type->output_count, port);
	int32_t i = input ? in : outp;
	if (i < 0) {
		if (edge_direction && (input ? outp : in) >= 0)
			return fail(bd->msg, COMPOSITE_ERR_UNKNOWN_PORT, "Invalid edge direction");
		return fail(bd->msg, COMPOSITE_ERR_UNKNOWN_PORT, "Unknown port: %s", ref);
	}
	*out = (CompositeRef){(uint32_t)j, (uint32_t)i};
	return COMPOSITE_OK;
}

/* Mark the inner input r fed; FAN_IN if it already is (01 §2.2). */
static enum composite_status feed(Build *bd, CompositeRef r)
{
	if (bd->fed[r.block][r.port]) {
		const CompositeBlockDef *b = &bd->t->blocks[r.block];
		return fail(bd->msg, COMPOSITE_ERR_FAN_IN, "Input already connected: %s.%s",
			    b->name, b->type->inputs[r.port].name);
	}
	bd->fed[r.block][r.port] = true;
	return COMPOSITE_OK;
}

static enum composite_status build_blocks(Build *bd, const cJSON *doc)
{
	CompositeType *t = bd->t;
	const char *name = t->def.name;
	uint32_t j = 0;
	const cJSON *e;
	cJSON_ArrayForEach(e, member(doc, "blocks"))
	{
		const char *type_name = string_of(e, "type");
		const BlockTypeDef *type = block_types_get(type_name);
		if (strcmp(type_name, name) == 0 || (type != NULL && contains(type, name)))
			return fail(bd->msg, COMPOSITE_ERR_RECURSIVE, "Recursive component: %s",
				    name);
		if (type == NULL)
			return fail(bd->msg, COMPOSITE_ERR_UNKNOWN_TYPE, "Unknown component: %s",
				    type_name);
		t->blocks[j].type = type;
		t->blocks[j].name = dup_str(string_of(e, "name"));
		bd->fed[j] = type->input_count > 0 ? calloc(type->input_count, sizeof(bool))
						   : NULL;
		if (t->blocks[j].name == NULL || (type->input_count > 0 && bd->fed[j] == NULL))
			return no_memory(bd->msg);
		j++;
	}
	return COMPOSITE_OK;
}

static enum composite_status build_edges(Build *bd, const cJSON *doc)
{
	uint32_t k = 0;
	const cJSON *e;
	cJSON_ArrayForEach(e, member(doc, "edges"))
	{
		CompositeEdge *edge = &bd->t->edges[k++];
		enum composite_status st =
			resolve(bd, string_of(e, "src"), false, true, &edge->src);
		if (st == COMPOSITE_OK)
			st = resolve(bd, string_of(e, "tgt"), true, true, &edge->tgt);
		if (st == COMPOSITE_OK)
			st = feed(bd, edge->tgt);
		if (st != COMPOSITE_OK)
			return st;
	}
	return COMPOSITE_OK;
}

static enum composite_status build_initials(Build *bd, const cJSON *doc)
{
	uint32_t k = 0;
	const cJSON *e;
	cJSON_ArrayForEach(e, member(doc, "initials"))
	{
		CompositeInitial *init = &bd->t->initials[k++];
		enum composite_status st = resolve(bd, string_of(e, "tgt"), true, true, &init->tgt);
		if (st == COMPOSITE_OK)
			st = feed(bd, init->tgt);
		if (st != COMPOSITE_OK)
			return st;
		const PortDescriptor *p =
			&bd->t->blocks[init->tgt.block].type->inputs[init->tgt.port];
		if (graph_json_to_initial(member(e, "src"), p->type, &init->value) != GRAPH_OK)
			return no_memory(bd->msg); /* the shape was checked */
	}
	return COMPOSITE_OK;
}

/* The effective default of inner port p (01 §2.1: its default, or
 * default_value(type) if it has none). */
static Linkable effective_default(const PortDescriptor *p)
{
	if (p->default_value.type == LINKABLE_UNSET)
		return linkable_default_value(p->type);
	return linkable_copy(&p->default_value);
}

/* The exposed ports of one direction: descriptor i of the type (index base +
 * i in ports and given) from entry i of array key. */
static enum composite_status build_ports(Build *bd, const cJSON *doc, bool input)
{
	CompositeType *t = bd->t;
	const char *key = input ? "inports" : "outports";
	uint32_t base = input ? 0 : t->def.input_count;
	uint32_t i = 0;
	const cJSON *e;
	cJSON_ArrayForEach(e, member(doc, key))
	{
		CompositeRef *r = &t->refs[base + i];
		enum composite_status st =
			resolve(bd, string_of(e, input ? "tgt" : "src"), input, false, r);
		if (st == COMPOSITE_OK && input)
			st = feed(bd, *r);
		if (st != COMPOSITE_OK)
			return st;
		const CompositeBlockDef *b = &t->blocks[r->block];
		const PortDescriptor *ip = input ? &b->type->inputs[r->port]
						 : &b->type->outputs[r->port];
		PortDescriptor *p = &t->ports[base + i];
		PortGiven *g = &t->given[base + i];
		const char *type = string_of(e, "type");
		const char *desc = string_of(e, "description");
		const cJSON *addr = member(e, "addressable");
		g->addressable = addr != NULL ? cJSON_IsTrue(addr) : -1;
		g->type = type != NULL ? dup_str(type) : NULL;
		g->description = desc != NULL ? dup_str(desc) : NULL;
		p->name = dup_str(string_of(e, "name"));
		const char *inner_type = ip->type != NULL ? ip->type : "number";
		const char *inner_desc = ip->description != NULL ? ip->description : "";
		p->type = dup_str(type != NULL ? type : inner_type);
		p->description = dup_str(desc != NULL ? desc : inner_desc);
		p->addressable = addr != NULL ? cJSON_IsTrue(addr) : ip->addressable;
		p->inner = ref_text(b->name, ip->name);
		p->default_value = effective_default(ip);
		if ((type != NULL && g->type == NULL) || (desc != NULL && g->description == NULL) ||
		    p->name == NULL || p->type == NULL || p->description == NULL ||
		    p->inner == NULL)
			return no_memory(bd->msg);
		i++;
	}
	return COMPOSITE_OK;
}

/* Allocate t's arrays for doc's entry counts. */
static bool allocate(CompositeType *t, const cJSON *doc)
{
	uint32_t nb = array_size(doc, "blocks");
	uint32_t ne = array_size(doc, "edges");
	uint32_t ni = array_size(doc, "initials");
	uint32_t nin = array_size(doc, "inports");
	uint32_t nout = array_size(doc, "outports");
	uint32_t np = nin + nout;
	/* calloc(0) may return NULL: allocate at least one of each. */
	t->blocks = calloc(nb > 0 ? nb : 1, sizeof(*t->blocks));
	t->edges = calloc(ne > 0 ? ne : 1, sizeof(*t->edges));
	t->initials = calloc(ni > 0 ? ni : 1, sizeof(*t->initials));
	t->ports = calloc(np > 0 ? np : 1, sizeof(*t->ports));
	t->given = calloc(np > 0 ? np : 1, sizeof(*t->given));
	t->refs = calloc(np > 0 ? np : 1, sizeof(*t->refs));
	t->cdef.block_count = nb;
	t->cdef.edge_count = ne;
	t->cdef.initial_count = ni;
	t->def.input_count = nin;
	t->def.output_count = nout;
	return t->blocks != NULL && t->edges != NULL && t->initials != NULL && t->ports != NULL &&
	       t->given != NULL && t->refs != NULL;
}

static enum composite_status build(CompositeType *t, const cJSON *doc, CompositeMessage *msg)
{
	const char *icon = string_of(doc, "icon");
	const char *desc = string_of(doc, "description");
	t->def.name = dup_str(string_of(doc, "name"));
	t->def.icon = dup_str(icon != NULL ? icon : "");
	t->def.description = dup_str(desc != NULL ? desc : "");
	if (!allocate(t, doc) || t->def.name == NULL || t->def.icon == NULL ||
	    t->def.description == NULL)
		return no_memory(msg);

	Build bd = {t, msg, calloc(t->cdef.block_count > 0 ? t->cdef.block_count : 1,
				   sizeof(bool *))};
	if (bd.fed == NULL)
		return no_memory(msg);
	enum composite_status st = build_blocks(&bd, doc);
	if (st == COMPOSITE_OK)
		st = build_edges(&bd, doc);
	if (st == COMPOSITE_OK)
		st = build_initials(&bd, doc);
	if (st == COMPOSITE_OK)
		st = build_ports(&bd, doc, true);
	if (st == COMPOSITE_OK)
		st = build_ports(&bd, doc, false);
	for (uint32_t j = 0; j < t->cdef.block_count; j++)
		free(bd.fed[j]);
	free(bd.fed);
	if (st != COMPOSITE_OK)
		return st;

	t->def.inputs = t->ports;
	t->def.outputs = t->ports + t->def.input_count;
	t->def.compute_on_input = false;
	t->def.timer = TIMER_NONE;
	t->def.compute = NULL; /* the inner blocks compute (03 §5) */
	t->def.state_size = 0;
	t->def.change_threshold = (double)NAN; /* the inner types' defaults apply */
	t->def.subgraph = true;
	t->def.composite = &t->cdef;
	t->cdef.blocks = t->blocks;
	t->cdef.edges = t->edges;
	t->cdef.initials = t->initials;
	t->cdef.inports = t->refs;
	t->cdef.outports = t->refs + t->def.input_count;
	return COMPOSITE_OK;
}

/* ---- Registration ------------------------------------------------------------- */

enum composite_status composite_types_register(const cJSON *doc, CompositeMessage *msg,
					       const BlockTypeDef **out)
{
	if (out != NULL)
		*out = NULL;
	if (!cJSON_IsObject(doc))
		return fail(msg, COMPOSITE_ERR_INVALID, "Invalid JSON");
	enum composite_status st = check_shape(doc, msg);
	if (st != COMPOSITE_OK)
		return st;
	const char *name = string_of(doc, "name");
	if (block_types_get(name) != NULL)
		return fail(msg, COMPOSITE_ERR_EXISTS, "Component exists: %s", name);
	CompositeType *t = calloc(1, sizeof(*t));
	if (t == NULL)
		return no_memory(msg);
	st = build(t, doc, msg);
	if (st == COMPOSITE_OK && (owned_count == HAKKAS_BLOCK_TYPES_MAX ||
				   block_types_register(&t->def) != BLOCK_TYPES_OK))
		st = fail(msg, COMPOSITE_ERR_FULL, "Too many block types");
	if (st != COMPOSITE_OK) {
		type_free(t);
		return st;
	}
	owned[owned_count++] = t;
	ok(msg);
	if (out != NULL)
		*out = &t->def;
	return COMPOSITE_OK;
}

enum composite_status composite_types_register_text(const char *text, size_t len,
						    CompositeMessage *msg,
						    const BlockTypeDef **out)
{
	if (out != NULL)
		*out = NULL;
	const char *end = NULL;
	cJSON *doc = cJSON_ParseWithLengthOpts(text, len, &end, false);
	if (doc == NULL) {
		/* cJSON reports an allocation failure like a syntax error. */
		return fail(msg, COMPOSITE_ERR_INVALID, "Invalid JSON");
	}
	/* Nothing but whitespace after the object. */
	const char *stop = text + len;
	while (end < stop && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
		end++;
	enum composite_status st = end == stop
					   ? composite_types_register(doc, msg, out)
					   : fail(msg, COMPOSITE_ERR_INVALID, "Invalid JSON");
	cJSON_Delete(doc);
	return st;
}

static CompositeType *find_owned(const char *name, size_t *index)
{
	for (size_t i = 0; name != NULL && i < owned_count; i++) {
		if (strcmp(owned[i]->def.name, name) == 0) {
			if (index != NULL)
				*index = i;
			return owned[i];
		}
	}
	return NULL;
}

static CompositeType *owner_of(const BlockTypeDef *def)
{
	for (size_t i = 0; def != NULL && i < owned_count; i++) {
		if (&owned[i]->def == def)
			return owned[i];
	}
	return NULL;
}

bool composite_types_owns(const BlockTypeDef *def)
{
	return owner_of(def) != NULL;
}

size_t composite_types_count(void)
{
	return owned_count;
}

void composite_types_reset(void)
{
	for (size_t i = 0; i < owned_count; i++) {
		block_types_unregister(owned[i]->def.name);
		type_free(owned[i]);
		owned[i] = NULL;
	}
	owned_count = 0;
}

/* ---- Registry operations ------------------------------------------------------ */

bool composite_types_in_use(const Graphs *gs, const BlockTypeDef *def)
{
	for (uint32_t i = 0; gs != NULL && i < graphs_count(gs); i++) {
		const Graph *g = graphs_at(gs, i);
		for (uint32_t k = 0; k < g->block_count; k++) {
			if (g->blocks[k]->type == def)
				return true;
		}
	}
	for (size_t i = 0; i < owned_count; i++) {
		const CompositeDef *c = &owned[i]->cdef;
		for (uint32_t j = 0; j < c->block_count; j++) {
			if (c->blocks[j].type == def)
				return true;
		}
	}
	return false;
}

/* The composite type called name for an operation: NOT_FOUND or IN_USE
 * (when check_use) refuse it. */
static enum composite_status operand(const Graphs *gs, const char *name, bool check_use,
				     CompositeMessage *msg, CompositeType **out, size_t *index)
{
	*out = find_owned(name, index);
	if (*out == NULL)
		return fail(msg, COMPOSITE_ERR_NOT_FOUND, "Unknown component: %s",
			    name != NULL ? name : "");
	if (check_use && composite_types_in_use(gs, &(*out)->def))
		return fail(msg, COMPOSITE_ERR_IN_USE, "Component in use: %s", name);
	return COMPOSITE_OK;
}

enum composite_status composite_types_unregister(const Graphs *gs, const char *name,
						 CompositeMessage *msg)
{
	CompositeType *t;
	size_t i = 0;
	enum composite_status st = operand(gs, name, true, msg, &t, &i);
	if (st != COMPOSITE_OK)
		return st;
	block_types_unregister(t->def.name);
	memmove(&owned[i], &owned[i + 1], (owned_count - i - 1) * sizeof(owned[0]));
	owned[--owned_count] = NULL;
	type_free(t);
	ok(msg);
	return COMPOSITE_OK;
}

enum composite_status composite_types_rename(const Graphs *gs, const char *from, const char *to,
					     CompositeMessage *msg)
{
	CompositeType *t;
	enum composite_status st = operand(gs, from, true, msg, &t, NULL);
	if (st != COMPOSITE_OK)
		return st;
	if (!graph_id_valid(to))
		return fail(msg, COMPOSITE_ERR_INVALID, "Invalid field: to");
	if (block_types_get(to) != NULL)
		return fail(msg, COMPOSITE_ERR_EXISTS, "Component exists: %s", to);
	char *name = dup_str(to);
	if (name == NULL)
		return no_memory(msg);
	block_types_unregister(t->def.name);
	free((char *)t->def.name);
	t->def.name = name;
	/* A slot was just freed and the name is free: this cannot fail. */
	(void)block_types_register(&t->def);
	ok(msg);
	return COMPOSITE_OK;
}

enum composite_status composite_types_change(const char *name, const char *description,
					     const char *icon, CompositeMessage *msg)
{
	CompositeType *t;
	enum composite_status st = operand(NULL, name, false, msg, &t, NULL);
	if (st != COMPOSITE_OK)
		return st;
	char *d = description != NULL ? dup_str(description) : NULL;
	char *i = icon != NULL ? dup_str(icon) : NULL;
	if ((description != NULL && d == NULL) || (icon != NULL && i == NULL)) {
		free(d);
		free(i);
		return no_memory(msg);
	}
	if (d != NULL) {
		free((char *)t->def.description);
		t->def.description = d;
	}
	if (i != NULL) {
		free((char *)t->def.icon);
		t->def.icon = i;
	}
	ok(msg);
	return COMPOSITE_OK;
}

/* ---- The authoring document --------------------------------------------------- */

/* Add item under key to obj; false (item freed) if item is NULL or memory
 * runs out. */
static bool add(cJSON *obj, const char *key, cJSON *item)
{
	if (item == NULL)
		return false;
	if (!cJSON_AddItemToObject(obj, key, item)) {
		cJSON_Delete(item);
		return false;
	}
	return true;
}

static bool append(cJSON *arr, cJSON *item)
{
	if (item == NULL)
		return false;
	if (!cJSON_AddItemToArray(arr, item)) {
		cJSON_Delete(item);
		return false;
	}
	return true;
}

/* The literal v as graph_json_from_initial writes it, but a number as a
 * plain number item (graph_json_number makes raw text, which only
 * graph_json_print should see): a document tree can be registered as is. */
static cJSON *initial_item(const Linkable *v)
{
	cJSON *item = graph_json_from_initial(v);
	if (item == NULL || !cJSON_IsRaw(item))
		return item;
	cJSON *number = cJSON_Parse(item->valuestring); /* exact (01 §6) */
	cJSON_Delete(item);
	return number;
}

/* A string item for "Block.port". */
static cJSON *ref_item(const char *block, const char *port)
{
	char *s = ref_text(block, port);
	cJSON *item = s != NULL ? cJSON_CreateString(s) : NULL;
	free(s);
	return item;
}

static cJSON *inner_ref(const CompositeType *t, CompositeRef r, bool input)
{
	const CompositeBlockDef *b = &t->blocks[r.block];
	const PortDescriptor *p = input ? &b->type->inputs[r.port] : &b->type->outputs[r.port];
	return ref_item(b->name, p->name);
}

static cJSON *port_doc(const CompositeType *t, uint32_t index, bool input)
{
	const PortDescriptor *p = &t->ports[index];
	const PortGiven *g = &t->given[index];
	cJSON *obj = cJSON_CreateObject();
	if (obj == NULL)
		return NULL;
	if (!add(obj, "name", cJSON_CreateString(p->name)) ||
	    !add(obj, input ? "tgt" : "src", cJSON_CreateString(p->inner)) ||
	    (g->type != NULL && !add(obj, "type", cJSON_CreateString(g->type))) ||
	    (g->description != NULL &&
	     !add(obj, "description", cJSON_CreateString(g->description))) ||
	    (g->addressable >= 0 && !add(obj, "addressable", cJSON_CreateBool(g->addressable)))) {
		cJSON_Delete(obj);
		return NULL;
	}
	return obj;
}

/* {"src": src, "tgt": tgt}, taking both. */
static cJSON *pair(cJSON *src, cJSON *tgt)
{
	cJSON *obj = cJSON_CreateObject();
	if (obj == NULL || !add(obj, "src", src) || !add(obj, "tgt", tgt)) {
		if (obj == NULL) {
			cJSON_Delete(src);
			cJSON_Delete(tgt);
		} else if (cJSON_GetObjectItemCaseSensitive(obj, "src") == NULL) {
			cJSON_Delete(tgt);
		}
		cJSON_Delete(obj);
		return NULL;
	}
	return obj;
}

static bool document_lists(const CompositeType *t, cJSON *blocks, cJSON *edges, cJSON *initials,
			   cJSON *inports, cJSON *outports)
{
	for (uint32_t j = 0; j < t->cdef.block_count; j++) {
		cJSON *b = cJSON_CreateObject();
		if (!append(blocks, b) || !add(b, "name", cJSON_CreateString(t->blocks[j].name)) ||
		    !add(b, "type", cJSON_CreateString(t->blocks[j].type->name)))
			return false;
	}
	for (uint32_t k = 0; k < t->cdef.edge_count; k++) {
		const CompositeEdge *e = &t->edges[k];
		if (!append(edges, pair(inner_ref(t, e->src, false), inner_ref(t, e->tgt, true))))
			return false;
	}
	for (uint32_t k = 0; k < t->cdef.initial_count; k++) {
		const CompositeInitial *init = &t->initials[k];
		if (!append(initials, pair(initial_item(&init->value),
					   inner_ref(t, init->tgt, true))))
			return false;
	}
	for (uint32_t i = 0; i < t->def.input_count; i++) {
		if (!append(inports, port_doc(t, i, true)))
			return false;
	}
	for (uint32_t i = 0; i < t->def.output_count; i++) {
		if (!append(outports, port_doc(t, t->def.input_count + i, false)))
			return false;
	}
	return true;
}

cJSON *composite_types_document(const BlockTypeDef *def)
{
	const CompositeType *t = owner_of(def);
	if (t == NULL)
		return NULL;
	cJSON *doc = cJSON_CreateObject();
	if (doc == NULL)
		return NULL;
	cJSON *blocks = cJSON_CreateArray(), *edges = cJSON_CreateArray();
	cJSON *initials = cJSON_CreateArray(), *inports = cJSON_CreateArray();
	cJSON *outports = cJSON_CreateArray();
	/* add() frees an item it could not add; the ones after it are freed
	 * here. */
	cJSON *lists[] = {blocks, edges, initials, inports, outports};
	static const char *const keys[] = {"blocks", "edges", "initials", "inports", "outports"};
	bool fine = add(doc, "name", cJSON_CreateString(t->def.name)) &&
		    add(doc, "icon", cJSON_CreateString(t->def.icon)) &&
		    add(doc, "description", cJSON_CreateString(t->def.description));
	size_t added = 0;
	for (; fine && added < 5; added++)
		fine = add(doc, keys[added], lists[added]);
	if (!fine) {
		/* lists[added - 1] was freed by add if it failed; the rest
		 * after it were never added. */
		for (size_t i = added; i < 5; i++)
			cJSON_Delete(lists[i]);
		cJSON_Delete(doc);
		return NULL;
	}
	if (!document_lists(t, blocks, edges, initials, inports, outports)) {
		cJSON_Delete(doc);
		return NULL;
	}
	return doc;
}

char *composite_types_source(const BlockTypeDef *def, size_t *len)
{
	cJSON *doc = composite_types_document(def);
	char *text = doc != NULL ? graph_json_print(doc) : NULL;
	cJSON_Delete(doc);
	if (text != NULL && len != NULL)
		*len = strlen(text);
	return text;
}

/* ---- Creating a group from a selection (03 §5.2) ------------------------------- */

/* An exposed port of the assembled type: the inner (block, port) and its
 * public name (NULL until assigned). */
typedef struct {
	const Block *block;
	uint32_t port;
	const char *public_name; /* a graph-level port's name, or NULL */
	char *name;		 /* the assigned name, owned */
} Exposed;

typedef struct {
	Exposed *items;
	uint32_t count;
	bool input;
} ExposedList;

static bool is_selected(const CompositeSelection *sel, const Block *b)
{
	for (size_t i = 0; i < sel->node_count; i++) {
		if (strcmp(sel->nodes[i], b->name) == 0)
			return true;
	}
	return false;
}

static bool exposed_has(const ExposedList *l, const Block *b, uint32_t port)
{
	for (uint32_t i = 0; i < l->count; i++) {
		if (l->items[i].block == b && l->items[i].port == port)
			return true;
	}
	return false;
}

static const char *inner_port_name(const ExposedList *l, const Exposed *x)
{
	return l->input ? block_input_port(x->block, x->port)->name
			: block_output_port(x->block, x->port)->name;
}

/* Whether a port of l already has the name. */
static bool name_taken(const ExposedList *l, const char *name)
{
	for (uint32_t i = 0; i < l->count; i++) {
		if (l->items[i].name != NULL && strcmp(l->items[i].name, name) == 0)
			return true;
	}
	return false;
}

/* Whether default-named x clashes: its inner port name is a public name, or
 * another default-named port has the same inner port name. */
static bool default_clashes(const ExposedList *l, const Exposed *x)
{
	const char *base = inner_port_name(l, x);
	for (uint32_t i = 0; i < l->count; i++) {
		const Exposed *y = &l->items[i];
		if (y == x)
			continue;
		const char *other = y->public_name != NULL ? y->public_name : inner_port_name(l, y);
		if (strcmp(other, base) == 0)
			return true;
	}
	return false;
}

/* Assign every port's name (composite_types.h, composite_types_assemble). */
static bool assign_names(ExposedList *l)
{
	/* Public names, then default names that do not clash, then the rest. */
	for (int pass = 0; pass < 3; pass++) {
		for (uint32_t i = 0; i < l->count; i++) {
			Exposed *x = &l->items[i];
			if (x->name != NULL)
				continue;
			const char *base = inner_port_name(l, x);
			char buf[2 * GRAPH_ID_MAX + 32];
			if (pass == 0 && x->public_name != NULL) {
				x->name = dup_str(x->public_name);
			} else if (pass == 1 && x->public_name == NULL && !default_clashes(l, x)) {
				x->name = dup_str(base);
			} else if (pass == 2) {
				int n = snprintf(buf, sizeof(buf), "%s_%s", x->block->name, base);
				/* A '.' (from the block name) would make the
				 * port unreferenceable (port_name_valid). */
				for (unsigned k = 2; n < 0 || (size_t)n > GRAPH_ID_MAX ||
						     strchr(buf, '.') != NULL || name_taken(l, buf);
				     k++) {
					n = snprintf(buf, sizeof(buf), "%s_%u", base, k);
					if (n < 0 || (size_t)n > GRAPH_ID_MAX)
						n = snprintf(buf, sizeof(buf), "port_%u", k);
				}
				x->name = dup_str(buf);
			} else {
				continue;
			}
			if (x->name == NULL)
				return false;
		}
	}
	return true;
}

static cJSON *exposed_doc(const ExposedList *l, const Exposed *x)
{
	cJSON *obj = cJSON_CreateObject();
	if (obj == NULL)
		return NULL;
	if (!add(obj, "name", cJSON_CreateString(x->name)) ||
	    !add(obj, l->input ? "tgt" : "src", ref_item(x->block->name, inner_port_name(l, x)))) {
		cJSON_Delete(obj);
		return NULL;
	}
	return obj;
}

static cJSON *endpoint_ref(const Block *b, uint32_t port, bool input)
{
	return ref_item(b->name, input ? block_input_port(b, port)->name
				       : block_output_port(b, port)->name);
}

/* The arrays of the assembled document. */
static bool assemble_lists(const Graph *g, const CompositeSelection *sel, cJSON *blocks,
			   cJSON *edges, cJSON *initials, ExposedList *in, ExposedList *out)
{
	/* Graph-level ports mapped to selected nodes, under their public
	 * names (only the first per inner input: no fan-in). */
	for (uint32_t i = 0; i < g->port_count[GRAPH_INPORT]; i++) {
		const GraphPort *p = &g->ports[GRAPH_INPORT][i];
		if (is_selected(sel, p->block) && !exposed_has(in, p->block, p->port))
			in->items[in->count++] = (Exposed){p->block, p->port, p->name, NULL};
	}
	for (uint32_t i = 0; i < g->port_count[GRAPH_OUTPORT]; i++) {
		const GraphPort *p = &g->ports[GRAPH_OUTPORT][i];
		if (is_selected(sel, p->block))
			out->items[out->count++] = (Exposed){p->block, p->port, p->name, NULL};
	}
	for (uint32_t i = 0; i < g->block_count; i++) {
		const Block *b = g->blocks[i];
		bool selected = is_selected(sel, b);
		if (selected) {
			cJSON *e = cJSON_CreateObject();
			if (!append(blocks, e) || !add(e, "name", cJSON_CreateString(b->name)) ||
			    !add(e, "type", cJSON_CreateString(block_component(b))))
				return false;
		}
		for (uint32_t k = 0; k < b->input_count; k++) {
			const Input *input = &b->inputs[k];
			if (selected && input->source == INPUT_SOURCE_INITIAL) {
				if (!append(initials, pair(initial_item(&input->initial),
							   endpoint_ref(b, k, true))))
					return false;
			} else if (input->source == INPUT_SOURCE_EDGE) {
				const Block *src = input->src_block;
				bool src_selected = is_selected(sel, src);
				if (selected && src_selected) {
					cJSON *from = endpoint_ref(src, input->src_output, false);
					if (!append(edges, pair(from, endpoint_ref(b, k, true))))
						return false;
				} else if (selected && !exposed_has(in, b, k)) {
					in->items[in->count++] = (Exposed){b, k, NULL, NULL};
				} else if (src_selected &&
					   !exposed_has(out, src, input->src_output)) {
					out->items[out->count++] =
						(Exposed){src, input->src_output, NULL, NULL};
				}
			}
		}
	}
	if (!assign_names(in) || !assign_names(out))
		return false;
	return true;
}

static void exposed_free(ExposedList *l)
{
	for (uint32_t i = 0; l->items != NULL && i < l->count; i++)
		free(l->items[i].name);
	free(l->items);
}

enum composite_status composite_types_assemble(const Graph *g, const CompositeSelection *sel,
					       cJSON **out, CompositeMessage *msg)
{
	*out = NULL;
	if (sel->node_count == 0)
		return fail(msg, COMPOSITE_ERR_INVALID, "Invalid field: nodes");
	for (size_t i = 0; i < sel->node_count; i++) {
		if (!graph_id_valid(sel->nodes[i]) || graph_find_block(g, sel->nodes[i]) == NULL)
			return fail(msg, COMPOSITE_ERR_UNKNOWN_PORT, "Unknown node: %.*s",
				    GRAPH_ID_MAX, sel->nodes[i] != NULL ? sel->nodes[i] : "");
	}
	/* Bounds: every graph port, plus every port of every block. */
	size_t nin = g->port_count[GRAPH_INPORT], nout = g->port_count[GRAPH_OUTPORT];
	for (uint32_t i = 0; i < g->block_count; i++) {
		nin += g->blocks[i]->input_count;
		nout += g->blocks[i]->output_count;
	}
	ExposedList in = {calloc(nin > 0 ? nin : 1, sizeof(Exposed)), 0, true};
	ExposedList outp = {calloc(nout > 0 ? nout : 1, sizeof(Exposed)), 0, false};
	cJSON *doc = cJSON_CreateObject();
	cJSON *lists[5] = {cJSON_CreateArray(), cJSON_CreateArray(), cJSON_CreateArray(),
			   cJSON_CreateArray(), cJSON_CreateArray()};
	static const char *const keys[] = {"blocks", "edges", "initials", "inports", "outports"};
	bool fine = in.items != NULL && outp.items != NULL && doc != NULL &&
		    add(doc, "name", cJSON_CreateString(sel->name != NULL ? sel->name : "")) &&
		    add(doc, "icon", cJSON_CreateString(sel->icon != NULL ? sel->icon : "")) &&
		    add(doc, "description",
			cJSON_CreateString(sel->description != NULL ? sel->description : ""));
	size_t added = 0;
	for (; fine && added < 5; added++)
		fine = add(doc, keys[added], lists[added]);
	if (fine)
		fine = assemble_lists(g, sel, lists[0], lists[1], lists[2], &in, &outp);
	for (uint32_t i = 0; fine && i < in.count; i++)
		fine = append(lists[3], exposed_doc(&in, &in.items[i]));
	for (uint32_t i = 0; fine && i < outp.count; i++)
		fine = append(lists[4], exposed_doc(&outp, &outp.items[i]));
	exposed_free(&in);
	exposed_free(&outp);
	if (!fine) {
		for (size_t i = added; i < 5; i++)
			cJSON_Delete(lists[i]);
		cJSON_Delete(doc);
		return no_memory(msg);
	}
	ok(msg);
	*out = doc;
	return COMPOSITE_OK;
}
