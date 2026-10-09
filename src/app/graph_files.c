/*
 * Graph files (05 §2): save, load, persist, delete. See app/graph_files.h.
 */
#include "app/graph_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/file_names.h"
#include "cJSON.h"
#include "graphs/graph_json.h"
#include "system/log.h"

const char *graph_files_status_str(enum graph_files_status st)
{
	switch (st) {
	case GRAPH_FILES_OK:
		return "ok";
	case GRAPH_FILES_ERR_INVALID:
		return "invalid id or path";
	case GRAPH_FILES_ERR_NOT_FOUND:
		return "not found";
	case GRAPH_FILES_ERR_TOO_BIG:
		return "file too big";
	case GRAPH_FILES_ERR_MALFORMED:
		return "malformed graph file";
	case GRAPH_FILES_ERR_EXISTS:
		return "graph exists";
	case GRAPH_FILES_ERR_STORAGE:
		return "storage error";
	case GRAPH_FILES_ERR_NO_MEMORY:
		return "out of memory";
	}
	return "?";
}

/* ---- Paths (05 §2.3) -------------------------------------------------------- */

/* snprintf into buf, false if it did not fit. */
static bool fits(int n, size_t size)
{
	return n >= 0 && (size_t)n < size;
}

bool graph_files_init(GraphFiles *gf, Graphs *gs, const char *datadir)
{
	memset(gf, 0, sizeof(*gf));
	gf->gs = gs;
	if (datadir == NULL || datadir[0] == '\0')
		return false;
	size_t len = strlen(datadir);
	/* "dir/" and "dir" name the same directory; keep "/" itself. */
	while (len > 1 && datadir[len - 1] == '/')
		len--;
	/* The shortest file path, "<datadir>/graphs/x.json.tmp", must fit. */
	size_t shortest = len + sizeof("/" GRAPH_FILES_GRAPHS_DIR "/x" FILE_NAME_SUFFIX
				       PAL_TMP_SUFFIX);
	if (shortest > PAL_PATH_MAX)
		return false;
	memcpy(gf->datadir, datadir, len);
	gf->datadir[len] = '\0';
	return true;
}

/* The separator after datadir: none if it is "/". */
static const char *sep(const GraphFiles *gf)
{
	return strcmp(gf->datadir, "/") == 0 ? "" : "/";
}

enum graph_files_status graph_files_dir(const GraphFiles *gf, const char *subdir, char *buf,
					size_t size)
{
	if (fits(snprintf(buf, size, "%s%s%s", gf->datadir, sep(gf), subdir), size))
		return GRAPH_FILES_OK;
	if (size > 0)
		buf[0] = '\0';
	return GRAPH_FILES_ERR_INVALID;
}

enum graph_files_status graph_files_path(const GraphFiles *gf, const char *subdir,
					 const char *name, char *buf, size_t size)
{
	char file[FILE_NAME_ENCODED_MAX + 1];
	if (size > 0)
		buf[0] = '\0';
	if (!file_name_encode(name, file, sizeof(file)))
		return GRAPH_FILES_ERR_INVALID;
	int n = snprintf(buf, size, "%s%s%s/%s", gf->datadir, sep(gf), subdir, file);
	/* An atomic write appends ".tmp" (pal_storage_write). */
	if (!fits(n, size) || (size_t)n + sizeof(PAL_TMP_SUFFIX) > PAL_PATH_MAX) {
		if (size > 0)
			buf[0] = '\0';
		return GRAPH_FILES_ERR_INVALID;
	}
	return GRAPH_FILES_OK;
}

static enum graph_files_status make_dir(const GraphFiles *gf, const char *subdir)
{
	char path[PAL_PATH_MAX];
	if (graph_files_dir(gf, subdir, path, sizeof(path)) != GRAPH_FILES_OK)
		return GRAPH_FILES_ERR_INVALID;
	enum pal_err err = pal_storage_mkdir(path);
	if (err != PAL_OK) {
		log_error("cannot create directory %s: %s", path, pal_err_str(err));
		return GRAPH_FILES_ERR_STORAGE;
	}
	return GRAPH_FILES_OK;
}

enum graph_files_status graph_files_make_dirs(const GraphFiles *gf)
{
	enum graph_files_status st = make_dir(gf, GRAPH_FILES_GRAPHS_DIR);
	return st != GRAPH_FILES_OK ? st : make_dir(gf, GRAPH_FILES_TYPES_DIR);
}

/* ---- Saving (05 §2.1) ------------------------------------------------------- */

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

/* Append item to array; false (item freed) if item is NULL or memory runs
 * out. */
static bool append(cJSON *array, cJSON *item)
{
	if (item == NULL)
		return false;
	if (!cJSON_AddItemToArray(array, item)) {
		cJSON_Delete(item);
		return false;
	}
	return true;
}

/* {"node": node, "port": port} */
static cJSON *endpoint(const char *node, const char *port)
{
	cJSON *obj = cJSON_CreateObject();
	if (obj == NULL)
		return NULL;
	if (!add(obj, "node", cJSON_CreateString(node)) ||
	    !add(obj, "port", cJSON_CreateString(port))) {
		cJSON_Delete(obj);
		return NULL;
	}
	return obj;
}

/* {"data": <value>} */
static cJSON *data_source(const Linkable *v)
{
	cJSON *obj = cJSON_CreateObject();
	if (obj != NULL && !add(obj, "data", graph_json_from_initial(v))) {
		cJSON_Delete(obj);
		return NULL;
	}
	return obj;
}

/* One connection entry: src (an endpoint or a data source), tgt, and the
 * edge's metadata if it has any. Takes src. */
static cJSON *connection(cJSON *src, const char *tgt, const char *tgt_port, const char *metadata)
{
	cJSON *obj = cJSON_CreateObject();
	if (obj == NULL) {
		cJSON_Delete(src);
		return NULL;
	}
	if (!add(obj, "src", src) || !add(obj, "tgt", endpoint(tgt, tgt_port)) ||
	    (metadata != NULL && !add(obj, "metadata", graph_json_from_edge_metadata(metadata)))) {
		cJSON_Delete(obj);
		return NULL;
	}
	return obj;
}

static cJSON *block_entry(const Block *b)
{
	cJSON *obj = cJSON_CreateObject();
	if (obj == NULL)
		return NULL;
	if (!add(obj, "component", cJSON_CreateString(block_component(b))) ||
	    !add(obj, "metadata", graph_json_from_metadata(&b->metadata))) {
		cJSON_Delete(obj);
		return NULL;
	}
	return obj;
}

static bool save_blocks(const Graph *g, cJSON *blocks)
{
	for (uint32_t i = 0; i < g->block_count; i++) {
		const Block *b = g->blocks[i];
		if (!add(blocks, b->name, block_entry(b)))
			return false;
	}
	return true;
}

/* Live connections per target block (creation order) and input (port
 * order), then the retained ones in load order (graphs/graph_files.h). */
static bool save_connections(const Graph *g, cJSON *conns)
{
	for (uint32_t i = 0; i < g->block_count; i++) {
		const Block *b = g->blocks[i];
		for (uint32_t k = 0; k < b->input_count; k++) {
			const Input *in = &b->inputs[k];
			const char *port = block_input_port(b, k)->name;
			cJSON *src;
			if (in->source == INPUT_SOURCE_INITIAL)
				src = data_source(&in->initial);
			else if (in->source == INPUT_SOURCE_EDGE)
				src = endpoint(in->src_block->name,
					       block_output_port(in->src_block, in->src_output)
						       ->name);
			else
				continue;
			if (src == NULL ||
			    !append(conns, connection(src, b->name, port, in->edge_metadata)))
				return false;
		}
	}
	for (uint32_t i = 0; i < g->retained_count; i++) {
		const RetainedConnection *c = &g->retained[i];
		cJSON *src = c->src != NULL ? endpoint(c->src->name, c->src_port)
					    : data_source(&c->data);
		if (src == NULL ||
		    !append(conns, connection(src, c->tgt->name, c->tgt_port, c->metadata)))
			return false;
	}
	return true;
}

static cJSON *port_entry(const char *name, const char *node, const char *port)
{
	cJSON *obj = cJSON_CreateObject();
	if (obj == NULL)
		return NULL;
	if (!add(obj, "name", cJSON_CreateString(name)) ||
	    !add(obj, "node", cJSON_CreateString(node)) ||
	    !add(obj, "port", cJSON_CreateString(port))) {
		cJSON_Delete(obj);
		return NULL;
	}
	return obj;
}

/* The live ports in creation order, then the retained ones. */
static bool save_ports(const Graph *g, enum graph_port_dir dir, cJSON *ports)
{
	for (uint32_t i = 0; i < g->port_count[dir]; i++) {
		const GraphPort *p = &g->ports[dir][i];
		if (!append(ports, port_entry(p->name, p->block->name,
					      graph_port_inner_name(dir, p))))
			return false;
	}
	for (uint32_t i = 0; i < g->retained_port_count[dir]; i++) {
		const RetainedPort *p = &g->retained_ports[dir][i];
		if (!append(ports, port_entry(p->name, p->block->name, p->port)))
			return false;
	}
	return true;
}

static cJSON *graph_document(const Graph *g)
{
	cJSON *root = cJSON_CreateObject();
	if (root == NULL)
		return NULL;
	cJSON *blocks = cJSON_CreateObject();
	cJSON *conns = cJSON_CreateArray();
	cJSON *inports = cJSON_CreateArray();
	cJSON *outports = cJSON_CreateArray();
	bool ok = add(root, "id", cJSON_CreateString(g->id)) &&
		  add(root, "name", cJSON_CreateString(g->name)) &&
		  add(root, "description", cJSON_CreateString(g->description)) &&
		  add(root, "icon", cJSON_CreateString(g->icon)) &&
		  add(root, "started", cJSON_CreateBool(g->started)) &&
		  add(root, "blocks", blocks) && add(root, "connections", conns) &&
		  add(root, "inports", inports) && add(root, "outports", outports);
	if (!ok) {
		/* Whatever was not added yet is still ours (add frees on failure). */
		if (cJSON_GetObjectItemCaseSensitive(root, "blocks") != blocks)
			cJSON_Delete(blocks);
		if (cJSON_GetObjectItemCaseSensitive(root, "connections") != conns)
			cJSON_Delete(conns);
		if (cJSON_GetObjectItemCaseSensitive(root, "inports") != inports)
			cJSON_Delete(inports);
		if (cJSON_GetObjectItemCaseSensitive(root, "outports") != outports)
			cJSON_Delete(outports);
		cJSON_Delete(root);
		return NULL;
	}
	if (!save_blocks(g, blocks) || !save_connections(g, conns) ||
	    !save_ports(g, GRAPH_INPORT, inports) || !save_ports(g, GRAPH_OUTPORT, outports)) {
		cJSON_Delete(root);
		return NULL;
	}
	return root;
}

char *graph_files_serialize(const Graph *g, size_t *len)
{
	cJSON *root = graph_document(g);
	char *text = graph_json_print(root);
	cJSON_Delete(root);
	if (text != NULL && len != NULL)
		*len = strlen(text);
	return text;
}

enum graph_files_status graph_files_save(const GraphFiles *gf, const Graph *g)
{
	char path[PAL_PATH_MAX];
	if (graph_files_path(gf, GRAPH_FILES_GRAPHS_DIR, g->id, path, sizeof(path)) !=
	    GRAPH_FILES_OK) {
		log_error("cannot save graph %s: its file path is too long", g->id);
		return GRAPH_FILES_ERR_INVALID;
	}
	enum graph_files_status st = make_dir(gf, GRAPH_FILES_GRAPHS_DIR);
	if (st != GRAPH_FILES_OK)
		return st;
	size_t len;
	char *text = graph_files_serialize(g, &len);
	if (text == NULL) {
		log_error("cannot save graph %s: out of memory", g->id);
		return GRAPH_FILES_ERR_NO_MEMORY;
	}
	enum pal_err err = pal_storage_write(path, text, len);
	free(text);
	if (err != PAL_OK) {
		log_error("cannot write %s: %s", path, pal_err_str(err));
		return GRAPH_FILES_ERR_STORAGE;
	}
	return GRAPH_FILES_OK;
}

enum graph_files_status graph_files_persist(GraphFiles *gf, const char *id, GraphsMessage *msg)
{
	GraphRefusal ctx = {.graph = id};
	enum graph_status refusal = GRAPH_OK;
	const Graph *g = NULL;
	if (!graph_id_valid(id))
		refusal = GRAPH_ERR_INVALID_ID;
	else if ((g = graphs_find(gf->gs, id)) == NULL)
		refusal = GRAPH_ERR_UNKNOWN_GRAPH;
	if (refusal != GRAPH_OK) {
		if (msg != NULL)
			graph_refusal_message(refusal, &ctx, msg->text, sizeof(msg->text));
		return refusal == GRAPH_ERR_INVALID_ID ? GRAPH_FILES_ERR_INVALID
						       : GRAPH_FILES_ERR_NOT_FOUND;
	}

	enum graph_files_status st = graph_files_save(gf, g);
	if (msg != NULL)
		msg->text[0] = '\0';
	if (st == GRAPH_FILES_OK)
		return st;
	/* 02 §3.3: a system-wide error event. The name is any string, so the
	 * message is sized for it; without memory it is cut like msg. */
	static const char fmt[] = "Unable to save graph: %s (%s)";
	size_t size = sizeof(fmt) + strlen(g->name) + strlen(g->id);
	char *text = malloc(size);
	GraphsMessage fallback;
	GraphsMessage *m = msg != NULL ? msg : &fallback;
	snprintf(m->text, sizeof(m->text), fmt, g->name, g->id);
	if (text != NULL)
		snprintf(text, size, fmt, g->name, g->id);
	graphs_notify_error(gf->gs, g->id, text != NULL ? text : m->text);
	free(text);
	return st;
}

/* ---- Loading (05 §2.2) ------------------------------------------------------ */

typedef struct {
	GraphFiles *gf;
	Graph *g;
	const char *origin;
} Loader;

/* Log a skipped entry with the refusal message of st. */
static void skipped(const Loader *ld, const char *what, enum graph_status st,
		    const GraphRefusal *ctx)
{
	char text[GRAPHS_MESSAGE_MAX];
	graph_refusal_message(st, ctx, text, sizeof(text));
	log_warn("%s: %s skipped: %s", ld->origin, what, text);
}

static const char *string_of(const cJSON *obj, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* A member that is absent (NULL) or has the type test accepts. */
static bool absent_or(const cJSON *obj, const char *key, cJSON_bool (*test)(const cJSON *))
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
	return v == NULL || test(v);
}

static enum graph_files_status no_memory(const Loader *ld)
{
	log_error("%s: out of memory", ld->origin);
	return GRAPH_FILES_ERR_NO_MEMORY;
}

static enum graph_files_status load_block(Loader *ld, const cJSON *entry)
{
	const char *name = entry->string;
	const char *component = cJSON_IsObject(entry) ? string_of(entry, "component") : NULL;
	if (component == NULL) {
		log_warn("%s: block %s skipped: no component", ld->origin, name);
		return GRAPH_FILES_OK;
	}
	BlockMetadata md;
	enum graph_status st =
		graph_json_to_metadata(cJSON_GetObjectItemCaseSensitive(entry, "metadata"), &md);
	if (st == GRAPH_ERR_NO_MEMORY)
		return no_memory(ld);
	if (st != GRAPH_OK) {
		log_warn("%s: block %s skipped: invalid metadata", ld->origin, name);
		return GRAPH_FILES_OK;
	}

	Graphs *gs = ld->gf->gs;
	GraphRefusal ctx = {.graph = ld->g->id, .node = name, .name = component};
	if (gs->resolve_type(component) != NULL) {
		st = graphs_add_node(gs, ld->g->id, name, component, &md, NULL);
	} else {
		st = graph_create_placeholder(ld->g, name, component, &md, NULL);
		if (st == GRAPH_OK)
			log_warn("%s: block %s: unknown component %s, kept as a placeholder",
				 ld->origin, name, component);
	}
	block_metadata_release(&md);
	if (st == GRAPH_ERR_NO_MEMORY)
		return no_memory(ld);
	if (st != GRAPH_OK) {
		char text[GRAPHS_MESSAGE_MAX];
		graph_refusal_message(st, &ctx, text, sizeof(text));
		log_warn("%s: block %s skipped: %s", ld->origin, name, text);
	}
	return GRAPH_FILES_OK;
}

/* A {"node","port"} endpoint with string members. */
static bool read_endpoint(const cJSON *obj, const char **node, const char **port)
{
	*node = cJSON_IsObject(obj) ? string_of(obj, "node") : NULL;
	*port = cJSON_IsObject(obj) ? string_of(obj, "port") : NULL;
	return *node != NULL && *port != NULL;
}

/* A file's initial data as the retained literal: kept as its JSON type, a
 * string unparsed (it is parsed for the port when the type is known). */
static bool retained_data(const cJSON *data, Linkable *out)
{
	if (cJSON_IsString(data))
		return linkable_from_string(out, data->valuestring, strlen(data->valuestring));
	if (cJSON_IsBool(data))
		*out = linkable_from_bool(cJSON_IsTrue(data));
	else
		*out = linkable_from_f64(data->valuedouble);
	return true;
}

/* A connection that references a placeholder (05 §2.2): retained as loaded. */
static enum graph_files_status retain_connection(Loader *ld, Block *sb, const char *src_port,
						 const cJSON *data, Block *tb, const char *tgt_port,
						 const char *metadata)
{
	RetainedConnection rc = {.src = sb, .src_port = (char *)src_port, .tgt = tb,
				 .tgt_port = (char *)tgt_port, .metadata = (char *)metadata};
	if (data != NULL && !retained_data(data, &rc.data))
		return no_memory(ld);
	enum graph_status st = graph_retain_connection(ld->g, &rc);
	linkable_clear(&rc.data);
	if (st == GRAPH_ERR_NO_MEMORY)
		return no_memory(ld);
	if (st != GRAPH_OK) {
		/* The failing end is the real block whose port is wrong, the
		 * source first; a fan-in is about the target. */
		bool src_bad = sb != NULL && !block_is_placeholder(sb) &&
			       block_output_index(sb, src_port) < 0;
		GraphRefusal ctx = {.node = src_bad ? sb->name : tb->name,
				    .port = src_bad ? src_port : tgt_port};
		skipped(ld, "connection", st, &ctx);
	}
	return GRAPH_FILES_OK;
}

/* A live initial: data parsed for the port's type (02 §3.4), then addinitial
 * with its refusals. */
static enum graph_files_status live_initial(Loader *ld, const cJSON *data, Block *tb,
					    const char *tgt, const char *tgt_port)
{
	int32_t index = tb != NULL ? block_input_index(tb, tgt_port) : -1;
	const char *type = index >= 0 ? block_input_port(tb, (uint32_t)index)->type : NULL;
	GraphRefusal ctx = {.graph = ld->g->id, .node = tgt, .port = tgt_port};
	if (index >= 0 && graph_input_retained(ld->g, tb, tgt_port)) {
		skipped(ld, "initial", GRAPH_ERR_INPUT_CONNECTED, &ctx);
		return GRAPH_FILES_OK;
	}
	Linkable v;
	enum graph_status st = graph_json_to_initial(data, type, &v);
	if (st == GRAPH_ERR_NO_MEMORY)
		return no_memory(ld);
	st = graphs_add_initial(ld->gf->gs, ld->g->id, tgt, tgt_port, &v, NULL);
	linkable_clear(&v);
	if (st == GRAPH_ERR_NO_MEMORY)
		return no_memory(ld);
	if (st != GRAPH_OK)
		skipped(ld, "initial", st, &ctx);
	return GRAPH_FILES_OK;
}

/* A live edge: addedge with its refusals. */
static enum graph_files_status live_edge(Loader *ld, Block *sb, const char *src,
					 const char *src_port, Block *tb, const char *tgt,
					 const char *tgt_port, const char *metadata)
{
	Graphs *gs = ld->gf->gs;
	GraphRefusal ctx = {.graph = ld->g->id, .node = tgt, .port = tgt_port};
	/* A retained connection into the input came first: a fan-in, once the
	 * source end is known to be valid (02 §3.4.1 checks the source first). */
	if (sb != NULL && block_output_index(sb, src_port) >= 0 && tb != NULL &&
	    block_input_index(tb, tgt_port) >= 0 && graph_input_retained(ld->g, tb, tgt_port)) {
		skipped(ld, "connection", GRAPH_ERR_INPUT_CONNECTED, &ctx);
		return GRAPH_FILES_OK;
	}
	GraphsMessage msg;
	enum graph_status st =
		graphs_add_edge(gs, ld->g->id, src, src_port, tgt, tgt_port, metadata, &msg);
	if (st == GRAPH_ERR_NO_MEMORY)
		return no_memory(ld);
	if (st != GRAPH_OK)
		log_warn("%s: connection skipped: %s", ld->origin, msg.text);
	return GRAPH_FILES_OK;
}

static enum graph_files_status load_connection(Loader *ld, const cJSON *entry)
{
	const cJSON *src = cJSON_GetObjectItemCaseSensitive(entry, "src");
	const cJSON *data = cJSON_GetObjectItemCaseSensitive(src, "data");
	const char *src_node = NULL, *src_port = NULL, *tgt, *tgt_port;
	bool shape = cJSON_IsObject(entry) && cJSON_IsObject(src) &&
		     read_endpoint(cJSON_GetObjectItemCaseSensitive(entry, "tgt"), &tgt, &tgt_port);
	if (data != NULL)
		shape = shape &&
			(cJSON_IsString(data) || cJSON_IsNumber(data) || cJSON_IsBool(data));
	else
		shape = shape && read_endpoint(src, &src_node, &src_port);
	if (!shape) {
		log_warn("%s: connection skipped: invalid entry", ld->origin);
		return GRAPH_FILES_OK;
	}
	char *metadata = NULL;
	if (data == NULL) {
		enum graph_status st = graph_json_to_edge_metadata(
			cJSON_GetObjectItemCaseSensitive(entry, "metadata"), &metadata);
		if (st == GRAPH_ERR_NO_MEMORY)
			return no_memory(ld);
		if (st != GRAPH_OK) {
			log_warn("%s: connection skipped: invalid metadata", ld->origin);
			return GRAPH_FILES_OK;
		}
	}

	Block *tb = graph_find_block(ld->g, tgt);
	Block *sb = src_node != NULL ? graph_find_block(ld->g, src_node) : NULL;
	enum graph_files_status st;
	if (data != NULL && tb != NULL && block_is_placeholder(tb))
		st = retain_connection(ld, NULL, NULL, data, tb, tgt_port, NULL);
	else if (data != NULL)
		st = live_initial(ld, data, tb, tgt, tgt_port);
	else if (sb != NULL && tb != NULL && (block_is_placeholder(sb) || block_is_placeholder(tb)))
		st = retain_connection(ld, sb, src_port, NULL, tb, tgt_port, metadata);
	else
		st = live_edge(ld, sb, src_node, src_port, tb, tgt, tgt_port, metadata);
	free(metadata);
	return st;
}

static enum graph_files_status load_port(Loader *ld, enum graph_port_dir dir, const cJSON *entry)
{
	const char *what = dir == GRAPH_INPORT ? "inport" : "outport";
	const char *name = cJSON_IsObject(entry) ? string_of(entry, "name") : NULL;
	const char *node = NULL, *port = NULL;
	if (name == NULL || !read_endpoint(entry, &node, &port)) {
		log_warn("%s: %s skipped: invalid entry", ld->origin, what);
		return GRAPH_FILES_OK;
	}
	GraphRefusal ctx = {.graph = ld->g->id, .node = node, .port = port, .name = name};
	Block *b = graph_find_block(ld->g, node);
	enum graph_status st;
	if (graph_id_valid(name) && graph_port_retained(ld->g, dir, name))
		st = GRAPH_ERR_PORT_EXISTS; /* the first port of a name wins */
	else if (b != NULL && block_is_placeholder(b))
		st = graph_retain_port(ld->g, dir, name, b, port);
	else
		st = graphs_add_port(ld->gf->gs, ld->g->id, dir, name, node, port, NULL);
	if (st == GRAPH_ERR_NO_MEMORY)
		return no_memory(ld);
	if (st != GRAPH_OK)
		skipped(ld, what, st, &ctx);
	return GRAPH_FILES_OK;
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

/* The minimum valid document (graph_files_load_text). */
static bool document_valid(const cJSON *root)
{
	return cJSON_IsObject(root) && graph_id_valid(string_of(root, "id")) &&
	       absent_or(root, "name", cJSON_IsString) &&
	       absent_or(root, "description", cJSON_IsString) &&
	       absent_or(root, "icon", cJSON_IsString) &&
	       absent_or(root, "started", cJSON_IsBool) &&
	       absent_or(root, "blocks", cJSON_IsObject) &&
	       absent_or(root, "connections", cJSON_IsArray) &&
	       absent_or(root, "inports", cJSON_IsArray) &&
	       absent_or(root, "outports", cJSON_IsArray);
}

static enum graph_files_status load_entries(Loader *ld, const cJSON *root)
{
	enum graph_files_status st = GRAPH_FILES_OK;
	const cJSON *e;
	cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "blocks"))
	{
		if ((st = load_block(ld, e)) != GRAPH_FILES_OK)
			return st;
	}
	cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "connections"))
	{
		if ((st = load_connection(ld, e)) != GRAPH_FILES_OK)
			return st;
	}
	cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "inports"))
	{
		if ((st = load_port(ld, GRAPH_INPORT, e)) != GRAPH_FILES_OK)
			return st;
	}
	cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "outports"))
	{
		if ((st = load_port(ld, GRAPH_OUTPORT, e)) != GRAPH_FILES_OK)
			return st;
	}
	return st;
}

enum graph_files_status graph_files_load_text(GraphFiles *gf, const char *text, size_t len,
					      const char *origin, Graph **out)
{
	if (out != NULL)
		*out = NULL;
	const char *end = NULL;
	cJSON *root = cJSON_ParseWithLengthOpts(text, len, &end, false);
	if (root != NULL && !only_whitespace(end, (size_t)(text + len - end))) {
		cJSON_Delete(root); /* trailing text after the document */
		root = NULL;
	}
	if (!document_valid(root)) {
		cJSON_Delete(root);
		log_error("%s: not a valid graph file", origin);
		return GRAPH_FILES_ERR_MALFORMED;
	}
	const char *id = string_of(root, "id");
	bool started = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "started"));
	Graph *g;
	GraphsMessage msg;
	enum graph_status gst = graphs_new_graph(gf->gs, id, string_of(root, "name"),
						 string_of(root, "description"),
						 string_of(root, "icon"), &g, &msg);
	if (gst != GRAPH_OK) {
		log_error("%s: graph not loaded: %s", origin, msg.text);
		cJSON_Delete(root);
		return gst == GRAPH_ERR_GRAPH_EXISTS ? GRAPH_FILES_ERR_EXISTS
						     : GRAPH_FILES_ERR_NO_MEMORY;
	}

	Loader ld = {gf, g, origin};
	enum graph_files_status st = load_entries(&ld, root);
	cJSON_Delete(root);
	if (st != GRAPH_FILES_OK) {
		/* Out of memory: no partly loaded graph is left behind. */
		graphs_delete_graph(gf->gs, g->id, g->name, NULL);
		return st;
	}
	/* Started only once everything is loaded (05 §2.2). */
	if (started)
		graphs_start(gf->gs, g->id, NULL);
	if (out != NULL)
		*out = g;
	return GRAPH_FILES_OK;
}

enum graph_files_status graph_files_load(GraphFiles *gf, const char *path, Graph **out)
{
	if (out != NULL)
		*out = NULL;
	struct pal_stat info;
	enum pal_err err = pal_storage_stat(path, &info);
	if (err == PAL_OK && info.is_dir)
		err = PAL_ERR_INVALID;
	if (err != PAL_OK) {
		log_error("cannot load %s: %s", path, pal_err_str(err));
		return err == PAL_ERR_NOT_FOUND ? GRAPH_FILES_ERR_NOT_FOUND
						: GRAPH_FILES_ERR_STORAGE;
	}
	if (info.size > HAKKAS_GRAPH_FILE_MAX) {
		log_error("cannot load %s: %llu bytes, more than the limit of %lu", path,
			  (unsigned long long)info.size, (unsigned long)HAKKAS_GRAPH_FILE_MAX);
		return GRAPH_FILES_ERR_TOO_BIG;
	}
	size_t cap = (size_t)info.size;
	char *buf = malloc(cap > 0 ? cap : 1);
	if (buf == NULL) {
		log_error("cannot load %s: out of memory", path);
		return GRAPH_FILES_ERR_NO_MEMORY;
	}
	size_t len = 0;
	err = pal_storage_read(path, buf, cap, &len);
	if (err != PAL_OK) {
		/* PAL_ERR_TOO_BIG: the file grew since stat; nothing writes
		 * graph files concurrently, so it is a storage problem. */
		free(buf);
		log_error("cannot read %s: %s", path, pal_err_str(err));
		return GRAPH_FILES_ERR_STORAGE;
	}
	enum graph_files_status st = graph_files_load_text(gf, buf, len, path, out);
	free(buf);
	return st;
}

/* ---- Loading many ----------------------------------------------------------- */

typedef struct {
	char **names;
	size_t count;
	size_t cap;
	bool no_memory;
} NameList;

static bool ends_with(const char *s, const char *suffix)
{
	size_t n = strlen(s), k = strlen(suffix);
	return n >= k && strcmp(s + n - k, suffix) == 0;
}

static bool collect_name(void *ctx, const char *name, bool is_dir)
{
	NameList *list = ctx;
	if (is_dir || !ends_with(name, FILE_NAME_SUFFIX))
		return true;
	if (list->count == list->cap) {
		size_t cap = list->cap == 0 ? 8 : list->cap * 2;
		char **names = realloc(list->names, cap * sizeof(*names));
		if (names == NULL) {
			list->no_memory = true;
			return false;
		}
		list->names = names;
		list->cap = cap;
	}
	size_t len = strlen(name) + 1;
	char *copy = malloc(len);
	if (copy == NULL) {
		list->no_memory = true;
		return false;
	}
	memcpy(copy, name, len);
	list->names[list->count++] = copy;
	return true;
}

static int compare_names(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Count one load's result into *first (the first failure) and *loaded. */
static void tally(enum graph_files_status st, enum graph_files_status *first, size_t *loaded)
{
	if (st == GRAPH_FILES_OK)
		(*loaded)++;
	else if (*first == GRAPH_FILES_OK)
		*first = st;
}

enum graph_files_status graph_files_load_all(GraphFiles *gf, size_t *loaded)
{
	size_t count = 0;
	if (loaded != NULL)
		*loaded = 0;
	char dir[PAL_PATH_MAX];
	if (graph_files_dir(gf, GRAPH_FILES_GRAPHS_DIR, dir, sizeof(dir)) != GRAPH_FILES_OK)
		return GRAPH_FILES_ERR_INVALID;
	NameList list = {0};
	enum pal_err err = pal_storage_list(dir, collect_name, &list);
	enum graph_files_status first = GRAPH_FILES_OK;
	if (err != PAL_OK) {
		log_error("cannot list %s: %s", dir, pal_err_str(err));
		first = err == PAL_ERR_NOT_FOUND ? GRAPH_FILES_ERR_NOT_FOUND
						 : GRAPH_FILES_ERR_STORAGE;
	} else if (list.no_memory) {
		log_error("cannot list %s: out of memory", dir);
		first = GRAPH_FILES_ERR_NO_MEMORY;
	} else {
		qsort(list.names, list.count, sizeof(*list.names), compare_names);
		for (size_t i = 0; i < list.count; i++) {
			char path[PAL_PATH_MAX];
			if (!fits(snprintf(path, sizeof(path), "%s/%s", dir, list.names[i]),
				  sizeof(path))) {
				log_error("cannot load %s/%s: path too long", dir, list.names[i]);
				tally(GRAPH_FILES_ERR_INVALID, &first, &count);
				continue;
			}
			Graph *g;
			enum graph_files_status st = graph_files_load(gf, path, &g);
			tally(st, &first, &count);
			char expected[FILE_NAME_ENCODED_MAX + 1];
			if (st == GRAPH_FILES_OK &&
			    (!file_name_encode(g->id, expected, sizeof(expected)) ||
			     strcmp(expected, list.names[i]) != 0))
				log_warn("%s: graph %s is saved as %s, not here", path, g->id,
					 expected);
		}
	}
	for (size_t i = 0; i < list.count; i++)
		free(list.names[i]);
	free(list.names);
	if (loaded != NULL)
		*loaded = count;
	return first;
}

enum graph_files_status graph_files_load_list(GraphFiles *gf, const char *paths, size_t *loaded)
{
	size_t count = 0;
	enum graph_files_status first = GRAPH_FILES_OK;
	const char *p = paths != NULL ? paths : "";
	for (;;) {
		const char *end = strchr(p, ':');
		size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
		if (len == 0 || len >= PAL_PATH_MAX) {
			log_error("invalid graph file name in list: \"%.*s\"",
				  (int)(len > 64 ? 64 : len), p);
			tally(GRAPH_FILES_ERR_INVALID, &first, &count);
		} else {
			char path[PAL_PATH_MAX];
			memcpy(path, p, len);
			path[len] = '\0';
			tally(graph_files_load(gf, path, NULL), &first, &count);
		}
		if (end == NULL)
			break;
		p = end + 1;
	}
	if (loaded != NULL)
		*loaded = count;
	return first;
}

/* ---- Deleting (02 §3.2) ----------------------------------------------------- */

enum graph_status graph_files_delete(GraphFiles *gf, const char *id, const char *name,
				     GraphsMessage *msg)
{
	char path[PAL_PATH_MAX];
	bool have_path = graph_files_path(gf, GRAPH_FILES_GRAPHS_DIR, id, path, sizeof(path)) ==
			 GRAPH_FILES_OK;
	enum graph_status st = graphs_delete_graph(gf->gs, id, name, msg);
	if (st != GRAPH_OK || !have_path)
		return st; /* a path that does not fit was never written */
	enum pal_err err = pal_storage_remove(path);
	if (err != PAL_OK && err != PAL_ERR_NOT_FOUND)
		log_error("cannot remove %s: %s", path, pal_err_str(err));
	return st;
}
