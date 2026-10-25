/*
 * The environment's description (04 §1, hakkas_env.h): validation, lookup,
 * equality, and its JSON form, which the POSIX external environment link
 * reads (04 §0.1) and which a later transport would share.
 *
 * Naming: functions are prefixed env_description_.
 *
 * Valid (decided, plan task 22): a name of 1..HAKKAS_ENV_ID_MAX bytes;
 * description, icon, unit and module strings of at most HAKKAS_ENV_TEXT_MAX
 * bytes; at most HAKKAS_ENV_PORTS_MAX ports in all; every port id
 * 1..HAKKAS_ENV_ID_MAX bytes and unique across in_ports and out_ports; a
 * type of number or bool; min <= max when both are set (neither NaN); an
 * addressable port with a count of 1..HAKKAS_ENV_COUNT_MAX, a port that is
 * not addressable with count 0; at most HAKKAS_ENV_MODULES_MAX modules, each
 * listing only port ids of the description. The carrier (plan task 61): 1 to
 * HAKKAS_ENV_IO_SLOTS_MAX I/O slots, built-in ones included, numbered from 0;
 * built-in slots among them, none twice; at most HAKKAS_ENV_COMM_SLOTS_MAX
 * communication slots numbered 1..HAKKAS_ENV_COMM_SLOTS_MAX, none twice. With
 * a carrier every module has one of its I/O slots, and is builtin exactly
 * when its slot is built-in; without one no module is builtin; no two
 * modules share a slot. The carrier's view-only values (its strings, power,
 * the communication parameters) are not checked (decided with the user
 * 2026-10-25).
 *
 * The JSON form (04 §1): {"name", "description"?, "icon"?, "inPorts"?: [port],
 * "outPorts"?: [port], "modules"?: [module]}, a port {"id", "type":
 * "number"|"bool", "description"?, "unit"?, "min"?, "max"?, "addressable"?,
 * "count"?} ("count" only with "addressable": true), a module {"slot"?:
 * integer, "builtin"?: bool, "type"?, "vendor"?, "revision"?, "serial"?,
 * "documentation"?, "ports"?: [port id]}, and "carrier"?: {"type"?,
 * "vendor"?, "revision"?, "serial"?, "documentation"?, "slots": integer,
 * "builtin"?: [{"slot": integer}], "power"?: {"voltage"?: {"min"?, "max"?},
 * "ac"?, "dc"?, "current"?}, "comm"?: [{"slot": integer, "type"?, "ip"?:
 * {"mode"?, "address"?, "prefix"?, "gateway"?, "dns"?: [string], "mac"?},
 * "wifi"?: {"ssid"?}, "rs485"?: {"bitrate"?, "parity"?}, "lorawan"?:
 * {"appEui"?, "devEui"?, "appKeySet"?}}]}. A view-only member of another type
 * is absent, not an error. A member that is null is absent; unknown members
 * are ignored (forward compatibility).
 *
 * Threading: none of its own; a description is read-only once made.
 */
#ifndef HAKKAS_DRIVERS_ENV_DESCRIPTION_H
#define HAKKAS_DRIVERS_ENV_DESCRIPTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "hakkas_env.h"

/* Room for a validation or parse message. */
#define ENV_DESCRIPTION_MESSAGE_MAX 160

/* Whether d is valid (above); if not, msg (cap bytes, if not NULL) says
 * why, e.g. "outPorts[2]: count: expected 1 to 1024". */
bool env_description_validate(const HakkasEnvDescription *d, char *msg, size_t cap);

/* The port called id: true with *actuator (an in_port) and *index (into
 * in_ports or out_ports); false if d has none. */
bool env_description_find(const HakkasEnvDescription *d, const char *id, bool *actuator,
			  uint32_t *index);

/* The number of channels of p: count if addressable, else 1. */
static inline uint32_t env_port_channels(const HakkasEnvPort *p)
{
	return p->addressable ? p->count : 1;
}

/* Whether index is a channel of p: 1..count if addressable, else 0. */
static inline bool env_port_has_channel(const HakkasEnvPort *p, uint32_t index)
{
	return p->addressable ? index >= 1 && index <= p->count : index == 0;
}

/* Whether a and b describe the same environment, member by member (NULL and
 * a string are different; module and port order matter). The communication
 * slots' parameters are not compared, only their slots and types: they are
 * live, view-only values (a DHCP lease). */
bool env_description_equal(const HakkasEnvDescription *a, const HakkasEnvDescription *b);

/* A new description from its JSON form (above), validated: one heap
 * allocation tree, freed with env_description_free. NULL, with msg set, if
 * json is not a valid description or memory runs out. */
HakkasEnvDescription *env_description_from_json(const cJSON *json, char *msg, size_t cap);

/* d in its JSON form (above; the payload of environment:description, 02
 * §3.7): absent strings and false flags left out, so env_description_from_json
 * of it gives a description equal to d. NULL if memory runs out. */
cJSON *env_description_to_json(const HakkasEnvDescription *d);

/* Free a description made by env_description_from_json. NULL is ignored. */
void env_description_free(HakkasEnvDescription *d);

#endif
