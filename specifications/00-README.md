# Hakkas — System Specification

## What this is

Hakkas is a flow-based-programming (FBP) runtime: users wire "blocks" (components with named, typed input/output ports) into a live dataflow graph. There is no separate edit/run mode — the graph is always running; structural changes (add/remove/connect/rename a block) and value changes take effect immediately, exactly like rewiring electronics while powered. A WebSocket protocol, a dialect of the FBP network protocol, lets a browser-based editor observe and mutate the running graph in real time. That frontend is a separate, already-existing artifact and is out of scope for this specification — `02-wire-protocol.md` defines the exact wire contract it expects.

This directory specifies hakkas' design and behavior completely enough to implement it in C.

## Core principle: the engine owns the model, the UI only mirrors it

The frontend never updates its own view in response to its own actions. Every user action is sent to the engine as a command; the engine applies (or refuses) it and broadcasts the resulting event to every client that has the graph open, including the sender; each client updates its view only from those events. This keeps the engine's model and every open view in sync, including when several users edit the same graph at once. See `02-wire-protocol.md` §1.1.

## Target platforms

- **Primary**: MCUs with no general-purpose OS — RP2354 (RP2350 family) and possibly STM32H5/STM32H7/STM32N6 — running **Zephyr RTOS**, with the device hosting the full WebSocket/FBP server and serving the frontend's static assets directly.
- **Secondary build tree**: a POSIX/Linux-native build, used for Raspberry Pi deployment and for x86_64 development. It shares the entire portable core with the Zephyr build; see `01-architecture-overview.md` §0 for the Platform Abstraction Layer (PAL) boundary that makes this possible with only a small, isolated set of platform-specific files.
- Hardware bring-up on the Zephyr targets (devicetree, board support, peripheral drivers) is the responsibility of a separate effort; hakkas consumes Zephyr's device driver subsystem rather than talking to hardware registers directly. The device firmware provides the environment interface hakkas queries for the available inputs and outputs (`04-drivers.md` §1).

## Document index

| File | Covers |
|---|---|
| `00-README.md` | This file — scope, target platforms, open questions |
| `01-architecture-overview.md` | Core graph/block/port data model, the generic data-driven block-execution engine, the concurrency model, the Platform Abstraction Layer, module map |
| `02-wire-protocol.md` | The complete WebSocket FBP-dialect protocol — the browser frontend's compatibility contract |
| `03-block-types.md` | The block type library: math/process/timing blocks, the generic execution engine, composite ("group") blocks, PID and schedule blocks |
| `04-drivers.md` | The hardware IO layer: the environment interface hakkas queries for external inputs/outputs, binding graph ports to them; networking on Zephyr |
| `05-app-lifecycle-persistence.md` | Startup/configuration, graph persistence format, the static/web server, logging |
| `fbp-protocol.html` | The upstream FBP network protocol (Flowhub/NoFlo), of which `02-wire-protocol.md` is a modified dialect. Reference only — where the two disagree, `02` wins. |

**Read order recommendation**: this file → `01` → `02` → `03` → `04` → `05`.

## Terminology

- **Graph** and **process** are synonyms: one dataflow network of wired blocks, typically one automation function such as "hot water regulator" or "ventilation flow alarm". One engine runs many graphs at once; each graph can be started and stopped independently (`01-architecture-overview.md` §2.2).
- **Engine**: the always-running runtime that hosts every graph. The engine itself is never "stopped" while the system is up.
- **Initial** (FBP term): a literal value wired to a block input instead of a block output. See `01-architecture-overview.md` §2.2.
- **Group** / **composite block type**: the same thing — a user-selected subgraph, given a name and exposed ports, that becomes a reusable user-defined block type (`03-block-types.md` §5).
- **Change threshold**: the minimum change in a numeric output for it to be propagated downstream (`01-architecture-overview.md` §3.3).

## Protocol compatibility policy

The frontend already exists and already speaks a specific wire protocol. **Default policy: match it exactly** — `02-wire-protocol.md` specifies every message shape the frontend sends and expects, and the C implementation should honor that contract rather than "improve" it, since breaking it breaks a real client with no corresponding update planned.

**Exception**: in areas where the frontend has no real implementation today — most notably authentication (whose login handshake is redesigned, requiring a matching frontend change) and the group/composite-block commands — the wire shapes are free to be designed cleanly rather than contorted to match incomplete or unused client code. `02-wire-protocol.md` marks these areas explicitly. Protocol changes are always *allowed*, just *discouraged* by default; the bar for changing an area the frontend actively uses is much higher than for an area it doesn't yet.

## Open questions (need your input before/while implementation)

1. **Raspberry Pi vs. x86_64 build tree specifics**: the POSIX build tree ("Target platforms" above) needs its own concrete choices — networking, timer facility, filesystem layout — beyond "mirror the Zephyr build's shape using POSIX primitives" (`01-architecture-overview.md` §0/§3.2.1). *Settled*: storage (plain files under the configured directories, atomic writes with `fsync` and directory sync), the clock (`CLOCK_MONOTONIC` since `pal_init`) and the random source (`getrandom()`) — `01-architecture-overview.md` §0.1; the file locations (`--confdir`, `--basedir`, `webdir`) — `05-app-lifecycle-persistence.md` §1.1–§1.2. Timers are settled for both build trees: there is no OS timer facility at all — timers are deadlines kept by the portable core, and the event loop's `poll()` timeout is the only timing primitive (`01-architecture-overview.md` §3.2 item 7). Networking is settled (plan task 12): plain non-blocking BSD sockets and `poll()`, numeric IPv4/IPv6 addresses, an `eventfd` wakeup for the cross-context enqueue, `MSG_NOSIGNAL`, `SIGINT`/`SIGTERM` as stop requests (`01-architecture-overview.md` §0.2, §3.2.1). Every part of this question is now decided.
2. **EEPROM module-descriptor format**: *decided — dropped.* The hardware platform it was written for no longer exists; no EEPROM is involved. Instead the environment (device firmware on Zephyr, a simulator on POSIX) provides a service provider interface that hakkas queries for the available external inputs and outputs, described like block types but not instantiable (`04-drivers.md` §1). Binding graph ports to them is open question 3; their representation is open question 6.
3. **Binding graph ports to the environment**: *deferred until the environment SPI (`04-drivers.md` §1) is implemented* — the mechanism isn't defined well enough yet. What is settled: the same process (graph) must run unchanged in different environments, by applying a different **bindings file** that maps `"<block>.<port>"` references to environment ports. Current leaning, from the sister project's `examples/bindings-emulator.json`: one bindings file per process and environment (`"driver"` names the environment; each binding pairs `"process": {"node", "port"}` with `"env": {"port", "index"}`, environment ports being addressable groups of indexed channels), paired with its process in the process list (`processes.json`, `05-app-lifecycle-persistence.md` §1.2). Cross-file conflicts (e.g. two processes driving the same environment output) are then checked at startup.
4. **Group / composite creation mechanism**: *decided* — **"save as component"**. Creating a group from a selection builds and registers a new composite block type (`03-block-types.md` §5.2) and leaves the selected blocks unchanged; the user replaces them by an instance manually if they want to. This keeps the action simple and predictable for the user. A separate "collapse" option (replace the selection by an instance) may be added later. The wire commands are those of `02-wire-protocol.md` §3.4.2. Graph-level inports/outports, which this question also mentioned, are now part of open question 13 (linking processes).
5. **PID and schedule block parameters**: *PID decided* (`03-block-types.md` §6.1): standard-form `Kp`/`Ti`/`Td`, direct/reverse acting, derivative on measurement with filter, conditional-integration anti-windup, bumpless manual/auto, a `saturated` output; the user will add the parameters `Pconst`, `Itime`, `Dtime`, `MaxD` from their existing PID implementations, on which the implementation is to be based. *Still open*: the schedule blocks' entry data model (`03-block-types.md` §6.2).
6. **Representing the hardware environment**: physical IO channels exist whether or not a user wants them; the user must not be able to delete them, but should be able to hide them and organize them into different graphs. How hardware channels appear in the graph model (pre-created non-deletable blocks? a dedicated "hardware" graph? something else?) is open. Closely related: what a physical output does when its graph is stopped, its block is unwired, before any graph has loaded, or after a crash (hold last value vs. go to a defined safe state).
7. **Change threshold semantics**: the threshold (`01-architecture-overview.md` §3.3) is currently specified as an absolute difference. Whether a relative (percentage) threshold, or a per-output rather than per-block override, is also needed is open.
8. **Numeric edge cases in type conversion**: *decided* for double → integer conversion — saturating, NaN → 0 (`01-architecture-overview.md` §2.1). Still open only if practice shows another behavior is needed (e.g. whether `to_u64` of a negative double should wrap like the i64 → u64 cast instead of clamping to 0).
9. **`Metric` arithmetic**: the rules in `01-architecture-overview.md` §2.1 are a statement of intent — unit tables, the per-block error flag's wire representation, and the rules for mixing metric with plain numbers are not specified, and the feature may be reworked or dropped once tried in practice.
10. **Work-queue capacity formula**: for now the work queue has a fixed, per-build configurable capacity (default 1024 entries, `01-architecture-overview.md` §3.2). Whether to size it from the total number of wires instead is deferred until real graph sizes are known.
11. **Wall-clock time**: Zephyr will provide a wall clock, but the source (SNTP, RTC) is not yet worked out. Until then all timestamps are derived from engine uptime (`01-architecture-overview.md` §3.4). All time is UTC.
12. **Graph auto-start**: this specification assumes a graph's started/stopped state is persisted and restored at boot (`05-app-lifecycle-persistence.md` §2), so a rebooted controller resumes control without user action. Confirm this is wanted.
13. **Linking processes together**: a process (graph) models one automation function — e.g. "hot water regulator" or "ventilation flow alarm" — and processes must be able to exchange values, through **ports**. The mechanism is not defined yet. Starting points in this specification: graph-level inports/outports (`01-architecture-overview.md` §2.2), currently metadata used only to define a group's external ports, and the reserved `"link"` port type (`01-architecture-overview.md` §2.1), which the frontend's type list already carries "for inter-graph port placeholders". Needs: how a link between two processes' ports is declared (wire messages, persistence), how values cross it (the same work queue and change threshold as any link?), and what happens when one end's process is stopped, deleted or not loaded.
