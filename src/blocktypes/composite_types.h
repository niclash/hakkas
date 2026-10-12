/*
 * Composite block types — groups (03 §5): building a composite type from its
 * authoring JSON (03 §5.1), registering it in the block type registry
 * (blocktypes/block_types.h), the registry operations the group commands of
 * 02 §3.4.2 use (unregister, rename, change description/icon), the
 * canonical authoring JSON for component:getsource (02 §3.5) and the type
 * file (05 §2.3, app/type_files.h), and assembling the authoring JSON from a
 * selection of graph nodes (03 §5.2).
 *
 * Naming: functions are prefixed composite_types_, the status enum
 * COMPOSITE_ / composite_status.
 *
 * Ownership: this module owns every composite type it registers (the
 * BlockTypeDef, its CompositeDef, ports and strings, graphs/block_type.h)
 * until it is unregistered or composite_types_reset runs. A registered type
 * refers to its inner types by pointer, so a type cannot be unregistered or
 * renamed while a graph has an instance of it or another composite type
 * contains it.
 *
 * The authoring format (03 §5.1; decisions of plan task 21 recorded there):
 *
 *   {"name": String, "icon"?: String, "description"?: String,
 *    "blocks": [{"name": String, "type": String}, ...],
 *    "connections"?: [{"src": "Block.port", "tgt": "Block.port"}, ...],
 *    "initials"?: [{"src": String|Number|Boolean, "tgt": "Block.port"}, ...],
 *    "inports"?: [{"name", "tgt", "type"?, "description"?, "addressable"?}, ...],
 *    "outports"?: [{"name", "src", "type"?, "description"?, "addressable"?}, ...]}
 *
 * "connections" may also be given under its former name "edges" (03 §5.1);
 * a document with both is refused ("Invalid field: edges").
 *
 * name is a type name (1..64 bytes, 05 §2.3); inner block names are node ids
 * (1..64 bytes), unique; "Block.port" references split on the last '.'
 * (01 §2.4); public port names are 1..64 bytes without '.' (so a parent
 * composite can reference them), unique per direction. A port's type,
 * description and addressable default to the mapped inner port's; an
 * explicit type is "bool", "number" or "string". An initial's value is
 * parsed for its target port's type (graph_json_to_initial). The exposed
 * input's default is the mapped inner port's.
 *
 * Checks, in this order, the first failure wins (each with its message):
 *  1. the shape above, else COMPOSITE_ERR_INVALID "Invalid field: <key>"
 *     (<key> the top-level key at fault; also an invalid or duplicate name);
 *  2. name not registered, else COMPOSITE_ERR_EXISTS
 *     "Component exists: <name>";
 *  3. per inner block, in order: no recursion — its type is not name and no
 *     composite it contains (transitively) is name — else
 *     COMPOSITE_ERR_RECURSIVE "Recursive component: <name>"; its type is
 *     registered, else COMPOSITE_ERR_UNKNOWN_TYPE "Unknown component: <type>";
 *  4. connections, then initials, then inports, then outports, each in order:
 *     every reference resolves, else COMPOSITE_ERR_UNKNOWN_PORT with
 *     "Unknown node: <block>" / "Unknown port: <block>.<port>" (a port of
 *     the wrong direction: "Invalid edge direction" for an edge or initial,
 *     "Unknown port: ..." for an inport or outport, as the wire's addedge,
 *     addinitial, addinport and addoutport report them, 02 §3.4.1); no inner
 *     input fed twice — by two edges, an edge and an initial, or an inport
 *     and anything else — else COMPOSITE_ERR_FAN_IN "Input already
 *     connected: <block>.<port>";
 *  5. room in the registry, else COMPOSITE_ERR_FULL "Too many block types".
 *
 * Threading: engine-thread only (01 §3.2).
 */
#ifndef HAKKAS_BLOCKTYPES_COMPOSITE_TYPES_H
#define HAKKAS_BLOCKTYPES_COMPOSITE_TYPES_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "graphs/block_type.h"
#include "graphs/graphs.h"

enum composite_status {
	COMPOSITE_OK = 0,
	COMPOSITE_ERR_INVALID = -1,	 /* not a type definition (JSON or shape) */
	COMPOSITE_ERR_EXISTS = -2,	 /* the name is a registered type */
	COMPOSITE_ERR_RECURSIVE = -3,	 /* the type would contain itself */
	COMPOSITE_ERR_UNKNOWN_TYPE = -4, /* an inner type is not registered */
	COMPOSITE_ERR_UNKNOWN_PORT = -5, /* a reference names no such block or port */
	COMPOSITE_ERR_FAN_IN = -6,	 /* an inner input would be fed twice */
	COMPOSITE_ERR_FULL = -7,	 /* the registry is full */
	COMPOSITE_ERR_IN_USE = -8,	 /* an instance or another composite uses the type */
	COMPOSITE_ERR_NOT_FOUND = -9,	 /* no composite type of that name */
	COMPOSITE_ERR_NO_MEMORY = -10,
};

/* Room for any message (a name in it may be cut). */
#define COMPOSITE_MESSAGE_MAX 192

typedef struct {
	char text[COMPOSITE_MESSAGE_MAX];
} CompositeMessage;

/* ---- Registration (03 §5.1) ----------------------------------------------- */

/* Build the composite type that doc describes and register it (the checks
 * above). On success *out (if not NULL) is the registered type; on failure
 * msg (if not NULL) holds the message and nothing changed. */
enum composite_status composite_types_register(const cJSON *doc, CompositeMessage *msg,
					       const BlockTypeDef **out);

/* The same from JSON text (len bytes, need not be NUL-terminated): text that
 * is not one JSON object is COMPOSITE_ERR_INVALID "Invalid JSON". */
enum composite_status composite_types_register_text(const char *text, size_t len,
						    CompositeMessage *msg,
						    const BlockTypeDef **out);

/* Whether def is a composite type registered by this module. */
bool composite_types_owns(const BlockTypeDef *def);

/* The number of registered composite types. */
size_t composite_types_count(void);

/* Unregister and free every composite type (shutdown, tests): no instance
 * may exist any more. */
void composite_types_reset(void);

/* ---- Registry operations (02 §3.4.2; the wire commands are
 * app/type_files.h's group commands) ------------------------------------------ */

/* Whether a graph of gs has an instance of def (a block of the graph whose
 * type is def) or another registered composite type contains it as an inner
 * block. */
bool composite_types_in_use(const Graphs *gs, const BlockTypeDef *def);

/* removegroup: unregister and free the composite type called name. Refused:
 * COMPOSITE_ERR_NOT_FOUND "Unknown component: <name>" (also for a built-in
 * type), COMPOSITE_ERR_IN_USE "Component in use: <name>". Deleting its file
 * is app/type_files.h's. */
enum composite_status composite_types_unregister(const Graphs *gs, const char *name,
						 CompositeMessage *msg);

/* renamegroup: rename the composite type from to to (unregister plus
 * register, so it moves to the end of the registration order, 01 §2.3).
 * Refused: NOT_FOUND (from), IN_USE (instances and containing types refer
 * to their type, 02 §3.4.2), INVALID "Invalid field: to" (to is no valid
 * name), EXISTS (to is registered, from itself included), NO_MEMORY. */
enum composite_status composite_types_rename(const Graphs *gs, const char *from, const char *to,
					     CompositeMessage *msg);

/* The checks of composite_types_rename alone, in its order, changing
 * nothing: COMPOSITE_OK if the rename would be applied (it can then fail
 * only with NO_MEMORY). renamegroup writes the renamed type's file before
 * the model changes (app/type_files.h). */
enum composite_status composite_types_rename_check(const Graphs *gs, const char *from,
						   const char *to, CompositeMessage *msg);

/* changegroup: set the description and/or icon (NULL keeps one) of the
 * composite type called name. Allowed while the type is in use: no instance
 * depends on them (decided, plan task 21). Refused: NOT_FOUND, NO_MEMORY. */
enum composite_status composite_types_change(const char *name, const char *description,
					     const char *icon, CompositeMessage *msg);

/* ---- The authoring JSON (03 §5.1, 02 §3.5 getsource, 05 §2.3) -------------- */

/* def's authoring document in canonical form (a plain tree: it can be
 * registered as is; print it with graph_json_print): the keys name, icon,
 * description, blocks, connections, initials, inports, outports in that
 * order, every one present; blocks, connections and initials in the
 * definition's order; references "Block.port"; initial values as graph files
 * store them (graph_json_from_initial); a port's type, description and
 * addressable only if they were given explicitly. NULL if def is not a composite type of this
 * module or memory runs out. */
cJSON *composite_types_document(const BlockTypeDef *def);

/* The document printed compactly with exact numbers (graph_json_print): a
 * heap string (free with free()), *len (if not NULL) its length. Registering
 * this text yields the same document again (the round trip of 03 §5.1). */
char *composite_types_source(const BlockTypeDef *def, size_t *len);

/* ---- Creating a group from a selection (03 §5.2) ---------------------------- */

typedef struct {
	const char *name; /* the new type's name */
	const char *description;
	const char *icon;
	const char *const *nodes; /* the selected node names */
	size_t node_count;
} CompositeSelection;

/*
 * Assemble the authoring document for the selected nodes of g (03 §5.2;
 * decisions of plan task 21 recorded there). Nothing is registered: the
 * result registers like a hand-authored document (composite_types_register).
 *  - blocks: the selected nodes {name, type} in the graph's creation order
 *    (a node named twice counts once);
 *  - connections: every edge between two selected nodes; initials: every
 *    initial on a selected node; per target node (creation order) and input
 *    (port order), as in a graph file (05 §2.1);
 *  - inports: first every graph-level inport mapped to a selected node, under
 *    its public name, in creation order (only the first of several mapped to
 *    the same input, which would feed it twice); then one per input of a
 *    selected node fed by an edge from outside the selection (per node and
 *    input as above) that no graph-level inport exposes already;
 *  - outports: first every graph-level outport mapped to a selected node,
 *    under its public name, in creation order; then one per output of a
 *    selected node with an edge to a node outside the selection (in the
 *    order those edges are found, per target node and input) that no
 *    graph-level outport exposes already;
 *  - a port without a public name is named after its inner port; when that
 *    clashes with another port of the same direction (a public name, or
 *    another such port with the same inner port name), it is
 *    "<block>_<port>" instead, and if that still clashes or is longer than 64
 *    bytes, "<port>_<n>" (else "port_<n>") with the smallest n >= 2 that is
 *    free;
 *  - name, description, icon from sel (NULL: "" for description and icon);
 *    node metadata, edge metadata and the placeholders' retained connections
 *    are not part of a type.
 * Refused: COMPOSITE_ERR_INVALID "Invalid field: nodes" (no node selected),
 * COMPOSITE_ERR_UNKNOWN_PORT "Unknown node: <name>" (a selected name is no
 * node of g), COMPOSITE_ERR_NO_MEMORY.
 */
enum composite_status composite_types_assemble(const Graph *g, const CompositeSelection *sel,
					       cJSON **out, CompositeMessage *msg);

#endif
