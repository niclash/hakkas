/*
 * The JSON forms of node and edge metadata and of initial values (05 §2.1,
 * 02 §3.4). See graphs/graph_json.h.
 */
#include "graphs/graph_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The type bits of a cJSON item (its low byte; the flags above it are kept). */
#define JSON_TYPE_MASK 0xFF

/* ---- Exact numbers (01 §6) ------------------------------------------------- */

/* The exact JSON text of v into buf (LINKABLE_NUMBER_TEXT_MAX bytes). */
static void number_text(double v, char *buf)
{
	if (isnan(v)) {
		snprintf(buf, LINKABLE_NUMBER_TEXT_MAX, "null");
	} else if (isinf(v)) {
		/* JSON has no infinity; this text overflows back to it. */
		snprintf(buf, LINKABLE_NUMBER_TEXT_MAX, "%s", v < 0 ? "-1e999" : "1e999");
	} else {
		Linkable l = linkable_from_f64(v);
		linkable_stringify(&l, buf, LINKABLE_NUMBER_TEXT_MAX);
	}
}

/* Turn every number in the list starting at item, and below it, into raw
 * text. Recursion is bounded by the tree's depth (CJSON_NESTING_LIMIT for a
 * parsed tree). */
static bool exact_numbers(cJSON *item)
{
	for (; item != NULL; item = item->next) {
		if (cJSON_IsNumber(item)) {
			char buf[LINKABLE_NUMBER_TEXT_MAX];
			number_text(item->valuedouble, buf);
			size_t len = strlen(buf) + 1;
			char *text = cJSON_malloc(len);
			if (text == NULL)
				return false;
			memcpy(text, buf, len);
			/* A number owns no valuestring; as raw it owns text. */
			item->valuestring = text;
			item->type = (item->type & ~JSON_TYPE_MASK) | cJSON_Raw;
		} else if (item->child != NULL && !exact_numbers(item->child)) {
			return false;
		}
	}
	return true;
}

char *graph_json_print(cJSON *item)
{
	if (item == NULL)
		return NULL;
	/* item itself, not its siblings: a member's next is its neighbour. */
	if (cJSON_IsNumber(item) || item->child != NULL) {
		cJSON *next = item->next;
		item->next = NULL;
		bool ok = exact_numbers(item);
		item->next = next;
		if (!ok)
			return NULL;
	}
	return cJSON_PrintUnformatted(item);
}

cJSON *graph_json_number(double v)
{
	if (isnan(v))
		return cJSON_CreateNull();
	char buf[LINKABLE_NUMBER_TEXT_MAX];
	number_text(v, buf);
	return cJSON_CreateRaw(buf);
}

/* Add item under key to obj; on failure (item NULL or no memory) item is
 * freed and false returned. */
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

/* ---- Node metadata ------------------------------------------------------- */

static bool is_model_key(const char *key)
{
	return strcmp(key, "x") == 0 || strcmp(key, "y") == 0 ||
	       strcmp(key, "change_threshold") == 0;
}

/* A position value: a finite number, or null for the default. */
static bool read_position(const cJSON *v, double *out)
{
	if (cJSON_IsNull(v)) {
		*out = BLOCK_DEFAULT_POSITION;
		return true;
	}
	if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble))
		return false;
	*out = v->valuedouble;
	return true;
}

/* A threshold value (01 §3.3): a number >= 0, or null for none (NAN). */
static bool read_threshold(const cJSON *v, double *out)
{
	if (cJSON_IsNull(v)) {
		*out = (double)NAN;
		return true;
	}
	if (!cJSON_IsNumber(v) || isnan(v->valuedouble) || v->valuedouble < 0)
		return false;
	*out = v->valuedouble;
	return true;
}

/* Print obj (owned, freed here) as the extra text: NULL for an empty object. */
static enum graph_status extra_text(cJSON *obj, char **out)
{
	*out = NULL;
	if (obj->child != NULL) {
		*out = graph_json_print(obj);
		if (*out == NULL) {
			cJSON_Delete(obj);
			return GRAPH_ERR_NO_MEMORY;
		}
	}
	cJSON_Delete(obj);
	return GRAPH_OK;
}

enum graph_status graph_json_to_metadata(const cJSON *obj, BlockMetadata *md)
{
	*md = block_metadata_default();
	if (obj == NULL)
		return GRAPH_OK;
	if (!cJSON_IsObject(obj))
		return GRAPH_ERR_INVALID_ARGUMENT;
	BlockMetadata m = block_metadata_default();
	const cJSON *v;
	if (((v = cJSON_GetObjectItemCaseSensitive(obj, "x")) != NULL && !read_position(v, &m.x)) ||
	    ((v = cJSON_GetObjectItemCaseSensitive(obj, "y")) != NULL && !read_position(v, &m.y)) ||
	    ((v = cJSON_GetObjectItemCaseSensitive(obj, "change_threshold")) != NULL &&
	     !read_threshold(v, &m.change_threshold)))
		return GRAPH_ERR_INVALID_ARGUMENT;

	cJSON *extra = cJSON_CreateObject();
	if (extra == NULL)
		return GRAPH_ERR_NO_MEMORY;
	for (const cJSON *c = obj->child; c != NULL; c = c->next) {
		if (is_model_key(c->string))
			continue;
		cJSON *copy = cJSON_Duplicate(c, true); /* keeps its key */
		if (copy == NULL) {
			cJSON_Delete(extra);
			return GRAPH_ERR_NO_MEMORY;
		}
		cJSON_AddItemToArray(extra, copy); /* cannot fail: both exist */
	}
	enum graph_status st = extra_text(extra, &m.extra);
	if (st == GRAPH_OK)
		*md = m;
	return st;
}

cJSON *graph_json_from_metadata(const BlockMetadata *md)
{
	cJSON *obj = cJSON_CreateObject();
	if (obj == NULL)
		return NULL;
	if (!add(obj, "x", graph_json_number(md->x)) || !add(obj, "y", graph_json_number(md->y)) ||
	    (!isnan(md->change_threshold) &&
	     !add(obj, "change_threshold", graph_json_number(md->change_threshold)))) {
		cJSON_Delete(obj);
		return NULL;
	}
	if (md->extra != NULL) {
		cJSON *extra = cJSON_Parse(md->extra);
		if (!cJSON_IsObject(extra)) {
			cJSON_Delete(extra);
			cJSON_Delete(obj);
			return NULL;
		}
		/* Move the members over, keys and order kept. */
		cJSON *c;
		while ((c = extra->child) != NULL)
			cJSON_AddItemToArray(obj, cJSON_DetachItemViaPointer(extra, c));
		cJSON_Delete(extra);
	}
	return obj;
}

/* Merge the opaque keys of patch into cur's extra; *out as extra_text. */
static enum graph_status merge_extra(const char *cur, const cJSON *patch, char **out)
{
	*out = NULL;
	cJSON *base = cur != NULL ? cJSON_Parse(cur) : cJSON_CreateObject();
	if (!cJSON_IsObject(base)) {
		cJSON_Delete(base);
		return GRAPH_ERR_NO_MEMORY; /* parse of our own text: only memory fails */
	}
	for (const cJSON *c = patch->child; c != NULL; c = c->next) {
		if (is_model_key(c->string))
			continue;
		cJSON *old = cJSON_GetObjectItemCaseSensitive(base, c->string);
		if (cJSON_IsNull(c)) {
			if (old != NULL)
				cJSON_Delete(cJSON_DetachItemViaPointer(base, old));
			continue;
		}
		cJSON *copy = cJSON_Duplicate(c, true);
		if (copy == NULL) {
			cJSON_Delete(base);
			return GRAPH_ERR_NO_MEMORY;
		}
		if (old != NULL)
			cJSON_ReplaceItemViaPointer(base, old, copy); /* in place, cannot fail */
		else
			cJSON_AddItemToArray(base, copy);
	}
	return extra_text(base, out);
}

enum graph_status graph_json_to_change(const BlockMetadata *cur, const cJSON *patch,
				       BlockChange *chg)
{
	*chg = (BlockChange){0};
	if (!cJSON_IsObject(patch))
		return GRAPH_ERR_INVALID_ARGUMENT;
	BlockChange c = {0};
	bool opaque = false;
	for (const cJSON *v = patch->child; v != NULL; v = v->next)
		opaque = opaque || !is_model_key(v->string);
	const cJSON *v;
	if ((v = cJSON_GetObjectItemCaseSensitive(patch, "x")) != NULL) {
		if (!read_position(v, &c.x))
			return GRAPH_ERR_INVALID_ARGUMENT;
		c.set_x = true;
	}
	if ((v = cJSON_GetObjectItemCaseSensitive(patch, "y")) != NULL) {
		if (!read_position(v, &c.y))
			return GRAPH_ERR_INVALID_ARGUMENT;
		c.set_y = true;
	}
	if ((v = cJSON_GetObjectItemCaseSensitive(patch, "change_threshold")) != NULL) {
		if (!read_threshold(v, &c.change_threshold))
			return GRAPH_ERR_INVALID_ARGUMENT;
		c.threshold = isnan(c.change_threshold) ? THRESHOLD_CLEAR : THRESHOLD_SET;
	}
	if (opaque) {
		enum graph_status st = merge_extra(cur->extra, patch, &c.extra);
		if (st != GRAPH_OK)
			return st;
		c.set_extra = true;
	}
	*chg = c;
	return GRAPH_OK;
}

/* ---- Edge metadata --------------------------------------------------------- */

enum graph_status graph_json_to_edge_metadata(const cJSON *obj, char **out)
{
	*out = NULL;
	if (obj == NULL || cJSON_IsNull(obj))
		return GRAPH_OK;
	if (!cJSON_IsObject(obj))
		return GRAPH_ERR_INVALID_ARGUMENT;
	cJSON *copy = cJSON_Duplicate(obj, true);
	if (copy == NULL)
		return GRAPH_ERR_NO_MEMORY;
	*out = graph_json_print(copy);
	cJSON_Delete(copy);
	return *out == NULL ? GRAPH_ERR_NO_MEMORY : GRAPH_OK;
}

cJSON *graph_json_from_edge_metadata(const char *text)
{
	if (text == NULL)
		return NULL;
	cJSON *obj = cJSON_Parse(text);
	if (!cJSON_IsObject(obj)) {
		cJSON_Delete(obj);
		return NULL;
	}
	return obj;
}

/* ---- Initial values -------------------------------------------------------- */

enum graph_status graph_json_to_initial(const cJSON *data, const char *port_type, Linkable *out)
{
	*out = (Linkable){0};
	if (cJSON_IsString(data))
		return linkable_parse(out, port_type, data->valuestring) ? GRAPH_OK
									  : GRAPH_ERR_NO_MEMORY;
	if (cJSON_IsNumber(data)) {
		*out = linkable_from_f64(data->valuedouble);
		return GRAPH_OK;
	}
	if (cJSON_IsBool(data)) {
		*out = linkable_from_bool(cJSON_IsTrue(data));
		return GRAPH_OK;
	}
	return GRAPH_ERR_INVALID_ARGUMENT;
}

/* A string item holding v's text form (non-finite f64, Metric). */
static cJSON *text_item(const Linkable *v)
{
	char small[LINKABLE_NUMBER_TEXT_MAX];
	size_t len = linkable_stringify(v, small, sizeof(small));
	if (len < sizeof(small))
		return cJSON_CreateString(small);
	char *buf = malloc(len + 1);
	if (buf == NULL)
		return NULL;
	linkable_stringify(v, buf, len + 1);
	cJSON *item = cJSON_CreateString(buf);
	free(buf);
	return item;
}

cJSON *graph_json_from_initial(const Linkable *v)
{
	char buf[LINKABLE_NUMBER_TEXT_MAX];
	switch (v->type) {
	case LINKABLE_STRING:
		return cJSON_CreateString(v->as.s);
	case LINKABLE_BOOL:
		return cJSON_CreateBool(v->as.b);
	case LINKABLE_I64:
		/* Exact integer text; it reads back as an f64 (02 §3.4). */
		linkable_stringify(v, buf, sizeof(buf));
		return cJSON_CreateRaw(buf);
	case LINKABLE_F64:
		if (isfinite(v->as.f64))
			return graph_json_number(v->as.f64);
		return text_item(v); /* "NaN", "Infinity", "-Infinity" (05 §2.1) */
	case LINKABLE_METRIC:
		return text_item(v); /* "<value> <unit>" */
	case LINKABLE_UNSET:
		break;
	}
	return NULL;
}

/* ---- Block type descriptors (02 §3.5) ----------------------------------- */

static const char *or_empty(const char *s)
{
	return s != NULL ? s : "";
}

static cJSON *port_descriptors(const PortDescriptor *ports, uint32_t count)
{
	cJSON *arr = cJSON_CreateArray();
	if (arr == NULL)
		return NULL;
	for (uint32_t i = 0; i < count; i++) {
		cJSON *p = cJSON_CreateObject();
		if (p == NULL || !cJSON_AddItemToArray(arr, p)) {
			cJSON_Delete(p);
			cJSON_Delete(arr);
			return NULL;
		}
		if (!add(p, "id", cJSON_CreateString(or_empty(ports[i].name))) ||
		    !add(p, "description", cJSON_CreateString(or_empty(ports[i].description))) ||
		    !add(p, "type", cJSON_CreateString(or_empty(ports[i].type))) ||
		    !add(p, "addressable", cJSON_CreateBool(ports[i].addressable))) {
			cJSON_Delete(arr);
			return NULL;
		}
	}
	return arr;
}

cJSON *graph_json_component(const BlockTypeDef *def)
{
	cJSON *obj = cJSON_CreateObject();
	if (obj == NULL)
		return NULL;
	if (!add(obj, "name", cJSON_CreateString(or_empty(def->name))) ||
	    !add(obj, "description", cJSON_CreateString(or_empty(def->description))) ||
	    !add(obj, "subgraph", cJSON_CreateBool(def->subgraph)) ||
	    !add(obj, "icon", cJSON_CreateString(or_empty(def->icon))) ||
	    !add(obj, "inPorts", port_descriptors(def->inputs, def->input_count)) ||
	    !add(obj, "outPorts", port_descriptors(def->outputs, def->output_count))) {
		cJSON_Delete(obj);
		return NULL;
	}
	return obj;
}
