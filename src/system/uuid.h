/*
 * Random (version 4) UUIDs in text form: graph ids created without one
 * (02 §3.2 new_graph) and the engine id when none is configured (05 §1.1
 * engine_id).
 *
 * Naming: functions and macros are prefixed uuid_ / UUID_.
 */
#ifndef HAKKAS_SYSTEM_UUID_H
#define HAKKAS_SYSTEM_UUID_H

#include <stdbool.h>

#include "platform/pal.h"

/* A UUID in its 36-character text form plus the NUL. */
#define UUID_TEXT_SIZE 37

/* A random (version 4, RFC 9562) UUID in lowercase text form into
 * out[UUID_TEXT_SIZE], drawn from random_bytes (pal_random_bytes, or a fake in
 * tests). False, out unspecified, if the random source failed. */
bool uuid_v4(pal_random_fn random_bytes, char *out);

#endif
