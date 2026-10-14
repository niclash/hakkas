/*
 * Type files (05 §2.3, 03 §5.1): the composite block types persisted in
 * <datadir>/types/<encoded name>.json, saved, deleted and loaded at startup
 * (05 §1.4 step 6); and the group commands (02 §3.4.2), which pair the
 * registry operations of blocktypes/composite_types.h with the files and
 * report their events, as graph_files_persist and _delete do for graphs.
 *
 * Naming: functions are prefixed type_files_; they share GraphFiles (its
 * datadir), the status enum and the file-name encoding with graph files
 * (app/graph_files.h, app/file_names.h).
 *
 * A type file holds the type's authoring JSON in canonical form
 * (composite_types_source, blocktypes/composite_types.h): saving a type and
 * registering the file again round-trips. Writes are atomic
 * (pal_storage_write, 05 §2.3). The name inside the file is authoritative;
 * the file name is only an index.
 *
 * Threading: engine-thread only (01 §3.2).
 */
#ifndef HAKKAS_APP_TYPE_FILES_H
#define HAKKAS_APP_TYPE_FILES_H

#include <stddef.h>

#include "app/graph_files.h"
#include "blocktypes/composite_types.h"
#include "graphs/block_type.h"

/* The largest type file that is loaded, in bytes; a larger one is skipped
 * (GRAPH_FILES_ERR_TOO_BIG) before anything is read. A port's build may
 * define it. */
#ifndef HAKKAS_TYPE_FILE_MAX
#define HAKKAS_TYPE_FILE_MAX (64 * 1024)
#endif

/* Write the composite type def to <datadir>/types/<encoded name>.json,
 * atomically, creating types/ if missing. GRAPH_FILES_ERR_INVALID (def is no
 * composite type, or the path does not fit), _NO_MEMORY, _STORAGE (logged). */
enum graph_files_status type_files_save(const GraphFiles *gf, const BlockTypeDef *def);

/* Remove the file of the type called name (after it was unregistered,
 * 02 §3.4.2 removegroup; or under its old name after a rename). A missing
 * file is fine (GRAPH_FILES_OK); another failure is logged
 * (GRAPH_FILES_ERR_STORAGE). GRAPH_FILES_ERR_INVALID for an invalid name. */
enum graph_files_status type_files_delete(const GraphFiles *gf, const char *name);

/*
 * Startup step 6 (05 §1.4): register the composite types of every regular
 * file ending in ".json" in <datadir>/types, in dependency order: in rounds,
 * each round registering, in byte order of the file names, every remaining
 * type whose inner types are all registered, until a round registers none
 * (decided, plan task 21). A file that cannot be read, is not JSON, or is
 * refused when its inner types are all there (composite_types_register) is
 * logged as an error and skipped at once; what is left after the last round
 * (an unknown inner type, a cycle, or a dependency on a skipped type) is
 * logged as an error, with the reason, and skipped. A file whose name is not
 * the encoding of its type's name is loaded with a warning. Returns
 * GRAPH_FILES_OK if every file loaded, else the first failure (a refused or
 * left-over type is GRAPH_FILES_ERR_MALFORMED; GRAPH_FILES_ERR_NOT_FOUND if
 * the directory does not exist). *loaded (if not NULL) is the number of
 * types registered.
 */
enum graph_files_status type_files_load_all(const GraphFiles *gf, size_t *loaded);

/*
 * ---- The group commands (02 §3.4.2; decisions of plan task 21b recorded
 * there) ----
 *
 * Each is issued in the context of the graph graph: it scopes the
 * graph-specific GROUP_* event (graphs_notify_group); the system-wide
 * COMPONENT event (graphs_notify_component) follows it for a new, renamed or
 * changed type. A refused command changes nothing, writes no file and
 * reports nothing: GRAPH_FILES_ERR_INVALID with the wire message in msg (if
 * not NULL), "Invalid id" / "Unknown graph: <graph>" first, then the
 * command's own refusals. If the type file cannot be written, the command is
 * refused as a whole too ("Unable to save component: <name>", the status of
 * the failed save, logged): the model and storage stay consistent. Out of
 * memory: GRAPH_FILES_ERR_NO_MEMORY, "Out of memory".
 */

/* addgroup: assemble a composite type from sel's nodes of graph (03 §5.2;
 * the nodes are left unchanged), register it under sel->name, save its
 * file. Refused, in this order: sel->name is no valid type name ("Invalid
 * field: name") or already registered ("Component exists: <name>"); the
 * assembly ("Invalid field: nodes" for no node, "Unknown node: <node>");
 * every refusal of the registration (03 §5.1, e.g. "Unknown component:
 * <type>" for a selected placeholder). If the file cannot be written, the
 * type is unregistered again. Reports GROUP_ADDED, then COMPONENT. */
enum graph_files_status type_files_add_group(GraphFiles *gf, const char *graph,
					     const CompositeSelection *sel, GraphsMessage *msg);

/* removegroup: unregister the type called name and delete its file.
 * Refused: "Unknown component: <name>" (also a built-in type), "Component
 * in use: <name>". A file that cannot be deleted is logged and the removal
 * stands (like runtime:delete_graph's file, 05 §2.3). Reports GROUP_REMOVED
 * (no COMPONENT event: the FBP protocol has no removal message). */
enum graph_files_status type_files_remove_group(GraphFiles *gf, const char *graph,
						const char *name, GraphsMessage *msg);

/* renamegroup: rename the type from to to. Refused, in this order:
 * "Unknown component: <from>", "Component in use: <from>", "Invalid field:
 * to", "Component exists: <to>". The file under the new name is written
 * first, then the type renamed (moving to the end of the registration
 * order), then the old file deleted (a failure there is logged and the
 * rename stands). Reports GROUP_RENAMED, then COMPONENT. */
enum graph_files_status type_files_rename_group(GraphFiles *gf, const char *graph,
						const char *from, const char *to,
						GraphsMessage *msg);

/* changegroup: set the type's description and, if icon is not NULL, its
 * icon; allowed while the type is in use. Refused: "Unknown component:
 * <name>" (also a built-in type). The file with the new values is written
 * first, then the type changed. Reports GROUP_CHANGED, then COMPONENT. */
enum graph_files_status type_files_change_group(GraphFiles *gf, const char *graph,
						const char *name, const char *description,
						const char *icon, GraphsMessage *msg);

/*
 * ---- Component graphs (02 §3.4.3; decided with the user 2026-10-14, plan
 * task 36) ----
 *
 * A component graph (graphs/graphs.h graphs_new_component) is the editable
 * definition of a composite type, its id the type's name: edited with the
 * graph commands, it becomes the type at network:persist. Every composite
 * type has one: made at startup for each type file, and kept in step by the
 * group commands.
 */

/* Build the component graph of the composite type def from its definition:
 * its blocks (on a grid: a type keeps no positions), connections, initials
 * and ports as graph ports; saved (it matches its file). Built quietly
 * (graphs_quiet): it mirrors the type, so no client sees its blocks being
 * added. GRAPH_FILES_ERR_INVALID if it cannot be built (logged; a
 * graph with def's name exists already, say), _NO_MEMORY. */
enum graph_files_status type_files_component_graph(GraphFiles *gf, const BlockTypeDef *def);

/* Startup, after type_files_load_all (05 §1.4 step 6): the component graph
 * of every composite type that has none. Returns how many were made. */
size_t type_files_component_graphs(GraphFiles *gf);

/* graph:clear with "main": false: create the component graph id (empty), or
 * empty the existing one, setting name, description and icon (NULL keeps
 * one, or gives the defaults of graphs_new_graph). Refused: "Invalid id",
 * "Graph exists: <id>" (a process has that id), "Component exists: <id>" (a
 * block type of that name has no component graph: a built-in type). *out is
 * the graph. */
enum graph_files_status type_files_clear_component(GraphFiles *gf, const char *id,
						   const char *name, const char *description,
						   const char *icon, Graph **out,
						   GraphsMessage *msg);

/* network:persist on a component graph: assemble the type from the whole
 * graph — its blocks, the connections between them, their initials, and its
 * graph ports as the type's ports (03 §5.2 with every node selected) — and
 * register it under the graph's id, replacing the type of that name, then
 * write its type file and report COMPONENT. Refused, changing nothing: "Not a
 * component: <id>" (no component graph; graph_files_persist saves processes),
 * "Empty component: <id>" (no block), "Component in use: <id>" (instances or
 * containing types refer to the old type), "Component exists: <id>" (a
 * built-in type), every refusal of the registration (03 §5.1, e.g.
 * "Recursive component: <id>"), "Unable to save component: <id>". */
enum graph_files_status type_files_persist_component(GraphFiles *gf, const char *id,
						     GraphsMessage *msg);

/* runtime:delete_graph on a component graph: unregister its type and delete
 * its file, then delete the graph (GRAPH_DELETED). name must be the graph's
 * name ("Graph name mismatch: <id>"); refused while the type is in use
 * ("Component in use: <id>"). */
enum graph_files_status type_files_delete_component(GraphFiles *gf, const char *id,
						    const char *name, GraphsMessage *msg);

#endif
