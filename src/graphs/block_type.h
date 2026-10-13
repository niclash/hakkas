/*
 * BlockTypeDef: the per-type data record that drives the one generic block
 * runtime (01 §2.3, 03 §2), and BlockCtx, what a type's compute function sees.
 *
 * Location: this header is the contract between the generic runtime
 * (graphs/, plan task 6) and the block types (blocktypes/: the registry and
 * every concrete type's record). It lives in graphs/ so that the dependency
 * runs one way only: blocktypes/ includes graphs/, never the reverse. The
 * runtime works on a const BlockTypeDef pointer and never needs the registry;
 * resolving a type name (blocktypes/block_types.h) is the caller's job.
 *
 * Naming: the type names BlockTypeDef, PortDescriptor, BlockCtx, ComputeFn,
 * TimerMode and Reason follow the spec; the enum constants are TIMER_ and
 * REASON_, helpers block_ctx_.
 *
 * Built-in types are static const data (no heap): port arrays, their default
 * values (LINKABLE_*_INIT from graphs/linkable.h: scalars or the static empty
 * string) and the record itself. A composite type (03 §5, plan task 21) is
 * built at runtime from its JSON (blocktypes/composite_types.h) and owns heap
 * data; it stays alive while registered, and it cannot be unregistered while
 * any instance or other composite type refers to it.
 */
#ifndef HAKKAS_GRAPHS_BLOCK_TYPE_H
#define HAKKAS_GRAPHS_BLOCK_TYPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphs/linkable.h"
#include "system/tz.h"

/* A port descriptor (01 §2.1, 03 §1), used for inputs and outputs alike. */
typedef struct {
	const char *name;
	const char *description;
	const char *type; /* "bool", "number" or "string" */
	/* An input's value while nothing is wired to it (01 §2.2 "Severing"),
	 * and an output's value before its first compute. The runtime takes
	 * its own reference with linkable_copy. */
	Linkable default_value;
	bool addressable; /* reserved metadata, never indexed (01 §2.1) */
	/* Composite types only (03 §5): the "InnerBlock.port" this port maps
	 * to, the target of an input or the source of an output. NULL for
	 * built-in types. */
	const char *inner;
} PortDescriptor;

typedef enum timer_mode {
	TIMER_NONE = 0,
	TIMER_PERIODIC,	 /* re-armed every period_ms (01 §2.3, "Cyclic") */
	TIMER_SCHEDULED, /* re-armed from compute's return value ("Scheduled") */
} TimerMode;

/* Why compute is being called (03 §2). */
typedef enum block_reason {
	REASON_START = 0,
	REASON_INPUT,
	REASON_TIMER,
	REASON_STOP,
} Reason;

/* What compute sees (01 §2.3). Owned by the runtime and valid only during
 * the call. */
typedef struct {
	const Linkable *inputs; /* the current input values, in the type's port order */
	/* The current output values, in port order, owned by the block. compute
	 * replaces the ones it sets with block_ctx_set_output; an output it does
	 * not set keeps its value. Pushing them downstream (output_set and the
	 * change threshold, 01 §3.3) is the runtime's job, after compute. */
	Linkable *outputs;
	void *state;	   /* state_size bytes, zeroed at creation; NULL when 0 */
	uint64_t now_ms;   /* engine monotonic time (01 §3.4) */
	/* The wall clock at now_ms: UTC ms since the Unix epoch (01 §3.4),
	 * and the engine's time zone (05 §1.1 time_zone) for local time. Read
	 * by the calendar primitives (03 §6.2), whose types set wall_clock.
	 * tz is never NULL when the runtime calls compute. */
	int64_t wall_ms;
	const TimeZone *tz;
	Reason reason;
	int changed_input; /* the input that changed when reason == REASON_INPUT, else -1 */
	/* TIMER_PERIODIC only: the block's current period in ms (0: no timer).
	 * compute may change it, for a type whose input drives its period
	 * (process/PID's sample_ms, 03 §6.1); after compute the runtime applies
	 * a changed value as block_set_period does, restarting the timer from
	 * now (a STOP compute's change only takes effect at the next start). */
	uint32_t period_ms;
} BlockCtx;

/* For TIMER_SCHEDULED types, returns the delay in ms until the next
 * REASON_TIMER call; negative means no timer armed. Every call re-arms or
 * cancels the block's one-shot timer. Ignored for the other timer modes, so
 * other types return 0 (01 §2.3). */
typedef int64_t (*ComputeFn)(BlockCtx *ctx);

typedef struct CompositeDef CompositeDef;

typedef struct {
	const char *name; /* e.g. "math/Add2"; unique in the registry */
	const char *icon;
	const char *description;
	const PortDescriptor *inputs;
	uint32_t input_count;
	const PortDescriptor *outputs;
	uint32_t output_count;
	bool compute_on_input; /* run compute whenever an input changes */
	TimerMode timer;
	uint32_t period_ms; /* TIMER_PERIODIC only: the default period */
	ComputeFn compute;  /* NULL for a composite type, whose inner blocks compute */
	size_t state_size;  /* 0 for stateless types */
	double change_threshold; /* the type default (01 §3.3); NAN = the engine-wide default */
	/* The type's compute reads the wall clock (ctx.wall_ms; a
	 * TIMER_SCHEDULED calendar primitive, 03 §6.2): when the wall clock
	 * steps, the runtime fires its armed timer at once so that it
	 * re-evaluates (runtime_set_wall_ms, 01 §3.4). */
	bool wall_clock;
	/* true for a composite type (03 §5): its ports carry inner references
	 * and its definition (inner blocks, edges, initials) is reached through
	 * composite. Reported as "subgraph" by component:component (02 §3.5). */
	bool subgraph;
	const CompositeDef *composite; /* the composite definition; NULL for built-ins */
} BlockTypeDef;

/*
 * A composite type's definition as the generic runtime instantiates it
 * (03 §5 "Runtime behavior"), every reference already resolved: inner blocks
 * by index into blocks, ports by index into the inner type's inputs or
 * outputs. Built and validated by blocktypes/composite_types.h from the
 * authoring format (03 §5.1); the runtime trusts it (no fan-in, every index
 * in range, no recursion).
 */
typedef struct {
	uint32_t block; /* index into CompositeDef.blocks */
	uint32_t port;	/* input or output index of that block's type */
} CompositeRef;

typedef struct {
	const char *name; /* unique within the definition */
	const BlockTypeDef *type;
} CompositeBlockDef;

typedef struct {
	CompositeRef src; /* an output */
	CompositeRef tgt; /* an input */
} CompositeEdge;

typedef struct {
	CompositeRef tgt; /* an input */
	Linkable value;	  /* the literal, parsed for the input's port type */
} CompositeInitial;

struct CompositeDef {
	const CompositeBlockDef *blocks;
	uint32_t block_count;
	const CompositeEdge *edges;
	uint32_t edge_count;
	const CompositeInitial *initials;
	uint32_t initial_count;
	/* The inner input of each of the type's inputs and the inner output of
	 * each of its outputs, in the type's port order (input_count and
	 * output_count entries). */
	const CompositeRef *inports;
	const CompositeRef *outports;
};

/* Replace output i with v, taking ownership of v (a move) and releasing the
 * previous value. */
static inline void block_ctx_set_output(BlockCtx *ctx, uint32_t i, Linkable v)
{
	linkable_clear(&ctx->outputs[i]);
	ctx->outputs[i] = v;
}

#endif
