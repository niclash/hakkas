# 01 — Core Architecture: Graph Engine, Concurrency Model, Module Map

## 0. Platform Abstraction Layer (PAL)

The core engine — the graph/block model, protocol dispatch, and persistence JSON handling — is portable, platform-independent C. No Zephyr headers, no POSIX headers, and no direct hardware access appear anywhere in the graph engine, block type, protocol, or persistence code. All of that code calls only a small set of PAL functions, implemented once per target:

- **`platform/zephyr/`** — the MCU implementation: Zephyr sockets (`zsock_*`) for networking, `k_timer`/`k_work` for timers and deferred work, one Zephyr thread (or the system work queue) as the engine thread, Zephyr's filesystem subsystem over LittleFS for persistence, and Zephyr's device driver subsystem for hardware access (`04-drivers.md`).
- **`platform/posix/`** — the Raspberry Pi / Linux / x86_64-development implementation: POSIX sockets, `timerfd`/`poll` for timers, a single process thread, a POSIX filesystem directory tree for persistence, and a **simulated IO provider** so a complete development system runs on a desktop with no hardware.

The PAL surface is small and deliberate: open/close/read/write/poll for sockets, start/stop/cancel for timers, a monotonic millisecond clock, read/write/rename/remove/exists/list for storage, a random-bytes source (for UUIDs and session secrets), an **IO-provider interface** (enumerate channels, write a channel, deliver channel events — `04-drivers.md`), and one function to enqueue work onto the engine's queue from a different execution context (needed because timer callbacks and the hardware-polling worker in `04-drivers.md` §1 run outside the engine's own thread and must hand work off rather than call into the engine directly — see §3.2.1). **Only a small, isolated number of files contain platform-specific code at all** — everything else, including every block type and the entire protocol layer, is platform-agnostic and compiles unchanged against either PAL implementation.

## 1. What the system does

Users wire "blocks" — components with named, typed input/output ports — into a dataflow graph. There is no separate edit/run mode: the graph is always live, and any structural change (add/remove/connect/rename) or value change takes effect immediately, mid-execution, exactly like rewiring electronics while powered. Each block recomputes its outputs whenever its inputs change (or, for timer-driven block types, on a schedule), and every output change propagates to every connected downstream input. A WebSocket protocol (`02-wire-protocol.md`) — a dialect of the FBP network protocol — lets a browser-based editor observe and mutate the running graph in real time.

## 2. Core data model

### 2.1 Value type — `Linkable`

Every port holds exactly one dynamically-typed value: a string, a signed 64-bit integer, a double, a boolean, a physical-unit-tagged `Metric` value, or "unset." There is no array/list value type. Type name strings used in port descriptors are `"bool"`, `"number"`, `"string"`. (`"link"` appears in the frontend's type list for inter-graph port placeholders; it is reserved and never produced by this implementation.) A `Metric` travels through `"number"` ports.

**`Metric`** — *provisional; this may be reworked or dropped once tried in practice (`00-README.md`, open question 9)*: a tagged physical-unit value — a magnitude (double) plus a unit, organized into categories (acceleration, angle, area, current, density, distance, energy, flow, force, frequency, gravity, humidity, mass, periodicity, power, pressure, ratio, resistance, temperature, velocity, voltage, volume). Parsing a metric from text splits on the first space into `(value, unit)` and matches the unit against per-category unit tables (not yet defined). **C model**: `struct { double value; const char *unit; enum metric_category category; }`.

Intent:
- **Origin**: IO modules produce `Metric` values (a temperature, a voltage, …), not bare numbers (`04-drivers.md`).
- **Mapping**: the user can re-map a measured metric into what it physically represents — e.g. a sensor that measures 0–10 V is mapped to a level sensor reading 0–300 mm (`distance`). Where this mapping lives (a channel setting or a dedicated mapping block type) is not decided.
- **Arithmetic preserves units**: adding/subtracting/comparing metrics of the same category preserves the category (two temperatures add to a temperature). Combining mismatched categories in an additive operation sets the block's **error flag** (a per-block-instance boolean; how it is shown on the wire is not yet specified) and the numeric result is still computed from the magnitudes. Multiplication/division follow physical dimensions where a known relation exists (mass × acceleration → force); otherwise the result is a plain number.
- Mixing a metric with a plain number is not yet specified.

**Type conversions**, used throughout block computation:
```
to_f64(v):  f64→v; i64→(double)v; metric→v.value; bool→1.0:0.0; string→parse as f64, else 0; unset→0.0
to_i64(v):  f64→(int64_t)v; i64→v; metric→(int64_t)v.value; bool→1:0; string→parse as i64, else 0; unset→0
to_u64(v):  f64→(uint64_t)v; i64→(uint64_t)v; metric→(uint64_t)v.value; bool→1:0; string→parse as u64, else 0; unset→0
to_bool(v): bool→v; f64→v!=0.0; metric→v.value!=0.0; i64→v!=0; string→(v=="true") [exact match only]; unset→false
default_value(typename): "bool"→false; "number"→0.0; "string"→""; else→0.0
```
Stringifying a value for display/wire transport uses each type's natural string representation; a value with no natural string form renders as `"<not stringable>"`.

The table above follows the value model's natural behavior; it doesn't define behavior for converting NaN, ±Infinity, or out-of-range doubles to integers. These inputs are rare and the exact result is deferred (`00-README.md`, open question 8): pick the most runtime-efficient behavior that is still well-defined in C (a plain C cast of such a value is undefined behavior and must not be used).

There is no "error" behavior in value computation — as in electronics, every operation has well-defined limits and a well-defined result when they are exceeded (e.g. IEEE-754 `±Infinity`/`NaN` for division by zero). Blocks never reject a value.

Port descriptors carry: `name`, `description`, `type`, `default` (the value the input holds when nothing is wired to it; falls back to `default_value(type)` if the block type doesn't specify one), `addressable` (bool — reserved metadata; nothing in the runtime currently indexes by address), and, for ports belonging to a composite block type (`03-block-types.md` §5), a `target`/`source` reference into the inner block graph.

### 2.2 Entities and ownership

```
Engine
 └─ Graphs  (engine-wide registry)
     └─ Graph  (one dataflow network, a.k.a. "process"; has an id, name, description, icon)
         ├─ Block[]        (named instances of a block type)
         │    ├─ Input[]   (named, typed, holds current value; fed by at most ONE source: an upstream output OR a literal "initial")
         │    └─ Output[]  (named, typed, holds current + last-propagated value + list of links to downstream inputs)
         ├─ Port[] (inports)   — a graph-level named input mapped to one inner block's input
         └─ Port[] (outports)  — a graph-level named output mapped to one inner block's output
```

- **Engine**: always running while the system is up. It hosts any number of graphs simultaneously; graphs are started and stopped individually, the engine never is.
- **`Graphs`**: owns the `id → Graph` map, a periodic timer (fires once per second, accumulating the `uptime` counter of every started graph), and the list of **subscribers** (§4) that every structural/lifecycle event is broadcast to. Also owns link-value subscription plumbing (`subscribe_links`/`unsubscribe_links`), forwarded down to the target `Graph`.
- **`Graph`**: owns its blocks (keyed by instance name) and its exported ports (inports/outports, keyed by exported name). Tracks `started`/`running`/`debug`/`uptime`/`time_started` (`running` mirrors `started`; `debug` is always `false` — there is no debug mode). Every mutating operation (`create_block`, `connect`, `disconnect`, `remove_block`, `rename_block`, `add_inport`, etc.) performs the mutation **and** reports it to `Graphs`, which fans it out to every subscriber — this is the sole mechanism driving the wire protocol's broadcast messages (`graph:addnode`, `graph:addedge`, etc. — `02-wire-protocol.md` §3.4).
- **Started vs. stopped graph**: a stopped graph keeps its full structure and its inputs keep accepting values (structural edits and initials still apply), but **no events propagate**: no compute steps run, no outputs are pushed downstream, and its timers are cancelled. Starting a graph runs every block's compute step once with its current inputs and pushes every output downstream unconditionally (bypassing the change threshold, §3.3), then resumes normal propagation. A block added to a started graph is started immediately.
- **`Block`**: the uniform contract every block type implements — `update(input, value)`, `get_input`/`get_output`, `set_initial`/`remove_initial`, `connect`/`disconnect_edge`/`disconnect_block`, `rename`/`rename_of`, `change(metadata)`, `start`/`stop`/`destroy`, `refresh`, `describe`, `subscribe_link`/`unsubscribe_link`. **This contract is implemented once, generically** — see §2.3 — not once per block type.
- **`Input`**: holds `value`, its single source (none, one upstream link, or one literal initial), and a list of **live-value subscriptions** used by the `network:edges` wire feature (`02-wire-protocol.md` §4.2) to stream a value to a specific client.
  - **No fan-in**: an input is connected to at most one source. Connecting an output to an already-fed input, or adding an initial to an input fed by an edge, is invalid (`02-wire-protocol.md` §3.4.1). Adding an initial to an input that already has one replaces the literal value.
  - **Initials** (FBP term — see `fbp-protocol.html`): a literal value wired to an input in place of an upstream output, like tying a pin to a fixed voltage. Adding an initial delivers its value to the input immediately (it propagates through the graph like any other input change, if the graph is started). Initials must be wired explicitly; there is no implicit "stored default" per input.
  - **Severing**: when an input's source is removed (edge removed, initial removed, upstream block removed), the input immediately reverts to its port's `default` value (§2.1), exactly as an unconnected pin in electronics falls to its defined level, and that change propagates. Any queued, not-yet-delivered values from the severed link are dropped (§3.2).
- **`Output`**: holds `value`, `last_propagated` (the value most recently pushed downstream, used by the change threshold, §3.3), and a list of links — each a `(destination_block, destination_input_name)` pair. An output may fan out to any number of inputs. Setting an output's value (`output_set`, §3.2) is **the single propagation primitive the entire dataflow engine is built on.** A newly connected link immediately receives the output's current `last_propagated` value — wiring a connection delivers the signal already present on it.
- **`Port`** (inport/outport): a graph-level named port mapped to one inner block's input/output. Its purpose is to define the external interface the graph has when it is used as a group/composite block type (`03-block-types.md` §5). In a top-level graph that isn't used as a composite, inports/outports have no dataflow effect — they are metadata. The full mechanism is not yet settled (`00-README.md`, open question 4).

### 2.3 One generic block runtime, not one implementation per block type

Almost all of a block type's logic is identical event-handling plumbing — react to an input changing, recompute, propagate the output. The only real per-type difference is *which function* does the computing. The engine should reflect that directly: implement the entire `Block` contract **once**, as shared generic code, driven by a per-block-type data record:

```
struct BlockTypeDef {
    const char *name, *icon, *description;
    InputDescriptor  inputs[N];
    OutputDescriptor outputs[M];
    bool             compute_on_input; // run compute whenever an input changes
    TimerMode        timer;            // TIMER_NONE | TIMER_PERIODIC | TIMER_SCHEDULED
    uint32_t         period_ms;        // TIMER_PERIODIC only: default period (a block may expose an input that overrides it)
    ComputeFn        compute;          // int64_t compute(BlockCtx *ctx) — see below
    size_t           state_size;       // 0 for stateless types; >0 reserves a per-instance opaque blob
    double           change_threshold; // type default for §3.3; NAN = use the engine-wide default
};

struct BlockCtx {
    const Linkable *inputs;  Linkable *outputs;  void *state;
    uint64_t now_ms;         // engine monotonic time (§3.4)
    Reason   reason;         // START | INPUT | TIMER | STOP
    int      changed_input;  // index of the input that changed, when reason == INPUT
    bool    *error;          // the block's error flag (§2.1, Metric)
};
```

`compute` returns, for `TIMER_SCHEDULED` types only, the delay in milliseconds until it wants to be called again with `reason = TIMER` (a negative value means "no timer armed"); every call re-arms or cancels the block's single one-shot timer. For other timer modes the return value is ignored.

A block *instance* is this type-definition pointer plus an array of current input/output values, its own optional change-threshold override (§3.3), and, when `state_size > 0`, a per-instance state blob the `compute` function reads and writes across invocations. This single mechanism covers every block type in `03-block-types.md`:
- **Event-driven** (`compute_on_input = true`, `TIMER_NONE`): the arithmetic/comparison blocks, the linear-transform block. `state_size = 0`, `compute` is a pure function of its inputs.
- **Scheduled** (`TIMER_SCHEDULED`, usually also `compute_on_input`): the block decides when it next runs — the pulse/PWM generator (`03-block-types.md` §3.1) arms its timer for the next edge; schedule blocks (`03-block-types.md` §6.2) arm for the next schedule boundary.
- **Cyclic** (`TIMER_PERIODIC`, usually also `compute_on_input`): the block runs at a fixed sample period and additionally reacts to input changes — the PID regulator (`03-block-types.md` §6.1).

The shared generic code implements the propagation chain in §3 exactly once, calling `compute(...)` at the right moments; it never needs to know which concrete block type it's running. The exact moments are specified in `03-block-types.md` §2.

### 2.4 Naming convention

A fully-qualified port reference is the string `"<block-instance-name>.<port-name>"`, parsed by splitting on the **last** `.` — never the first, since instance names are user-chosen and may themselves contain dots. Use one single, correct implementation of this parse rule everywhere a `"block.port"` string needs to be split; there is no scenario where a different splitting rule is correct.

## 3. Propagation semantics

### 3.1 Concurrency model

Each block is an independent, single-threaded unit of state: its operations (`update`, `refresh`, `connect`, ...) are processed one at a time, to completion, never overlapping with another operation on the *same* block. A call from one block into another (e.g. an output's `set()` calling a downstream block's `update()`) is asynchronous: it enqueues work for the receiving block and returns immediately, rather than calling into it directly. Within one block, a `refresh()` triggered from inside `update()` is effectively "the next thing this block does to itself," so a single block's own chain (`update` → set input → `refresh` → compute → set output) looks synchronous from that block's perspective — but anything that crosses to another block is always queued work, never a nested call.

### 3.2 The engine loop

Model the whole engine as **one thread with one global FIFO work queue**. Queue entries are typed:
- **input update** `(target_block, input_port, value)` — produced by `output_set`, by initials, and by severing (§2.2);
- **timer expiry** `(target_block)` — produced by a block's timer callback;
- **IO event** `(channel, value)` — produced by the hardware/simulated IO layer (`04-drivers.md`).

Entries reference blocks by a **handle** (slot index + generation counter), never a raw pointer, so an entry whose block has since been removed is recognized as stale and discarded. When a link is removed, queued input updates that were produced by that link are dropped as well (e.g. each input carries a source-generation counter that increments whenever its source changes; an input update carries the generation it was sent under and is discarded on mismatch).

1. `output_set(output, value)`: store the new value on the output; if it differs from `last_propagated` by the change threshold (§3.3), set `last_propagated` and, for each downstream link, push an input update `(link.block, link.input, value)` onto the global queue — never call into the downstream block directly.
2. A drain loop pops the queue head, dispatches it (for an input update: `block_update(block, input, value)`, which sets the input and — if the block's graph is started and its type has `compute_on_input` — runs its compute-and-propagate step, which may push more work onto the same queue), and repeats until the queue is empty.
3. This gives **FIFO ordering per block** (a block always processes its queued updates in the order they arrived) and **breadth-first propagation overall** (a block's own refresh completes and re-queues its downstream effects before any of *those* effects are processed) — without needing locks or a scheduler.
4. **Cycles do not deadlock**: a cycle in the graph just keeps enqueuing and draining. Don't add cycle detection beyond this — an unconditional feedback loop will consume CPU indefinitely, which is a property of the graph the user built, not something the engine guards against.
5. There is **no global ordering guarantee stronger than "FIFO per block, breadth-first overall."** Two independent chains' relative interleaving is not specified further than that; don't invent a stronger guarantee than this.
6. **Input changes trigger compute only for `compute_on_input` types** — other types store the new value and use it at their next timer-driven compute. The exact `start`/`stop`/`destroy`/`connect`/`subscribe_link` behavior of the generic runtime is specified in `03-block-types.md` §2.
7. **Timers**: the engine needs a timer facility for scheduled/cyclic block types and the once-per-second uptime counter. Timer callbacks only enqueue a timer-expiry entry onto the same global queue rather than touching block state from the timer-firing context, keeping all propagation on one consistent code path.
8. **Socket I/O and protocol dispatch** integrate with this same loop (socket readability and timer expiry checked from the same place) rather than running on separate threads — the whole system is single-threaded and deterministic, with no synchronization primitives needed in the core engine.
9. **Queue capacity and overflow**: the queue is a fixed-capacity ring buffer (no unbounded growth on an MCU). The engine computes the capacity it needs from the total number of wires (edges + initials) across all graphs, re-evaluating it when wiring changes — the exact formula is not yet defined (`00-README.md`, open question 10). If the queue is nevertheless full, the **oldest** entry is dropped to make room; the engine counts drops and logs a rate-limited `warn`.

### 3.2.1 Running this on each PAL target

The loop shape is identical on both build trees; only the primitives backing it differ:
- **Zephyr build**: one Zephyr thread (or the system work queue). The global queue can be a `k_fifo`/`k_msgq`, or — since everything runs on one thread with no cross-thread contention to guard against — a plain ring buffer is equally valid and simpler. Timers use `k_timer`, with the expiry callback enqueuing work rather than mutating state directly (Zephyr timer callbacks typically run in ISR or system-workqueue context — keep them to "enqueue and return"; do the actual computation only from the engine's own thread). Socket I/O is polled via `zsock_poll`. The hardware-module-polling worker (`04-drivers.md` §1) runs as its own Zephyr worker and must also only *enqueue* readings for the engine thread — never call block logic directly from that context.
- **POSIX build**: the identical loop shape using `poll()`/`timerfd` instead of Zephyr's primitives — one thread, one queue.

Either way: `poll for I/O/timer readiness → drain readable sockets into protocol dispatch → drain the update queue → repeat`. Write this loop once against the PAL's abstract primitives (§0) rather than duplicating it per platform.

### 3.3 Change threshold — only changes propagate

An output is pushed downstream only when its value has **changed** since it was last propagated:
- **Numbers** (`i64`, `f64`, `Metric` magnitude): changed when `|new − last_propagated| >= threshold`. Comparing against `last_propagated` (not the previous computed value) means slow drift still propagates once it accumulates past the threshold. A transition into or out of NaN counts as a change; NaN → NaN does not. A change of `Metric` category/unit always counts as a change.
- **Booleans, strings, unset, and any change of value type** (e.g. number → string): changed on any difference.
- The very first value after `start` is always propagated (§2.2).

The threshold is resolved in this order, first match wins:
1. the **block instance's** override (`change_threshold` in the node's metadata — `05-app-lifecycle-persistence.md` §2, settable via `graph:addnode`/`graph:changenode`, `02-wire-protocol.md` §3.4);
2. the **block type's** default (`BlockTypeDef.change_threshold`);
3. the **engine-wide** default from the configuration file (`change_threshold`, initial default `0.05` — `05-app-lifecycle-persistence.md` §1).

A threshold of `0` propagates every distinct value. The threshold is absolute; whether relative thresholds or per-output overrides are needed is open (`00-README.md`, open question 7).

### 3.4 Time

All engine timing uses a monotonic millisecond clock starting at boot (engine **uptime**), obtained from the PAL. Until a wall-clock source is worked out (`00-README.md`, open question 11), timestamps on the wire are derived from uptime (`02-wire-protocol.md` §3.3). Once a wall clock exists, it is always **UTC** — there is no time-zone handling anywhere in the system.

### 3.5 Futures collapse to direct calls

Anything that looks like it needs an asynchronous result (resolving a block type by name, building a graph's description) is, in a single-threaded engine, a plain synchronous function call with a return value — `block_types_get(name)` returns a `BlockTypeDef*` directly; `graph_describe()` builds and returns a JSON value directly. The only place genuine asynchrony belongs is I/O that must go through the event loop (socket reads/writes, timers, file access) — never a future/promise abstraction layered over otherwise-synchronous engine logic. In particular, graph loading creates each block and moves on without needing to poll for the block's existence — a block simply exists once `create_block` returns.

## 4. Event subscription / broadcast model

A `GraphNotify`-style callback interface is what `Graphs` fans every structural/lifecycle event out to: added/removed/renamed/changed block; added/removed connection; added/removed/renamed in/outport; added/removed initial; graph created/deleted/started/stopped/status; generic error. Each subscribed connection registers one such callback set (optionally scoped to a single graph once a client has issued `graph:connect` — `02-wire-protocol.md` §4.1), which turns each callback directly into an outgoing wire message. These events are the **only** way any client — including the one whose command caused the change — learns about a change to the model (`02-wire-protocol.md` §1.1). Each event must therefore describe the state actually applied, and be emitted after the mutation is complete. This is naturally a list of `(connection, optional_graph_filter)` registrations that the graph-mutation functions iterate and notify synchronously — notification is just "write JSON to this connection's outgoing buffer," which the event loop then flushes; no asynchrony is needed here.

## 5. Startup / module wiring (`05-app-lifecycle-persistence.md` has full detail)

```
Configuration → SystemContext (file locations, log level, auth)
             → BlockTypes (registers all built-in block types)
             → Authorizer
             → RuntimeEngine
                 ├─ Graphs  (graph registry)
                 ├─ Protocol dispatcher (environment/runtime/network/graph/component/trace)
                 ├─ Web server on port             → static asset serving only
                 ├─ WebSocket listener on port+1   → FBP protocol; routes text frames into protocol dispatch
                 └─ IO layer (`04-drivers.md`)
             → load each configured/discovered graph file
```

## 6. Module map

| Module | Responsibility | Platform-specific? | Spec reference |
|---|---|---|---|
| `graphs/` | Core engine: Graph, Graphs, the generic block runtime, Input, Output, Port, Link, the work queue, change-threshold logic, event-notification dispatch, the `Linkable` value type + converters | No — pure portable C | this doc, §2–4 |
| `blocktypes/` | Block type registry + every concrete block type's data record (math, linear transform, PWM timer, composite/"group" blocks, PID + schedule) | No | `03-block-types.md` |
| `protocol/` | JSON envelope parsing, the six sub-protocol dispatchers, every request/response/broadcast message, authentication | No (credential *storage* touches the PAL for persistence; auth logic itself does not) | `02-wire-protocol.md` |
| `system/` | File locations, logging, configuration-file parsing | No | `05-app-lifecycle-persistence.md` §1, §4 |
| `app/` | Graph-file and composite-type JSON load/save | No (storage access goes through the PAL) | `05-app-lifecycle-persistence.md` §2 |
| **`platform/zephyr/`** | PAL implementation: Zephyr sockets, `k_timer`/`k_work`, LittleFS, the Zephyr IO provider (device driver subsystem access) | **Yes — isolated here** | this doc, §0 |
| **`platform/posix/`** | PAL implementation: POSIX sockets, `timerfd`/`poll`, a POSIX filesystem tree, the simulated IO provider | **Yes — isolated here** | this doc, §0 |
| `drivers/` | The platform-independent side of the IO layer: channel table, logical-id binding, delivering IO events to `io/*` blocks; later the generic descriptor-driven module driver | No — talks only to the PAL's IO-provider interface | `04-drivers.md` |
| `engine/` | Top-level wiring/startup sequence | No | `05-app-lifecycle-persistence.md` §1.4 |

**Source layout and build**: each module above lives under `src/<module>/`; each PAL implementation lives under `src/platform/<port>/` (`src/platform/posix/` now, `src/platform/zephyr/` later). The repository is a plain CMake project with no RTOS dependency: it builds the portable core as a library (`hakkas_core`), the POSIX runtime, and the unit tests, all on a desktop host. Zephyr is integrated later as a separate layer (e.g. a Zephyr module) that compiles the same portable-core source list under Zephyr's toolchain, adding only `src/platform/zephyr/`. The code is C17.

Recommended C-level building blocks (libraries to consider, not a final decision):
- **Event loop / scheduling**: Zephyr's own kernel primitives on the Zephyr build (`zsock_poll`, `k_timer`); `poll()`/`timerfd` on the POSIX build.
- **Networking transport**: Zephyr's native BSD-sockets-style API (`zephyr/net/socket.h`) on the Zephyr build, since the device hosts the full WebSocket/FBP server and serves the frontend's static assets. For WebSocket framing/handshake: Zephyr has a minimal WebSocket implementation in its networking subsystem (`zephyr/net/websocket.h`) — evaluate whether it's sufficient or needs a thin RFC6455 layer written against the PAL (so the same framing code runs on both build trees). Avoid Linux-only libraries like `libwebsockets` if sharing this code across both PALs matters.
- **JSON**: a small, allocator-friendly library suited to constrained memory (one supporting arena/static allocation ideally) over a heap-heavy one; `cJSON` is a reasonable default and runs on both build trees.
- **Persistence**: Zephyr's built-in LittleFS integration on MCU targets; a plain POSIX directory tree on the other build tree — see `05-app-lifecycle-persistence.md` for the JSON schema stored either way. The `app/` loader/saver code should only call PAL storage primitives, never a filesystem API directly.
- **GPIO/I2C/hardware IO**: consumed via Zephyr's device driver subsystem (`zephyr/drivers/gpio.h`, `zephyr/drivers/i2c.h`) per `04-drivers.md`'s boot-time module-discovery design; hardware bring-up itself is a separate effort's responsibility.
- **Development**: either build tree works for x86_64 development — the POSIX tree for the fastest edit/compile/test loop, Zephyr's `native_sim` board when Zephyr-specific behavior needs exercising.
