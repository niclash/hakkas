# Colibri Emulator — Specification

> **Draft** (started 2026-10-23). Nothing is implemented yet. Decisions are marked *decided* with their date; *proposal* marks a suggestion still waiting for one. Open questions are in §10.

## 1. What this is

The Colibri emulator lets a user test a hakkas program (processes and their bindings files) on a desktop before it is loaded into a Colibri device. It emulates a Colibri system's **I/O side**: a carrier board with its I/O module slots and the modules in them. The user sees each module's faceplate in a browser, sets its inputs (a voltage on an AIV terminal, a contact on a DII), and watches its outputs (an SSR's LED, an AQV's voltage) follow what the hakkas processes do.

It is a **separate project** in `emulators/colibri/`, with its own build and tests; hakkas does not depend on it, and it depends on hakkas only through the environment link protocol (`specifications/04-drivers.md` §0.1).

**In scope**: the carrier board's I/O slots; the I/O modules, as plugins (§7); the slot scan cycle and its timing (§4); environments made from scratch, copied from a device, and compared (§5.1); a control channel (§8), on which a browser UI and external programs set inputs and follow outputs; the UI is generic, so other emulators can use it too.

**Not part of the emulator**: application testing — plant models, scenarios — which external programs do through the control channel (§8.1).

**Out of scope for now**: emulating the MCU module and the Comm module (Ethernet, RS-485, WiFi, LoRaWAN); hakkas itself runs as its POSIX build on the desktop instead of the MCU. The carrier board's power supply and its I2C/SPI infrastructure are not emulated (§2).

**Implementation** (*decided*, 2026-10-23): TypeScript on Node.js; module plugins are data files, not code (§7).

## 2. The emulated system

A Colibri system is, conceptually:

- one **carrier board**: power supply, the I2C and SPI infrastructure (I2C multiplexed per slot, one SPI chip select per slot, plus two more for a module with several SPI devices; each slot powered and addressed individually), **one I/O slot built into the board**, and the slots below;
- **1…n I/O module slots** (M.2 connectors) — Colibri 8 has 7, Colibri-16pi 15;
- one **Comm module slot** (not emulated);
- one **MCU module slot** (not emulated: hakkas runs on the desktop).

The built-in I/O slot exists so that the firmware's (Zephyr's) architecture treats the carrier board's own I/O like any module's (*decided*, 2026-10-23). Colibri 8 thus has **8 I/O slots** in the scan cycle (§4). The built-in slot is **slot 0**; nothing in it is emulated for now (*decided*, 2026-10-23): it takes its turn in the scan cycle with no channels, like an empty slot.

On the device, the firmware identifies each I/O module at boot from its EEPROM and runs the module's driver cyclically (§4); it then offers the modules' channels to hakkas as the environment (`04-drivers.md` §1). The emulator takes the firmware's place towards hakkas: it is the **environment program** of `04-drivers.md` §0.1.

**Fidelity** (*decided*, 2026-10-23): the emulator models each module at the level of its **channels** — the values the firmware's driver delivers to and takes from hakkas — plus the module's **field side**, the physical terminals the user manipulates in the UI. There is no emulation of I2C/SPI registers or of the driver code in a module's EEPROM. What the emulator does reproduce is the **timing** of the firmware's slot scan, including what happens when values arrive faster than a slot is served (§4).

### 2.1 The module catalogue

Collected 2026-10-23 from the CurrentMakers catalogue (https://currentmakers.com/products/catalog/?category=colibri), its product pages and the STM32World wiki pages they link. The pages give hardware, not the channels a driver offers. **Channel names** (*decided*, 2026-10-23): `ai` analog input, `aq` analog output, `di` digital input, `dq` digital output — whatever the hardware (a relay, a FET, a triac is a `dq`); channels that are none of these keep their own names (`freq`, `duty`, `count`, `fault`, PID1's parameters). Temperatures and pressures are `ai` too, the unit telling what they measure.

| Type | Hardware (from the pages) | Channels offered to hakkas (*proposal*) |
|---|---|---|
| AIV | 2 × 0–10 V in, ADS1115 16-bit ADC, I2C | `ai` 2 × number, V, 0–10 |
| AIC | 2 × 0–20 mA in, ADS1115 16-bit ADC, I2C | `ai` 2 × number, mA, 0–20 |
| AQV | 2 × 0–10 V out, MCP4725 12-bit DAC, I2C | `aq` 2 × number, V, 0–10 (actuator) |
| Pt1000 | 2 × Pt1000 (also Ni1000 DIN, Ni1000 Tk5000), MAX31865 per input (SPI), open/short detection | `ai` 2 × number, °C; `fault` 2 × bool (sensor) |
| DP1 | 1 × 0–500 Pa differential pressure (SM7391, I2C) | `ai` 1 × number, Pa, 0–500 |
| DP2 / DP3 | 0–10 kPa / 0–100 kPa (wiki only: planned) | as DP1, in Pa |
| DII | 2 isolated digital in, on-board STM32C071; per channel frequency, duty cycle and a 32-bit unsigned counter that rolls over (*decided*, 2026-10-23) | `freq` 2 × number, Hz; `duty` 2 × number, %, 0–100; `count` 2 × number, 0–4294967295 |
| DIU | 4 digital in, not isolated, on-board STM32C071 (*decided*, 2026-10-23; the product page's 2 is wrong); delivers as DII | `freq`, `duty`, `count` 4 × each, as DII |
| DIO1 | IEC 61131-2, MAX14906 (4 channels per its datasheet), each channel input or output, current limit set in software (100 mA advised) | `dir` 4 × bool (actuator: true makes the channel an output, set by the user program — *decided*, 2026-10-23); `di` 4 × bool (sensor); `dq` 4 × bool (actuator); fault flags: whatever the MAX14906 reports, per channel and per module (not defined yet). In the emulator `di` and `dq` are not connected (*decided*, 2026-10-23): `dir` and `dq` are shown, `di` is set, independently |
| FET | 4 × N-channel MOSFET sink, 1 A, 60 V, PCA9536, I2C | `dq` 4 × bool (actuator) |
| SSR | 2 × solid-state relay, 80 mA, 60 V DC, PCA9536 | `dq` 2 × bool (actuator) |
| TRIAC1 | 4 × triac, 500 mA AC, zero-cross detection, PCA9536 | `dq` 4 × bool (actuator) |
| Fallback | 1 × SPDT relay (wiki only) | `dq` 1 × bool (actuator) |
| PID1 | 1 × Pt1000 in (MAX31865), 1 × 0–10 V out (MCP4725 12-bit), STM32F030 running a full PID regulator; every PID parameter is reachable through an output channel; the emulator does not regulate — the output is a settable input like any other (*decided*, 2026-10-23) | inputs `ai` °C (the Pt1000), `output` V 0–10, `saturated` bool; outputs — the parameters, named as hakkas' `process/PID` (`specifications/03-block-types.md` §6.1): `setpoint`, `kp`, `ki`, `kd`, `max_dgain`, `min`, `max`, `reverse`, `auto`, `manual`, `sample_ms` (*decided*, 2026-10-23) |
| RS485I / RS485U | RS-485 — not defined yet; outside the scope of the emulator and of hakkas (*decided*, 2026-10-23) | none: the emulator places the module's picture in its slot, with no controls or indicators; not described to hakkas |

The catalogue also lists MCU modules (MCU1 STM32WLE5, MCU2 STM32H562RG, MCU3 STM32F405RG, MCU5 STM32F103RE, MCU6 STM32H735RG, MCU7 RP2350) and the carriers Colibri 3a and Colibri 8; neither kind is emulated.

## 3. Place in the system

```
 browser UI (§8.2)          external programs: scripts, demos, automation
    │  WebSocket             │  Unix socket — both the control protocol (§8.3)
 colibri-emulator ── module plugins (§7), the scan cycle (§4), environments (§5)
    │  Unix socket, newline-delimited JSON (04-drivers.md §0.1)
 hakkas (POSIX build) ── processes + bindings files
    │  WebSocket, the FBP wire protocol (02-wire-protocol.md)
 browser ── hakkas frontend
```

- **Towards hakkas** the emulator is the server of the environment link: it listens on a Unix socket (hakkas' `environment.socket`), sends its `describe` (§6) first on every connection, then all current sensor values, then changes; it takes hakkas' `write` messages for the actuators; it may send `clock` (§9). It follows every rule of `04-drivers.md` §0.1, including re-sending all current sensor values after a reconnect.
- **Towards the user and external programs** it serves the generic UI and offers the control channel (§8).
- The two programs are started separately; the emulator does not start or stop hakkas.

## 4. The slot scan cycle

The firmware serves its I/O slots **one at a time, in turn**, and so does the emulator: every **cycle time** (default **10 ms**) it serves the next slot — slot 0 (the built-in slot), then slot 1, 2, …, n, then slot 0 again. A module's channels are therefore refreshed once every (n + 1) × cycle time: **80 ms** on Colibri 8 with the default.

**Every slot takes its turn** (*decided*, 2026-10-23), filled or not: on the device a dummy driver is loaded for an empty slot, so the runtime's critical path does not depend on which slots are filled. The emulator gives an empty slot a dummy module with no channels.

Serving a slot (*proposal*):
1. hand the module the actuator values queued for its channels since the slot was last served (§4.1);
2. compute every input channel's value from its source and noise (§7.2);
3. send every sensor value that changed to hakkas, and every changed state to the UI.

So a change on the field side reaches hakkas, and a write from hakkas reaches the field side, only when its slot is served — the latency a program sees on the device.

### 4.1 Values arriving faster than the cycle

The point of emulating the cycle is the timing (*decided*, 2026-10-23): a program that writes an actuator more often than its slot is served, or that depends on seeing every edge of a fast input, must show the same behaviour on the desktop as on the device. That requires the emulator to treat the values between two turns of a slot **as the firmware does**: the firmware **queues** the values hakkas writes (*decided*, 2026-10-23), and so does the emulator — every write reaches the module, in order, none is coalesced. (*Decided*, 2026-10-23:)
- **One queue per I/O module**, for the writes to all its output channels.
- **A turn applies every queued value**, in order, and leaves the queue empty.
- **The queue holds 10 values** (its size on the device is not determined yet; it is a setting, §5).
- **A full queue drops the new value.** So when a program writes a module's outputs more than 10 times within one scan cycle, the last writes are lost and the outputs keep older values — until hakkas writes them again, which it does only when the value changes. **On the input side**, a driver forwards a value to its channel only in its slot's turn (*decided*, 2026-10-23); nothing between turns is reported. An input's source is therefore sampled at its slot's turn, so a `square` faster than the cycle shows only the samples a turn hits; a `counter` (§7.2) counts at its rate regardless.

The emulator counts, per channel, the writes received, the writes queued, the queue's current and highest length, the writes **dropped** on overflow and the sensor changes sent, and shows them in the UI (§8), so the user can see where a program outruns the cycle.

## 5. Environments

An **environment** is one emulated Colibri system:
- **Carrier board**: which carrier (Colibri 8, Colibri-16pi, …) — the number of I/O slots and what the built-in slot provides follow from it.
- **Number of I/O slots**: 1…n, for a carrier made from scratch.
- **I/O module cycle time**: default 10 ms.
- **Write queue size** per module (§4.1): default 10.
- **Modules**: which module type (plugin) sits in which slot; a slot may be empty.
- **The input channels' sources and noise** (§7.2), so a setup comes back as it was left.

Each environment is a file, `<name>.json`, its file name being the environment's name (§6). Example:

```json
{"carrier": "colibri/Colibri8", "slots": 7, "cycleMs": 10, "queueSize": 10,
 "modules": [{"slot": 1, "type": "colibri/AIV", "serial": "EMU-0001"},
             {"slot": 3, "type": "colibri/SSR"}],
 "inputs": {"slot1.ai.1": {"source": 5.0, "noise": 0.02}}}
```

### 5.1 The environment library

(*Decided*, 2026-10-23.) The user makes **many environments**; they are listed and selectable in the UI, and kept in a **central library** so they are reused between projects:
- The library is a directory (*proposal*: `$XDG_DATA_HOME/colibri-emulator/`, i.e. `~/.local/share/colibri-emulator/`, overridden with `--library <dir>`), holding `environments/` (the environment files) and `bindings/` (bindings files the user keeps beside them, §5.2; the emulator itself does not read them).
- The UI lists the library's environments and lets the user **create** one from scratch, **duplicate**, **rename** and **delete** one, and **select** the one the emulator runs. One environment runs at a time; the emulator starts with the one that ran last, or the one named on the command line.
- Every change made in the UI is written back to the environment's file at once.

Changing the cycle time or the queue size takes effect at once. **Selecting another environment, or changing the carrier, the slot count or the modules, is a power cycle** of the emulated device (*decided*, 2026-10-23): it changes the environment description, which hakkas accepts only at its startup (`04-drivers.md` §0.1), so the emulator closes the link and the UI tells the user to restart hakkas.

### 5.2 Bindings are made in hakkas' UI

(*Decided*, 2026-10-23.) Bindings belong to a device, not to the emulator: hakkas' frontend gets the environment's ports from the device it is connected to — a Colibri device, or hakkas on the desktop running against the emulator — and offers a page for binding the processes' ports to them. On that page the user **selects a bindings file from the local file system**, typically in the library's `bindings/`; the user names the files and so controls which bindings go with which environment and device. That is hakkas' side (`specifications/04-drivers.md` §4, `06-frontend.md`), and needs additions to its wire protocol, each put to the user first.

### 5.3 Copying from a device, comparing

Required (*decided*, 2026-10-23), details to be discussed later:
- **Copied from a device**: the user takes the environment (carrier, modules per slot, their metadata) from a connected Colibri device, so the desktop tests run against exactly what the device has.
- **Compared**: two environments (e.g. the emulator's and a device's, or two environment files) are compared, showing the slots and channels that differ — e.g. before loading a tested program into a device.

## 6. The environment description

The emulator describes itself to hakkas per `04-drivers.md` §1:

- `name`: the environment's name (§5) (question 7).
- **Ports are named per slot**: `slot<n>-<channel group>`, e.g. `slot1-ai` (AIV in slot 1), `slot3-dq` (SSR in slot 3). A group of identical channels is one addressable port with `count`; a module with several groups has several ports (DII in slot 2: `slot2-freq`, `slot2-duty` and `slot2-count`). The channel group ids and their types, units and ranges come from the module's plugin (§7.1).
- **Modules**: one entry per filled slot, with `slot`, `type`, `vendor`, `revision`, `serial`, `documentation` from the plugin and the configuration, and the ids of its ports.

The emulator gives the same ports, port ids and module list that the firmware gives for the same modules in the same slots, so that one bindings file works on both. This requires the firmware's naming to be fixed in `04-drivers.md` §1 (it is not yet), and both to follow it.

## 7. Module plugins

Every I/O module type is a **plugin**, also the built-in ones, so that others can make modules for Colibri and supply their emulation with them. **A plugin is data, not code** (*decided*, 2026-10-23): the emulation of a module is nearly nothing — channels with their ranges, a value source per input, noise, indicators and controls — so it is described in a file rather than programmed:

```
colibri-pt1000/
  module.json      the manifest (§7.1)
  faceplate.svg    optional: the module's picture (§7.4)
```

Plugins are found in the emulator's built-in plugin directory and in directories named on the command line (*proposal*). A plugin's type name is `<vendor>/<Name>` (e.g. `colibri/Pt1000`), as hakkas' block types are `family/Name`. When slot 0 gets channels to emulate, they come from a plugin too.

### 7.1 The manifest

```json
{"type": "colibri/Pt1000", "vendor": "CurrentMakers", "revision": "1",
 "description": "2 × Pt1000 temperature inputs",
 "documentation": "https://currentmakers.com/products/colibri/colibri-pt1000/",
 "channels": [
   {"id": "ai", "direction": "input", "type": "number", "count": 2,
    "unit": "°C", "min": -50, "max": 150, "resolution": 0.01,
    "default": 21, "noise": 0.2, "control": "slider", "description": "Pt1000 temperature"},
   {"id": "fault", "direction": "input", "type": "bool", "count": 2,
    "control": "toggle", "description": "Sensor open or shorted"}
 ],
 "faceplate": "faceplate.svg",
 "ui": { … the faceplate bindings of §8.4 … }}
```

A channel:
- `id`, `type` (`number` or `bool`), `description`, `unit`, `min`/`max` — become the environment description's port (§6); `count` > 1 makes the port addressable.
- A module with **no channels** (RS485I, RS485U) is only placed with its picture: no controls, no indicators, nothing described to hakkas.
- `direction`: `input` (a module input: a sensor to hakkas, an environment output port) or `output` (a module output: an actuator, an environment input port). A module may have both (DIO1, PID1).
- **Inputs**: `resolution` (values are rounded to it), `default` (the value at power-up), `noise` (§7.2) and `control` — the UI's default control: `slider`, `number`, `toggle` or `button` (momentary).
- **Outputs**: `indicator` — how the UI shows the value hakkas writes: `lamp` (bool), `number`, `bar` or `meter` (number, between `min` and `max`). Without one: `lamp` for a bool, `number` for a number.

### 7.2 Inputs: value source and noise

At each turn of its slot (§4), every input channel produces a value, which goes to hakkas if it changed:

> value = round(clamp(source(t) + noise, min, max), resolution)

- **The source** is what the user or an external program set (§8): a **constant** (a slider, a number, a toggle, a button) or a **function** of the emulator's time:
  - `sine` — offset, amplitude, period;
  - `square` — low, high, period, duty (a bool channel toggles);
  - `ramp` — from, to, period, repeating (a sawtooth) or not;
  - `counter` — an increasing integer at a rate per second that **rolls over** past the channel's `max` to its `min` (a DII's count: 0…4294967295, its 32-bit unsigned counter). The rate is a number or another input channel of the module (*proposal*), so a DII's count follows its frequency: `{"counter": {"rate": "freq.1"}}`;
  - `sequence` — a list of `[seconds, value]` steps, repeating or holding its last value.
- **Noise** (numbers only): a uniformly distributed random value in ±`noise` (e.g. ±0.2 °C on a Pt1000), added anew at every turn. Its default comes from the manifest; it is set per channel at runtime, 0 turns it off.

So a noisy Pt1000 channel sends a new value every 80 ms on Colibri 8. Hakkas' change threshold (`01-architecture-overview.md` §3.3, engine-wide) filters it like real sensor noise; a `process/Deadband` filters a channel differently.

The source and noise of every channel are part of the emulator's state (§8) and are kept in the environment's file (§5).

### 7.3 Outputs

An output channel shows the value that arrives on it from hakkas, at its slot's turn (§4.1), on its indicator. Nothing else happens to it.

### 7.4 Graphics assets

A plugin's faceplate is an optional **SVG** whose elements carry ids; the manifest's `ui` section binds those elements to channel values (an LED's colour to a relay channel, a label's text to a voltage) and may make elements operable (a slider on an analog input's terminal). The emulator does not draw: it hands the assets and bindings to the generic UI (§8). Every channel is usable without a faceplate, from the signal panel (§8.2).

## 8. The UI and the control channel

### 8.1 Principle

The emulator's state — the input channels' sources and noise, the channel values, the output values, the settings, the counters of §4.1 — is available on a **control channel**, with one protocol (§8.3) for every client: the browser UI and **external programs** (a Python script, a demo, an automation). All clients are equal: each may set inputs and follows the whole state.

It follows hakkas' principle (`../../../specifications/00-README.md`): **the emulator owns the state, clients only mirror it**. A client sends a request; the emulator applies it and broadcasts the resulting state to every connected client, the sender included, which updates its view only from that. When two clients set the same input, the last one wins and every client sees it.

The control channel is **separate from hakkas' environment link** (*decided*, 2026-10-23): hakkas exchanges channel values on the MCU's side of the modules, while clients work on the **field side** — a voltage on a terminal, a contact closed — and what they set reaches hakkas only through the module's behaviour and the scan cycle (§4), as on the device. (The link protocol could not tell the two kinds of client apart anyway: the emulator sends `describe` first on every connection, `04-drivers.md` §0.1.)

**Application testing is not part of the emulator** (*decided*, 2026-10-23): plant models, scenarios and the like — testing a Colibri application against a simulated real world, e.g. a two-stage furnace regulator whose setpoint comes from a sensor's pulse train — are written outside it, in Python or whatever is at hand, as clients of the control channel. The emulator's job is to make that easy: a simple, documented protocol, on a transport a script reaches without a library.

### 8.2 The UI layout (*proposal*)

The UI knows nothing about Colibri; it renders what the emulator describes:
- **The carrier board**: a photo or drawing of the board (an asset of the carrier's description, §5), with the place of each slot marked (an element with the id `slot-<n>`).
- **The modules**: each module's faceplate (§7.2) drawn in its slot's place on the carrier, with its LEDs and terminal values bound to the state and, where the plugin has them, controls on it.
- **The signal panel**, beside the drawing: one row per input channel (its control or function, §7.2, and its noise) and per output channel (its indicator, §7.1), with the channel's name (`slot1-ai[1]`), unit and value. The UI builds it from the channel list alone, so every module is usable, also one without a faceplate.
- **The environment library** (§5.1): the environments, which one runs, its settings; and the counters (§4.1).

The UI is its own package (*proposal*: `emulators/ui/`, with the Colibri emulator serving its built files and the assets over HTTP), so that other emulators reuse it.

### 8.3 The control protocol (*proposal*)

JSON messages, the same on every transport:
- emulator → client: `scene` (the carrier and module assets, the slots' places, bindings and controls, the channel list; sent on connect and after every configuration change), `state` (changed values, by key; all of them on connect), `settings` (the runtime settings and their allowed values: carrier, slot count, cycle time, the module types available per slot), `stats` (the counters of §4.1).
- client → emulator: `set` (an input's source — a constant or a function — or its noise, by key), `configure` (a setting change, e.g. a module put into slot 4).

**Transports**: a **WebSocket** (the browser UI; also usable from scripts) and a **Unix socket** with newline-delimited JSON, like hakkas' environment link (`04-drivers.md` §0.1) — reachable from a shell (`socat`) or a few lines of Python. Only the framing differs.

Keys name a slot, a channel and, for an input, its parts: `slot1.ai.1` (the channel's value: for an input what hakkas gets, for an output what hakkas wrote), `slot1.ai.1.source` (e.g. `5.0`, or `{"sine": {"offset": 5, "amplitude": 2, "period": 60}}`), `slot1.ai.1.noise` (e.g. `0.02`).

### 8.4 Bindings and controls on a faceplate (*proposal*)

A binding ties an SVG element to a state key, e.g.:
- `{"element": "led1", "attr": "fill", "key": "dq.1", "map": {"true": "#e33", "false": "#422"}}`
- `{"element": "volt1", "text": "ai.1", "format": "%.2f V"}`
- `{"element": "fault2", "visible": "fault.2"}`

A control makes an element operable:
- `{"element": "term1", "control": "slider", "key": "ai.1.source", "min": 0, "max": 10, "step": 0.01}`
- `{"element": "sw1", "control": "toggle", "key": "di.1.source"}`, `"button"` (momentary), `"number"` (typed entry).

A manifest's keys omit the slot; the emulator prefixes it (`slot1.dq.1`), so a manifest is written once, independent of the slot.

## 9. Later

- **Simulated time** through the link's `clock` message: setting the wall clock (e.g. to test a schedule on Monday at 06:59). Running faster than real time would also need hakkas to run on simulated time (its timers, PID and Interval use its monotonic clock) — a change to hakkas.
- **Fault injection**: a broken wire, a dead sensor, an out-of-range value — once it is decided what the firmware reports for them (`04-drivers.md` §0 rule 2: every channel reports a value even when its sensor is dead).
- The MCU and Comm modules.

## 10. Open questions

1. *Decided* (2026-10-23): the built-in I/O slot is slot 0, with nothing to emulate for now (§2).
2. **The channels each module offers** (§2.1; names decided 2026-10-23): the units, ranges and resolutions are still guesses from the hardware; they must match what the firmware's drivers offer.
3. *Decided* (2026-10-23): DII/DIU deliver per channel frequency, duty cycle (0–100 %) and a 32-bit unsigned counter that rolls over; the DIU has 4 inputs, the DII 2 (§2.1).
4. *Decided* (2026-10-23): DIO1's direction is set by the user program through an output channel (`dir`, true = output); its fault reporting is not defined yet, presumably everything the MAX14906 reports (§2.1).
5. *Decided* (2026-10-23): PID1 is a full PID regulator whose parameters, named as hakkas' `process/PID`'s, are output channels, with a Pt1000 input; the emulator does not regulate (§2.1).
6. *Decided* (2026-10-23): the write queue (§4.1) — one per module, emptied at each turn, 10 values for now, a full queue drops the new value; on the input side a driver forwards a value to its channel only in its slot's turn. The firmware's behaviour is to be reviewed later (the user).
7. **The environment name check**: hakkas refuses a bindings file whose `environment` differs from the running environment's `name` (`specifications/04-drivers.md` §4). With the user choosing the bindings file (§5.2), should that check go — the file's `environment` kept as information, and a binding to a port the environment lacks still skipped with a warning (`04` §4 item 6) — or should the emulator's environments report the device's name?
