# 03 — Block Type Library & Execution Model

## 1. Value model

Every port holds exactly one `Linkable` value: a string, a signed 64-bit integer, a double, a boolean, or "unset." There is no list/array value type. See `01-architecture-overview.md` §2.1 for the full value model and type-conversion rules (`to_f64`/`to_i64`/`to_u64`/`to_bool`/`default_value`), which every block type's computation uses to normalize its inputs.

Port descriptors carry `name`, `description`, `type` (`"bool"`/`"number"`/`"string"`), `default` (the value an input holds when nothing is wired to it — always `default_value(type)`), and `addressable` (reserved metadata).

**Resting values** (decided with the user 2026-10-14): there are no per-port default values. An input fed by nothing holds its type's resting value — `0` for a number, `false` for a bool, `""` for a string (`default_value`, `01-architecture-overview.md` §2.1) — as an absent signal or voltage in electronics has a physical resting point, 0 V. Every built-in type is designed so that the resting value of each input is meaningful (e.g. "off", "no limit", "no sampling"); a test checks that every built-in input rests at it. The engine therefore sends no `default` in `component:component`, and the frontend shows the resting value (`06-frontend.md` §4.2). `timing/Interval` and `process/PID`, which had other defaults, were changed accordingly (§3.1, §6.1). Composite block types (§5) additionally carry a `target`/`source` reference into their inner block graph.

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

Unless noted, the following are stateless, event-driven blocks (`compute_on_input = true`, `TIMER_NONE`), with ports named `in1..inN → out` and every port of type `"number"` with default `0`. The comparison blocks' `out` port is of type `"bool"`; the `logic/*` blocks' ports are all `"bool"` with default `false`. Every type uses the engine-wide change threshold unless a type default is listed here. The table's order is the registration order (`01-architecture-overview.md` §2.3), family by family: `math/`, `compare/`, `logic/`, `process/`, `timing/` (decided, plan task 24; `compare/` since plan task 47).

**Former names** (decided 2026-10-14 with the user, implemented in plan task 47): the bitwise integer blocks `math/And2`, `math/Or2`, `math/Xor`, `math/And4` and `math/Or4` were removed (boolean logic is `logic/*`), and the comparisons moved from `math/` to their own family, `compare/` (`math/Greater` is now `compare/Greater`, and so on). Graph and composite type files that name a comparison's `math/` name still load: the loaders (`05-app-lifecycle-persistence.md` §2.2, §2.3) map it to the `compare/` name, so the block is the `compare/` type and a save writes the new name (`block_types_alias`, `src/blocktypes/block_types.h`). Only the file loaders do: commands (`graph:addnode`, `describe type`, the C API) take the current names. A graph file naming a removed bitwise block loads it as a placeholder (`05-app-lifecycle-persistence.md` §2.2); a composite type using one does not load (`"Unknown component: math/And2"`, §5.1).

| Name | Icon | Description | Formula |
|---|---|---|---|
| `math/Add4` | add | `out = in1+in2+in3+in4` | `to_f64(in1)+to_f64(in2)+to_f64(in3)+to_f64(in4)` |
| `math/Add2` | add | `out = in1+in2` | `to_f64(in1)+to_f64(in2)` |
| `math/Mult2` | close | `out = in1*in2` | `to_f64(in1)*to_f64(in2)` |
| `math/Divide` | divide | `out = in1/in2` | `to_f64(in1)/to_f64(in2)` (IEEE-754 semantics for `in2==0`: `±Infinity`/`NaN`, since the value type is a double) |
| `math/Subtract` | remove | `out = in1-in2` | `to_f64(in1)-to_f64(in2)` |
| `math/Modulo` | percent | `out = in1 MOD in2` | `to_f64(in1) mod to_f64(in2)` (floored modulo, see below) |
| `math/Remainder` | percent | `out = in1 REMAINDER in2` | `to_f64(in1) % to_f64(in2)` (truncating remainder, C `fmod`) |
| `compare/Greater` | compare | `out = in1 > in2` | `to_f64(in1) > to_f64(in2)` (bool result) |
| `compare/Less` | compare | `out = in1 < in2` | `to_f64(in1) < to_f64(in2)` (bool result) |
| `compare/Equal` | compare | `out = in1 == in2` | `to_f64(in1) == to_f64(in2)` (bool result) |
| `compare/NotEqual` | compare | `out = in1 != in2` | `to_f64(in1) != to_f64(in2)` (bool result) |
| `compare/LessEqual` | compare | `out = in1 <= in2` | `to_f64(in1) <= to_f64(in2)` (bool result) |
| `compare/GreaterEqual` | compare | `out = in1 >= in2` | `to_f64(in1) >= to_f64(in2)` (bool result) |
| `logic/And` | logic | `out = in1 AND in2 (boolean)` | `to_bool(in1) && to_bool(in2)`; bool ports, see §6.2 |
| `logic/Or` | logic | `out = in1 OR in2 (boolean)` | `to_bool(in1) \|\| to_bool(in2)` |
| `logic/Xor` | logic | `out = in1 XOR in2 (boolean)` | `to_bool(in1) != to_bool(in2)` |
| `logic/Not` | logic | `out = NOT in (boolean)` | `!to_bool(in)`; one input, `in` |
| `process/Linear` | function | `out = k*in+m` | `(to_f64(in)*to_f64(k))+to_f64(m)`; ports named `in`, `k`, `m` → `out` |
| `process/Deadband` | filter | `out` follows `in` only when it moves by at least `threshold` | see below |
| `process/PID` | tune | PID regulator | cyclic, stateful; see §6.1 |
| `process/Curve` | show_chart | `output` follows a curve of up to 7 points | stateless; see §3.2 |
| `process/AutoMan` | pan_tool | `output = override ? manual : input` | stateless; see §3.3 |
| `process/Alarm` | notifications | alarm states of a condition | stateful; see §7 |
| `process/Delay` | hourglass_empty | `output` takes `input` once it has been stable for `milliseconds` | scheduled, stateful; see §3.4 |
| `process/Trigger` | bolt | `output` false when `arm` rises, true when `fire` rises | stateful; see §3.5 |
| `process/Limit` | compress | `output = input` capped to `[min, max]` | stateless; see §3.6 |
| `process/Hysteresis` | swap_vert | Schmitt trigger: `output` on above `threshold+hysteresis`, off below `threshold-hysteresis` | stateful; see §3.7 |
| `process/Exponential` | function | `output = a*exp(b*input)` | `to_f64(a)*exp(to_f64(b)*to_f64(input))`; see §3.8 |
| `process/Logarithmic` | function | `output = a*ln(b*input)` | `to_f64(a)*log(to_f64(b)*to_f64(input))`; see §3.8 |
| `process/Integrator` | functions | `output` = ∫ `input` dt / `divisor`, within `[min, max]` | cyclic, stateful; see §3.9 |
| `timing/Interval` | timer | periodic pulse generator: PWM or edge generator | see §3.1 |
| `timing/MinuteRange` | schedule | true within a time-of-day range | calendar primitive, §6.2 |
| `timing/DayOfWeek` | date_range | true on the selected weekdays | calendar primitive, §6.2 |
| `timing/DayOfMonth` | date_range | true on the selected days of the month | calendar primitive, §6.2 |
| `timing/MonthOfYear` | date_range | true during the selected months | calendar primitive, §6.2 |
| `timing/DayOfYear` | date_range | true on the selected days of the year | calendar primitive, §6.2 |
| `timing/WeekOfYear` | date_range | true during the selected ISO weeks | calendar primitive, §6.2 |

**Results and edge cases** (decided, plan task 5):
- The arithmetic blocks and `process/Linear` output an f64; the comparisons a bool.
- `math/Modulo` is the **floored** modulo `a − b·floor(a/b)`, computed exactly from `fmod`: a non-zero result has the sign of the divisor (`-7 mod 3 = 2`, `7 mod -3 = -2`), a zero result is `+0` for a positive divisor and `-0` for a negative one. NaN when either operand is NaN, the divisor is `±0`, or the dividend is infinite. With an infinite divisor the result is the dividend if it has the divisor's sign (or is zero), else the divisor (`-5 mod Infinity = Infinity`). This is exactly Python's float `%`; like it, a tiny result of the sign opposite to the divisor can round up to the divisor itself.
- `math/Remainder` is C's `fmod` (`a − b·trunc(a/b)`): the result has the sign of the dividend (`-7 % 3 = -1`). NaN when either operand is NaN, the divisor is `±0`, or the dividend is infinite; the dividend itself for an infinite divisor.
- Comparisons follow IEEE-754: with a NaN operand every comparison is `false` except `NotEqual`, which is `true`; `0 == -0`.
- `process/Linear` multiplies, rounds, then adds (no fused multiply-add), so every target gives the same result.
- Port descriptions: `inN` → `"Operand N"`, `out` → `"Result"`; for `process/Linear`, `in` → `"Input value"`, `k` → `"Gain"`, `m` → `"Offset"`, `out` → `"k*in+m"`. `addressable` is `false`. An output's default (its value before the first compute) is `0`, or `false` for a bool output.
- Every compute ignores `reason` (`START`, `INPUT` and `STOP` compute the same result; the runtime decides what propagates, §2).

Values are plain numbers; there are no unit rules (`Metric` was dropped, `00-README.md` open question 9).

The `timing/` family is expected to grow: `timing/Interval` covers periodic pulses, and other timing behaviors (one-shot/monostable, delay-on/delay-off, debounce, …) will be separate block types rather than modes of one block.

**`process/Deadband`** (decided, `00-README.md` open question 7): inputs `in` (number) and `threshold` (number, default `0`), output `out` (number). On every input change, `out` is set to `in` only if `|in − out| >= threshold` (or on a NaN transition, or the first value after `start`); otherwise `out` keeps its value. It is the graph-visible way to filter a signal more (or less) than the engine-wide change threshold; its type default change threshold is `0`, so the engine adds no filtering of its own on its output. A `relative` (bool) input for percentage deadbands may be added later if needed.

Decisions (plan task 21c):
- **The first value after `start`** is the `START` compute: it sets `out` to the current `in` unconditionally, and the runtime pushes it (§2). A value arriving after that is compared with it like any other, so after a restart `out` may hold a stale `in` that is within `threshold` of the upstream's first pushed value.
- **A change of `threshold` alone** re-evaluates: the current `in` is compared with `out` under the new threshold, so lowering it lets a held `out` catch up at once; raising it never moves `out`.
- **A NaN or negative `threshold`** counts as `0` (every distinct value passes); `+Infinity` holds every finite move, so only a NaN transition or a restart moves `out`.
- **NaN**: a transition into or out of NaN always moves `out`; NaN → NaN does not. Equal infinities (`|∞ − ∞|` is NaN) do not move it.
- `STOP` changes nothing. The type keeps no state of its own: the held value is its current output. `out` is always an f64 (`to_f64(in)`). Port descriptions: `in` → `"Input value"`, `threshold` → `"Smallest change of in that moves out"`, `out` → `"in, held while it moves less than threshold"`. Registration order: after `process/Linear`, as in the table.

### 3.1 `timing/Interval` — periodic pulse generator (PWM / edge generator)

A scheduled block (`compute_on_input = true`, `TIMER_SCHEDULED`, per-instance state: the start time of the current cycle and the cycle's latched parameters). It outputs a boolean square wave with a given period and duty cycle. With a small period it is a PWM source. With a longer period it is an edge generator: for example, `milliseconds = 1000`, `duty-cycle = 50` gives a rising edge every second and a falling edge half a second later.

| Port | Direction | Type | Default | Description |
|---|---|---|---|---|
| `milliseconds` | input | number | `0` | Period of one full cycle, in milliseconds (`<= 0`: idle, `out` false). |
| `duty-cycle` | input | number | `0` | Percentage of each period during which `out` is `true`, `0`–`100` (clamped to that range). |

Resting values (§1, 2026-10-14; the defaults were 1000 ms and 50 % until then): a new Interval is idle until its period is set, and then false until its duty cycle is set.
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

### 3.2 `process/Curve` — piecewise linear curve

Decided with the user 2026-10-15; implemented in plan task 48 (`src/blocktypes/process_more.c`). Converts one value into another along a curve of up to seven points `(x1, y1)` … `(x7, y7)`, interpolating linearly between them. The first and last points used are also the output's limits. Stateless, `compute_on_input`, `TIMER_NONE`, the engine-wide change threshold; every compute ignores `reason` except `STOP`, which changes nothing. Registered after `process/PID`.

| Port | Direction | Type | Description |
|---|---|---|---|
| `input` | input | number | The value to convert |
| `x1` … `x7` | input | number | X coordinate of point 1 … 7 |
| `y1` … `y7` | input | number | Y coordinate of point 1 … 7 |
| `output` | output | number | The value on the curve at `input` |
| `min` | output | bool | true while `input` is below the first point (`output` is held at its Y) |
| `max` | output | bool | true while `input` is above the last point (`output` is held at its Y) |

Every input rests at `0` (§1); the outputs start at `0` and `false`.

**The points used**: the points are taken in order, 1 to 7. Point 1 is used if its X is finite; each later point is used only if its X is finite and **greater than the X of the last point used**; any other point is ignored. So unused points resting at `(0, 0)` after a curve with positive X drop out by themselves; on a curve whose X values are all below `0`, unused points are set to copies of the last one. Example: X `0 10 20 30 0 0 0`, Y `5 15 15 40 0 0 0` uses `(0,5) (10,15) (20,15) (30,40)`.

**The output**, with `in = to_f64(input)` and the used points `P1 … Pn` (renumbered):
- `in < X(P1)`: `output = Y(P1)`, `min` true.
- `in > X(Pn)`: `output = Y(Pn)`, `max` true.
- otherwise `in` lies on a segment `Pi … Pi+1` (`X(Pi) <= in <= X(Pi+1)`, the first such segment): `output = Y(Pi) + (in − X(Pi))·(Y(Pi+1) − Y(Pi))/(X(Pi+1) − X(Pi))`; on a point exactly, its Y. `min` and `max` are false.
- With one point used, `output` is its Y whatever `in` is; `min`/`max` as above.
- A NaN `input` gives a NaN `output`, `min` and `max` false. With no point used (every X non-finite), `output` is `0`, `min` and `max` false. A non-finite Y goes into the result as IEEE-754 arithmetic gives it.
- Port descriptions: as the table above.

### 3.3 `process/AutoMan` — manual override

Decided with the user 2026-10-15; implemented in plan task 49 (`src/blocktypes/process_more.c`). Lets a person override a value by hand: while `override` is false the output follows `input`, while it is true it follows `manual`. Stateless, `compute_on_input`, `TIMER_NONE`, the engine-wide change threshold; every compute ignores `reason` except `STOP`, which changes nothing. No bumpless transfer: switching moves `output` at once to the other value. Registered after `process/Curve`.

| Port | Direction | Type | Description |
|---|---|---|---|
| `input` | input | number | The automatic value |
| `override` | input | bool | Manual override: `output` follows `manual` instead of `input` |
| `manual` | input | number | The manual value |
| `output` | output | number | `manual` while overridden, else `input` |

`output = to_bool(override) ? to_f64(manual) : to_f64(input)`. Every input rests at its type's resting value (§1): not overridden, so a new block passes `input` through. `override` is a bool: it is a switch.

### 3.4 `process/Delay` — on/off delay

Decided with the user 2026-10-15; implemented in plan task 51 (`src/blocktypes/process_more.c`). Passes a boolean `input` to `output` only once it has been **stable** for `milliseconds`: every change of `input` starts the wait over, and `output` changes only when the wait ends. It delays both edges (on and off) and filters out pulses shorter than `milliseconds`. A scheduled block (`compute_on_input`, `TIMER_SCHEDULED`; state: the engine time of the last change of `input`). Registered after `process/Alarm`.

| Port | Direction | Type | Description |
|---|---|---|---|
| `input` | input | bool | The signal to delay |
| `milliseconds` | input | number | How long `input` must be stable before `output` takes it (`<= 0`: no delay) |
| `output` | output | bool | `input`, once it has been stable for `milliseconds` |

- **A change of `input`** (`to_bool`) records the time and arms the timer for `milliseconds`; when it expires with `input` unchanged since, `output = input`. If `input` returns to `output`'s value before then, nothing is pending (the timer is cancelled), which is what the restarted wait would give.
- **`milliseconds`** is rounded to whole milliseconds, at most `2^53`; `<= 0`, NaN or infinite means **no delay**: `output` follows `input` at once (its resting value `0`, §1). A change of `milliseconds` while a change of `input` is pending applies to that wait, counted from the change of `input`: if it has already lasted that long, `output` takes `input` at once.
- **Start**: `output = input` at once, with nothing pending (decided with the user); only changes after the start are delayed. `STOP` changes nothing; nothing pending survives a stop.
- **Late timers**: a timer that fires late applies the change then; it is never applied early.

### 3.5 `process/Trigger` — arm and fire

Decided with the user 2026-10-15; implemented in plan task 52 (`src/blocktypes/process_more.c`). `output` changes state **only on two edges**, each regardless of the other input: a **rising edge of `arm`** (false → true) sets `output` to false, and a **rising edge of `fire`** sets it to true. Nothing else changes it: not the levels of the inputs, not falling edges. So a `fire` that is already true when `arm` rises leaves `output` false until `fire` goes false and then true again. Stateful (the last values of `arm` and `fire`), `compute_on_input`, `TIMER_NONE`. Registered after `process/Delay`.

| Port | Direction | Type | Description |
|---|---|---|---|
| `arm` | input | bool | Its rising edge sets `output` to false |
| `fire` | input | bool | Its rising edge sets `output` to true |
| `output` | output | bool | false since the last rising `arm`, true since the last rising `fire`, whichever came last |

- Inputs are read with `to_bool`. One input changes per compute (§2), so the two edges never coincide.
- **Start**: `output` is false, and the inputs' values at the start are recorded without counting as edges (an input already true at the start must go false and true again). `STOP` changes nothing; the recorded values do not survive a stop.

### 3.6 `process/Limit` — limiter

Decided with the user 2026-10-15; implemented in plan task 53 (`src/blocktypes/process_more.c`). `output` is `input` capped to `[min, max]`. Stateless, `compute_on_input`, `TIMER_NONE`, the engine-wide change threshold; every compute ignores `reason` except `STOP`, which changes nothing. Registered after `process/Trigger`.

| Port | Direction | Type | Description |
|---|---|---|---|
| `input` | input | number | The value to limit |
| `min` | input | number | Lower limit (equal limits: none) |
| `max` | input | number | Upper limit (equal limits: none) |
| `output` | output | number | `input`, capped to `[min, max]` |

The limits follow `process/PID`'s (§6.1): **equal limits are no limits** (both rest at `0`, §1, which would otherwise pin `output` to 0), limits given the wrong way round are swapped, and a NaN limit is no limit on its side. A NaN `input` gives a NaN `output`.

### 3.7 `process/Hysteresis` — Schmitt trigger

Decided with the user 2026-10-15; implemented in plan task 54 (`src/blocktypes/process_more.c`). A comparator with hysteresis, like a Schmitt trigger in electronics: it switches **on** when `input > threshold + hysteresis` and **off** when `input < threshold − hysteresis`; in between it keeps its state. `inverted` negates the output. Stateful (on/off, before inversion), `compute_on_input`, `TIMER_NONE`. Registered after `process/Limit`.

| Port | Direction | Type | Description |
|---|---|---|---|
| `input` | input | number | The value compared |
| `threshold` | input | number | The middle of the switching band |
| `hysteresis` | input | number | Half the band's width: on above `threshold + hysteresis`, off below `threshold − hysteresis` |
| `inverted` | input | bool | Negates `output` |
| `output` | output | bool | The state (on = true), negated when `inverted` |

- `hysteresis` is used as its absolute value; NaN counts as `0`. With `0` (its resting value) it is a plain comparator that keeps its state when `input` equals `threshold` exactly.
- A NaN `input` or `threshold` holds the state.
- `output = state XOR to_bool(inverted)`: a change of `inverted` flips `output` at once and leaves the state alone.
- **Start**: the state is on if `input > threshold + hysteresis`, otherwise off (also inside the band). `STOP` changes nothing.

### 3.8 `process/Exponential` and `process/Logarithmic`

Decided with the user 2026-10-15; implemented in plan task 55 (`src/blocktypes/process_more.c`). The equivalents of `process/Linear`'s `out = k*in+m`: `process/Exponential` computes `output = a·exp(b·input)`, `process/Logarithmic` `output = a·ln(b·input)` (the natural logarithm). Stateless, `compute_on_input`, `TIMER_NONE`, the engine-wide change threshold; every compute ignores `reason` except `STOP`. Registered after `process/Hysteresis`, in that order. Ports `input`, `a`, `b` (numbers) → `output` (number), as the other new `process/` types name them (`process/Linear` keeps `in`/`out`). Results follow IEEE-754 (C `exp`, `log`): `ln` of `0` is `−Infinity`, of a negative number NaN, and `0 · ±Infinity` is NaN — so a Logarithmic block at rest (`a = b = 0`) outputs NaN, and an Exponential one `0`. Port descriptions: `input` → `"Input value"`, `a` → `"Scale"`, `b` → `"Rate"`, `output` → `"a*exp(b*input)"` / `"a*ln(b*input)"`.

### 3.9 `process/Integrator` — integral over time

Decided with the user 2026-10-15; implemented in plan task 56 (`src/blocktypes/process_more.c`). Integrates `input` over time: `output` grows by `input × seconds / divisor`, held within `[min, max]` so that it does not wind up. A **cyclic** block like `process/PID` (`TIMER_PERIODIC` + `compute_on_input`; state: the accumulated value, the last `input`, the engine time of the last step). Registered after `process/Logarithmic`.

| Port | Direction | Type | Description |
|---|---|---|---|
| `input` | input | number | The value integrated |
| `min` | input | number | Lower limit of `output` (equal limits: none) |
| `max` | input | number | Upper limit of `output` (equal limits: none) |
| `divisor` | input | number | Time scale: `1` seconds, `3600` hours, `86400` days (`<= 0`: 1) |
| `reset` | input | bool | While true, `output` is 0 (within the limits) and nothing integrates |
| `sample_ms` | input | number | How often `output` is updated, in milliseconds (`<= 0`: only on input changes) |
| `output` | output | number | The integral, within `[min, max]` |

- **Each step** (a sample tick, or any input change) adds the **previous** `input` × the seconds since the last step / `divisor`, clamps to the limits, and records the current `input` and time. `input` is taken as constant between changes, so the result is exact whatever `sample_ms` is: `sample_ms` only sets how often `output` moves while `input` stays the same. With `sample_ms <= 0` (its resting value), NaN or infinite, there is no timer and `output` catches up at the next input change. `sample_ms` is rounded to whole milliseconds, at least 1 and at most `2^32 − 1`, and a change re-arms the timer, as for `process/PID` (§6.1).
- **Limits** as `process/PID`'s (§6.1): equal limits are no limits (both rest at `0`), wrong-way-round limits are swapped, a NaN limit is no limit on its side. The accumulated value itself is clamped (anti-windup): integrating back from a limit moves `output` at once. A change of a limit clamps at once.
- **`divisor`**: `<= 0`, NaN or infinite counts as `1` (seconds; its resting value `0` gives seconds). A change applies from that step on; time already integrated keeps its scale.
- **`reset`**: while true, `output = clamp(0)` and the time passing is not integrated; when it goes false, integration starts from there.
- A non-finite `input` integrates nothing for the time it holds (`output` is held).
- **Start**: `output = clamp(0)` and the time starts at the start; nothing survives a stop. `STOP` changes nothing.

## 4. The environment's inputs and outputs — no IO block types

**Decided** (`00-README.md`, open question 6). There are no `io/*` block types: a graph contains no blocks that represent hardware. The environment's inputs and outputs are described by the environment itself, like a block type that cannot be instantiated (`04-drivers.md` §1), and a per-environment **bindings file** connects ordinary block ports to them (`04-drivers.md` §4, which defines its form; implemented in plan task 22). The same graph therefore runs unchanged in any environment.
- **Environment inputs** (a sensor, a digital input) deliver their values to the bound block ports as IO events through the engine queue (`01-architecture-overview.md` §3.2), propagating like any other value change. A bound input has the sensor as its one source (`04-drivers.md` §4).
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
- **Change threshold**: each inner block uses its own type's default, then the engine-wide default (`01-architecture-overview.md` §3.3); there is no per-instance override (open question 7).

**Defining a type as a graph** (decided with the user 2026-10-14, plan task 36; `02-wire-protocol.md` §3.4.3): every composite type also has a **component graph**, a graph that never runs (`"main": false`), whose blocks, connections and initials are the type's inside and whose graph inports and outports — each mapped to a port of one of its blocks — are the type's ports. A client creates a type by building such a graph (`graph:clear`, the graph commands) and persisting it; the type is then assembled from the graph as §5.2 assembles a selection of all its nodes, and stored in the type file below. The type file stays the persistence format; at startup each type file is registered and its component graph built from it.

### 5.1 Authoring and persistence format

```json
{
  "name": "Example Block", "icon": "timelapse", "description": "...",
  "blocks":   [ { "name": "Add1", "type": "math/Add2" }, ... ],
  "connections": [ { "src": "Add1.out", "tgt": "Add3.in1" }, ... ],
  "initials": [ { "src": 23.5, "tgt": "Add1.in2" }, ... ],
  "inports":  [ { "name": "in1", "tgt": "Add1.in3", "type": "number", "description": "...", "addressable": false }, ... ],
  "outports": [ { "name": "out", "src": "Add3.out", "type": "number", "description": "..." } ]
}
```

The member is `"connections"` (hakkas' term, `00-README.md` Terminology); the loader also accepts the former `"edges"`, and a save writes `"connections"`. Decided (plan task 21g): a document with both `"connections"` and `"edges"` is refused as `"Invalid field: edges"` (the shape check below); one with only `"edges"` is checked and reported under that name.

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
- **Shape**: `name` is a type name (1–64 bytes, `05-app-lifecycle-persistence.md` §2.3); `icon` and `description` are optional strings (`""`); `blocks` is required (it may be empty), each `{name, type}` with a name of 1–64 bytes, unique; `connections` (or `edges`, above), `initials`, `inports`, `outports` are optional arrays. References are `"Block.port"` split on the last `.` (`01-architecture-overview.md` §2.4). An initial's `src` is a string, number or boolean, parsed for its target port's type as a graph-file initial is (`02-wire-protocol.md` §3.4). A public port name is 1–64 bytes **without `.`** (a parent composite could not reference it), unique among the inports (resp. outports); inports and outports are separate namespaces. `type`, when given, is `"bool"`, `"number"` or `"string"`; `description` a string; `addressable` (also optional, also taken from the inner port when omitted) a boolean.
- **Checks and messages**, the first failure wins: the shape (`"Invalid field: <key>"`, the top-level key at fault; text that is not one JSON object is `"Invalid JSON"`); `name` already registered (`"Component exists: <name>"`); per inner block in order, recursion (`"Recursive component: <name>"`: the block's type is the new type's name, or a composite that contains it) then an unknown type (`"Unknown component: <type>"`); then the connections, initials, inports and outports in that order, each reference as the matching wire command reports it (`02-wire-protocol.md` §3.4.1: `"Unknown node: <block>"`, `"Unknown port: <block>.<port>"`, `"Invalid edge direction"` for an edge or initial whose end has the wrong direction — an inport or outport naming a port of the wrong direction is `"Unknown port"`, as `addinport`/`addoutport`); then fan-in, `"Input already connected: <block>.<port>"`.
- **"Fed twice"** is any second source on one inner input: two edges, an edge and an initial, two initials, or an inport and any other source (two inports included), since an exposed input's outside source is a source of the inner input. One inner output may be exposed by several outports.
- **Recursion**: since a type refers to its inner types by pointer and a type in use can be neither unregistered nor renamed (below), a registered type can never contain one registered after it; the only recursion that can be attempted is a type naming itself, or a cycle among type files (refused at startup as left over, `05-app-lifecycle-persistence.md` §1.4 step 6). The transitive check is kept anyway.
- **Canonical form** (what `component:getsource` returns and a type file holds): compact JSON with the keys `name`, `icon`, `description`, `blocks`, `connections`, `initials`, `inports`, `outports` in that order, every one present; entries in the definition's order; references written `"Block.port"`; initial values as a graph file stores them (`05-app-lifecycle-persistence.md` §2.1: `"5"` on a number port becomes `5`); a port's `type`, `description` and `addressable` written only when they were given, so a port that takes them from its inner port keeps doing so. Registering the canonical form yields the same canonical form (the round trip).
- **Registry operations** (the model side of `02-wire-protocol.md` §3.4.2, whose wire commands use them, plan task 21b; `00-README.md` open question 4 is decided): unregistering and renaming are refused while the type is in use — a graph has an instance of it, or another composite type contains it (`"Component in use: <name>"`); an unknown name, or a built-in type, is `"Unknown component: <name>"`; renaming to a registered name is `"Component exists: <to>"`, to an invalid name `"Invalid field: to"`, and moves the type to the end of the registration order (`01-architecture-overview.md` §2.3). **Changing the description or icon is allowed while the type is in use**: no instance depends on them; "updating a type that is in use is refused" (above) applies to changes that instances would see, of which only rename exists. The messages of this section are the `network:error` texts of the group commands (`02-wire-protocol.md` §3.4.2), which also define the order of their checks and what a failed save does.

### 5.2 Creating a group from a selection

Creating a group from selected nodes in a graph assembles the authoring format above:
1. The selected nodes' `{name, type}` pairs become the `blocks` array. Every edge with both endpoints inside the selection becomes a `connections` entry. Every initial targeting a selected node becomes an `initials` entry.
2. Every edge crossing the selection boundary identifies an exposed port. An inbound crossing edge (source outside, target inside) becomes an `inports` entry; an outbound crossing edge becomes an `outports` entry. Graph-level inports/outports (`02-wire-protocol.md` §3.4) that target selected nodes are exposed too, under their public names. Other exposed ports get a default public name derived from the inner port name, disambiguated with the inner block's name if two would clash, unless an explicit name is supplied at creation time.
3. The assembled JSON is registered exactly as if it had been hand-authored (§5.1) and persisted.

Decisions (plan task 21; the assembly is the model function `composite_types_assemble`, `src/blocktypes/composite_types.h`, reached from the wire through `graph:addgroup`, plan task 21b; the selected nodes are not replaced — open question 4, "save as component"):
- **Order**: `blocks` in the graph's creation order whatever the selection's order (a node named twice counts once); `connections` and `initials` per target node (creation order) and input (port order), as in a graph file (`05-app-lifecycle-persistence.md` §2.1). `inports`: first every graph-level inport mapped to a selected node, in creation order, under its public name — only the first of several mapped to the same input, since two would feed it twice (§5.1) — then one per input of a selected node fed by an edge from outside the selection, per node and input, unless a graph-level inport exposes it already. `outports`: first every graph-level outport mapped to a selected node (several may share an output), then one per output of a selected node with an edge to a node outside the selection, in the order those edges are met (per target node and input), each output once, unless a graph-level outport exposes it already.
- **Default names**: a port without a public name is named after its inner port. If that name is a public name of the same direction, or another default-named port of that direction has the same inner port name, it is `<block>_<port>` instead (every one of the clashing ports, so none is privileged); if that is taken too, contains a `.` or is longer than 64 bytes, `<port>_<n>` (or `port_<n>` if that is too long) with the smallest free `n >= 2`. `graph:addgroup` has no member for names supplied at creation time (decided, plan task 21b): every exposed port gets its default name; such a member may be added later.
- **Left out**: node metadata (positions and other UI keys), edge metadata and a placeholder's retained connections are not part of a type. A selected placeholder node goes into `blocks` with its component name, so registering the result is refused as an unknown type. `description` and `icon` come from the request (`""` if none).
- **Refusals**: an empty selection is `"Invalid field: nodes"`, a name that is no node of the graph `"Unknown node: <name>"`; everything else is refused, if at all, when the result is registered (§5.1).

## 6. PID and schedule block types

Two block families: the PID regulator (§6.1), implemented in plan task 23 (the user may later refine it with their own PID code), and the calendar blocks and schedule types (§6.2), implemented in plan task 24.

### 6.1 `process/PID` — PID regulator

A **cyclic** block (`TIMER_PERIODIC` + `compute_on_input` — `01-architecture-overview.md` §2.3) with per-instance state: the integral accumulator, the previous (filtered) measured value and the derivative filter state.
- **On every sample tick** (`reason = TIMER`, every `sample_ms`), it performs the full PID step: advance the integral by `error × dt`, compute the derivative over `dt`, compute and propagate `output`. The integral only exists in relation to a fixed sample rate, so the output is always updated at that rate.
- **On any input change** (`reason = INPUT`), it takes the new parameter or value into account immediately, recomputing `output` from the current proportional term and the existing integral, without advancing the integral or derivative state. A change to `sample_ms` re-arms the periodic timer.

**Decided behavior** (`00-README.md`, open question 5 — PID part; revised by the user 2026-10-13 to the parallel form, implemented in plan task 23):
1. **Parallel gain form**: `out = Kp·e + Ki·∫e dt + D`, where the derivative term `D` acts on the measurement (item 3) with gain `Kd`. `Ki = 0` disables the integral term, `Kd = 0` the derivative term. Gains are in output units per input unit (per second for `Ki`, times seconds for `Kd`).
2. **Acting direction**: a `reverse` input — `false` (direct): `e = setpoint − input`; `true` (reverse, e.g. cooling): `e = input − setpoint`.
3. **Derivative on the measurement**, not on the error, so a setpoint step causes no output kick.
4. **Max D gain** (`max_dgain`; at rest `0`, no limit): the derivative is filtered as `Kd·s / (1 + (Kd/max_dgain)·s)`, so its high-frequency gain is limited to `max_dgain` — a fast change in the measurement is never amplified by more than that, which keeps measurement noise from making the output jump. A `max_dgain <= 0` (or NaN) means no limit (an unfiltered derivative).
5. **Output limits and anti-windup**: the output is clamped to `[min, max]` — equal limits, as when neither is set (both `0`), are **no limits**; the integral does not advance while the output is at a limit and the error would drive it further beyond it (conditional integration), so the regulator recovers at once when the error reverses.
6. **Manual/auto**: an `auto` input (at rest `false`: manual, so a regulator nobody has set up outputs its `manual`, `0`); in manual the output follows `manual` (still clamped). Switching manual → auto is **bumpless**: the integral is preset so that the output continues from the manual value.
7. **Timing**: the full step runs every `sample_ms` (at rest `0`: no sampling until it is set) using the actual elapsed time as `dt`; an input change recomputes only the proportional part immediately (above). On `start` the integral is reset, or preset from `manual` when starting in manual.
8. **Outputs**: `output` (number), and `saturated` (bool, true while the output is at a limit) for alarms.

Ports: inputs `setpoint`, `input`, `kp`, `ki`, `kd`, `max_dgain`, `min`, `max`, `reverse` (bool), `auto` (bool), `manual`, `sample_ms`; outputs `output`, `saturated` (bool).

Port names (renamed by the user 2026-10-15, before any release; formerly `measured_value`, `output_min`, `output_max`, `manual_value`): `input`, `min`, `max` and `manual`, like the other `process/` blocks. There is no alias for the former names: a graph file connecting them loses those connections (§2.2 of `05-app-lifecycle-persistence.md` skips them).

| Port | Direction | Type | Default | Description |
|---|---|---|---|---|
| `setpoint` | input | number | `0` | Desired value of the measurement |
| `input` | input | number | `0` | Measured value of the controlled process |
| `kp` | input | number | `0` | Proportional gain (0: no proportional term) |
| `ki` | input | number | `0` | Integral gain, per second (0: no integral term) |
| `kd` | input | number | `0` | Derivative gain, in seconds (0: no derivative term) |
| `max_dgain` | input | number | `0` | Highest gain of the derivative term (<= 0: no limit) |
| `min` | input | number | `0` | Lower output limit (equal limits: none) |
| `max` | input | number | `0` | Upper output limit (equal limits: none) |
| `reverse` | input | bool | `false` | Reverse acting (error = input - setpoint), e.g. cooling |
| `auto` | input | bool | `false` | Automatic (true) or manual (false) mode |
| `manual` | input | number | `0` | Output in manual mode (clamped to the limits) |
| `sample_ms` | input | number | `0` | Sample period in milliseconds (<= 0: no sampling) |

Resting values (§1, decided 2026-10-14; until then `kp` 1, `max_dgain` 10, `max` 100, `auto` true, `sample_ms` 1000): a regulator nobody has set up rests in manual with output `0`, unlimited and without sampling; it is configured by setting its inputs.
| `output` | output | number | `0` | Regulator output, within the output limits |
| `saturated` | output | bool | `false` | true while the output is at a limit |

Decisions (plan task 23, `src/blocktypes/process.c`):
- **Type record**: icon `tune`, description `"PID regulator"`, `TIMER_PERIODIC` with `period_ms = 0` (`sample_ms` at rest: no sampling), `compute_on_input`, the engine-wide change threshold; registered after `process/Deadband`. The defaults are those of the table above (an output's default is its value before the first compute).
- **State**: the integral **term** `I` (in output units, advanced by `Ki·e·dt`), the derivative term `D` of the last sample step, the measurement at that step, and the engine time of that step. Since `I` is stored as a term, changing `ki` never bumps the output; `ki = 0` clears `I` at once, `kd = 0` clears `D` at once.
- **Derivative discretization**: `D(s) = −σ·Kd·s/(1 + Tf·s)·PV(s)` with `σ = +1` direct, `−1` reverse, and `Tf = |Kd|/max_dgain`, by backward Euler (`s ≈ (1 − z⁻¹)/dt`): `D_k = (Tf·D_{k−1} + Kd·Δy)/(Tf + dt)`, `Δy = −σ·(pv_k − pv_{k−1})`. It is stable for every `Tf >= 0` and `dt > 0`; a measurement step within one sample gives `Kd/(Tf + dt) < max_dgain` times the step, decaying by `Tf/(Tf + dt)` per sample after it. `max_dgain <= 0` or NaN means `Tf = 0`: the unfiltered `Kd·Δy/dt`. A negative `Kd` inverts the term (the filter uses `|Kd|`). The first sample step after `start` (or after an invalid measurement) only records the measurement, with `D = 0`, so the arrival of the first measured value causes no kick.
- **Sample step** (`reason = TIMER`), `dt` = engine time since the last step (or the start) in seconds, so a late tick whose missed ticks were skipped (01 §3.2) integrates the time that actually passed: advance `D`; then the integral by `inc = Ki·e·dt` with **conditional integration**: with `u0 = P + I + D` before the step, an `inc > 0` is cut to `max(0, max − u0)` and an `inc < 0` to `min(0, min − u0)` — the integral never drives the output beyond a limit (it lands on the limit, or stays when already beyond it), and an increment back towards the range always applies, so the output leaves the limit at the first sample after the error reverses. Then `output = clamp(P + I + D)`.
- **Input change** (`reason = INPUT`): `output = clamp(P + I + D)` with the current `P = Kp·e` and the stored `I` and `D`; nothing advances.
- **Limits**: `saturated` is true while `output <= min` or `output >= max` (at a limit, including exactly). A NaN limit is no limit on its side (`∓Infinity`); limits given the wrong way round (`min > max`) are swapped; **equal limits are no limits** (2026-10-14: both rest at `0`, which would otherwise pin the output to 0), so the output is unclamped and never saturated.
- **Manual/auto**: in manual every compute sets `output = clamp(manual)`; a sample step there still advances `D` (tracking the measurement) but not `I`. Switching to auto (any compute that sees `auto` true after false) presets `I = output − P − D` from the current output, so the output continues without a jump and the next sample integrates from there. With `ki = 0` there is no integral to preset (`I = 0`): the output goes to `P + D` at once. `START` resets the state; in auto the output is `clamp(P)`, in manual `clamp(manual)` with `I` preset from it.
- **Non-finite inputs**: a non-finite `setpoint`, `input`, `kp`, `ki` or `kd` in auto, or a non-finite `manual` in manual, **holds** the output (and `saturated`). A sample step with invalid inputs advances neither `I` nor `D` (it restarts the derivative at the next valid sample) but still counts as a step, so the invalid time is not integrated afterwards. A result that is not finite (overflow) holds the output as well.
- **`sample_ms`**: rounded to whole milliseconds, at least 1 and at most `2^32 − 1`; `<= 0`, NaN or infinite means **no sampling** (no timer; input changes still recompute the output, nothing integrates). Every compute but `STOP` sets the period from it through `BlockCtx.period_ms` (01 §3.2): a change re-arms the timer from now, the next sample one new period later, its `dt` still measured from the last step. Leaving "no sampling" restarts `dt` from that moment, so the idle time is not integrated. A value set while stopped applies at the start.
- `STOP` changes nothing.

**Later**: the user may refine or replace this implementation with their own field-proven PID code (adding parameters such as `Pconst`, `Itime`, `Dtime`, `MaxD`); this built-in version is the starting point.

### 6.2 Calendar blocks and schedule types

**Decided** (`00-README.md`, open question 5 — schedule part): schedules are not one configurable block. They are built from small **calendar primitive** blocks whose settings are ordinary input ports (so they can be wired, given initials, and work identically in every environment), combined with logic blocks; the **standard schedule types** the industry knows (day, week, month schedules, …) are delivered with the system as **composite block types** (§5) built from those primitives. Block metadata is never used for this (it only supports the user interface, `01-architecture-overview.md` §2.2).

Calendar primitives — each a scheduled block (`TIMER_SCHEDULED`) that reads the wall clock from the environment (`04-drivers.md` §1) and arms its timer for its next boundary; each has a `bool` output `out`, true while the current time matches:
- `timing/MinuteRange` — input `range` (string, e.g. `"12:30-15:45"`): true within that time-of-day range. The start is included and the end excluded (true from 12:30:00 until just before 15:45); a range may wrap past midnight (`"22:00-06:00"`).
- `timing/DayOfWeek` — input `days` (selector string, 1–7) and input `sunday_first` (bool: whether day 1 is Sunday rather than Monday): true on the selected weekdays.
- `timing/MonthOfYear` — input `months` (selector string, 1–12): true during the selected months.
- `timing/DayOfMonth` — input `days` (selector string, 1–31): true on the selected days of the month ("the 1st of the month"); a day a month does not have simply never matches (`"31"` is never true in April).
- Further primitives, e.g. `timing/DayOfYear`, `timing/WeekOfYear` (ISO 8601 week numbers 1–53, weeks starting on Monday), and holiday calendars such as `timing/HolidaysOfSweden`.

**Selector strings** (all day/week/month/year selectors): a single value, a range, a list, or a mix — `"3"`, `"1-5"`, `"6,7"`, `"1-3,5"` — so e.g. Monday–Friday is one `DayOfWeek` block with `"1-5"`.

**Boolean logic**: `logic/And`, `logic/Or`, `logic/Not` and `logic/Xor` with `bool` inputs and a `bool` output, for combining calendar primitives.

**Standard schedule types** (day, week, month schedules, …) are composites of the primitives and logic blocks. They are **not built into hakkas**: they are supplied separately as a **standard library** of composite type files (§5.1), which a deployment installs into its `types/` directory if it wants them. Once installed they are ordinary user types.

**Local time** (decided, `00-README.md` open question 11): the calendar primitives evaluate the wall clock in local time from the engine-wide `time_zone` POSIX TZ string (`05-app-lifecycle-persistence.md` §1.1). Daylight-saving edges: in spring the skipped local hour does not exist, and a range starting inside it begins at the jump; in autumn the repeated hour is evaluated on local wall-clock time, so a range covering it can be true twice.

Decisions (plan task 24; `src/blocktypes/calendar.h`, `calendar.c`, `logic.c`, `src/system/tz.h`):
- **The primitives**: `timing/MinuteRange` (input `range`), `timing/DayOfWeek` (`days`, `sunday_first`), `timing/DayOfMonth` (`days`, 1–31), `timing/MonthOfYear` (`months`, 1–12), `timing/DayOfYear` (`days`, 1–366, 1 = January 1, so 60 is March 1, or February 29 in a leap year, and 366 only exists in leap years) and `timing/WeekOfYear` (`weeks`, ISO 8601 weeks 1–53). The setting inputs are strings defaulting to `""` (so a new block is false until it is set), `sunday_first` a bool defaulting to `false`; the output `out` is a bool defaulting to `false`. Each is stateless, `compute_on_input` and `TIMER_SCHEDULED`, with the type flag `wall_clock` (`01-architecture-overview.md` §2.3, §3.4); every compute ignores `reason` except `STOP`, which changes nothing. Registered after `timing/Interval`, in this order (the §3 table). `timing/HolidaysOfSweden` is **not implemented**: which days count as holidays is for the user to decide.
- **Selector strings**: a comma-separated list of items, each a value or a range `a-b`, values in decimal digits only (no sign; leading zeros allowed). Spaces and tabs are allowed around values, `-` and `,`. A range whose first value is greater than its last **wraps around**: `"6-1"` over weekdays is Saturday, Sunday, Monday; `"11-2"` over months is November to February. Repeated or overlapping items are fine. A selector is **invalid** if it is empty (or only whitespace), has an empty item (`"1,,3"`, `"1,"`), a value outside the block's range (`0`, `8` for weekdays; `32` for days of the month), or any other character.
- **`MinuteRange` strings**: `"HH:MM-HH:MM"`, the hour one or two digits (0–23), the minute two digits; spaces and tabs allowed around the times and the `-`. The end may be `"24:00"`, the end of the day, so `"00:00-24:00"` is all day. An **equal start and end is an empty range** (never true): with the start included and the end excluded, `[s, s)` is empty; all day is written `"00:00-24:00"`.
- **Invalid settings**: an invalid or empty setting makes `out` false with no timer armed (no error behavior in the dataflow, `01-architecture-overview.md` §2.1); setting a valid one computes and arms again. A value that is not a string is read in its text form (`linkable_stringify`: a number `3` is `"3"`; unset is invalid).
- **Local time**: `ctx.wall_ms` (UTC, `01-architecture-overview.md` §3.4) in whole seconds, converted with the `time_zone` POSIX TZ string (`05-app-lifecycle-persistence.md` §1.1) by the portable core (`src/system/tz.h`; no time-zone database, no C library time functions). `sunday_first` makes day 1 Sunday and day 7 Saturday; otherwise day 1 is Monday (ISO 8601).
- **Timers**: every compute returns the delay to the next instant at which `out` may change: for `MinuteRange` the next start or end of the range, for the day-based primitives the next local midnight at which the day's match differs. The boundary is a local time, converted to UTC as the first instant at which local time reaches it (`tz_next_reach`); the block also wakes at every daylight-saving transition (where local time jumps without passing the boundaries in between) and re-evaluates. A wake-up that changes nothing propagates nothing. A day-based primitive looks at most 400 days ahead; if its match changes on none of them (week 53, which is years away in most years, or a selector that matches every day), it wakes after that and looks again. An input change computes at once and re-arms the timer (§2).
- **Daylight-saving edges**, as above: in spring the skipped local hour does not exist; a boundary inside it is reached at the jump, so a range starting in it begins at the jump, one ending in it ends at the jump, and one lying entirely in it (`"02:00-03:00"` in Sweden) is not true that day. In autumn the repeated hour is evaluated on local wall-clock time: a range in it (`"02:30-02:45"`) is true twice, and one covering it (`"01:00-03:00"`) is true for three hours. A day starts at local midnight in whatever offset is in effect.
- **Wall-clock steps** (the clock set by the user, an NTP correction, the first synchronization): the runtime re-runs every armed calendar primitive at once (`01-architecture-overview.md` §3.4).
- **`logic/*`**: `logic/And`, `logic/Or`, `logic/Xor` with inputs `in1`, `in2`, and `logic/Not` with input `in`; every port `"bool"` with default `false`, every input read with `to_bool`; port descriptions `"Operand 1"`, `"Operand 2"`, `"Operand"`, `out` → `"Result"`; icon `logic`. Registered after the `compare/` family (the §3 table).
- **The standard library** is the directory **`library/schedules/`** of the repository: one composite type file (§5.1, hakkas terms, `"connections"`) per type, named after the type without its `schedule/` prefix (e.g. `WeekSchedule.json` holds `schedule/WeekSchedule`; decided 2026-10-14, the user's rename). A deployment installs it by copying the files into `<datadir>/types/` **under the encoded type name** (`05-app-lifecycle-persistence.md` §2.3, e.g. `schedule%2FWeekSchedule.json`; the command is in `README.md`), the name the engine saves and removes the type by; a file under another name loads with a warning (`05-app-lifecycle-persistence.md` §1.4 step 6). They are then ordinary user types, loaded at startup (`05-app-lifecycle-persistence.md` §1.4 step 6). The types, all with icon `schedule` and the output `out` (bool):
  - `schedule/DaySchedule` — input `range`: `timing/MinuteRange` alone, true every day within the range;
  - `schedule/WeekSchedule` — inputs `range`, `days`, `sunday_first`: `timing/MinuteRange` AND `timing/DayOfWeek`;
  - `schedule/MonthSchedule` — inputs `range`, `days`: `timing/MinuteRange` AND `timing/DayOfMonth`;
  - `schedule/YearSchedule` — inputs `range`, `months`: `timing/MinuteRange` AND `timing/MonthOfYear`.
  The inner blocks are `Time` (the range), `Days`/`Months` and `Both` (`logic/And`). Each input maps one primitive's setting, so its default is the primitive's (`""`: false until set).

## 7. Alarms

### 7.1 `process/Alarm` — alarm handling

Decided with the user 2026-10-15 (`00-README.md` open question 15; more details about alarms are to be documented later); implemented in plan task 50 (`src/blocktypes/process_more.c`). An alarm block follows an **alarm condition** (`input`) and keeps the alarm's **state**, which a person acknowledges. Its presentation inputs (`category`, `class`) are for alarm views in the user interface and do not change its behavior. Stateful (the current state, the last `input` and `acknowledge` values), `compute_on_input`, `TIMER_NONE`. Registered after `process/AutoMan`.

**States**: `Normal`, `Activated`, `Deactivated`, `Reactivated`, `Acknowledged`, `Disabled`, `Blocked`. The alarm is in exactly one of them at a time.

| Port | Direction | Type | Description |
|---|---|---|---|
| `input` | input | bool | The alarm condition: true while the alarmed situation exists |
| `acknowledge` | input | bool | A push button: a person has noticed the alarm (acts on its rising edge) |
| `block` | input | bool | Blocks the alarm: no state changes; state `Blocked` |
| `disable` | input | bool | Disables the alarm: no state changes; state `Disabled` |
| `category` | input | string | Free text for the presentation (e.g. "Elevators", "Heating") |
| `class` | input | string | The alarm class, `"A"`, `"B"`, `"C"` or `"D"` (below; `"C"` when not set) |
| `normal`, `activated`, `deactivated`, `reactivated`, `acknowledged`, `disabled`, `blocked` | output | bool | true while the alarm is in that state (exactly one is true) |
| `state` | output | string | The current state's name, e.g. `"Activated"` |

- **`block` and `disable`** both stop the alarm's state changes. A **disabled** alarm is a lasting condition (the alarm is switched off); a **blocked** alarm is put aside for a while, to stop it **fluttering** — a condition going on and off all the time, raising many alarm events. Alarm views present them differently. Every input rests at its type's resting value (§1): a new alarm is neither disabled nor blocked (the input was `enable`, resting at false, until the user chose `disable` on 2026-10-15).
- **Classes** say how soon the alarm must be attended to:
  - `A` — attention required 24/7. E.g. "Person stuck in elevator", "Fire alarm".
  - `B` — attend to during working hours. E.g. "Heating system failure", "Nutrient tank empty".
  - `C` — attend to soon. E.g. "Heat pump inoperable".
  - `D` — a minor event that may be of interest. E.g. "Regulator not moving".

**Transitions** (decided with the user 2026-10-15). "Input on/off" is a change of `to_bool(input)`; "Ack" is a **rising edge** of `acknowledge` (false → true; holding it true does nothing more):

| State | Input on | Input off | Ack |
|---|---|---|---|
| `Normal` | → `Activated` | — | — |
| `Activated` | — | → `Deactivated` | → `Acknowledged` |
| `Deactivated` | → `Reactivated` | — | → `Normal` |
| `Reactivated` | — | → `Deactivated` | → `Acknowledged` |
| `Acknowledged` | — | → `Normal` | — |

`Deactivated`: the condition is gone but nobody has acknowledged the alarm. `Reactivated`: the condition came back before anyone acknowledged it.

- **Disabled and blocked**: while `disable` is true the state is `Disabled`; otherwise, while `block` is true, it is `Blocked` — **`Disabled` wins over `Blocked`**. In either, changes of `input` and presses of `acknowledge` change nothing.
- **Enabled or unblocked again** (neither `disable` nor `block` true any more): the alarm goes to **`Normal` first, then `input` is evaluated**: if the condition holds, it goes on to `Activated` (a new alarm, to be acknowledged). The outputs are pushed once, after the compute, so they show only the final state (`Activated`, not the passing `Normal`); the passing `Normal` matters once alarm events are recorded, which is among the details to be documented later.
- **Start** (`reason = START`): the same — `Normal`, then `input` evaluated (or `Disabled`/`Blocked`). Nothing of the state survives a stop. `STOP` changes nothing.

**Class** (decided with the user 2026-10-15): only `"A"`, `"B"`, `"C"` and `"D"` are classes, matched exactly (upper case, no spaces). The block reads **any other value as `"C"`** — the unset input's `""` (§1; there are no per-port defaults, so `"C"` is not a port default) as well as `"b"`, `"X"` or a number — since a value arriving over a connection cannot be refused. The frontend will offer only the four classes as the input's fixed value (to be specified in `06-frontend.md` when alarms get their views).
