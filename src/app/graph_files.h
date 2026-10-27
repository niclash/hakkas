/*
 * Graph files (05 §2): saving a graph to its canonical JSON file, loading
 * graph files into the Graphs registry, persist (02 §3.3 network:persist)
 * and delete (02 §3.2 runtime:delete_graph) with their files.
 *
 * Naming: functions are prefixed graph_files_, the status enum
 * GRAPH_FILES_ / graph_files_status.
 *
 * Storage goes through the PAL only (platform/pal.h, 01 §0.1). Files live in
 * <datadir>/graphs/<encoded id>.json (05 §2.3, app/file_names.h); composite
 * types in <datadir>/types/ are app/type_files.h's. A process's environment
 * bindings are saved with it in <datadir>/bindings/<encoded id>.json (a
 * bindings file, 04 §4) and the links between processes in
 * <datadir>/links.json (app/links_file.h; plan task 62).
 *
 * The format (05 §2.1, decisions of plan task 10 recorded there): a compact
 * JSON object with the keys id, name, description, icon, started, blocks,
 * connections, inports, outports in that order; a block reference is
 * {"block","port"} (hakkas' terms; the wire's FBP form is {"node","port"},
 * 02 §3.4); numbers are exact (graphs/graph_json.h). Blocks in creation
 * order. Connections per target block (creation order) and input (port
 * order): its initial or its edge; then the retained connections of
 * placeholder blocks in load order. Graph ports: the live ones in creation
 * order, then the retained ones.
 *
 * Round trip: saving is a fixed point of loading. A file this code wrote
 * loads into a graph that, started as the file's flag says (loading leaves
 * it stopped, below), saves to the same bytes; the first save of any
 * other valid file is its canonical form: the same graph, but in the order
 * above, compact, numbers in their shortest exact form, initials in the
 * types they were parsed into (02 §3.4: "5" on a number port becomes 5, an
 * integer becomes an f64).
 *
 * Loading (05 §2.2) goes through the Graphs registry and its commands, so a
 * file entry is refused exactly as the equivalent wire command would be, and
 * loading reports the same events those commands report: graph created,
 * then each block, edge, initial and graph port. Loading never starts a
 * graph (decided, plan task 21e; 00-README open question 12): the file's
 * started flag is handed to the caller, the host, which decides what to
 * start (hakkas.h, 05 §1.2).
 *
 * Threading: engine-thread only (01 §3.2).
 */
#ifndef HAKKAS_APP_GRAPH_FILES_H
#define HAKKAS_APP_GRAPH_FILES_H

#include <stdbool.h>
#include <stddef.h>

#include "graphs/graphs.h"
#include "hakkas_env.h"
#include "platform/pal.h"

/* The largest graph file that is loaded, in bytes (05 §2.2); a larger one is
 * refused as GRAPH_FILES_ERR_TOO_BIG before anything is read. A port's build
 * may define it. The parsed tree takes several times the file's size. */
#ifndef HAKKAS_GRAPH_FILE_MAX
#define HAKKAS_GRAPH_FILE_MAX (256 * 1024)
#endif

/* The subdirectories of datadir (05 §2.3). */
#define GRAPH_FILES_GRAPHS_DIR "graphs"
#define GRAPH_FILES_TYPES_DIR "types"
#define GRAPH_FILES_BINDINGS_DIR "bindings"

enum graph_files_status {
	GRAPH_FILES_OK = 0,
	GRAPH_FILES_ERR_INVALID = -1,	/* an invalid id, or a path too long for PAL_PATH_MAX */
	GRAPH_FILES_ERR_NOT_FOUND = -2, /* no such file, directory or graph */
	GRAPH_FILES_ERR_TOO_BIG = -3,	/* the file is larger than HAKKAS_GRAPH_FILE_MAX */
	GRAPH_FILES_ERR_MALFORMED = -4, /* not a graph document (see graph_files_load_text) */
	GRAPH_FILES_ERR_EXISTS = -5,	/* a graph with the file's id is already loaded */
	GRAPH_FILES_ERR_STORAGE = -6,	/* a storage operation failed (logged with its cause) */
	GRAPH_FILES_ERR_NO_MEMORY = -7,
};

/* A short, static description of st, for log lines. */
const char *graph_files_status_str(enum graph_files_status st);

/* The graph files of one Graphs registry under one datadir. */
typedef struct {
	Graphs *gs;
	char datadir[PAL_PATH_MAX];
	/* The running environment's name, which a saved bindings file names
	 * (04 §4); "" without an environment. */
	char environment[HAKKAS_ENV_ID_MAX + 1];
} GraphFiles;

/* Make gf the graph files of gs under datadir (copied; 05 §1.1 datadir).
 * False if datadir is NULL, empty or too long to build any file path. */
bool graph_files_init(GraphFiles *gf, Graphs *gs, const char *datadir);

/* The path of subdir (GRAPH_FILES_GRAPHS_DIR or GRAPH_FILES_TYPES_DIR) into
 * buf (size bytes): <datadir>/<subdir>. For the .tmp cleanup at startup
 * (05 §1.4 step 2, pal_storage_remove_tmp). GRAPH_FILES_ERR_INVALID if it
 * does not fit. */
enum graph_files_status graph_files_dir(const GraphFiles *gf, const char *subdir, char *buf,
					size_t size);

/* The path of the file for name (a graph id, or a type name) in subdir:
 * <datadir>/<subdir>/<encoded name>.json (05 §2.3). GRAPH_FILES_ERR_INVALID
 * if name is no valid id (1..64 bytes) or the path, with an atomic write's
 * ".tmp", does not fit PAL_PATH_MAX or size. */
enum graph_files_status graph_files_path(const GraphFiles *gf, const char *subdir,
					 const char *name, char *buf, size_t size);

/* Set the environment's name for saved bindings files (cut to
 * HAKKAS_ENV_ID_MAX bytes; NULL: none). */
void graph_files_set_environment(GraphFiles *gf, const char *name);

/* The path of <datadir>/<name> (a file directly in datadir, e.g.
 * LINKS_FILE_NAME) into buf: GRAPH_FILES_ERR_INVALID if it does not fit,
 * with an atomic write's ".tmp". */
enum graph_files_status graph_files_top_path(const GraphFiles *gf, const char *name, char *buf,
					     size_t size);

/* Create <datadir>/graphs and <datadir>/types if missing (datadir itself
 * must exist). GRAPH_FILES_ERR_STORAGE (logged) on failure. */
enum graph_files_status graph_files_make_dirs(const GraphFiles *gf);

/* ---- Saving (05 §2.1) ------------------------------------------------------ */

/* g's canonical JSON text: a heap string (free with free()) of *len bytes
 * plus a NUL, or NULL if memory runs out. */
char *graph_files_serialize(const Graph *g, size_t *len);

/* Write g to its file, atomically (pal_storage_write, 05 §2.3), including its
 * current started state. GRAPH_FILES_ERR_INVALID, _NO_MEMORY, _STORAGE
 * (logged). */
enum graph_files_status graph_files_save(const GraphFiles *gf, const Graph *g);

/* g's environment bindings as a bindings file (04 §4: {"environment",
 * "bindings": [{"block", "port", "env", "index"?}]}, in creation order): a
 * heap string of *len bytes plus a NUL, *count (if not NULL) the bindings,
 * or NULL if memory runs out. */
char *graph_files_serialize_bindings(const GraphFiles *gf, const Graph *g, size_t *len,
				     size_t *count);

/* Write every link between processes to <datadir>/links.json (removed when
 * there is none), atomically: with every save, and after every wire command
 * that changed the links (02 §3.8, plan task 79). GRAPH_FILES_ERR_INVALID,
 * _NO_MEMORY, _STORAGE (logged). */
enum graph_files_status graph_files_save_links(const GraphFiles *gf);

/* network:persist (02 §3.3): save the graph with this id — its graph file,
 * its bindings file (removed when it has no bindings) and the links file
 * with every link between processes (removed when there is none; plan task
 * 62). A refused command
 * (an invalid id: "Invalid id"; no such graph: "Unknown graph: <id>") changes
 * nothing and reports nothing: the message goes to the requester only, like
 * every refusal (02 §3.4.1). A failed save reports an ERROR event (system-
 * wide, 02 §3.3) with the message "Unable to save graph: <name> (<id>)" —
 * also when its bindings or the links could not be written. On
 * any failure msg (if not NULL) holds the message (cut to fit). Success
 * reports nothing (no wire reply). */
enum graph_files_status graph_files_persist(GraphFiles *gf, const char *id, GraphsMessage *msg);

/* ---- Loading (05 §2.2) ----------------------------------------------------- */

/*
 * Load the graph document text (len bytes, need not be NUL-terminated) into
 * gf's registry; origin names it in log lines (e.g. its path). On success *out
 * (if not NULL) is the new graph.
 *
 * The minimum valid document is a JSON object (followed by nothing but
 * whitespace; nested no deeper than CJSON_NESTING_LIMIT) with an "id" that is a valid
 * graph id (1..64 bytes). "name", "description" and "icon" are strings
 * (absent: as new_graph's defaults), "started" a boolean (absent: false),
 * "blocks" an object and "connections", "inports", "outports" arrays (each
 * absent: empty). Anything else, or text that is not JSON, refuses the whole
 * document: GRAPH_FILES_ERR_MALFORMED, logged as an error, nothing created. A
 * graph with the same id already loaded: GRAPH_FILES_ERR_EXISTS.
 *
 * Within a valid document, each entry is loaded on its own; an invalid entry
 * is logged as a warning and skipped, the rest still loads:
 *  - a block whose value is not an object with a string "component", or
 *    whose "metadata" is invalid (graph_json_to_metadata), or that addnode
 *    would refuse (an invalid or duplicate name);
 *  - a block whose component is not registered becomes a placeholder
 *    (graph_create_placeholder), with a warning;
 *  - a connection that is not {"src":{"block","port"} or {"data"},
 *    "tgt":{"block","port"}[,"metadata":{...}]} with string names, whose data
 *    is not a string, number or boolean, whose edge metadata is not an object,
 *    or that addedge / addinitial would refuse (unknown node or port, wrong
 *    direction, fan-in: the first connection to an input wins);
 *  - a connection that references a placeholder is retained as loaded
 *    (graph_retain_connection) if both its nodes exist; it is skipped like
 *    the wire command if the port on its real end does not exist with the
 *    right direction, or if the target input already has a source (live or
 *    retained; the first connection to an input wins either way);
 *  - a graph port that is not {"name","block","port"} with string values, or
 *    that addinport / addoutport would refuse, or whose name an earlier port
 *    of the file already has; one mapped to a placeholder is retained
 *    (graph_retain_port).
 * A block reference may use "node" (earlier hakkas files) or "process" (the
 * sister project's files, e.g. examples/example.json) in place of "block";
 * one that has more than one of the three members is invalid (the entry is
 * skipped as above). Other members of a reference, such as the "data" the
 * sister project writes in a connection's tgt, are ignored.
 * An initial's data is parsed for its port (graph_json_to_initial).
 *
 * The graph is left stopped, whatever its "started" flag: *started (if not
 * NULL) is that flag (false if absent, and on any failure), for the caller
 * to act on (05 §2.1: information for the host).
 *
 * GRAPH_FILES_ERR_NO_MEMORY at any point deletes the partly loaded graph.
 */
enum graph_files_status graph_files_load_text(GraphFiles *gf, const char *text, size_t len,
					      const char *origin, Graph **out, bool *started);

/* Load the graph file at path (any path: hakkas_process_load's files need not live
 * in datadir). Read through the PAL into a buffer sized by pal_storage_stat;
 * refused as GRAPH_FILES_ERR_TOO_BIG beyond HAKKAS_GRAPH_FILE_MAX bytes.
 * Otherwise as graph_files_load_text; failures are logged. */
enum graph_files_status graph_files_load(GraphFiles *gf, const char *path, Graph **out,
					 bool *started);

/* Called by graph_files_load_all for each graph loaded, with its file's
 * started flag. It may start the graph. */
typedef void (*graph_files_loaded_fn)(void *ctx, Graph *g, bool started);

/* Every saved graph (`run`, 05 §1.2; hakkas_process_load_saved): load
 * every regular file ending in ".json" in <datadir>/graphs, in byte order of
 * their names (so graphs are created in the same order every boot), calling
 * fn (if not NULL) after each one that loaded. A file whose name is not the
 * encoding of its graph's id is loaded but logged as a warning (persist
 * writes to the id's file name). Every file is tried; returns GRAPH_FILES_OK
 * if all loaded, else the status of the first failure
 * (GRAPH_FILES_ERR_NOT_FOUND if the directory does not exist). *loaded (if
 * not NULL) is the number of graphs loaded. */
enum graph_files_status graph_files_load_all(GraphFiles *gf, graph_files_loaded_fn fn, void *ctx,
					     size_t *loaded);

/* ---- Reading and listing (shared with app/type_files.h) -------------------- */

/* Read the file at path, at most max bytes (else GRAPH_FILES_ERR_TOO_BIG
 * before anything is read), into a new heap buffer *text (free with free())
 * of *len bytes plus a NUL. Through the PAL, sized by pal_storage_stat.
 * GRAPH_FILES_ERR_NOT_FOUND, _STORAGE (also for a directory), _NO_MEMORY;
 * every failure is logged. */
enum graph_files_status graph_files_read(const char *path, size_t max, char **text, size_t *len);

/* The names of the regular files ending in ".json" in dir, in byte order: a
 * heap array *names of *count heap strings (graph_files_list_free; NULL and
 * 0 for none). GRAPH_FILES_ERR_NOT_FOUND (no such directory), _STORAGE,
 * _NO_MEMORY, logged, with *names NULL and *count 0. */
enum graph_files_status graph_files_list(const char *dir, char ***names, size_t *count);

void graph_files_list_free(char **names, size_t count);

/* ---- Deleting (02 §3.2) ---------------------------------------------------- */

/* runtime:delete_graph: graphs_delete_graph (its refusals, messages and
 * events), then remove <datadir>/graphs/<encoded id>.json. A graph that was
 * never persisted has no file, which is fine; another failure to remove it
 * is logged as an error but does not undo the deletion (the file would load
 * again at the next boot). */
enum graph_status graph_files_delete(GraphFiles *gf, const char *id, const char *name,
				     GraphsMessage *msg);

#endif
