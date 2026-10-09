# Hakkas — System Specification

## What this is

Hakkas is a flow-based-programming (FBP) runtime: users wire "blocks" (components with named, typed input/output ports) into a live dataflow graph. There is no separate edit/run mode — the graph is always running; structural changes (add/remove/connect/rename a block) and value changes take effect immediately, exactly like rewiring electronics while powered. A WebSocket protocol, a dialect of the FBP network protocol, lets a browser-based editor observe and mutate the running graph in real time. That frontend is a separate, already-existing artifact and is out of scope for this specification — `02-wire-protocol.md` defines the exact wire contract it expects.

This directory specifies hakkas' design and behavior completely enough to implement it in C.

## Core principle: the engine owns the model, the UI only mirrors it

The frontend never updates its own view in response to its own actions. Every user action is sent to the engine as a command; the engine applies (or refuses) it and broadcasts the resulting event to every client that has the graph open, including the sender; each client updates its view only from those events. This keeps the engine's model and every open view in sync, including when several users edit the same graph at once. See `02-wire-protocol.md` §1.1.

## Target platforms

- **Primary**: MCUs with no general-purpose OS — RP2354 (RP2350 family) and possibly STM32H5/STM32H7/STM32N6 — running **Zephyr RTOS**, with the device hosting the full WebSocket/FBP server and serving the frontend's static assets directly.
- **Secondary build tree**: a POSIX/Linux-native build, used for Raspberry Pi deployment and for x86_64 development. It shares the entire portable core with the Zephyr build; see `01-architecture-overview.md` §0 for the Platform Abstraction Layer (PAL) boundary that makes this possible with only a small, isolated set of platform-specific files.
- Hardware bring-up on the Zephyr targets (devicetree, board support, peripheral drivers) is the responsibility of a separate effort; hakkas consumes Zephyr's device driver subsystem rather than talking to hardware registers directly. See `04-drivers.md` §1 for the boot-time, module-identification design this implies.

## Document index

| File | Covers |
|---|---|
| `00-README.md` | This file — scope, target platforms, open questions |
| `01-architecture-overview.md` | Core graph/block/port data model, the generic data-driven block-execution engine, the concurrency model, the Platform Abstraction Layer, module map |
| `02-wire-protocol.md` | The complete WebSocket FBP-dialect protocol — the browser frontend's compatibility contract |
| `03-block-types.md` | The block type library: math/process/timing blocks, the generic execution engine, composite ("group") blocks, PID and schedule blocks |
| `04-drivers.md` | The hardware IO layer: boot-time module discovery, the generic descriptor-driven driver, binding logical IO to physical channels |
| `05-app-lifecycle-persistence.md` | Startup/configuration, graph persistence format, the static/web server, logging |
| `fbp-protocol.html` | The upstream FBP network protocol (Flowhub/NoFlo), of which `02-wire-protocol.md` is a modified dialect. Reference only — where the two disagree, `02` wins. |

**Read order recommendation**: this file → `01` → `02` → `03` → `04` → `05`.

## Terminology

- **Graph** and **process** are synonyms: one dataflow network of wired blocks. One engine runs many graphs at once; each graph can be started and stopped independently (`01-architecture-overview.md` §2.2).
- **Engine**: the always-running runtime that hosts every graph. The engine itself is never "stopped" while the system is up.
- **Initial** (FBP term): a literal value wired to a block input instead of a block output. See `01-architecture-overview.md` §2.2.
- **Group** / **composite block type**: the same thing — a user-selected subgraph, given a name and exposed ports, that becomes a reusable user-defined block type (`03-block-types.md` §5).
- **Change threshold**: the minimum change in a numeric output for it to be propagated downstream (`01-architecture-overview.md` §3.3).

## Protocol compatibility policy

The frontend already exists and already speaks a specific wire protocol. **Default policy: match it exactly** — `02-wire-protocol.md` specifies every message shape the frontend sends and expects, and the C implementation should honor that contract rather than "improve" it, since breaking it breaks a real client with no corresponding update planned.

**Exception**: in areas where the frontend has no real implementation today — most notably authentication (whose login handshake is redesigned, requiring a matching frontend change) and the group/composite-block commands — the wire shapes are free to be designed cleanly rather than contorted to match incomplete or unused client code. `02-wire-protocol.md` marks these areas explicitly. Protocol changes are always *allowed*, just *discouraged* by default; the bar for changing an area the frontend actively uses is much higher than for an area it doesn't yet.

## Open questions (need your input before/while implementation)

1. **Raspberry Pi vs. x86_64 build tree specifics**: the POSIX build tree ("Target platforms" above) needs its own concrete choices — networking (plain POSIX sockets), timer facility, filesystem layout — none of which are pinned down beyond "mirror the Zephyr build's shape using POSIX primitives" (`01-architecture-overview.md` §0/§3.2.1). Fine as a later implementation detail unless you want to settle it now.
2. **EEPROM module-descriptor format**: `04-drivers.md` §3 requires each pluggable module to carry a self-describing descriptor so new module types need no firmware update, but doesn't define its byte layout — that depends on what the separate hardware effort has settled on, or needs to be designed jointly.
3. **Logical-IO-to-physical-channel binding mechanism**: a graph must be able to run against differently-configured hardware, so the mapping from a graph's logical IO references to actually-discovered modules/channels is resolved per-deployment at boot (`04-drivers.md` §4) — whether that lives in a separate config file, in per-deployment graph metadata, or elsewhere is open. The whole IO hookup (`04-drivers.md`) is provisional and is expected to change once it is connected to Zephyr.
4. **Group / composite creation mechanism**: `03-block-types.md` §5 defines what a group (composite block type) is and how a selection of blocks becomes one, but the exact user action, wire messages, and whether the selected blocks are replaced by an instance of the new type are not settled — needs a decision alongside the corresponding frontend work. The same applies to graph-level inports/outports (`01-architecture-overview.md` §2.2), which exist to define a group's external ports.
5. **PID and schedule block parameters**: `03-block-types.md` §6 sketches plausible ports for `process/PID` and the day/week/month schedule blocks, but gains/anti-windup behavior, manual override, and the schedule-entry data model are reserved for you to specify exactly.
6. **Representing the hardware environment**: physical IO channels exist whether or not a user wants them; the user must not be able to delete them, but should be able to hide them and organize them into different graphs. How hardware channels appear in the graph model (pre-created non-deletable blocks? a dedicated "hardware" graph? something else?) is open. Closely related: what a physical output does when its graph is stopped, its block is unwired, before any graph has loaded, or after a crash (hold last value vs. go to a defined safe state).
7. **Change threshold semantics**: the threshold (`01-architecture-overview.md` §3.3) is currently specified as an absolute difference. Whether a relative (percentage) threshold, or a per-output rather than per-block override, is also needed is open.
8. **Numeric edge cases in type conversion**: converting NaN, ±Infinity, or out-of-range doubles to integers (`01-architecture-overview.md` §2.1). Rare; to be decided later in favor of the most runtime-efficient well-defined behavior.
9. **`Metric` arithmetic**: the rules in `01-architecture-overview.md` §2.1 are a statement of intent — unit tables, the per-block error flag's wire representation, and the rules for mixing metric with plain numbers are not specified, and the feature may be reworked or dropped once tried in practice.
10. **Work-queue capacity formula**: the engine sizes its work queue from the total number of wires (`01-architecture-overview.md` §3.2); the exact formula is not yet defined.
11. **Wall-clock time**: Zephyr will provide a wall clock, but the source (SNTP, RTC) is not yet worked out. Until then all timestamps are derived from engine uptime (`01-architecture-overview.md` §3.4). All time is UTC.
12. **Graph auto-start**: this specification assumes a graph's started/stopped state is persisted and restored at boot (`05-app-lifecycle-persistence.md` §2), so a rebooted controller resumes control without user action. Confirm this is wanted.
