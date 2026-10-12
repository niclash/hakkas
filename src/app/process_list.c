/*
 * The process list (05 §1.2). See app/process_list.h.
 */
#include "app/process_list.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "platform/pal.h"
#include "system/log.h"

typedef struct {
	const char *path; /* the list file */
	ProcessList *list;
	bool no_memory;
} Parser;

/* rel resolved against the directory of the list file (05 §1.2): a heap
 * string, or NULL (logged as what) if it does not fit PAL_PATH_MAX or
 * memory runs out. */
static char *resolve(Parser *p, const char *rel, const char *what)
{
	const char *slash = strrchr(p->path, '/');
	size_t dir_len = rel[0] == '/' || slash == NULL ? 0 : (size_t)(slash - p->path) + 1;
	size_t len = dir_len + strlen(rel);
	if (len >= PAL_PATH_MAX) {
		log_error("%s: %s: path too long", p->path, what);
		return NULL;
	}
	char *s = malloc(len + 1);
	if (s == NULL) {
		p->no_memory = true;
		return NULL;
	}
	memcpy(s, p->path, dir_len);
	memcpy(s + dir_len, rel, len - dir_len + 1);
	return s;
}

static bool nonempty_string(const cJSON *v)
{
	return cJSON_IsString(v) && v->valuestring[0] != '\0';
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
			log_warn("%s: %s%sunknown member \"%s\" ignored", p->path, where,
				 where[0] != '\0' ? ": " : "", m->string);
	}
}

/* Append an entry for the graph file rel. False (logged) if it fails. */
static bool add_entry(Parser *p, const char *rel, const char *where, ProcessListEntry **slot)
{
	ProcessList *l = p->list;
	ProcessListEntry *e = &l->entries[l->count];
	memset(e, 0, sizeof(*e));
	e->graph = resolve(p, rel, where);
	if (e->graph == NULL)
		return false;
	l->count++;
	*slot = e;
	return true;
}

static bool parse_array(Parser *p, const cJSON *arr)
{
	const cJSON *v;
	size_t i = 0;
	cJSON_ArrayForEach(v, arr)
	{
		char where[32];
		snprintf(where, sizeof(where), "[%zu]", i++);
		if (!nonempty_string(v)) {
			log_error("%s: %s: expected a graph file path (a non-empty string)",
				  p->path, where);
			return false;
		}
		ProcessListEntry *e;
		if (!add_entry(p, v->valuestring, where, &e))
			return false;
	}
	return true;
}

static bool parse_process(Parser *p, const cJSON *v, size_t i)
{
	static const char *const known[] = {"graph", "bindings", "start", NULL};
	char where[32];
	snprintf(where, sizeof(where), "processes[%zu]", i);
	const cJSON *graph = cJSON_GetObjectItemCaseSensitive(v, "graph");
	if (!cJSON_IsObject(v) || !nonempty_string(graph)) {
		log_error("%s: %s: expected an object with a graph file path \"graph\"", p->path,
			  where);
		return false;
	}
	const cJSON *bindings = cJSON_GetObjectItemCaseSensitive(v, "bindings");
	if (bindings != NULL && !cJSON_IsNull(bindings) && !nonempty_string(bindings)) {
		log_error("%s: %s: bindings: expected a file path (a non-empty string)", p->path,
			  where);
		return false;
	}
	const cJSON *start = cJSON_GetObjectItemCaseSensitive(v, "start");
	if (start != NULL && !cJSON_IsNull(start) && !cJSON_IsBool(start)) {
		log_error("%s: %s: start: expected true or false", p->path, where);
		return false;
	}
	unknown_members(p, v, where, known);
	ProcessListEntry *e;
	if (!add_entry(p, graph->valuestring, where, &e))
		return false;
	if (cJSON_IsBool(start))
		e->start = cJSON_IsTrue(start) ? PROCESS_LIST_START_YES : PROCESS_LIST_START_NO;
	if (nonempty_string(bindings)) {
		e->bindings = resolve(p, bindings->valuestring, where);
		if (e->bindings == NULL)
			return false;
	}
	return true;
}

static bool parse_links(Parser *p, const cJSON *links)
{
	if (links == NULL || cJSON_IsNull(links))
		return true;
	if (!cJSON_IsArray(links)) {
		log_error("%s: links: expected an array", p->path);
		return false;
	}
	const cJSON *v;
	size_t i = 0;
	cJSON_ArrayForEach(v, links)
	{
		if (!cJSON_IsObject(v) ||
		    !nonempty_string(cJSON_GetObjectItemCaseSensitive(v, "from")) ||
		    !nonempty_string(cJSON_GetObjectItemCaseSensitive(v, "to"))) {
			log_error("%s: links[%zu]: expected an object with \"from\" and \"to\"",
				  p->path, i);
			return false;
		}
		i++;
	}
	p->list->link_count = i;
	return true;
}

static bool parse_object(Parser *p, const cJSON *obj)
{
	static const char *const known[] = {"processes", "links", NULL};
	const cJSON *processes = cJSON_GetObjectItemCaseSensitive(obj, "processes");
	if (!cJSON_IsArray(processes)) {
		log_error("%s: processes: expected an array", p->path);
		return false;
	}
	unknown_members(p, obj, "", known);
	if (!parse_links(p, cJSON_GetObjectItemCaseSensitive(obj, "links")))
		return false;
	const cJSON *v;
	size_t i = 0;
	cJSON_ArrayForEach(v, processes)
	{
		if (!parse_process(p, v, i++))
			return false;
	}
	return true;
}

/* Whether the len bytes at s are JSON whitespace. */
static bool only_whitespace(const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
			return false;
	}
	return true;
}

enum graph_files_status process_list_parse(const char *text, size_t len, const char *path,
					   ProcessList *out)
{
	memset(out, 0, sizeof(*out));
	const char *end = NULL;
	cJSON *root = cJSON_ParseWithLengthOpts(text, len, &end, false);
	if (root != NULL && !only_whitespace(end, (size_t)(text + len - end))) {
		cJSON_Delete(root);
		root = NULL;
	}
	if (!cJSON_IsArray(root) && !cJSON_IsObject(root)) {
		cJSON_Delete(root);
		log_error("%s: not a valid process list: expected a JSON array of graph file "
			  "paths or an object with \"processes\"",
			  path);
		return GRAPH_FILES_ERR_MALFORMED;
	}
	const cJSON *items =
		cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "processes");
	size_t cap = cJSON_IsArray(items) ? (size_t)cJSON_GetArraySize(items) : 0;
	Parser p = {path, out, false};
	if (cap > 0 && (out->entries = calloc(cap, sizeof(*out->entries))) == NULL) {
		cJSON_Delete(root);
		log_error("%s: out of memory", path);
		return GRAPH_FILES_ERR_NO_MEMORY;
	}
	bool ok = cJSON_IsArray(root) ? parse_array(&p, root) : parse_object(&p, root);
	cJSON_Delete(root);
	if (ok)
		return GRAPH_FILES_OK;
	process_list_free(out);
	if (p.no_memory) {
		log_error("%s: out of memory", path);
		return GRAPH_FILES_ERR_NO_MEMORY;
	}
	return GRAPH_FILES_ERR_MALFORMED;
}

enum graph_files_status process_list_load(const char *path, ProcessList *out)
{
	memset(out, 0, sizeof(*out));
	char *text;
	size_t len;
	enum graph_files_status st = graph_files_read(path, HAKKAS_PROCESS_LIST_MAX, &text, &len);
	if (st != GRAPH_FILES_OK)
		return st; /* logged */
	st = process_list_parse(text, len, path, out);
	free(text);
	return st;
}

void process_list_free(ProcessList *l)
{
	for (size_t i = 0; i < l->count; i++) {
		free(l->entries[i].graph);
		free(l->entries[i].bindings);
	}
	free(l->entries);
	memset(l, 0, sizeof(*l));
}
