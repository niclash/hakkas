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
		if (cJSON_IsString(type) && block_types_get(type->valuestring) == NULL)
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
	graphs_notify_group(gf->gs, g, GRAPH_EVENT_GROUP_CHANGED, def, NULL);
	graphs_notify_component(gf->gs, def);
	set_message(msg, "");
	return GRAPH_FILES_OK;
}
