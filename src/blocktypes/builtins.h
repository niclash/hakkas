/*
 * Internal to blocktypes/: the built-in type families (03 §3), each a static
 * const array of definitions in the order of the 03 §3 table, registered by
 * block_types_register_builtins in that order, family by family: math/,
 * compare/, logic/, process/ (Linear, Deadband, PID, then the types of
 * blocktypes/process_more.c), timing/ (Interval, then the calendar
 * primitives).
 */
#ifndef HAKKAS_BLOCKTYPES_BUILTINS_H
#define HAKKAS_BLOCKTYPES_BUILTINS_H

#include <stddef.h>

#include "graphs/block_type.h"

/* math/... (blocktypes/math.c). */
extern const BlockTypeDef math_block_types[];
extern const size_t math_block_types_count;

/* compare/Greater ... compare/GreaterEqual (blocktypes/compare.c). */
extern const BlockTypeDef compare_block_types[];
extern const size_t compare_block_types_count;

/* logic/And, Or, Xor, Not (blocktypes/logic.c). */
extern const BlockTypeDef logic_block_types[];
extern const size_t logic_block_types_count;

/* process/Linear, process/Deadband, process/PID (blocktypes/process.c). */
extern const BlockTypeDef process_block_types[];
extern const size_t process_block_types_count;

/* process/Curve ... process/Integrator, after process/PID
 * (blocktypes/process_more.c). */
extern const BlockTypeDef process_more_block_types[];
extern const size_t process_more_block_types_count;

/* process/SelectIn ... process/MultiplexOut, after process/Integrator
 * (blocktypes/select.c). */
extern const BlockTypeDef select_block_types[];
extern const size_t select_block_types_count;

/* timing/Interval (blocktypes/timing.c). */
extern const BlockTypeDef timing_block_types[];
extern const size_t timing_block_types_count;

/* The calendar primitives timing/MinuteRange ... timing/WeekOfYear
 * (blocktypes/calendar.c). */
extern const BlockTypeDef calendar_block_types[];
extern const size_t calendar_block_types_count;

#endif
