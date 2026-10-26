/*
 * Type files (05 §2.3): save, delete, load at startup. See app/type_files.h.
 */
#include "app/type_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/file_names.h"
#include "blocktypes/block_types.h"
#include "blocktypes/composite_types.h"
#include "cJSON.h"
#include "graphs/graph_json.h"
#include "system/log.h"

/* Write doc, a type's canonical document, to the file of name, atomically,
 * creating types/ if missing. */
static enum graph_files_status write_doc(const GraphFiles *gf, const char *name, cJSON *doc)
{
	char path[PAL_PATH_MAX];
	if (graph_files_path(gf, GRAPH_FILES_TYPES_DIR, name, path, sizeof(path)) !=
	    GRAPH_FILES_OK) {
		log_error("cannot save type %s: its file path is too long", name);
		return GRAPH_FILES_ERR_INVALID;
	}
	char dir[PAL_PATH_MAX];
	enum pal_err err = PAL_OK;
	if (graph_files_dir(gf, GRAPH_FILES_TYPES_DIR, dir, sizeof(dir)) == GRAPH_FILES_OK)
		err = pal_storage_mkdir(dir);
	if (err != PAL_OK) {
		log_error("cannot create directory %s: %s", dir, pal_err_str(err));
		return GRAPH_FILES_ERR_STORAGE;
	}
	char *text = doc != NULL ? graph_json_print(doc) : NULL;
	if (text == NULL) {
		log_error("cannot save type %s: out of memory", name);
		return GRAPH_FILES_ERR_NO_MEMORY;
	}
	err = pal_storage_write(path, text, strlen(text));
	free(text);
	if (err != PAL_OK) {
		log_error("cannot write %s: %s", path, pal_err_str(err));
		return GRAPH_FILES_ERR_STORAGE;
	}
	return GRAPH_FILES_OK;
}

enum graph_files_status type_files_save(const GraphFiles *gf, const BlockTypeDef *def)
{
	if (def == NULL || !composite_types_owns(def))
		return GRAPH_FILES_ERR_INVALID;
	cJSON *doc = composite_types_document(def);
	enum graph_files_status st = write_doc(gf, def->name, doc);
	cJSON_Delete(doc);
	return st;
}

enum graph_files_status type_files_delete(const GraphFiles *gf, const char *name)
{
	char path[PAL_PATH_MAX];
	if (graph_files_path(gf, GRAPH_FILES_TYPES_DIR, name, path, sizeof(path)) !=
	    GRAPH_FILES_OK)
		return GRAPH_FILES_ERR_INVALID;
	enum pal_err err = pal_storage_remove(path);
	if (err != PAL_OK && err != PAL_ERR_NOT_FOUND) {
		log_error("cannot remove %s: %s", path, pal_err_str(err));
		return GRAPH_FILES_ERR_STORAGE;
	}
	return GRAPH_FILES_OK;
}

/* ---- Loading at startup (05 §1.4 step 6) ---------------------------------------- */

/* One file of types/ while loading. */
typedef struct {
	char *file;  /* the file name (owned by the name list) */
	cJSON *doc;  /* the parsed document; NULL if it did not read */
	bool done;   /* registered or refused */
} TypeFile;

/* The document's name, or NULL. */
static const char *doc_name(const cJSON *doc)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(doc, "name");
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* The first inner type of doc that is not registered, or NULL if all are (or
 * the blocks are malformed: registering reports that). */
static const char *missing_inner_type(const cJSON *doc)
{
	const cJSON *blocks = cJSON_GetObjectItemCaseSensitive(doc, "blocks");
	if (!cJSON_IsArray(blocks))
		return NULL;
	const cJSON *e;
	cJSON_ArrayForEach(e, blocks)
	{
		const cJSON *type = cJSON_GetObjectItemCaseSensitive(e, "type");
		if (cJSON_IsString(type) &&
		    block_types_get(block_types_alias(type->valuestring)) == NULL)
			return type->valuestring;
	}
	return NULL;
}

/* Parse the file at path into tf->doc; a failure (logged) if it cannot be
 * read or is not one JSON object. */
static enum graph_files_status read_doc(const char *path, TypeFile *tf)
{
	char *text;
	size_t len;
	enum graph_files_status st = graph_files_read(path, HAKKAS_TYPE_FILE_MAX, &text, &len);
	if (st != GRAPH_FILES_OK)
		return st; /* logged */
	const char *end = NULL;
	tf->doc = cJSON_ParseWithLengthOpts(text, len, &end, false);
	while (tf->doc != NULL && end < text + len &&
	       (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
		end++;
	bool valid = tf->doc != NULL && cJSON_IsObject(tf->doc) && end == text + len;
	free(text);
	if (!valid) {
		cJSON_Delete(tf->doc);
		tf->doc = NULL;
		log_error("%s: composite type skipped: Invalid JSON", path);
		return GRAPH_FILES_ERR_MALFORMED;
	}
	return GRAPH_FILES_OK;
}

static void first_failure(enum graph_files_status st, enum graph_files_status *first)
{
	if (st != GRAPH_FILES_OK && *first == GRAPH_FILES_OK)
		*first = st;
}

/* Register tf's document (its inner types are all there, or it is left over
 * and this reports why); logs the outcome. */
static bool register_doc(const char *dir, TypeFile *tf, const TypeFile *files, size_t n)
{
	CompositeMessage msg;
	const BlockTypeDef *def;
	const char *missing = missing_inner_type(tf->doc);
	enum composite_status st = composite_types_register(tf->doc, &msg, &def);
	if (st != COMPOSITE_OK) {
		/* An inner type that is another file's which did not load
		 * either: a cycle, or a dependency on a skipped type. */
		bool other = false;
		for (size_t i = 0; st == COMPOSITE_ERR_UNKNOWN_TYPE && missing != NULL && i < n;
		     i++) {
			/* Not registered: it would not be missing. */
			const char *name = files[i].doc != NULL ? doc_name(files[i].doc) : NULL;
			other = other || (&files[i] != tf && name != NULL &&
					  strcmp(name, missing) == 0);
		}
		log_error("%s/%s: composite type skipped: %s%s", dir, tf->file, msg.text,
			  other ? " (a type that did not load either: a dependency cycle, or "
				  "it depends on a skipped type)"
				: "");
		return false;
	}
	char expected[FILE_NAME_ENCODED_MAX + 1];
	if (!file_name_encode(def->name, expected, sizeof(expected)) ||
	    strcmp(expected, tf->file) != 0)
		log_warn("%s/%s: type %s is saved as %s, not here", dir, tf->file, def->name,
			 expected);
	return true;
}

enum graph_files_status type_files_load_all(const GraphFiles *gf, size_t *loaded)
{
	size_t count = 0;
	if (loaded != NULL)
		*loaded = 0;
	char dir[PAL_PATH_MAX];
	if (graph_files_dir(gf, GRAPH_FILES_TYPES_DIR, dir, sizeof(dir)) != GRAPH_FILES_OK)
		return GRAPH_FILES_ERR_INVALID;
	char **names;
	size_t n;
	enum graph_files_status first = graph_files_list(dir, &names, &n);
	TypeFile *files = n > 0 ? calloc(n, sizeof(*files)) : NULL;
	if (n > 0 && files == NULL) {
		log_error("cannot load the types in %s: out of memory", dir);
		graph_files_list_free(names, n);
		return GRAPH_FILES_ERR_NO_MEMORY;
	}
	for (size_t i = 0; i < n; i++) {
		files[i].file = names[i];
		char path[PAL_PATH_MAX];
		enum graph_files_status st = GRAPH_FILES_ERR_INVALID;
		if (snprintf(path, sizeof(path), "%s/%s", dir, names[i]) < (int)sizeof(path))
			st = read_doc(path, &files[i]);
		else
			log_error("cannot load %s/%s: path too long", dir, names[i]);
		first_failure(st, &first);
	}

	/* Rounds (05 §1.4 step 6): every type whose inner types are all
	 * registered, until a round registers none. */
	for (bool progress = true; progress;) {
		progress = false;
		for (size_t i = 0; i < n; i++) {
			TypeFile *tf = &files[i];
			if (tf->doc == NULL || tf->done || missing_inner_type(tf->doc) != NULL)
				continue;
			tf->done = true;
			if (register_doc(dir, tf, files, n)) {
				count++;
				progress = true;
			} else {
				first_failure(GRAPH_FILES_ERR_MALFORMED, &first);
			}
		}
	}
	/* What is left: an unknown inner type, a cycle, or a dependency on a
	 * skipped type. Registering reports the reason. */
	for (size_t i = 0; i < n; i++) {
		if (files[i].doc != NULL && !files[i].done) {
			register_doc(dir, &files[i], files, n); /* refused, logged */
			first_failure(GRAPH_FILES_ERR_MALFORMED, &first);
		}
	}
	for (size_t i = 0; i < n; i++)
		cJSON_Delete(files[i].doc);
	free(files);
	graph_files_list_free(names, n);
	if (loaded != NULL)
		*loaded = count;
	return first;
}

/* ---- The group commands (02 §3.4.2) ----------------------------------------------- */

/* Keep the component graph of the type called name in step with the type
 * (02 §3.4.3): drop it, then build it again from def (NULL: only drop it). */
static void sync_component_graph(GraphFiles *gf, const char *name, const BlockTypeDef *def)
{
	graphs_quiet(gf->gs, true);
	Graph *g = graphs_find(gf->gs, name);
	if (g != NULL && g->component)
		(void)graphs_delete_graph(gf->gs, name, g->name, NULL);
	if (def != NULL)
		(void)type_files_component_graph(gf, def);
	graphs_quiet(gf->gs, false);
}

static void set_message(GraphsMessage *msg, const char *text)
{
	if (msg != NULL)
		snprintf(msg->text, sizeof(msg->text), "%s", text);
}

/* The graph a group command is issued in, or NULL with "Invalid id" or
 * "Unknown graph: <id>" in msg. */
static Graph *group_graph(const GraphFiles *gf, const char *id, GraphsMessage *msg)
{
	enum graph_status st = GRAPH_ERR_INVALID_ID;
	Graph *g = NULL;
	if (graph_id_valid(id)) {
		g = graphs_find(gf->gs, id);
		st = g != NULL ? GRAPH_OK : GRAPH_ERR_UNKNOWN_GRAPH;
	}
	if (g == NULL && msg != NULL) {
		GraphRefusal r = {.graph = id};
		graph_refusal_message(st, &r, msg->text, sizeof(msg->text));
	}
	return g;
}

/* A refusal of the composite registry, its message in msg. */
static enum graph_files_status refused(GraphsMessage *msg, enum composite_status st,
				       const CompositeMessage *cm)
{
	set_message(msg, cm->text);
	return st == COMPOSITE_ERR_NO_MEMORY ? GRAPH_FILES_ERR_NO_MEMORY : GRAPH_FILES_ERR_INVALID;
}

/* The type file of name could not be written (st, logged). */
static enum graph_files_status unsaved(GraphsMessage *msg, enum graph_files_status st,
				       const char *name)
{
	if (msg != NULL)
		snprintf(msg->text, sizeof(msg->text), "Unable to save component: %.64s", name);
	return st;
}

/* The composite type called name, or NULL. */
static const BlockTypeDef *composite_named(const char *name)
{
	const BlockTypeDef *def = name != NULL ? block_types_get(name) : NULL;
	return def != NULL && composite_types_owns(def) ? def : NULL;
}

/* doc's string member key set to value; false if memory runs out. */
static bool replace_str(cJSON *doc, const char *key, const char *value)
{
	cJSON *item = cJSON_CreateString(value);
	if (item == NULL)
		return false;
	if (!cJSON_ReplaceItemInObjectCaseSensitive(doc, key, item)) {
		cJSON_Delete(item);
		return false;
	}
	return true;
}

enum graph_files_status type_files_add_group(GraphFiles *gf, const char *graph,
					     const CompositeSelection *sel, GraphsMessage *msg)
{
	Graph *g = group_graph(gf, graph, msg);
	if (g == NULL)
		return GRAPH_FILES_ERR_INVALID;
	CompositeMessage cm;
	if (!graph_id_valid(sel->name)) {
		set_message(msg, "Invalid field: name");
		return GRAPH_FILES_ERR_INVALID;
	}
	if (block_types_get(sel->name) != NULL) {
		if (msg != NULL)
			snprintf(msg->text, sizeof(msg->text), "Component exists: %s", sel->name);
		return GRAPH_FILES_ERR_INVALID;
	}
	cJSON *doc;
	enum composite_status st = composite_types_assemble(g, sel, &doc, &cm);
	if (st != COMPOSITE_OK)
		return refused(msg, st, &cm);
	const BlockTypeDef *def;
	st = composite_types_register(doc, &cm, &def);
	cJSON_Delete(doc);
	if (st != COMPOSITE_OK)
		return refused(msg, st, &cm);
	enum graph_files_status fs = type_files_save(gf, def);
	if (fs != GRAPH_FILES_OK) {
		/* Just registered, so not in use: this cannot fail. */
		(void)composite_types_unregister(gf->gs, sel->name, NULL);
		return unsaved(msg, fs, sel->name);
	}
	sync_component_graph(gf, sel->name, def);
	g = graphs_find(gf->gs, graph); /* the context may be the graph just synced */
	graphs_notify_group(gf->gs, g, GRAPH_EVENT_GROUP_ADDED, def, NULL);
	graphs_notify_component(gf->gs, def);
	set_message(msg, "");
	return GRAPH_FILES_OK;
}

enum graph_files_status type_files_remove_group(GraphFiles *gf, const char *graph,
						const char *name, GraphsMessage *msg)
{
	Graph *g = group_graph(gf, graph, msg);
	if (g == NULL)
		return GRAPH_FILES_ERR_INVALID;
	CompositeMessage cm;
	enum composite_status st = composite_types_unregister(gf->gs, name, &cm);
	if (st != COMPOSITE_OK)
		return refused(msg, st, &cm);
	(void)type_files_delete(gf, name); /* a failure is logged; the removal stands */
	sync_component_graph(gf, name, NULL);
	g = graphs_find(gf->gs, graph); /* the context may be the graph just synced */
	graphs_notify_group(gf->gs, g, GRAPH_EVENT_GROUP_REMOVED, NULL, name);
	set_message(msg, "");
	return GRAPH_FILES_OK;
}

enum graph_files_status type_files_rename_group(GraphFiles *gf, const char *graph,
						const char *from, const char *to,
						GraphsMessage *msg)
{
	Graph *g = group_graph(gf, graph, msg);
	if (g == NULL)
		return GRAPH_FILES_ERR_INVALID;
	CompositeMessage cm;
	enum composite_status st = composite_types_rename_check(gf->gs, from, to, &cm);
	if (st != COMPOSITE_OK)
		return refused(msg, st, &cm);
	const BlockTypeDef *def = composite_named(from);
	/* The file under the new name first: the model changes only once it is
	 * written. */
	cJSON *doc = composite_types_document(def);
	enum graph_files_status fs = GRAPH_FILES_ERR_NO_MEMORY;
	if (doc != NULL && replace_str(doc, "name", to))
		fs = write_doc(gf, to, doc);
	cJSON_Delete(doc);
	if (fs != GRAPH_FILES_OK)
		return unsaved(msg, fs, to);
	st = composite_types_rename(gf->gs, from, to, &cm);
	if (st != COMPOSITE_OK) {
		(void)type_files_delete(gf, to);
		return refused(msg, st, &cm);
	}
	(void)type_files_delete(gf, from); /* a failure is logged; the rename stands */
	sync_component_graph(gf, from, NULL);
	sync_component_graph(gf, to, def);
	g = graphs_find(gf->gs, graph); /* the context may be the graph just synced */
	graphs_notify_group(gf->gs, g, GRAPH_EVENT_GROUP_RENAMED, def, from);
	graphs_notify_component(gf->gs, def);
	set_message(msg, "");
	return GRAPH_FILES_OK;
}

enum graph_files_status type_files_change_group(GraphFiles *gf, const char *graph,
						const char *name, const char *description,
						const char *icon, GraphsMessage *msg)
{
	Graph *g = group_graph(gf, graph, msg);
	if (g == NULL)
		return GRAPH_FILES_ERR_INVALID;
	const BlockTypeDef *def = composite_named(name);
	if (def == NULL) {
		if (msg != NULL) {
			GraphRefusal r = {.name = name};
			graph_refusal_message(GRAPH_ERR_UNKNOWN_COMPONENT, &r, msg->text,
					      sizeof(msg->text));
		}
		return GRAPH_FILES_ERR_INVALID;
	}
	/* The file with the new values first, as for a rename. */
	cJSON *doc = composite_types_document(def);
	enum graph_files_status fs = GRAPH_FILES_ERR_NO_MEMORY;
	if (doc != NULL && (description == NULL || replace_str(doc, "description", description)) &&
	    (icon == NULL || replace_str(doc, "icon", icon)))
		fs = write_doc(gf, name, doc);
	cJSON_Delete(doc);
	if (fs != GRAPH_FILES_OK)
		return unsaved(msg, fs, name);
	CompositeMessage cm;
	enum composite_status st = composite_types_change(name, description, icon, &cm);
	if (st != COMPOSITE_OK) {
		(void)type_files_save(gf, def); /* the previous values again */
		return refused(msg, st, &cm);
	}
	sync_component_graph(gf, name, def);
	g = graphs_find(gf->gs, graph); /* the context may be the graph just synced */
	graphs_notify_group(gf->gs, g, GRAPH_EVENT_GROUP_CHANGED, def, NULL);
	graphs_notify_component(gf->gs, def);
	set_message(msg, "");
	return GRAPH_FILES_OK;
}

/* ---- component:source (02 §3.5) --------------------------------------------------- */

/* Whether the request's name (with its library, if any) names the type full. */
static bool source_name_matches(const char *name, const char *library, const char *full)
{
	if (strcmp(name, full) == 0)
		return true;
	size_t n = library != NULL ? strlen(library) : 0;
	return n > 0 && strncmp(full, library, n) == 0 && full[n] == '/' &&
	       strcmp(full + n + 1, name) == 0;
}

enum graph_files_status type_files_add_source(GraphFiles *gf, const char *name,
					      const char *library, const char *code, size_t len,
					      GraphsMessage *msg)
{
	const char *end = NULL;
	cJSON *doc = cJSON_ParseWithLengthOpts(code, len, &end, false);
	while (doc != NULL && end < code + len &&
	       (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
		end++;
	if (doc == NULL || !cJSON_IsObject(doc) || end != code + len) {
		cJSON_Delete(doc);
		set_message(msg, "Invalid JSON");
		return GRAPH_FILES_ERR_INVALID;
	}
	const char *full = doc_name(doc);
	if (full == NULL || !source_name_matches(name, library, full)) {
		cJSON_Delete(doc);
		set_message(msg, "Invalid field: name");
		return GRAPH_FILES_ERR_INVALID;
	}
	const Graph *g = graphs_find(gf->gs, full);
	if (g != NULL && !g->component) {
		if (msg != NULL)
			snprintf(msg->text, sizeof(msg->text), "Graph exists: %.64s", full);
		cJSON_Delete(doc);
		return GRAPH_FILES_ERR_INVALID;
	}
	CompositeMessage cm;
	const BlockTypeDef *def;
	enum composite_status st = composite_types_register(doc, &cm, &def);
	cJSON_Delete(doc);
	if (st != COMPOSITE_OK)
		return refused(msg, st, &cm);
	enum graph_files_status fs = type_files_save(gf, def);
	if (fs != GRAPH_FILES_OK) {
		char kept[GRAPH_ID_MAX + 1];
		snprintf(kept, sizeof(kept), "%s", def->name);
		/* Just registered, so not in use: this cannot fail. */
		(void)composite_types_unregister(gf->gs, kept, NULL);
		return unsaved(msg, fs, kept);
	}
	sync_component_graph(gf, def->name, def);
	graphs_notify_component(gf->gs, def);
	set_message(msg, "");
	return GRAPH_FILES_OK;
}

/* ---- Component graphs (02 §3.4.3) ------------------------------------------------- */

/* "Block.port" split on the last '.' into node and port (buf holds both). */
static bool split_ref(const char *ref, char *buf, size_t size, const char **node,
		      const char **port)
{
	if (ref == NULL || strlen(ref) >= size)
		return false;
	strcpy(buf, ref);
	char *dot = strrchr(buf, '.');
	if (dot == NULL)
		return false;
	*dot = '\0';
	*node = buf;
	*port = dot + 1;
	return true;
}

/* The type of input port of node in g ("" if unknown). */
static const char *input_type(const Graph *g, const char *node, const char *port)
{
	const Block *b = graph_find_block(g, node);
	int32_t i = b != NULL && b->type != NULL ? block_input_index(b, port) : -1;
	return i >= 0 ? b->type->inputs[i].type : "";
}

/* Positions for a type's blocks, which a type file does not keep (03 §5.1):
 * a grid, four to a row. */
static BlockMetadata grid_position(size_t i)
{
	BlockMetadata md = block_metadata_default();
	md.x = BLOCK_DEFAULT_POSITION + (double)(i % 4) * 260.0;
	md.y = BLOCK_DEFAULT_POSITION + (double)(i / 4) * 260.0;
	return md;
}

static enum graph_files_status build_component_graph(GraphFiles *gf, const BlockTypeDef *def);

enum graph_files_status type_files_component_graph(GraphFiles *gf, const BlockTypeDef *def)
{
	graphs_quiet(gf->gs, true);
	enum graph_files_status st = build_component_graph(gf, def);
	graphs_quiet(gf->gs, false);
	return st;
}

/* item's string member key, or NULL. */
static const char *member(const cJSON *item, const char *key)
{
	return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, key));
}

/* The type file status of a refused graph operation st. */
static enum graph_files_status files_status(enum graph_status st)
{
	return st == GRAPH_ERR_NO_MEMORY ? GRAPH_FILES_ERR_NO_MEMORY : GRAPH_FILES_ERR_INVALID;
}

/* The connections, initials and ports of def's document into the component
 * graph name, which has its blocks. */
static enum graph_status build_wiring(GraphFiles *gf, const Graph *g, const cJSON *doc,
				      GraphsMessage *msg)
{
	const char *name = g->id;
	char a[2 * GRAPH_ID_MAX + 2], b[2 * GRAPH_ID_MAX + 2];
	const char *an, *ap, *bn, *bp;
	enum graph_status st = GRAPH_OK;
	const cJSON *item;
	cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(doc, "connections"))
	{
		if (!split_ref(member(item, "src"), a, sizeof(a), &an, &ap) ||
		    !split_ref(member(item, "tgt"), b, sizeof(b), &bn, &bp))
			return GRAPH_ERR_INVALID_ARGUMENT;
		st = graphs_add_edge(gf->gs, name, an, ap, bn, bp, NULL, msg);
		if (st != GRAPH_OK)
			return st;
	}
	cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(doc, "initials"))
	{
		if (!split_ref(member(item, "tgt"), b, sizeof(b), &bn, &bp))
			return GRAPH_ERR_INVALID_ARGUMENT;
		Linkable v;
		const cJSON *data = cJSON_GetObjectItemCaseSensitive(item, "src");
		st = graph_json_to_initial(data, input_type(g, bn, bp), &v);
		if (st != GRAPH_OK)
			return st;
		st = graphs_add_initial(gf->gs, name, bn, bp, &v, msg);
		linkable_clear(&v);
		if (st != GRAPH_OK)
			return st;
	}
	static const char *const keys[2] = {"inports", "outports"};
	static const char *const refs[2] = {"tgt", "src"};
	for (int dir = GRAPH_INPORT; dir <= GRAPH_OUTPORT; dir++) {
		cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(doc, keys[dir]))
		{
			if (!split_ref(member(item, refs[dir]), b, sizeof(b), &bn, &bp))
				return GRAPH_ERR_INVALID_ARGUMENT;
			st = graphs_add_port(gf->gs, name, (enum graph_port_dir)dir,
					     member(item, "name"), bn, bp, msg);
			if (st != GRAPH_OK)
				return st;
		}
	}
	return GRAPH_OK;
}

static enum graph_files_status build_component_graph(GraphFiles *gf, const BlockTypeDef *def)
{
	cJSON *doc = composite_types_document(def);
	if (doc == NULL)
		return GRAPH_FILES_ERR_NO_MEMORY;
	const char *name = def->name;
	GraphsMessage msg;
	Graph *g = NULL;
	enum graph_status st = graphs_new_component(gf->gs, name, name, def->description,
						    def->icon, &g, &msg);
	const cJSON *item;
	size_t i = 0;
	cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(doc, "blocks"))
	{
		if (st != GRAPH_OK)
			break;
		BlockMetadata md = grid_position(i++);
		st = graphs_add_node(gf->gs, name, member(item, "name"), member(item, "type"), &md,
				     &msg);
	}
	if (st == GRAPH_OK)
		st = build_wiring(gf, g, doc, &msg);
	cJSON_Delete(doc);
	if (st != GRAPH_OK) {
		log_error("component graph %s: %s", name, msg.text);
		if (g != NULL)
			(void)graphs_delete_graph(gf->gs, name, g->name, NULL);
		return files_status(st);
	}
	graphs_mark_saved(gf->gs, g, false);
	return GRAPH_FILES_OK;
}

size_t type_files_component_graphs(GraphFiles *gf)
{
	size_t made = 0;
	for (size_t i = 0; i < block_types_count(); i++) {
		const BlockTypeDef *def = block_types_at(i);
		if (composite_types_owns(def) && graphs_find(gf->gs, def->name) == NULL &&
		    type_files_component_graph(gf, def) == GRAPH_FILES_OK)
			made++;
	}
	return made;
}

/* The component graph called id, or NULL with its refusal in msg: "Invalid
 * id", "Unknown graph: <id>", or "Not a component: <id>" for a process. */
static Graph *component_graph(const GraphFiles *gf, const char *id, GraphsMessage *msg)
{
	Graph *g = group_graph(gf, id, msg);
	if (g != NULL && !g->component) {
		if (msg != NULL)
			snprintf(msg->text, sizeof(msg->text), "Not a component: %.64s", id);
		return NULL;
	}
	return g;
}

enum graph_files_status type_files_clear_component(GraphFiles *gf, const char *id,
						   const char *name, const char *description,
						   const char *icon, Graph **out,
						   GraphsMessage *msg)
{
	*out = NULL;
	Graph *g = graphs_find(gf->gs, id);
	GraphRefusal r = {.graph = id};
	enum graph_status st;
	if (!graph_id_valid(id)) {
		st = GRAPH_ERR_INVALID_ID;
	} else if (g != NULL && !g->component) {
		st = GRAPH_ERR_GRAPH_EXISTS;
	} else if (g == NULL && block_types_get(id) != NULL) {
		/* A built-in type's name, or a type made otherwise. */
		if (msg != NULL)
			snprintf(msg->text, sizeof(msg->text), "Component exists: %.64s", id);
		return GRAPH_FILES_ERR_INVALID;
	} else if (g == NULL) {
		st = graphs_new_component(gf->gs, id, name, description, icon, &g, NULL);
	} else {
		st = graphs_clear_component(gf->gs, id, name, description, icon, NULL);
	}
	if (st != GRAPH_OK) {
		if (msg != NULL)
			graph_refusal_message(st, &r, msg->text, sizeof(msg->text));
		return files_status(st);
	}
	*out = g;
	set_message(msg, "");
	return GRAPH_FILES_OK;
}

enum graph_files_status type_files_persist_component(GraphFiles *gf, const char *id,
						     GraphsMessage *msg)
{
	Graph *g = component_graph(gf, id, msg);
	if (g == NULL)
		return GRAPH_FILES_ERR_INVALID;
	if (g->block_count == 0) {
		if (msg != NULL)
			snprintf(msg->text, sizeof(msg->text), "Empty component: %.64s", id);
		return GRAPH_FILES_ERR_INVALID;
	}
	/* The type is the whole graph: every block, the connections between
	 * them, their initials, and the graph's ports as its ports (03 §5.2 with
	 * every node selected: nothing crosses the selection). */
	const char **nodes = malloc(g->block_count * sizeof(*nodes));
	if (nodes == NULL)
		return GRAPH_FILES_ERR_NO_MEMORY;
	for (uint32_t i = 0; i < g->block_count; i++)
		nodes[i] = g->blocks[i]->name;
	CompositeSelection sel = {.name = id, .description = g->description, .icon = g->icon,
				  .nodes = nodes, .node_count = g->block_count};
	cJSON *doc;
	CompositeMessage cm;
	enum composite_status st = composite_types_assemble(g, &sel, &doc, &cm);
	free(nodes);
	if (st != COMPOSITE_OK)
		return refused(msg, st, &cm);

	/* Replacing the type: refused while it is in use (instances refer to it,
	 * 03 §5); the old definition comes back if the new one is refused. */
	const BlockTypeDef *old = composite_named(id);
	cJSON *old_doc = NULL;
	if (old != NULL) {
		if (composite_types_in_use(gf->gs, old)) {
			cJSON_Delete(doc);
			if (msg != NULL)
				snprintf(msg->text, sizeof(msg->text), "Component in use: %.64s",
					 id);
			return GRAPH_FILES_ERR_INVALID;
		}
		old_doc = composite_types_document(old);
		if (old_doc == NULL) {
			cJSON_Delete(doc);
			return GRAPH_FILES_ERR_NO_MEMORY;
		}
		(void)composite_types_unregister(gf->gs, id, NULL);
	} else if (block_types_get(id) != NULL) {
		cJSON_Delete(doc);
		if (msg != NULL)
			snprintf(msg->text, sizeof(msg->text), "Component exists: %.64s", id);
		return GRAPH_FILES_ERR_INVALID;
	}
	const BlockTypeDef *def;
	st = composite_types_register(doc, &cm, &def);
	cJSON_Delete(doc);
	enum graph_files_status fs = GRAPH_FILES_OK;
	if (st == COMPOSITE_OK) {
		fs = type_files_save(gf, def);
		if (fs != GRAPH_FILES_OK)
			(void)composite_types_unregister(gf->gs, id, NULL);
	}
	if (st != COMPOSITE_OK || fs != GRAPH_FILES_OK) {
		if (old_doc != NULL)
			(void)composite_types_register(old_doc, NULL, NULL);
		cJSON_Delete(old_doc);
		return st != COMPOSITE_OK ? refused(msg, st, &cm) : unsaved(msg, fs, id);
	}
	cJSON_Delete(old_doc);
	graphs_mark_saved(gf->gs, g, false);
	graphs_notify_component(gf->gs, def);
	set_message(msg, "");
	return GRAPH_FILES_OK;
}

enum graph_files_status type_files_delete_component(GraphFiles *gf, const char *id,
						    const char *name, GraphsMessage *msg)
{
	Graph *g = component_graph(gf, id, msg);
	if (g == NULL)
		return GRAPH_FILES_ERR_INVALID;
	if (name == NULL || strcmp(name, g->name) != 0) {
		GraphRefusal r = {.graph = id};
		if (msg != NULL)
			graph_refusal_message(GRAPH_ERR_NAME_MISMATCH, &r, msg->text,
					      sizeof(msg->text));
		return GRAPH_FILES_ERR_INVALID;
	}
	if (composite_named(id) != NULL) {
		CompositeMessage cm;
		enum composite_status st = composite_types_unregister(gf->gs, id, &cm);
		if (st != COMPOSITE_OK)
			return refused(msg, st, &cm); /* "Component in use: <id>" */
		(void)type_files_delete(gf, id); /* a failure is logged; the removal stands */
	}
	(void)graphs_delete_graph(gf->gs, id, name, NULL);
	set_message(msg, "");
	return GRAPH_FILES_OK;
}
