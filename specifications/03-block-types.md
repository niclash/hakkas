# 03 — Block Type Library & Execution Model

## 1. Value model

Every port holds exactly one `Linkable` value: a string, a signed 64-bit integer, a double, a boolean, a physical-unit-tagged `Metric`, or "unset." There is no list/array value type. See `01-architecture-overview.md` §2.1 for the full value model and type-conversion rules (`to_f64`/`to_i64`/`to_u64`/`to_bool`/`default_value`), which every block type's computation uses to normalize its inputs.

Port descriptors carry `name`, `description`, `type` (`"bool"`/`"number"`/`"string"`), `default` (the value an input holds when nothing is wired to it — defaults to `default_value(type)` when not stated), and `addressable` (reserved metadata). Composite block types (§5) additionally carry a `target`/`source` reference into their inner block graph.

## 2. Execution model

`01-architecture-overview.md` §2.3 defines the generic block runtime this entire library is built on: one shared implementation of the `Block` contract, driven by a per-type data record (`BlockTypeDef`: inputs, outputs, `compute_on_input`, timer mode, a `compute` function, an optional per-instance state size, and a default change threshold). This section specifies exactly when that generic runtime calls `compute` and what it does with the result. A block's "started" state is its graph's started state (`01-architecture-overview.md` §2.2).

**Compute step** (shared by every trigger): if the block is started, call `compute(ctx)` with `ctx.reason` set to the trigger; then for each output, apply `output_set` (`01-architecture-overview.md` §3.2), which pushes the output downstream only if it changed by more than the block's change threshold (`01-architecture-overview.md` §3.3). For a `TIMER_SCHEDULED` type, re-arm (or cancel, if negative) the block's one-shot timer with `compute`'s return value, relative to the current engine time — after every compute step, whatever its reason.

| Event | Behavior |
|---|---|
| `update(input, value)` | Store the value in the input, then fire that input's live-value subscriptions (`02-wire-protocol.md` §4.2). Then, if the type has `compute_on_input`, run the compute step with `reason = INPUT` and `changed_input` set. Otherwise the value is only used at the next timer-driven compute. |
| timer expiry | Run the compute step with `reason = TIMER`. A `TIMER_PERIODIC` timer re-arms itself at its period, drift-free (`01-architecture-overview.md` §3.2, timer decisions); a `TIMER_SCHEDULED` timer is re-armed from `compute`'s return value. |
| `start()` (graph started, or block added to a started graph) | Arm the periodic timer if `TIMER_PERIODIC`. Run the compute step once with `reason = START` and push **every** output downstream unconditionally, bypassing the change threshold. |
| `stop()` (graph stopped) | Cancel the block's timer. Run `compute` once with `reason = STOP`, so a stateful type can clean up, but **do not propagate** its outputs — a stopped graph propagates nothing. Then mark it stopped. |
| `destroy()` (block removed) | As `stop()`, then disconnect every output link. Each downstream input reverts to its default (`01-architecture-overview.md` §2.2). Queued entries addressed to this block become stale and are discarded. |
| `connect(output, dest, input)` | Register the link and deliver the output's current `last_propagated` value to the new destination (if the graph is started). There is no compute step — wiring delivers the signal already on the output. |
| `subscribe_link`/`unsubscribe_link` | Register/remove the live-value subscription on the destination input. On subscribe, immediately send the input's current value to the subscriber. There is no compute step. |

**Propagation chain**: `A.update(port, v)` → `A.inputs[port] := v` → if `A` is started and `compute_on_input`: `compute(ctx)` → for each output whose change exceeds the threshold, for each downstream link `(B, portX)`: enqueue `B.update(portX, newValue)`.

The engine delivers this as true queued message-passing, not nested/recursive calls (`01-architecture-overview.md` §3.2): when a block computes a new output, push `(downstream_block, downstream_port, value)` onto the global queue and drain it breadth-first, rather than recursing immediately into the downstream block's `update`. Cycles in the graph never deadlock under this model — they simply keep enqueuing work.

Decisions (plan task 6), refining the table above:
- `destroy()` runs the `stop()` step only if the block is started (a block in a stopped graph was already stopped and gets no second `STOP`), then removes every link into and out of the block, initials included; each downstream input reverts to its port default.
- `connect` delivers `last_propagated` only in a started graph, where every output has been pushed by `start`, so the delivered value is always one that was actually propagated. In a stopped graph the input keeps its value (its default, since it had no source) until the graph starts and the upstream block pushes.
- A value delivered by a structural operation (initial, severing, `connect`) runs the target's `update` at once, the same code as a queued input update (`01-architecture-overview.md` §2.2, implementation decisions); in a stopped graph `update` only stores the value and fires the live-value subscriptions.
- A port descriptor without a default (unset) uses `default_value(type)` (§1).

Decisions (plan task 8), on timers (`01-architecture-overview.md` §3.2 item 7 and timer decisions):
- `start()` arms a periodic timer before the `START` compute, its first tick one period after the start; a scheduled type's timer is armed from the `START` compute's return value.
- `stop()` cancels the timer before the `STOP` compute and ignores its return value, so a stopped graph has no armed timer; `destroy()` cancels it as well. A queued expiry of a cancelled or re-armed timer is discarded.
- A timer expiry that arrives while the loop is late runs one compute step; a periodic block's missed ticks are skipped.

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
- Timer resolution is the engine clock's, 1 ms. The block computes each transition from the cycle start time rather than from the time the previous timer actually fired, so timer latency doesn't accumulate into drift.

Decisions (plan task 8, `src/blocktypes/timing.c`):
- **Whole milliseconds**: the latched `period` is `to_f64(milliseconds)` rounded to the nearest ms, at least 1 ms (a positive period below 0.5 ms runs at 1 ms) and at most 2^53 ms; `on_time` is `period × clamp(duty-cycle) / 100` rounded to the nearest ms.
- **"Non-number"** is defined through the converters (`01-architecture-overview.md` §2.1): `milliseconds` whose `to_f64` is NaN or ±Infinity makes the block idle, like a value `<= 0`. Text that is not a number and an unset value convert to `0` and are idle by the `<= 0` rule; numeric text such as `"300"` is a valid period. A NaN `duty-cycle` clamps to `0`.
- **Late timers**: when the timer fires late, `out` takes the waveform's value at the current time. Whole cycles that ended meanwhile are skipped, not replayed (their edges are not emitted), and the next cycle starts on the original grid (cycle start + k × period), latching the inputs current then.
- **Input changes while running** don't touch the cycle; the compute only applies a transition that is already due and re-arms the timer for the next one (every compute re-arms a scheduled timer, §2). While idle, any input change re-latches, and a valid period starts a fresh cycle at once.
- **100 % ↔ 0 %**: no special case — the change is latched at the next cycle start like any other, so a 100 % cycle runs to its end and the next cycle is all `false` (or the reverse).
- **Stop and restart**: `stop()` leaves `out` as it was (not propagated); nothing of the interrupted cycle survives, and `start()` begins a fresh cycle at the restart time.

## 4. The environment's inputs and outputs — no IO block types

**Decided** (`00-README.md`, open question 6). There are no `io/*` block types: a graph contains no blocks that represent hardware. The environment's inputs and outputs are described by the environment itself, like a block type that cannot be instantiated (`04-drivers.md` §1), and a per-environment **bindings file** connects ordinary block ports to them (`04-drivers.md` §4; its exact form is open question 3). The same graph therefore runs unchanged in any environment.
- **Environment inputs** (a sensor, a digital input) deliver their values to the bound block ports as IO events through the engine queue (`01-architecture-overview.md` §3.2), propagating like any other value change.
- **Environment outputs** (a relay, a 0–10 V output) take the value of the bound block port and hand it to the environment. The engine never performs blocking hardware access on its own thread.
- **An output keeps its last value** whenever no running process drives it — before its process is loaded, while it is stopped, when the binding is missing or removed. Hakkas does nothing in those cases. A timeout after which a held value is released (e.g. 1–10 minutes) may be added later. What the hardware does if hakkas itself crashes or hangs (watchdogs, on Zephyr or in hardware) is out of scope.
- **User interface**: environment ports cannot be created or deleted by the user, but the frontend should let users hide them and organize them; that needs dedicated handling in the frontend.

## 5. Groups — user-defined (composite) block types

A **group** and a **composite block type** are the same thing: a user selects a set of blocks in a graph, chooses "create group", gives it a name, and from then on it can be instantiated like any built-in block type. The group's exposed inputs and outputs are its ports. It is the mechanism that lets a user-assembled subgraph become a reusable component.

> **Decided** (`00-README.md`, open question 4): creating a group is **"save as component"** — the new type is registered and the selected blocks are left unchanged; the user replaces them by an instance manually if wanted. A "collapse" option may be added later. The wire commands are `02-wire-protocol.md` §3.4.2.

### Runtime behavior

On construction, given the type's definition (inner block list, port mappings, internal wiring):
1. For every `(name, type)` in the inner block list, instantiate that block type as an inner block, named and tracked within the composite instance.
2. For every exposed input, resolve its `tgt` (`"InnerBlock.port"`, split per `01-architecture-overview.md` §2.4) to an `(inner_block, inner_port)` pair.
3. For every exposed output, resolve its `src` the same way.
4. For every initial, wire the literal to the target inner input.
5. For every internal edge (`"Block.port" → "Block.port"`), wire the two inner blocks together.

The outer block's `start()`/`stop()`/`destroy()` apply to every inner block, so purely internal inner blocks are lifecycle-managed too. Outer-facing operations (`get_input`/`get_output`/`update`/`connect`/`disconnect_edge`) map directly to the mapped inner `(block, port)`: an edge into the composite's input is a link to the inner input, and a link from the composite's output is registered on the inner output. Values go through the same global queue as any other link, with no extra hop. An exposed input with nothing wired to it outside reverts to the inner port's default, exactly like any other input.

Decisions (plan task 21; `src/graphs/graph.h` "Composite instances", `src/graphs/block.c`):
- **One generic runtime**: an instance is an ordinary `Block` of the composite type, and its inner blocks are ordinary blocks of their types, run by the same code (`01-architecture-overview.md` §2.3). The instance has no compute and no timer of its own; its value arrays are never read. Nesting works the same way at every level: a mapping is followed through nested instances down to a non-composite block.
- **Structure stays on the instance**: the instance's inputs hold the sources of its exposed inputs (edge, initial, edge metadata) like any block's, so events, the `graph:connect` snapshot, graph files and every `graph:*` command treat it as one ordinary block. Only the dataflow is mapped: a link from an exposed output is registered on the mapped inner output, with the instance's input as its destination; an update for an exposed input (from the queue, an initial, a severed input's default, a new edge's `last_propagated`) and a live-value watch (`network:edges`) go to the mapped inner input, whose source generation queued values carry (`01-architecture-overview.md` §3.2), so values queued under a removed edge go stale. Reading an exposed input (`get_input`) reads the inner input. Removing an instance removes only its own links from the inner outputs; the inner links go with the inner blocks.
- **Inner blocks are not graph nodes**: they are kept in the instance (in the definition's order), not in the graph, are found by no graph command and reported by no event (an instance is one `graph:addnode`). Their names are the definition's, in the instance's own namespace: they can never clash with a graph node name and never change (a rename of the instance renames only the instance).
- **Lifecycle**: the instance's `start()`/`stop()` start/stop every inner block in the definition's order (each by the rules above: `START` pushes, timers armed and cancelled); removing the instance (or destroying its graph) frees every inner block like a removed block — watches detached, timers cancelled, queued entries stale. Building an instance creates the inner blocks, wires the internal edges, then stores the inner initials (nothing computes until it starts).
- **Exposed port defaults**: an exposed port's descriptor carries the mapped inner port's effective default (its default, or `default_value` of its type), so a severed exposed input reverts to exactly the inner default.
- **Change threshold**: an instance override (`change_threshold` in the instance's node metadata) applies to every inner block of the instance, nested ones included; without one each inner block uses its own type's default, then the engine-wide default (`01-architecture-overview.md` §3.3).

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

Decisions (plan task 21; `src/blocktypes/composite_types.h`):
- **Shape**: `name` is a type name (1–64 bytes, `05-app-lifecycle-persistence.md` §2.3); `icon` and `description` are optional strings (`""`); `blocks` is required (it may be empty), each `{name, type}` with a name of 1–64 bytes, unique; `edges`, `initials`, `inports`, `outports` are optional arrays. References are `"Block.port"` split on the last `.` (`01-architecture-overview.md` §2.4). An initial's `src` is a string, number or boolean, parsed for its target port's type as a graph-file initial is (`02-wire-protocol.md` §3.4). A public port name is 1–64 bytes **without `.`** (a parent composite could not reference it), unique among the inports (resp. outports); inports and outports are separate namespaces. `type`, when given, is `"bool"`, `"number"` or `"string"`; `description` a string; `addressable` (also optional, also taken from the inner port when omitted) a boolean.
- **Checks and messages**, the first failure wins: the shape (`"Invalid field: <key>"`, the top-level key at fault; text that is not one JSON object is `"Invalid JSON"`); `name` already registered (`"Component exists: <name>"`); per inner block in order, recursion (`"Recursive component: <name>"`: the block's type is the new type's name, or a composite that contains it) then an unknown type (`"Unknown component: <type>"`); then the edges, initials, inports and outports in that order, each reference as the matching wire command reports it (`02-wire-protocol.md` §3.4.1: `"Unknown node: <block>"`, `"Unknown port: <block>.<port>"`, `"Invalid edge direction"` for an edge or initial whose end has the wrong direction — an inport or outport naming a port of the wrong direction is `"Unknown port"`, as `addinport`/`addoutport`); then fan-in, `"Input already connected: <block>.<port>"`.
- **"Fed twice"** is any second source on one inner input: two edges, an edge and an initial, two initials, or an inport and any other source (two inports included), since an exposed input's outside source is a source of the inner input. One inner output may be exposed by several outports.
- **Recursion**: since a type refers to its inner types by pointer and a type in use can be neither unregistered nor renamed (below), a registered type can never contain one registered after it; the only recursion that can be attempted is a type naming itself, or a cycle among type files (refused at startup as left over, `05-app-lifecycle-persistence.md` §1.4 step 6). The transitive check is kept anyway.
- **Canonical form** (what `component:getsource` returns and a type file holds): compact JSON with the keys `name`, `icon`, `description`, `blocks`, `edges`, `initials`, `inports`, `outports` in that order, every one present; entries in the definition's order; references written `"Block.port"`; initial values as a graph file stores them (`05-app-lifecycle-persistence.md` §2.1: `"5"` on a number port becomes `5`); a port's `type`, `description` and `addressable` written only when they were given, so a port that takes them from its inner port keeps doing so. Registering the canonical form yields the same canonical form (the round trip).
- **Registry operations** (the model side of `02-wire-protocol.md` §3.4.2; its wire commands are plan task 21b; `00-README.md` open question 4 is decided): unregistering and renaming are refused while the type is in use — a graph has an instance of it, or another composite type contains it (`"Component in use: <name>"`); an unknown name, or a built-in type, is `"Unknown component: <name>"`; renaming to a registered name is `"Component exists: <to>"`, to an invalid name `"Invalid field: to"`, and moves the type to the end of the registration order (`01-architecture-overview.md` §2.3). **Changing the description or icon is allowed while the type is in use**: no instance depends on them; "updating a type that is in use is refused" (above) applies to changes that instances would see, of which only rename exists. These messages are provisional, like the commands.

### 5.2 Creating a group from a selection

Creating a group from selected nodes in a graph assembles the authoring format above:
1. The selected nodes' `{name, type}` pairs become the `blocks` array. Every edge with both endpoints inside the selection becomes an `edges` entry. Every initial targeting a selected node becomes an `initials` entry.
2. Every edge crossing the selection boundary identifies an exposed port. An inbound crossing edge (source outside, target inside) becomes an `inports` entry; an outbound crossing edge becomes an `outports` entry. Graph-level inports/outports (`02-wire-protocol.md` §3.4) that target selected nodes are exposed too, under their public names. Other exposed ports get a default public name derived from the inner port name, disambiguated with the inner block's name if two would clash, unless an explicit name is supplied at creation time.
3. The assembled JSON is registered exactly as if it had been hand-authored (§5.1) and persisted.

Decisions (plan task 21; the assembly is the model function `composite_types_assemble`, `src/blocktypes/composite_types.h`, reachable from the wire through `graph:addgroup` once plan task 21b implements it; the selected nodes are not replaced — open question 4, "save as component"):
- **Order**: `blocks` in the graph's creation order whatever the selection's order (a node named twice counts once); `edges` and `initials` per target node (creation order) and input (port order), as in a graph file (`05-app-lifecycle-persistence.md` §2.1). `inports`: first every graph-level inport mapped to a selected node, in creation order, under its public name — only the first of several mapped to the same input, since two would feed it twice (§5.1) — then one per input of a selected node fed by an edge from outside the selection, per node and input, unless a graph-level inport exposes it already. `outports`: first every graph-level outport mapped to a selected node (several may share an output), then one per output of a selected node with an edge to a node outside the selection, in the order those edges are met (per target node and input), each output once, unless a graph-level outport exposes it already.
- **Default names**: a port without a public name is named after its inner port. If that name is a public name of the same direction, or another default-named port of that direction has the same inner port name, it is `<block>_<port>` instead (every one of the clashing ports, so none is privileged); if that is taken too, contains a `.` or is longer than 64 bytes, `<port>_<n>` (or `port_<n>` if that is too long) with the smallest free `n >= 2`. A name supplied at creation time would replace these (part of the open wire design).
- **Left out**: node metadata (positions and `change_threshold` overrides), edge metadata and a placeholder's retained connections are not part of a type. A selected placeholder node goes into `blocks` with its component name, so registering the result is refused as an unknown type. `description` and `icon` come from the request (`""` if none).
- **Refusals**: an empty selection is `"Invalid field: nodes"`, a name that is no node of the graph `"Unknown node: <name>"`; everything else is refused, if at all, when the result is registered (§5.1).

## 6. Planned block types — interface sketch, pending exact specification

Two block families are confirmed as needed but reserved for more exact specification. **Everything below is a starting sketch, not a final design.**

### 6.1 `process/PID` — PID regulator

A **cyclic** block (`TIMER_PERIODIC` + `compute_on_input` — `01-architecture-overview.md` §2.3) with per-instance state: the integral accumulator, the previous (filtered) measured value and the derivative filter state.
- **On every sample tick** (`reason = TIMER`, every `sample_ms`), it performs the full PID step: advance the integral by `error × dt`, compute the derivative over `dt`, compute and propagate `output`. The integral only exists in relation to a fixed sample rate, so the output is always updated at that rate.
- **On any input change** (`reason = INPUT`), it takes the new parameter or value into account immediately, recomputing `output` from the current proportional term and the existing integral, without advancing the integral or derivative state. A change to `sample_ms` re-arms the periodic timer.

**Decided behavior** (`00-README.md`, open question 5 — PID part):
1. **Standard (ISA) gain form**: `out = Kp·(e + (1/Ti)·∫e dt + Td·de/dt)`, with `Ti` and `Td` in seconds; `Ti = 0` disables the integral term, `Td = 0` the derivative term.
2. **Acting direction**: a `reverse` input — `false` (direct): `e = setpoint − measured_value`; `true` (reverse, e.g. cooling): `e = measured_value − setpoint`.
3. **Derivative on the measurement**, not on the error, so a setpoint step causes no output kick; the derivative is filtered by a first-order low-pass with time constant `Td/10`.
4. **Anti-windup by conditional integration**: the integral does not advance while the output is at `output_min`/`output_max` and the error would drive it further beyond that limit.
5. **Manual/auto**: an `auto` input (default `true`); in manual the output follows `manual_value`. Switching manual → auto is **bumpless**: the integral is preset so that the output continues from the manual value.
6. **Timing**: the full step runs every `sample_ms` (default `1000`) using the actual elapsed time as `dt`; an input change recomputes only the proportional part immediately (above). On `start` the integral is reset, or preset from `manual_value` when starting in manual.
7. **Outputs**: `output` (number), and `saturated` (bool, true while the output is clamped at a limit) for alarms.

Ports (provisional names): inputs `setpoint`, `measured_value`, `kp`, `ti`, `td`, `output_min`, `output_max`, `reverse` (bool), `auto` (bool), `manual_value`, `sample_ms`; outputs `output`, `saturated` (bool).

**To be completed by the user**: the parameter set will be aligned with the user's existing, field-proven PID implementations — the parameters `Pconst`, `Itime`, `Dtime` and `MaxD` will be added (and the port names above adjusted to match), and the implementation is to be based on the user's code rather than written from scratch (plan task 23).

### 6.2 Calendar blocks and schedule types

**Decided** (`00-README.md`, open question 5 — schedule part): schedules are not one configurable block. They are built from small **calendar primitive** blocks whose settings are ordinary input ports (so they can be wired, given initials, and work identically in every environment), combined with logic blocks; the **standard schedule types** the industry knows (day, week, month schedules, …) are delivered with the system as **composite block types** (§5) built from those primitives. Block metadata is never used for this (it only supports the user interface, `01-architecture-overview.md` §2.2).

Calendar primitives — each a scheduled block (`TIMER_SCHEDULED`) that reads the wall clock from the environment (`04-drivers.md` §1) and arms its timer for its next boundary; each has a `bool` output `out`, true while the current time matches:
- `timing/MinuteRange` — input `range` (string, e.g. `"12:30-15:45"`): true within that time-of-day range. The start is included and the end excluded (true from 12:30:00 until just before 15:45); a range may wrap past midnight (`"22:00-06:00"`).
- `timing/DayOfWeek` — input `days` (selector string, 1–7) and input `sunday_first` (bool: whether day 1 is Sunday rather than Monday): true on the selected weekdays.
- `timing/MonthOfYear` — input `months` (selector string, 1–12): true during the selected months.
- Further primitives, e.g. `timing/DayOfYear`, `timing/WeekOfYear` (ISO 8601 week numbers 1–53, weeks starting on Monday), and holiday calendars such as `timing/HolidaysOfSweden`.

**Selector strings** (all day/week/month/year selectors): a single value, a range, a list, or a mix — `"3"`, `"1-5"`, `"6,7"`, `"1-3,5"` — so e.g. Monday–Friday is one `DayOfWeek` block with `"1-5"`.

**Boolean logic**: `logic/And`, `logic/Or`, `logic/Not` and `logic/Xor` with `bool` inputs and a `bool` output, for combining calendar primitives (the `math/And2`/`Or2` blocks are bitwise integer operations with number outputs).

**Standard schedule types** (day, week, month schedules, …) are composites of the primitives and logic blocks. They are **not built into hakkas**: they are supplied separately as a **standard library** of composite type files (§5.1), which a deployment installs into its `types/` directory if it wants them. Once installed they are ordinary user types.

Time zone: all of this is UTC unless open question 11 decides otherwise.
