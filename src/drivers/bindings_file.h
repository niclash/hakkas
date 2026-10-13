/*
 * The bindings file (04 §4; decided, environment SPI design, step 3): the
 * per-process, per-environment file that binds a process's block ports to
 * the environment's channels, paired with its process in the process list
 * (05 §1.2 "bindings"):
 *
 *	{"environment": "<name>",
 *	 "bindings": [{"block": "<block>", "port": "<port>",
 *		       "env": "<environment port id>", "index"?: <1..count>}, ...]}
 *
 * This module parses the text; drivers/environment.h applies the entries
 * (environment_bind_all), and the C API reads the file (hakkas.h
 * hakkas_bindings_apply).
 *
 * Validation (decided, plan task 22): one JSON object (whitespace around it
 * only) with a non-empty string "environment" and an array "bindings"
 * (empty allowed) of objects, each with non-empty strings "block", "port"
 * and "env" and an optional integer "index" of at least 1 (a null "index"
 * is absent). Whether the names resolve is checked when the bindings are
 * applied: there "index" must be present exactly for an addressable
 * environment port. A member the format does not know, at the top level or
 * in an entry, is logged as a warning and ignored (forward compatibility,
 * like the process list). Anything else makes the file malformed.
 *
 * Naming: functions are prefixed bindings_file_; the type is BindingsFile.
 *
 * Threading: any; warnings go to the log (engine thread, 05 §4).
 */
#ifndef HAKKAS_DRIVERS_BINDINGS_FILE_H
#define HAKKAS_DRIVERS_BINDINGS_FILE_H

#include <stddef.h>

#include "drivers/environment.h"

enum bindings_file_status {
	BINDINGS_FILE_OK = 0,
	BINDINGS_FILE_MALFORMED = -1,
	BINDINGS_FILE_NO_MEMORY = -2,
};

typedef struct {
	char *environment;	   /* owned */
	EnvBindingRequest *entries; /* in file order; their strings owned */
	size_t count;
} BindingsFile;

/* Parse text (len bytes, need not be NUL-terminated) from the file origin
 * (named in warnings). BINDINGS_FILE_OK with *out filled (free with
 * bindings_file_free); otherwise *out is empty and msg (cap bytes, if not
 * NULL) says why, e.g. "bindings[1]: index: expected an integer of at least
 * 1" or "Out of memory". */
enum bindings_file_status bindings_file_parse(const char *text, size_t len, const char *origin,
					      BindingsFile *out, char *msg, size_t cap);

/* Free f's strings and entries and make it empty. */
void bindings_file_free(BindingsFile *f);

#endif
