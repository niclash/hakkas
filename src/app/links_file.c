/*
 * The saved links between processes (05 §2.3). See app/links_file.h.
 */
#include "app/links_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "graphs/graph.h"
#include "system/log.h"

typedef struct {
	const char *origin;
	LinksFile *file;
	bool no_memory;
} Parser;

static bool nonempty_string(const cJSON *v)
{
	return cJSON_IsString(v) && v->valuestring[0] != '\0';
}

/* A heap copy of the len bytes at s, or NULL (p->no_memory set). */
static char *dup_n(Parser *p, const char *s, size_t len)
{
	char *copy = malloc(len + 1);
	if (copy == NULL) {
		p->no_memory = true;
		return NULL;
	}
	memcpy(copy, s, len);
	copy[len] = '\0';
	return copy;
}

/* Split the reference ref ("<process>.<port>", on its last '.', 01 §2.4)
 * into heap copies. False if it has no process or port (logged) or memory
 * runs out. */
static bool split_ref(Parser *p, const char *ref, size_t i, const char *member,
		      const char *port_kind, char **process, char **port)
{
	size_t process_len;
	const char *port_name;
	if (!port_ref_split(ref, &process_len, &port_name)) {
		log_warn("%s: links[%zu]: %s: expected \"<process>.<%s>\"", p->origin, i, member,
			 port_kind);
		return false;
	}
	*process = dup_n(p, ref, process_len);
	*port = dup_n(p, port_name, strlen(port_name));
	return *process != NULL && *port != NULL;
}

static bool parse_links(Parser *p, const cJSON *links)
{
	if (links == NULL || cJSON_IsNull(links))
		return true;
	if (!cJSON_IsArray(links)) {
		log_warn("%s: links: expected an array", p->origin);
		return false;
	}
	size_t n = (size_t)cJSON_GetArraySize(links);
	LinksFile *f = p->file;
	if (n > 0 && (f->links = calloc(n, sizeof(*f->links))) == NULL) {
		p->no_memory = true;
		return false;
	}
	const cJSON *v;
	size_t i = 0;
	cJSON_ArrayForEach(v, links)
	{
		const cJSON *from = cJSON_GetObjectItemCaseSensitive(v, "from");
		const cJSON *to = cJSON_GetObjectItemCaseSensitive(v, "to");
		if (!cJSON_IsObject(v) || !nonempty_string(from) || !nonempty_string(to)) {
			log_warn("%s: links[%zu]: expected an object with \"from\" and \"to\"",
				 p->origin, i);
			return false;
		}
		LinksFileLink *k = &f->links[f->count++];
		if (!split_ref(p, from->valuestring, i, "from", "outport", &k->from_process,
			       &k->from_port) ||
		    !split_ref(p, to->valuestring, i, "to", "inport", &k->to_process, &k->to_port))
			return false;
		i++;
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

enum graph_files_status links_file_parse(const char *text, size_t len, const char *origin,
					 LinksFile *out)
{
	memset(out, 0, sizeof(*out));
	const char *end = NULL;
	cJSON *root = cJSON_ParseWithLengthOpts(text, len, &end, false);
	if (root != NULL && !only_whitespace(end, (size_t)(text + len - end))) {
		cJSON_Delete(root);
		root = NULL;
	}
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		log_warn("%s: not a links file: expected a JSON object with \"links\"", origin);
		return GRAPH_FILES_ERR_MALFORMED;
	}
	const cJSON *m;
	cJSON_ArrayForEach(m, root)
	{
		if (strcmp(m->string, "links") != 0)
			log_warn("%s: unknown member \"%s\" ignored", origin, m->string);
	}
	Parser p = {origin, out, false};
	bool ok = parse_links(&p, cJSON_GetObjectItemCaseSensitive(root, "links"));
	cJSON_Delete(root);
	if (ok)
		return GRAPH_FILES_OK;
	links_file_free(out);
	if (p.no_memory) {
		log_error("%s: out of memory", origin);
		return GRAPH_FILES_ERR_NO_MEMORY;
	}
	return GRAPH_FILES_ERR_MALFORMED;
}

void links_file_free(LinksFile *f)
{
	for (size_t i = 0; i < f->count; i++) {
		free(f->links[i].from_process);
		free(f->links[i].from_port);
		free(f->links[i].to_process);
		free(f->links[i].to_port);
	}
	free(f->links);
	memset(f, 0, sizeof(*f));
}

/* "<process>.<port>" as a new cJSON string, or NULL. */
static cJSON *ref(const char *process, const char *port)
{
	size_t n = strlen(process) + strlen(port) + 2;
	char *s = malloc(n);
	if (s == NULL)
		return NULL;
	snprintf(s, n, "%s.%s", process, port);
	cJSON *j = cJSON_CreateString(s);
	free(s);
	return j;
}

char *links_file_serialize(const Runtime *rt, size_t *len)
{
	cJSON *root = cJSON_CreateObject();
	cJSON *links = cJSON_AddArrayToObject(root, "links");
	bool ok = root != NULL && links != NULL;
	for (uint32_t i = 0; ok && i < runtime_link_count(rt); i++) {
		const ProcessLink *l = runtime_link_at(rt, i);
		cJSON *o = cJSON_CreateObject();
		cJSON *from = ref(l->src_graph->id, l->src_port);
		cJSON *to = ref(l->tgt_graph->id, l->tgt_port);
		ok = o != NULL && from != NULL && to != NULL &&
		     cJSON_AddItemToObject(o, "from", from);
		if (!ok) {
			cJSON_Delete(from);
			cJSON_Delete(to);
			cJSON_Delete(o);
			break;
		}
		ok = cJSON_AddItemToObject(o, "to", to) && cJSON_AddItemToArray(links, o);
	}
	char *text = ok ? cJSON_PrintUnformatted(root) : NULL;
	cJSON_Delete(root);
	if (text != NULL && len != NULL)
		*len = strlen(text);
	return text;
}
