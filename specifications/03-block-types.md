# 03 — Block Type Library & Execution Model

## 1. Value model

Every port holds exactly one `Linkable` value: a string, a signed 64-bit integer, a double, a boolean, a physical-unit-tagged `Metric`, or "unset." There is no list/array value type. See `01-architecture-overview.md` §2.1 for the full value model and type-conversion rules (`to_f64`/`to_i64`/`to_u64`/`to_bool`/`default_value`), which every block type's computation uses to normalize its inputs.

Port descriptors carry `name`, `description`, `type` (`"bool"`/`"number"`/`"string"`), `default` (the value an input holds when nothing is wired to it — defaults to `default_value(type)` when not stated), and `addressable` (reserved metadata). Composite block types (§5) additionally carry a `target`/`source` reference into their inner block graph.

## 2. Execution model

`01-architecture-overview.md` §2.3 defines the generic block runtime this entire library is built on: one shared implementation of the `Block` contract, driven by a per-type data record (`BlockTypeDef`: inputs, outputs, `compute_on_input`, timer mode, a `compute` function, an optional per-instance state size, and a default change threshold). This section specifies exactly when that generic runtime calls `compute` and what it does with the result. A block's "started" state is its graph's started state (`01-architecture-overview.md` §2.2).

**Compute step** (shared by every trigger): if the block is started, call `compute(ctx)` with `ctx.reason` set to the trigger; then for each output, apply `output_set` (`01-architecture-overview.md` §3.2), which pushes the output downstream only if it changed by more than the block's change threshold (`01-architecture-overview.md` §3.3). For a `TIMER_SCHEDULED` type, re-arm (or cancel, if negative) the block's one-shot timer with `compute`'s return value.

| Event | Behavior |
|---|---|
| `update(input, value)` | Store the value in the input, then fire that input's live-value subscriptions (`02-wire-protocol.md` §4.2). Then, if the type has `compute_on_input`, run the compute step with `reason = INPUT` and `changed_input` set. Otherwise the value is only used at the next timer-driven compute. |
| timer expiry | Run the compute step with `reason = TIMER`. A `TIMER_PERIODIC` timer re-arms itself at its period; a `TIMER_SCHEDULED` timer is re-armed from `compute`'s return value. |
| `start()` (graph started, or block added to a started graph) | Arm the periodic timer if `TIMER_PERIODIC`. Run the compute step once with `reason = START` and push **every** output downstream unconditionally, bypassing the change threshold. |
| `stop()` (graph stopped) | Cancel the block's timer. Run `compute` once with `reason = STOP`, so a stateful type can clean up, but **do not propagate** its outputs — a stopped graph propagates nothing. Then mark it stopped. |
| `destroy()` (block removed) | As `stop()`, then disconnect every output link. Each downstream input reverts to its default (`01-architecture-overview.md` §2.2). Queued entries addressed to this block become stale and are discarded. |
| `connect(output, dest, input)` | Register the link and deliver the output's current `last_propagated` value to the new destination (if the graph is started). There is no compute step — wiring delivers the signal already on the output. |
| `subscribe_link`/`unsubscribe_link` | Register/remove the live-value subscription on the destination input. On subscribe, immediately send the input's current value to the subscriber. There is no compute step. |

**Propagation chain**: `A.update(port, v)` → `A.inputs[port] := v` → if `A` is started and `compute_on_input`: `compute(ctx)` → for each output whose change exceeds the threshold, for each downstream link `(B, portX)`: enqueue `B.update(portX, newValue)`.

The engine delivers this as true queued message-passing, not nested/recursive calls (`01-architecture-overview.md` §3.2): when a block computes a new output, push `(downstream_block, downstream_port, value)` onto the global queue and drain it breadth-first, rather than recursing immediately into the downstream block's `update`. Cycles in the graph never deadlock under this model — they simply keep enqueuing work.

## 3. Built-in block types

Unless noted, the following are stateless, event-driven blocks (`compute_on_input = true`, `TIMER_NONE`), with ports named `in1..inN → out` and every port of type `"number"` with default `0`. The comparison blocks' `out` port is of type `"bool"`. Every type uses the engine-wide change threshold unless a type default is listed here.

| Name | Icon | Description | Formula |
|---|---|---|---|
| `math/Add4` | add | `out = in1+in2+in3+in4` | `to_f64(in1)+to_f64(in2)+to_f64(in3)+to_f64(in4)` |
| `math/Add2` | add | `out = in1+in2` | `to_f64(in1)+to_f64(in2)` |
| `math/Mult2` | close | `out = in1*in2` | `to_f64(in1)*to_f64(in2)` |
| `math/Divide` | divide | `out = in1/in2` | `to_f64(in1)/to_f64(in2)` (IEEE-754 semantics for `in2==0`: `±Infinity`/`NaN`, since the value type is a double) |
| `math/Subtract` | remove | `out = in1-in2` | `to_f64(in1)-to_f64(in2)` |
| `math/Modulo` | percent | `out = in1 MOD in2` | `to_f64(in1) mod to_f64(in2)` (floored modulo, see below) |
| `math/Remainder` | percent | `out = in1 REMAINDER in2` | `to_f64(in1) % to_f64(in2)` (truncating remainder, C `fmod`) |
| `math/And2` | logic | `out = in1 AND in2` | `to_i64(in1) & to_i64(in2)` (bitwise) |
| `math/Or2` | logic | `out = in1 OR in2` | `to_i64(in1) \| to_i64(in2)` |
| `math/Xor` | logic | `out = in1 XOR in2` | `to_i64(in1) ^ to_i64(in2)` |
| `math/And4` | logic | `out = in1 AND in2 AND in3 AND in4` | bitwise AND chain |
| `math/Or4` | logic | `out = in1 OR in2 OR in3 OR in4` | bitwise OR chain |
| `math/Greater` | compare | `out = in1 > in2` | `to_f64(in1) > to_f64(in2)` (bool result) |
| `math/Less` | compare | `out = in1 < in2` | `to_f64(in1) < to_f64(in2)` (bool result) |
| `math/Equal` | compare | `out = in1 == in2` | `to_f64(in1) == to_f64(in2)` (bool result) |
| `math/NotEqual` | compare | `out = in1 != in2` | `to_f64(in1) != to_f64(in2)` (bool result) |
| `math/LessEqual` | compare | `out = in1 <= in2` | `to_f64(in1) <= to_f64(in2)` (bool result) |
| `math/GreaterEqual` | compare | `out = in1 >= in2` | `to_f64(in1) >= to_f64(in2)` (bool result) |
| `process/Linear` | function | `out = k*in+m` | `(to_f64(in)*to_f64(k))+to_f64(m)`; ports named `in`, `k`, `m` → `out` |
| `timing/Interval` | timer | periodic pulse generator: PWM or edge generator | see §3.1 |
| `io/DigitalInput` | digital-input | logical digital input bound to a physical channel | see §4 |
| `io/DigitalOutput` | digital-output | logical digital output bound to a physical channel | see §4 |
| `io/AnalogInput` | analog-input | logical analog input bound to a physical channel | see §4 |
| `io/AnalogOutput` | analog-output | logical analog output bound to a physical channel | see §4 |

**Results and edge cases** (decided, plan task 5):
- The arithmetic blocks and `process/Linear` output an f64; the comparisons a bool; the bitwise blocks (`And2`, `Or2`, `Xor`, `And4`, `Or4`) an **i64**, so all 64 bits of the result survive (an f64 would round values beyond 2^53). Their port type is still `"number"`.
- `math/Modulo` is the **floored** modulo `a − b·floor(a/b)`, computed exactly from `fmod`: a non-zero result has the sign of the divisor (`-7 mod 3 = 2`, `7 mod -3 = -2`), a zero result is `+0` for a positive divisor and `-0` for a negative one. NaN when either operand is NaN, the divisor is `±0`, or the dividend is infinite. With an infinite divisor the result is the dividend if it has the divisor's sign (or is zero), else the divisor (`-5 mod Infinity = Infinity`). This is exactly Python's float `%`; like it, a tiny result of the sign opposite to the divisor can round up to the divisor itself.
- `math/Remainder` is C's `fmod` (`a − b·trunc(a/b)`): the result has the sign of the dividend (`-7 % 3 = -1`). NaN when either operand is NaN, the divisor is `±0`, or the dividend is infinite; the dividend itself for an infinite divisor.
- Comparisons follow IEEE-754: with a NaN operand every comparison is `false` except `NotEqual`, which is `true`; `0 == -0`.
- `process/Linear` multiplies, rounds, then adds (no fused multiply-add), so every target gives the same result.
- Port descriptions: `inN` → `"Operand N"`, `out` → `"Result"`; for `process/Linear`, `in` → `"Input value"`, `k` → `"Gain"`, `m` → `"Offset"`, `out` → `"k*in+m"`. `addressable` is `false`. An output's default (its value before the first compute) is `0`, or `false` for a bool output.
- Every compute ignores `reason` (`START`, `INPUT` and `STOP` compute the same result; the runtime decides what propagates, §2).

**Metric inputs**: the formulas above show the plain-number computation. When inputs carry `Metric` values, the arithmetic and comparison blocks also apply the unit rules in `01-architecture-overview.md` §2.1 (category preserved for same-category addition, error flag on mismatch, dimensional multiplication). These rules are provisional, and not implemented yet (`00-README.md`, open question 9): a `Metric` input converts to its magnitude via `to_f64`/`to_i64`, the result is a plain number, and the error flag is never set.

The `timing/` family is expected to grow: `timing/Interval` covers periodic pulses, and other timing behaviors (one-shot/monostable, delay-on/delay-off, debounce, …) will be separate block types rather than modes of one block.

### 3.1 `timing/Interval` — periodic pulse generator (PWM / edge generator)

A scheduled block (`compute_on_input = true`, `TIMER_SCHEDULED`, per-instance state: the start time of the current cycle and the cycle's latched parameters). It outputs a boolean square wave with a given period and duty cycle. With a small period it is a PWM source. With a longer period it is an edge generator: for example, `milliseconds = 1000`, `duty-cycle = 50` gives a rising edge every second and a falling edge half a second later.

| Port | Direction | Type | Default | Description |
|---|---|---|---|---|
| `milliseconds` | input | number | `1000` | Period of one full cycle, in milliseconds. |
| `duty-cycle` | input | number | `50` | Percentage of each period during which `out` is `true`, `0`–`100` (clamped to that range). |
| `out` | output | bool | `false` | The generated pulse train. |

**Algorithm**:
- At the start of a cycle, latch `period = milliseconds` and `on_time = period × clamp(duty-cycle, 0, 100) / 100`. The latched values are used for the whole cycle.
- **Cycle start**: if `on_time > 0`, set `out = true` and arm the timer for `on_time`. If `on_time == 0`, `out` stays `false` and the timer is armed for `period`.
- **On-time elapsed**: if `on_time < period`, set `out = false` and arm the timer for the rest of the period (`period − on_time`). If `on_time == period` (100 %), `out` stays `true` and a new cycle starts.
- **Period elapsed**: start a new cycle (re-latching the current inputs).
- Each change of `out` is propagated (§2). A bool output always propagates on change, regardless of the change threshold.
- **Input changes** (`reason = INPUT`) take effect at the start of the next cycle and never glitch the current one. The one exception is an inert block: if the latched `period <= 0`, the block is idle (`out = false`, no timer armed), and a change to a valid period starts a fresh cycle immediately.
- `milliseconds <= 0` (or a non-number) makes the block idle: `out = false`, no timer.
- `start()` begins a fresh cycle at once. `stop()` cancels the timer. Per the general rules, `out` is not propagated while stopped.
- Timer resolution is whatever the PAL timer provides (typically 1 ms). The block computes each transition from the cycle start time rather than from the time the previous timer actually fired, so timer latency doesn't accumulate into drift.

## 4. IO block types

> **Provisional.** IO events come from the platform's IO provider — Zephyr on the device, a simulator on the desktop (`04-drivers.md`). The details will be worked out when the system is hooked up to Zephyr, and this section may change substantially.

`io/AnalogInput`/`io/DigitalInput` have zero inputs and one output (`out`). `io/AnalogOutput`/`io/DigitalOutput` have one input (`input`) and zero outputs. Each is bound to a physical channel (`04-drivers.md` §4):
- An input block receives **IO events** for its bound channel from the engine queue (`01-architecture-overview.md` §3.2) and sets `out` from each event, subject to the change threshold. Analog channels deliver `Metric` values; digital channels deliver booleans.
- An output block hands each new input value to the IO provider for its bound channel. The engine never performs blocking hardware access on its own thread.

A logical IO port with no live binding on a given deployment (module absent, or this board variant doesn't have it) holds its default value and takes no other action. This is what lets the same graph run unmodified across differently-configured hardware (`04-drivers.md` §4). `addressable` is `false` on all four built-in descriptors (no address indexing is implemented).

How hardware channels are represented to the user (they can't be deleted, but can be hidden and organized into graphs), and what a physical output does when its graph stops or it is unwired, are open (`00-README.md`, open question 6).

## 5. Groups — user-defined (composite) block types

A **group** and a **composite block type** are the same thing: a user selects a set of blocks in a graph, chooses "create group", gives it a name, and from then on it can be instantiated like any built-in block type. The group's exposed inputs and outputs are its ports. It is the mechanism that lets a user-assembled subgraph become a reusable component.

> **Provisional.** The exact creation mechanism (user action, wire messages, whether the selected blocks are replaced by an instance) is not settled (`00-README.md`, open question 4). The runtime behavior and file format below are the parts that are settled.

### Runtime behavior

On construction, given the type's definition (inner block list, port mappings, internal wiring):
1. For every `(name, type)` in the inner block list, instantiate that block type as an inner block, named and tracked within the composite instance.
2. For every exposed input, resolve its `tgt` (`"InnerBlock.port"`, split per `01-architecture-overview.md` §2.4) to an `(inner_block, inner_port)` pair.
3. For every exposed output, resolve its `src` the same way.
4. For every initial, wire the literal to the target inner input.
5. For every internal edge (`"Block.port" → "Block.port"`), wire the two inner blocks together.

The outer block's `start()`/`stop()`/`destroy()` apply to every inner block, so purely internal inner blocks are lifecycle-managed too. Outer-facing operations (`get_input`/`get_output`/`update`/`connect`/`disconnect_edge`) map directly to the mapped inner `(block, port)`: an edge into the composite's input is a link to the inner input, and a link from the composite's output is registered on the inner output. Values go through the same global queue as any other link, with no extra hop. An exposed input with nothing wired to it outside reverts to the inner port's default, exactly like any other input.

### 5.1 Authoring and persistence format

```json
{
  "name": "Example Block", "icon": "timelapse", "description": "...",
  "blocks":   [ { "name": "Add1", "type": "math/Add2" }, ... ],
  "edges":    [ { "src": "Add1.out", "tgt": "Add3.in1" }, ... ],
  "initials": [ { "src": 23.5, "tgt": "Add1.in2" }, ... ],
  "inports":  [ { "name": "in1", "tgt": "Add1.in3", "type": "number", "description": "...", "addressable": false }, ... ],
  "outports": [ { "name": "out", "src": "Add3.out", "type": "number", "description": "..." } ]
}
```

`type` and `description` on `inports`/`outports` are optional and are taken from the mapped inner port when omitted.

Registering a new composite type from this JSON:
1. Resolve each inner `{name,type}` entry against the block type registry. A type may itself reference another previously registered composite type.
2. Build the composite type's definition and register it under `name`. From then on it can be instantiated exactly like a built-in type.

Registration is refused if:
- `name` is already registered (built-in or composite);
- an inner type is unknown;
- a mapped port doesn't exist;
- the type would contain itself, directly or through another composite (no recursion);
- an inner input would be fed twice (no fan-in, `01-architecture-overview.md` §2.2).

The same JSON shape is both the authoring format and the persisted format: saving a composite type and reloading it later round-trips correctly. Types are stored in the `types/` directory, which shares a parent directory with `graphs/` (`05-app-lifecycle-persistence.md` §2.3). Updating or deleting a type that is in use is refused (`02-wire-protocol.md` §3.4.2); whether in-place updates of a type used by live instances should be supported is open.

`subgraph` in a type's `component:component` description (`02-wire-protocol.md` §3.5) is `true` for composite types and `false` for built-in types.

### 5.2 Creating a group from a selection

Creating a group from selected nodes in a graph assembles the authoring format above:
1. The selected nodes' `{name, type}` pairs become the `blocks` array. Every edge with both endpoints inside the selection becomes an `edges` entry. Every initial targeting a selected node becomes an `initials` entry.
2. Every edge crossing the selection boundary identifies an exposed port. An inbound crossing edge (source outside, target inside) becomes an `inports` entry; an outbound crossing edge becomes an `outports` entry. Graph-level inports/outports (`02-wire-protocol.md` §3.4) that target selected nodes are exposed too, under their public names. Other exposed ports get a default public name derived from the inner port name, disambiguated with the inner block's name if two would clash, unless an explicit name is supplied at creation time.
3. The assembled JSON is registered exactly as if it had been hand-authored (§5.1) and persisted.

## 6. Planned block types — interface sketch, pending exact specification

Two block families are confirmed as needed but reserved for more exact specification. **Everything below is a starting sketch, not a final design.**

### 6.1 `process/PID` — PID regulator

A **cyclic** block (`TIMER_PERIODIC` + `compute_on_input` — `01-architecture-overview.md` §2.3) with per-instance state: the integral accumulator and the previous error or measured value.
- **On every sample tick** (`reason = TIMER`, every `sample_ms`), it performs the full PID step: advance the integral by `error × dt`, compute the derivative over `dt`, compute and propagate `output`. The integral only exists in relation to a fixed sample rate, so the output is always updated at that rate.
- **On any input change** (`reason = INPUT`), it takes the new parameter or value into account immediately, recomputing `output` from the current proportional term and the existing integral, without advancing the integral or derivative state. A change to `sample_ms` re-arms the periodic timer.

Starting point for the port set (gains, anti-windup behavior, manual/auto override, derivative-on-error vs. derivative-on-measurement, and output clamping all need further specification):
- Inputs: `setpoint` (number), `measured_value` (number), `kp`/`ki`/`kd` (number, gains), `output_min`/`output_max` (number, clamp bounds for anti-windup), `sample_ms` (number, sample period), `enable` (bool, bypass).
- Output: `output` (number).

### 6.2 Schedule blocks — `timing/DaySchedule`, `timing/WeekSchedule`, `timing/MonthSchedule`

Calendar-driven blocks producing an output based on the current time of day, day of week, or day of month against a configured schedule (e.g. "on from 06:00 to 22:00," "on every weekday," "on the 1st of the month"). These are scheduled blocks (`TIMER_SCHEDULED`) that arm their timer for the next schedule boundary. All schedule times are **UTC** (`01-architecture-overview.md` §3.4). Needs:
- A wall-clock source, which is not yet worked out (`00-README.md`, open question 11). Until it is, these blocks can't be implemented meaningfully.
- A schedule configuration — a list of time ranges or day/date selectors mapped to output values — which doesn't fit the plain scalar-port model used elsewhere. This likely needs a dedicated configuration structure attached to the block instance (outside the normal input-port mechanism, perhaps part of the block's metadata in the graph JSON) rather than a `Linkable` port value.
- Output: likely `bool` for simple on/off schedules, though a richer per-entry value is plausible depending on use case.
