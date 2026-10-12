/*
 * Type files (05 §2.3, 03 §5.1): the composite block types persisted in
 * <datadir>/types/<encoded name>.json, saved, deleted and loaded at startup
 * (05 §1.4 step 6).
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

#endif
