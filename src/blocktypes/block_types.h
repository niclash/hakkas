/*
 * The block type registry (01 §3.5, 03 §5.1): every block type known to the
 * engine, by name, in registration order (the order component:list reports
 * them in, 02 §3.5).
 *
 * Naming: functions are prefixed block_types_, the status enum
 * BLOCK_TYPES_ / block_types_err.
 *
 * Storage: a fixed table of HAKKAS_BLOCK_TYPES_MAX pointers inside the
 * registry (no heap), default 128; a port's build may define the macro to
 * change it. The registry does not own the definitions: built-ins are static
 * const data (block_types_register_builtins registers them at startup);
 * composite types (03 §5, plan task 21) are registered at runtime and their
 * owner keeps each definition alive while it is registered.
 *
 * Lookup is a linear scan with strcmp: the table is small, and lookups happen
 * on graph edits and loads, never while values propagate.
 *
 * Removing a composite type (02 §3.4.2 removegroup) uses
 * block_types_unregister; the caller checks first that no graph or composite
 * still uses it. Renaming (renamegroup) is unregister followed by registering
 * the renamed definition, which moves the type to the end of the order.
 *
 * Threading: engine-thread only (01 §3.2), no locking.
 */
#ifndef HAKKAS_BLOCKTYPES_BLOCK_TYPES_H
#define HAKKAS_BLOCKTYPES_BLOCK_TYPES_H

#include <stdbool.h>
#include <stddef.h>

#include "graphs/block_type.h"

#ifndef HAKKAS_BLOCK_TYPES_MAX
#define HAKKAS_BLOCK_TYPES_MAX 128
#endif

enum block_types_err {
	BLOCK_TYPES_OK = 0,
	BLOCK_TYPES_ERR_INVALID = -1, /* NULL definition, or a NULL or empty name */
	BLOCK_TYPES_ERR_EXISTS = -2,  /* a type of that name is already registered */
	BLOCK_TYPES_ERR_FULL = -3,    /* HAKKAS_BLOCK_TYPES_MAX types are registered */
};

/* Add def at the end of the registration order. def must stay valid until
 * it is unregistered. Duplicate names are refused (03 §5.1). */
enum block_types_err block_types_register(const BlockTypeDef *def);

/* Remove the type called name. Returns false if there is none. The order of
 * the remaining types is kept. */
bool block_types_unregister(const char *name);

/* The type called name (exact, case-sensitive match), or NULL. */
const BlockTypeDef *block_types_get(const char *name);

/* The number of registered types. */
size_t block_types_count(void);

/* The i-th registered type in registration order, or NULL if
 * i >= block_types_count(). */
const BlockTypeDef *block_types_at(size_t i);

/* Unregister every type (shutdown, tests). */
void block_types_reset(void);

/* Register every built-in type (03 §3), in the order of the 03 §3 table.
 * Stops at and returns the first failure; called once at startup on an
 * empty registry. */
enum block_types_err block_types_register_builtins(void);

#endif
