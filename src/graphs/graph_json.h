/*
 * The JSON forms of the graph model's metadata and initial values (05 §2.1,
 * 02 §3.4), shared by graph files (app/graph_files.h) and the wire protocol
 * (plan task 18): node metadata from and to JSON, the changenode merge, edge
 * metadata, initial values, and exact printing.
 *
 * Naming: functions are prefixed graph_json_. JSON values are cJSON trees
 * (third_party/cjson, 01 §6).
 *
 * Opaque metadata (decided, plan task 10): node metadata keys other than x, y
 * and change_threshold, and edge metadata, are kept in the model as compact
 * JSON object text (BlockMetadata.extra, Input.edge_metadata): one heap
 * string per node or edge, sized exactly, with no cJSON tree kept alive
 * (01 §2.2 "Memory"). It is printed by graph_json_print, so its numbers are
 * exact.
 *
 * Numbers (01 §6, third_party/cjson/README.md "Numbers"): cJSON parses
 * numbers exactly but does not print every double exactly, so every tree is
 * printed through graph_json_print, which first turns each number into raw
 * text in linkable_stringify's f64 form (the shortest that reads back to the
 * same double). A number that overflowed to ±Infinity when parsed (such as
 * 1e999, the only way JSON expresses it) is printed as 1e999 / -1e999, which
 * parses back to the same value.
 */
#ifndef HAKKAS_GRAPHS_GRAPH_JSON_H
#define HAKKAS_GRAPHS_GRAPH_JSON_H

#include "cJSON.h"
#include "graphs/graph.h"
#include "graphs/linkable.h"

/* Print item compactly (no whitespace) with exact numbers (see above). Every
 * number in the tree is turned into raw text in place first, so the tree
 * should not be read as numbers afterwards. Returns a heap string, or NULL if
 * memory runs out. Free it with free(): cJSON allocates with malloc, the
 * core's heap (no allocation hooks, 01 §6). */
char *graph_json_print(cJSON *item);

/* A new number item for v, printed exactly by graph_json_print (cJSON_Raw in
 * linkable_stringify's form; ±Infinity as ±1e999). NaN has no JSON form: it
 * gives a null item. NULL if memory runs out. */
cJSON *graph_json_number(double v);

/* ---- Node metadata (05 §2.1, 02 §3.4 addnode / changenode) ------------- */

/* Read a node metadata object into *md: x and y (finite numbers; absent or
 * null: BLOCK_DEFAULT_POSITION), change_threshold (a number >= 0, +Infinity
 * included; absent or null: no override, NAN) and every other key, kept in
 * order as md->extra (NULL if there are none; the caller owns it, release
 * with block_metadata_release). obj NULL gives block_metadata_default().
 * Refused, with *md the default: INVALID_ARGUMENT (obj is no object, or x, y
 * or change_threshold has another type or value), NO_MEMORY. */
enum graph_status graph_json_to_metadata(const cJSON *obj, BlockMetadata *md);

/* The metadata object for md: "x", "y", "change_threshold" if set, then the
 * keys of md->extra in their order. NULL if memory runs out or md->extra is
 * not a JSON object. */
cJSON *graph_json_from_metadata(const BlockMetadata *md);

/* The changenode merge (02 §3.4): the change that merges the keys of patch
 * (an object) into cur. "x", "y": a finite number sets it, null resets it to
 * BLOCK_DEFAULT_POSITION; "change_threshold": a number >= 0 sets the
 * override, null removes it (THRESHOLD_CLEAR); any other key: null removes
 * it, any other value adds it (at the end) or replaces it (in its place).
 * Keys not in patch are kept. *chg is the change to pass to
 * graph_change_block / graphs_change_node; chg->extra is owned by the caller
 * (block_change_release). Refused, with *chg empty: INVALID_ARGUMENT (patch
 * is no object, or a bad x, y or change_threshold), NO_MEMORY. */
enum graph_status graph_json_to_change(const BlockMetadata *cur, const cJSON *patch,
				       BlockChange *chg);

/* ---- Edge metadata (02 §3.4 addedge / changeedge) ---------------------- */

/* The opaque edge metadata obj as text for the model: *out is a heap string
 * (free with free()) for an object, NULL for a NULL item or JSON null
 * (no metadata). Refused: INVALID_ARGUMENT (any other type), NO_MEMORY. */
enum graph_status graph_json_to_edge_metadata(const cJSON *obj, char **out);

/* The edge metadata text of the model (Input.edge_metadata,
 * RetainedConnection.metadata) as a new object item, or NULL if text is
 * NULL, is not a JSON object, or memory runs out. */
cJSON *graph_json_from_edge_metadata(const char *text);

/* ---- Initial values (05 §2.1, 02 §3.4 "Initial value encoding") -------- */

/* The literal of an initial from its JSON data, for an input of port type
 * port_type: a string is parsed by the port's type (linkable_parse), a number
 * becomes an f64, a boolean a bool. Refused, *out unset: INVALID_ARGUMENT
 * (data NULL, null, an array or an object), NO_MEMORY. */
enum graph_status graph_json_to_initial(const cJSON *data, const char *port_type, Linkable *out);

/* The literal v as stored in a graph file (decided, 05 §2.1): a string or a
 * bool natively; a finite f64 as an exact number; NaN, Infinity, -Infinity as
 * those strings; a Metric as its string form "<value> <unit>"; an i64 as an
 * exact JSON integer. Each reads back through graph_json_to_initial (an i64
 * comes back as an f64, like every JSON number). NULL for an unset value or
 * if memory runs out. */
cJSON *graph_json_from_initial(const Linkable *v);

#endif
