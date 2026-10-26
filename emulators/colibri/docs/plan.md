# Colibri emulator — implementation plan

The order in which the Colibri emulator gets built (hakkas plan task 65). The specification,
`../specifications/00-README.md` (and, for the generic control protocol and UI,
`../../specifications/`), is the source of truth; this file only sequences the work.
Each task is one self-contained change with its tests, committed on `main`, and marked `[x]`
when its commit lands. As in hakkas' own plan (`../../../docs/plan.md`): one task at a time,
reported before the next starts; a task that hits an open question stops until it is decided,
and the decision goes into the spec in the same change.

Section numbers (§n) refer to the emulator spec; `04 §n` etc. to hakkas' `specifications/`.

## Decisions (with the user, 2026-10-25)

Task E0 writes these into the spec. The numbers are those of the questions first raised here.

1. **Module channels**: build the plugins from the catalogue guesses (§2.1), marked
   provisional, and adjust them when the firmware's values are known.
2. **DII/DIU**: the on-board STM32C071 measures each input and reports **frequency, duty cycle,
   count and state** every scan cycle (80 ms on Colibri 8). The state was missing from §2.1. In
   the emulator each terminal's field-side source is one pulse train (frequency and duty cycle,
   a constant or a function). The four channels are derived from it, and are sampled at the
   slot's turn: `count` counts pulses, rolling over at 2³², and the state is the input's level
   at the turn.
3. **DIO1 faults**: one fault channel that accumulates all faults, leaving room to report each
   of the others separately later.
4. **Colibri 8 carrier**: supply 7–30 V, AC or DC, 2 A recommended; one communication slot,
   taking one of Ethernet, WiFi, RS-485 (Modbus/RTU) and LoRaWAN. The type name is `colibri_8`
   (machine-readable), and its human-readable name is "Colibri 8".
5. **Pictures**: the signal panel first, then simple schematic SVGs drawn here; photos or vendor
   drawings may replace them later.
6. **Persisting**: every UI change goes to the emulator at once and takes effect at once
   (§8.1). The environment file is written only by an explicit **`persist`** command of the
   control protocol, which saves the structural changes. This replaces §5.1's "written back at
   once".
7. **Copying from a device**: the emulator's Node server connects to the device. Cross-origin
   access is to be avoided.
8. **One hakkas per emulator**: one-to-one in both directions; a second connection is refused.
   Later, "deploy on desktop" will launch hakkas and the emulator as a pair (see Later).
9. **UI technology**: Vue 3 and Vite, like hakkas' frontend; Rete.js may be needed there too.
10. **The control protocol** is specified in `emulators/specifications/`, beside the generic UI
    it serves, not in the Colibri emulator's own spec.

Follow-ups, decided the same day:
- **A. Type names**: machine-readable `colibri_<module>` (`colibri_aiv`, `colibri_pt1000`,
  `colibri_dii`, …; communication modules `colibri_ethernet`, `colibri_wifi`,
  `colibri_rs485`, `colibri_lorawan`). The human-readable name includes "Colibri":
  `colibri_pt1000` is shown as "Colibri Pt1000", `colibri_8` as "Colibri 8".
- **C. Slot 0's built-in module**: has no real name; its type is `carrier`, with no
  human-readable name.
- **D. `persist`** (the emulator only; a real device's environment cannot be changed): it saves
  the whole environment file, i.e. the structure, the settings and the inputs' sources and
  noise. The UI shows an "unsaved" mark. Library operations act on files at once. Selecting
  another environment with unsaved changes asks first.
- **E. DIO1's fault**: one `fault` bool per channel (4×), true when any fault of that channel
  or of the module is reported. Each kind of fault becomes a separate flag later.
- **F. DII/DIU channels**: `di` = state, `count` = pulse counter, `duty` = duty cycle,
  `freq` = frequency.

- **B. Human-readable names**: an I/O module's machine-readable type and its human-readable
  name are both in its EEPROM from the factory and never change. The environment description
  carries the name as a `name` member of the carrier, each module and each communication slot
  (04 §1). That is an addition to hakkas' `environment` group, which is not part of the upstream
  FBP protocol. In the emulator the name lives in the module's descriptor, the plugin's own
  `module.json` (and the carrier's `carrier.json`), so modules are added and changed by editing
  a file. Hakkas' side is hakkas plan task 68 (below, before E6). A description without a name
  (slot 0's `carrier`) is shown by its type.

**Small decisions** made unless the user objects. Each one goes into the spec in task E0:
- Server written in TypeScript, compiled with `tsc`, run on Node 24. Tests use vitest, and
  Playwright for the UI.
- Command line: `colibri-emulator [--library <dir>] [--plugins <dir>]… [--socket <path>]
  [--control-socket <path>] [--http <port>] [<environment>]`. Sockets default to
  `$XDG_RUNTIME_DIR/colibri-emulator/{environment,control}.sock`. HTTP and the WebSocket
  listen on localhost only, with no login.
- A non-addressable port (count 1, §7.1) has keys without an index (`slot4.ai`), matching the
  link (04 §0.1, no `index`).
- An output channel starts at 0/false (or its manifest's `default`) until hakkas writes it.
- A function source's time starts when the source is set. The noise's random generator can be
  seeded (`--seed`), so tests are reproducible.
- The emulator checks hakkas' `hello` version (1) and closes the link on any other version.
- No `clock` messages yet (§9, later).

## Phase 0 — Specification

- [x] **E0. Settle the spec**: record the decisions above in the emulator spec (and in 04 §1
  where they touch hakkas' description: the type names, follow-ups A and B); turn §4's, §7's
  and §8's proposals into decisions; write the control protocol in `emulators/specifications/`
  (decision 10: every message, key and `configure` operation, `persist`, error replies), for
  review before any code depends on it; and complete §2.1 with the final channel table per
  plugin (ids, types, counts, units, ranges, resolutions, defaults, noise, controls), with
  DII/DIU's state and DIO1's fault.
  *Done* (2026-10-25): the emulator spec rewritten for implementation (§2.1 channel table, §3 one
  hakkas and the link rules, §3.1 the command line, §4 serving a slot and deadline-based turns,
  §5/§5.1 persist and power cycles, §5.3 the server-side copy, §5.4 `colibri_8` and the
  communication module descriptors `comm.json`, §6, §7 descriptors with `name`, signals and
  measured channels, §8.3 the keys, rows and settings fields); the shared
  `emulators/specifications/00-README.md` and `01-control-protocol.md`; hakkas' 04 §1 (`name`,
  the type names, DII/DIU state, DIO1 fault). Decided on the way, unless the user objects: the
  pulse-train signal's measuring rules (§7.2), the `create`/`select`/`rename` rules and their
  refusals (protocol §4.4), state sent at most every 50 ms and stats once a second (§3.5–§3.6),
  the `restart` link state cleared by a connection that stays 2 s (§3.7), HTTP default port 8090.

## Phase 1 — The emulated system and the link to hakkas (no UI)

- [x] **E1. Project skeleton**: `emulators/colibri/` with `package.json`, `tsconfig.json`,
  `src/`, `tests/`, `npm run build`, `npm test`; the command line (task E0's options), logging;
  a `README.md` with how to run it against hakkas (`environment.socket` in `hakkas.json`).
  *Done*: `src/cli.ts` (`node:util` `parseArgs`, defaults from an injected `Host` so tests do not
  touch the real environment), `src/log.ts`, `src/names.ts` (the environment name rule),
  `src/package.ts` (the version, the built-in `data/` directory), `src/emulator.ts` (runs until
  the stop signal; a timer keeps the process alive until sockets exist), `src/main.ts`; tests for
  the command line, the log, names, and the built command as a process (help, version, usage
  error, `SIGINT`/`SIGTERM`). Decided on the way (spec §3.1): `--log-level`, `--http 0`, the
  temporary-directory fallback without `XDG_RUNTIME_DIR`, the exit codes 0/1/2.
- [x] **E2. Plugins and carrier definitions** (§5.4, §7.1): load and validate `module.json` and
  `carrier.json` (each with its type and human-readable `name`, follow-ups A and B) from the
  built-in directory (`data/`) and `--plugins` directories (`modules/`, `carriers/`, `comm/`,
  a later definition replacing an earlier one), with clear errors per file; the built-in
  plugins for every catalogue module (§2.1: AIV, AIC, AQV, Pt1000, DP1–DP3, DII, DIU, DIO1,
  FET, SSR, TRIAC1, Fallback, PID1, RS485I/U without channels, slot 0's `carrier` without
  channels), the four communication module types and the `colibri_8` carrier (decision 4).
  *Done*: `src/check.ts` (reading JSON with a path per problem, every problem reported in one
  pass), `src/sources.ts` (the source forms of control protocol §5, and which a row accepts),
  `src/descriptors.ts` (the three descriptor kinds), `src/catalog.ts` (loading, replacing, the
  carriers' cross-check), the built-in `data/` (18 module types, `colibri_8`, 4 communication
  module types); the emulator loads the catalog at startup. Tests for sources, descriptors
  (every refusal), the built-in catalog against the spec's §2.1 table, and the loader (a later
  directory replacing a type, unreadable and invalid files, missing pictures, a carrier naming
  unknown types). Decided on the way (spec §7.1): an input without a default rests at false or
  0 in its range; a slider needs a range; bar and meter indicators need one; unknown members are
  warnings; a missing picture is left out; a carrier with an unknown built-in module is skipped,
  an unknown communication type left out of its slot.
- [x] **E3. Environment files and the library** (§5, §5.1): load, validate and save
  `<name>.json` (atomic write, only on `persist`, decision 6); list, create, duplicate, rename
  and delete; remember the last one run; the `environments/` and `bindings/` directories.
  *Done*: `src/files.ts` (atomic write: a synced temporary file renamed, or linked for an
  exclusive create; the directory synced), `src/environment.ts` (the file's form, the check
  against the catalog in errors and warnings, writing it back with unknown members kept),
  `src/library.ts` (list, read, write, create, duplicate, rename, delete, the last one run, the
  startup choice); the emulator opens its environment at startup. Tests for the file form (the
  spec's example, every refusal, an embedded carrier, unknown members kept), the catalog check,
  the library's operations and refusals, the startup choice, and the process exiting 1 for an
  unknown environment. Decided on the way (spec §5, §5.1): unknown members are kept at the top
  level only; communication parameters are checked for their JSON types only; an unknown
  environment on the command line refuses startup; a library without a usable last one starts
  with the first by name; an environment that cannot be run leaves the emulator running without
  one.
- [x] **E4. The model** (§7.2, §7.3): an emulated system built from an environment and its
  plugins, with slots, channels, state keys (§8.3), sources (constant, `sine`, `square`, `ramp`,
  `counter`, `sequence`, and decision 2's pulse train with its derived channels), noise, clamping and rounding. The clock
  and the random generator are injected, so it is tested without real time.
  *Done*: `src/system.ts` (`System.build`: the slots, every channel and signal with its key, port
  and label, the communication modules its carrier takes; `sample(slot)` computes a slot's
  inputs and returns the changed ones; `set` for sources and noise with the protocol's
  refusals; `applyOutput`; `state()`; `toEnvironment()` writing only what differs from the
  defaults), `src/evaluate.ts` (the functions, fitting to range and resolution, rolling over),
  `src/random.ts` (a seeded generator for `--seed`); the emulator builds the system at startup.
  Tests with a fake clock and random generator: every function, rounding without binary
  fractions, the pulse train's four measurements, its count at a high rate and across a change
  of source, an integrated frequency ramp, the 32-bit roll-over, the refusals, the environment's
  inputs applied and persisted. Decided on the way (protocol §4.1, §5; spec §7.2, §8.3): an
  invalid source's refusal carries the problem; a counter starts at the row's minimum and has
  no noise; a pulse count takes no noise; a fitted value stays within range after rounding; a
  pulse train's phase continues across a new source.
- [x] **E5. The scan cycle and the write queues** (§4, §4.1): serves one slot per cycle time,
  deadline-based so it does not drift; one queue per module (size from the environment),
  applied in order at the module's turn, a full queue drops the new value; inputs sampled only
  at their turn; the counters of §4.1. Cycle time and queue size can change while running.
  *Done*: `src/scan.ts` (`Scanner`: `poll()` serves the turns due, `write(ch, value)` queues or
  drops, `setCycleMs`/`setQueueSize`, `stats()`/`resetStats()`, `start()`/`stop()` on a timer;
  each turn is reported with the outputs applied and the inputs changed, for E7 and E9); the
  emulator runs it at startup. Tests on a fake clock: the slot order with empty slots, a late
  turn not moving the next deadline, skipping and its rate-limited warning, a new cycle time,
  sampling only at a slot's turn, queue order, a full queue, a resized queue, every counter and
  the reset; one test on real timers. Decided on the way (spec §4, §4.1): a smaller queue keeps
  what it holds; `sent` counts each change a turn reports; a reset keeps a queue's current
  length (and makes it the highest); only filled slots have part counters; the first turn is
  served at once when the cycle starts.
- [x] **Hakkas task 68 (in hakkas, `../../../docs/plan.md`). Names in the environment
  description** (follow-up B): an optional `name` for the carrier, each module and each
  communication slot, in `src/hakkas_env.h`, the JSON form and its validation (a string of at
  most 1024 bytes, like the other metadata), the identical-description check, and
  `environment:describe`. The frontend's Environment panel shows the name, else the type.
- [x] **E6. The environment description** (§6, 04 §1): from the model, ports `slot<n>-<group>`,
  modules (built-in marked), the carrier with power and communication slots; tested against
  04 §0's validation limits (port ids, counts, 256 ports, 64 modules, slots).
  *Done*: `src/description.ts` (`describe(system)`: ports per slot and group, sensors and
  actuators apart, in slot and channel order, a group of one a plain port; the filled slots'
  modules with names and serials, the built-in one marked; the carrier with power and its
  communication slots' types, names and parameters; `checkDescription`: hakkas' checks of
  04 §0). The emulator checks the description before running an environment; one hakkas would
  refuse is logged and not run. Tests: every member, every rule of the check, every module type
  of the catalog passing it; also checked once by hand with hakkas' own validator
  (`env_description_from_json` + `env_description_validate`) on the same descriptions, all
  accepted. Decided on the way (spec §6): absent members left out, an unknown module type's
  slot not listed, no carrier serial yet.
- [x] **E7. The environment link server** (§3, 04 §0.1): a Unix socket with newline-delimited
  JSON. On every connection: `describe`, then all current sensor values, then the changes;
  `hello` check; `write` into the module's queue; malformed lines warned about and ignored; the
  256 KiB line limit; one connection at a time (decision 8); a power cycle (§5.1) closes the
  link.
  *Done*: `src/link.ts` (`LinkServer`: `listen()` with the stale-socket check, `serve()` an
  environment or none (another one is a power cycle), `turn()` sends a turn's changes,
  `close()` removes the socket file; the link state and notices as events for E9); the emulator
  listens at startup and serves the environment it runs. Tests over a real socket: the
  description and current values, changes at a turn, writes reaching the queue, every warning
  and the limit of 8, unknown messages ignored, the version check, the 256 KiB limit at and over
  it, a second connection refused with a notice, no environment, a power cycle and `restart`
  cleared after 2 s; the command's run removes its socket. Decided on the way (spec §3): see
  its E7 bullet (current values, `index`/`null`, no environment, the socket directory, when a
  power cycle means `restart`).
- [x] **E8. Integration with hakkas**: a test that runs `build/debug/hakkas run` against the
  emulator. A process binds an AIV input and an SSR output; the test checks the description
  hakkas accepts, a value in both directions with the scan latency, the reconnect (sensor
  values re-sent, actuators re-written), and a power cycle (hakkas' "changed its description").
  Skipped when the hakkas build is absent.

## Phase 2 — The control channel
  *Done*: `tests/hakkas.test.ts`, part of `npm test`, skipped without the hakkas build
  (`HAKKAS_BIN`, else `build/debug/hakkas`). hakkas runs with its own configuration, the process
  `threshold` (`compare/Greater`, `in1` bound to `slot1-ai[1]`, `out` to `slot3-dq[1]`, `in2`
  5) and its bindings file; the emulator's link, model and scan cycle run in the test, on real
  time. Checked: the description accepted and both bindings applied with no warning; the
  process's first write at its start; a sensor change sampled at slot 1's turn and the result
  applied at a later slot 3 turn; switching back; the emulator's link closed and opened again
  (hakkas logs `disconnected` and `reconnected`, re-writes its actuator, and later changes get
  through); a power cycle (an SSR moved to slot 4): hakkas logs that the description changed and
  the link stays `restart`; hakkas exits 0 each time. Also run once by hand with the built
  command (`node dist/main.js … lab`) and hakkas: described and accepted.
- [x] **E9. The control protocol** (§8.1, §8.3, its spec in `emulators/specifications/`): a transport-neutral
  session with `scene`, `state`, `settings` and `stats` to the client and `set`/`configure`
  from it. The emulator applies each request and broadcasts the result to every client, the
  sender included, and answers a refused request with an error.
- [x] **E10. Transports**: the Unix socket with newline-delimited JSON, and an HTTP server
  (localhost only) that serves the UI's built files, the plugins' and carriers'
  assets, and the WebSocket. `examples/` gets a short Python script that sets an input and
  follows an output.
- [x] **E11. Configuration over the channel** (§5.1): library operations, selecting an
  environment, carrier and modules per slot, communication modules and their parameters,
  cycle time and queue size; power cycles where §5.1 says so; `persist` and the unsaved
  state (decision 6, follow-up D).
  *Done* (E9–E11 in one go, at the user's request, 2026-10-25): `src/core.ts` (`Core`: the
  running environment with its system, scanner and link; the clients; requests one at a time in
  arrival order; state at most every 50 ms and a request's changes before its `ok`; stats once
  a second, only what changed; power cycles keeping the inputs' sources where their keys still
  exist), `src/settings.ts` (the form and `configure`), `src/scene.ts`, `src/transports.ts`
  (`ControlSocket`; `HttpServer` on 127.0.0.1 and ::1 with the WebSocket at `/control` through
  the `ws` package, the Origin check, the assets and the UI's files), `examples/follow.py`; the
  emulator runs them all (`src/emulator.ts`), and `System.setMany` sets keys all or none. Tests:
  the core's every request and refusal with two clients, configuration (slot type and serial in
  one request, the form's fields coming and going, communication modules and their parameters,
  refusals all or none), persist and its failure, the library (create, copy, rename, the running
  one renamed, delete, select with and without unsaved changes, one that cannot be run, none
  running), resetstats; the transports (lines, a too-long line or WebSocket message — 1009 —,
  origins, 404/405, nothing served outside the UI and the plugins' pictures, a live control
  socket left alone); with hakkas, the whole emulator driven over its control socket (a value
  and a square wave through a process to a relay); the command's run logs its three listeners
  and removes both sockets. Two fixes found by repeated runs: an ended link connection is gone at
  once (hakkas' quick reconnection after a power cycle was refused as a second runtime), and
  an emulator that cannot start removes no socket file that is not its own. Decided on the way
  (control protocol §1–§4, spec §3.1, §8.3): see the *decided* notes of 2026-10-25 there —
  `Invalid field`, `No environment`, `Cannot be run`, `Persist failed`, `null` settings values,
  the order of `configure`, no change for an equal value, empty parts' `null` type, the HTTP
  server's details, `features` empty until E16, the settings in detail.

## Phase 3 — The UI (`emulators/ui/`, generic; Vue 3 + Vite, decision 9)

- [x] **E12. UI skeleton and the signal panel** (§8.2): a Vue 3 + Vite package that connects
  to the WebSocket and mirrors the state. The signal panel has one row per channel: a control
  or function editor and noise for each input, an indicator for each output, name, unit and
  value. Unit tests, and a Playwright suite against a real emulator.
  *Done*: `emulators/ui/` (`src/protocol/client.ts`: the WebSocket client, its reactive mirror,
  requests by id, reconnecting; `src/sources.ts`; `App.vue`, `SignalPanel.vue`, `RowView.vue`,
  `SourceEditor.vue`/`SourceFields.vue`, `NumberField.vue`), the same tool versions as hakkas'
  frontend. Unit tests (the client against a fake socket: the mirror, answers by id, notices,
  reconnecting, another protocol version; the source helpers). Browser tests, one real
  emulator each, a control client and a stand-in hakkas: the panel built from the scene, a
  typed value and a slider, typing kept while the values stream in, another client's changes,
  the source editor (a function, Cancel, back to a constant, a sequence's steps, a pulse train
  driving the measured channels), an output following hakkas' write, a toggle, noise, a refusal
  as a notice, a lost connection. Found on the way: Vue sets an input's value at every render,
  which undid typing in a noisy row (hence `NumberField`). Decided (shared spec
  `../../specifications/00-README.md`, "The UI, as built").
- [x] **E13. Library, settings and statistics**: the environment list (select, create,
  duplicate, rename, delete), the slot editor (module per slot, built-in slots fixed), the
  carrier, communication modules, cycle time and queue size, the power-cycle notice ("restart
  hakkas"), Persist and the unsaved mark, the §4.1 counters with dropped writes standing out.
  *Done*: `emulators/ui/` — `LibraryPanel.vue` (with `ModalDialog.vue`), `SettingsPanel.vue`
  (`TextField.vue` beside `NumberField.vue`), `StatsPanel.vue`, `src/settings.ts` (grouping,
  lists, option keys, the counters in the scene's order), `App.vue` (the column and the tabs,
  Persist, the restart banner). Unit tests for the helpers; browser tests: create (refusals in the
  dialog), duplicate, rename, delete; a setting changed and persisted by another client, a
  refused value; *Run* with unsaved changes, discarding and persisting first; a slot's module and
  serial with hakkas connected (the restart banner and badge, the new part on the signal panel),
  the built-in slot fixed; a communication module's parameters persisted; dropped writes standing
  out and reset. Decided (shared spec, "The UI, as built"): the layout, the library's actions, the
  settings' controls and when they send, the counters' display.
- [ ] **E14. Carrier and faceplates** (§7.4, §8.4): the carrier's picture with its
  `slot-<n>` places, each module's faceplate in its slot, bindings (attributes, text,
  visibility) and controls (slider, toggle, button, number) on the SVG elements.
  **To be re-planned** (decided with the user 2026-10-25/26): the device's picture with its
  channels' state moves to hakkas' UI (hakkas plan, Phase 11), the same for the emulator and a
  real device. Done towards it: the MCU module, named from `--mcu` (hakkas plan task 73). What
  the emulator keeps — its pictures as data for the description, and whether its own UI shows
  the picture with controls — is decided with hakkas' tasks 75–78.
- [ ] **E15. Artwork** (decision 5): the Colibri 8 board and the modules' faceplates as SVGs.
  Changed 2026-10-26: the vendor's renders (PNG, in the descriptors' directories) are the
  pictures, converted to WebP; I/O modules turned 90° clockwise to match the carrier.
  Hakkas task 75 (2026-10-26) put `pictureUrl`, `positions` and `places` in the descriptors,
  measured from the renders with the I/O modules turned clockwise and cropped to the board
  (board at 427–1475 × 257–806 of the 1904×1064 render; communication modules 675–1228 ×
  3–1060; the carrier uncropped): the hosted pictures must be prepared exactly so, or the
  numbers measured again. The links follow the user's pattern
  `https://currentmakers.com/data/colibri/render-top_<type>.png` and are provisional.

## Phase 4 — Copying an environment from a device

- [ ] **E16. Copy from a device** (§5.3, decision 7): a device client (hakkas' `auth` and
  `environment:describe`), converting the description into an environment file under a name
  the user gives, with warnings for module and carrier types that have no plugin or
  definition; the UI dialog. Tested against hakkas on the desktop running against a second
  emulator, which should come out as an identical twin.

## Later (§9)

- Simulated time through the link's `clock` message; running faster than real time needs a
  change to hakkas.
- Fault injection, once the firmware's fault reporting is defined (decisions 1 and 3).
- "Deploy on desktop": launching hakkas and the emulator as a pair (decision 8).
- MCU and Comm modules; RS-485 channels.
