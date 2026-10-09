/*
 * File names of persisted graphs and composite types (05 §2.3): a graph id
 * or type name, encoded so it is a safe and reversible file name.
 *
 * Naming: functions are prefixed file_name_. Portable: no storage access, so
 * the composite types of plan task 21 share it.
 *
 * Encoding: the bytes A–Z a–z 0–9 _ - are kept, every other byte is written
 * as %XX (two uppercase hex digits), and ".json" is appended:
 * "math/MyBlock" → "math%2FMyBlock.json", a UUID is unchanged. Names are
 * 1..FILE_NAME_ID_MAX bytes (02 §3.4.1), so an encoded name has at most
 * FILE_NAME_ENCODED_MAX bytes, within LittleFS's 255-byte name limit also
 * with the ".tmp" of an atomic write.
 *
 * Decoding accepts only what encoding produces (the canonical form), so a
 * decoded name encodes back to the same file name.
 */
#ifndef HAKKAS_APP_FILE_NAMES_H
#define HAKKAS_APP_FILE_NAMES_H

#include <stdbool.h>
#include <stddef.h>

/* Longest graph id or type name, in bytes (02 §3.4.1). */
#define FILE_NAME_ID_MAX 64

/* The suffix of every encoded name. */
#define FILE_NAME_SUFFIX ".json"

/* Longest encoded name, without the NUL: 3 × 64 + 5 = 197 bytes. */
#define FILE_NAME_ENCODED_MAX (3 * FILE_NAME_ID_MAX + sizeof(FILE_NAME_SUFFIX) - 1)

/* Encode name into out (size bytes). False, with out "" if size > 0, if name
 * is NULL, empty, longer than FILE_NAME_ID_MAX bytes, or the result does not
 * fit. */
bool file_name_encode(const char *name, char *out, size_t size);

/* Decode the file name file into out (size bytes). False, with out "" if
 * size > 0, if file is not the canonical encoding of a valid name (no
 * ".json" suffix, a byte that should have been kept is escaped or one that
 * should have been escaped is not, lowercase or missing hex digits, an empty
 * or too long name) or the result does not fit. */
bool file_name_decode(const char *file, char *out, size_t size);

#endif
