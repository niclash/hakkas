# Colibri Emulator — Specification

> **Draft** (started 2026-10-23). Nothing is implemented yet. Decisions are marked *decided* with their date; *proposal* marks a suggestion still waiting for one. Open questions are in §10.

## 1. What this is

The Colibri emulator lets a user test a hakkas program (processes and their bindings files) on a desktop before it is loaded into a Colibri device. It emulates a Colibri system's **I/O side**: a carrier board with its I/O module slots and the modules in them. The user sees each module's faceplate in a browser, sets its inputs (a voltage on an AIV terminal, a contact on a DII), and watches its outputs (an SSR's LED, an AQV's voltage) follow what the hakkas processes do.

It is a **separate project** in `emulators/colibri/`, with its own build and tests; hakkas does not depend on it, and it depends on hakkas only through the environment link protocol (`specifications/04-drivers.md` §0.1).

**In scope**: the carrier board's I/O slots; the I/O modules, as plugins (§7); the slot scan cycle and its timing (§4); environments made from scratch, copied from a device, and compared (§5.1); a control channel (§8), on which a browser UI and external programs set inputs and follow outputs; the UI is generic, so other emulators can use it too.

**Not part of the emulator**: application testing — plant models, scenarios — which external programs do through the control channel (§8.1).

**Out of scope for now**: emulating the MCU module and the Comm module (Ethernet, RS-485, WiFi, LoRaWAN); hakkas itself runs as its POSIX build on the desktop instead of the MCU. The carrier board's power supply and its I2C/SPI infrastructure are not emulated (§2).

**Implementation** (*decided*, 2026-10-23): TypeScript on Node.js; module plugins' behaviour as JavaScript modules loaded at runtime (§7.3).

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

Collected 2026-10-23 from the CurrentMakers catalogue (https://currentmakers.com/products/catalog/?category=colibri), its product pages and the STM32World wiki pages they link. The pages give hardware, not the channels a driver offers; the **channels** column is this specification's proposal, to be confirmed (question 2).

| Type | Hardware (from the pages) | Channels offered to hakkas (*proposal*) |
|---|---|---|
| AIV | 2 × 0–10 V in, ADS1115 16-bit ADC, I2C | `ain` 2 × number, V, 0–10 |
| AIC | 2 × 0–20 mA in, ADS1115 16-bit ADC, I2C | `ain` 2 × number, mA, 0–20 |
| AQV | 2 × 0–10 V out, MCP4725 12-bit DAC, I2C | `aout` 2 × number, V, 0–10 (actuator) |
| Pt1000 | 2 × Pt1000 (also Ni1000 DIN, Ni1000 Tk5000), MAX31865 per input (SPI), open/short detection | `temp` 2 × number, °C; `fault` 2 × bool (sensor) |
| DP1 | 1 × 0–500 Pa differential pressure (SM7391, I2C) | `pressure` 1 × number, Pa, 0–500 |
| DP2 / DP3 | 0–10 kPa / 0–100 kPa (wiki only: planned) | as DP1, in Pa |
| DII | 2 isolated digital in, on-board STM32C071 | `di` 2 × bool; `count` 2 × number; `pulse` 2 × number (question 3) |
| DIU | digital in, not isolated, on-board STM32C071 — 2 channels on the product page, 4 on the wiki overview | as DII (question 3) |
| DIO1 | IEC 61131-2, MAX14906 (4 channels per its datasheet), each channel input or output, current limit set in software (100 mA advised) | `di` 4 × bool (sensor); `do` 4 × bool (actuator); `fault` 4 × bool (sensor) (question 4) |
| FET | 4 × N-channel MOSFET sink, 1 A, 60 V, PCA9536, I2C | `do` 4 × bool (actuator) |
| SSR | 2 × solid-state relay, 80 mA, 60 V DC, PCA9536 | `relay` 2 × bool (actuator) |
| TRIAC1 | 4 × triac, 500 mA AC, zero-cross detection, PCA9536 | `triac` 4 × bool (actuator) |
| Fallback | 1 × SPDT relay (wiki only) | `relay` 1 × bool (actuator) |
| PID1 | 1 × Pt1000 in (MAX31865), 1 × 0–10 V out (MCP4725 12-bit), STM32F030 running a PID regulator | sensors `temp`, `output`; actuators `setpoint`, `kp`, `ki`, `kd`, `min`, `max`, `manual`, `manualValue` (`04-drivers.md` §1; question 5) |
| RS485I / RS485U | RS-485 | not described to hakkas (`04-drivers.md` §1) |

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
1. hand the module the actuator values hakkas has written for its channels since the slot was last served (§4.1);
2. run the module's behaviour (§7.3), which computes its sensor values from its field side, its actuator values and the elapsed time;
3. send every sensor value that changed to hakkas, and every changed state to the UI.

So a change on the field side reaches hakkas, and a write from hakkas reaches the field side, only when its slot is served — the latency a program sees on the device.

### 4.1 Values arriving faster than the cycle

The point of emulating the cycle is the timing (*decided*, 2026-10-23): a program that writes an actuator more often than its slot is served, or that depends on seeing every edge of a fast input, must show the same behaviour on the desktop as on the device. That requires the emulator to treat the values between two turns of a slot **as the firmware does** — question 6. Until decided, the emulator coalesces (only the latest write per channel reaches the module; a field-side pulse shorter than a cycle is seen by the state channel only if it lasts until the slot's turn, though a counter still counts it).

The emulator counts, per channel, the writes received, the writes **coalesced or dropped** (never reaching the module) and the sensor changes sent, and shows them in the UI (§8), so the user can see where a program outruns the cycle.

## 5. Configuration

- **Carrier board**: which carrier (Colibri 8, Colibri-16pi, …) — the number of I/O slots and what the built-in slot provides follow from it.
- **Number of I/O slots**: 1…n, for a carrier made from scratch.
- **I/O module cycle time**: default 10 ms.
- **Modules**: which module type (plugin) sits in which slot; a slot may be empty.

These are kept in a **configuration file** (*proposal*: given on the command line) and can be changed at runtime from the UI, which writes the file back. Example:

```json
{"carrier": "colibri/Colibri8", "slots": 7, "cycleMs": 10,
 "modules": [{"slot": 1, "type": "colibri/AIV", "serial": "EMU-0001"},
             {"slot": 3, "type": "colibri/SSR"}]}
```

Changing the cycle time takes effect at once. **Changing the carrier, the slot count or the modules is a power cycle** of the emulated device (*decided*, 2026-10-23): it changes the environment description, which hakkas accepts only at its startup (`04-drivers.md` §0.1), so the emulator closes the link and the UI tells the user to restart hakkas.

The field-side values the user has set (*proposal*) are kept with the configuration, so a test setup comes back as it was left.

### 5.1 Environments from scratch, from a device, and compared

Required (*decided*, 2026-10-23); the details are to be discussed (question 7):
- **From scratch**: the user builds an environment without a physical twin — carrier, slots, modules — in the UI.
- **Copied from a device**: the user takes the environment (carrier, modules per slot, their metadata) from a connected Colibri device, so the desktop tests run against exactly what the device has.
- **Compared**: two environments (e.g. the emulator's and a device's, or two configuration files) are compared, showing the slots and channels that differ — e.g. before loading a tested program into a device.

## 6. The environment description

The emulator describes itself to hakkas per `04-drivers.md` §1:

- `name`: decided with §5.1 (question 7).
- **Ports are named per slot**: `slot<n>-<channel group>`, e.g. `slot1-ain` (AIV in slot 1), `slot3-relay` (SSR in slot 3). A group of identical channels is one addressable port with `count`; a module with several groups has several ports (DII in slot 2: `slot2-di` bool, `slot2-count` and `slot2-pulse` numbers). The channel group ids and their types, units and ranges come from the module's plugin (§7.1).
- **Modules**: one entry per filled slot, with `slot`, `type`, `vendor`, `revision`, `serial`, `documentation` from the plugin and the configuration, and the ids of its ports.

The emulator gives the same ports, port ids and module list that the firmware gives for the same modules in the same slots, so that one bindings file works on both. This requires the firmware's naming to be fixed in `04-drivers.md` §1 (it is not yet), and both to follow it.

## 7. Module plugins

Every I/O module type is a **plugin**, also the built-in ones, so that others can make modules for Colibri and supply their emulation with them. A plugin is a directory (a package) with:

```
colibri-aiv/
  module.json      the manifest (§7.1)
  faceplate.svg    the module's picture (§7.2)
  behaviour.js     optional: behaviour beyond the default (§7.3)
```

Plugins are found in the emulator's built-in plugin directory and in directories named on the command line (*proposal*). A plugin's type name is `<vendor>/<Name>` (e.g. `colibri/AIV`), as hakkas' block types are `family/Name`. When slot 0 gets channels to emulate, they come from a plugin too.

### 7.1 The manifest

```json
{"type": "colibri/AIV", "vendor": "CurrentMakers", "revision": "1",
 "description": "2 × 0–10 V analog inputs",
 "documentation": "https://currentmakers.com/products/colibri/colibri-aiv/",
 "channels": [
   {"id": "ain", "direction": "sensor", "type": "number", "count": 2,
    "unit": "V", "min": 0, "max": 10, "resolution": 0.001, "description": "0–10 V input"}
 ],
 "faceplate": "faceplate.svg",
 "ui": { … the bindings of §8.3 … }}
```

- `channels` become the description's ports (§6); `direction` is `sensor` (an environment output port) or `actuator` (an environment input port); `count` > 1 makes the port addressable.
- A module may keep **internal state** that is neither (a DII's counter, PID1's integral) — the behaviour's own business.

### 7.2 Graphics assets

A plugin's faceplate is an **SVG** whose elements carry ids; the manifest's `ui` section binds those elements to channel values and field-side values (an LED's colour to a relay channel, a label's text to a voltage) and declares the controls the user operates (a slider on an analog input's terminal, a toggle on a digital input). The emulator does not draw: it hands the assets and bindings to the generic UI (§8).

### 7.3 Behaviour

Most modules need no code: by default, a sensor channel takes the value the user set on its field side (clamped to `min`/`max`, rounded to `resolution`), and an actuator channel's value is shown on its field side.

A module that does more supplies a behaviour, a JavaScript module whose `serve` function is called each time its slot is served (§4) with its field-side values, its actuator values since the last call, its internal state and the time since the last call; it returns its sensor values, field-side indications and new internal state. Examples: DII/DIU counting edges and measuring pulse width; DIO1's channel modes and fault flags; Pt1000's open/short faults; PID1 regulating its output from its Pt1000 input and its parameters.

(*Proposal*) A field-side input is a constant value by default; where the plugin allows, it may also be a **signal** the behaviour evaluates at each turn — e.g. a pulse train `{"hz": 42.5, "duty": 0.5}` on a DII terminal, from which the behaviour counts the edges and measures the pulse width since its last turn, as the module's own MCU would. A client cannot time individual pulses shorter than the scan cycle over a socket; a signal lets it represent such a sensor anyway.

(*Proposal*) A behaviour runs inside the emulator's process with no access to anything but its arguments by convention; isolating third-party plugins (a worker thread or `vm` context) is left until there are third-party plugins.

## 8. The UI and the control channel

### 8.1 Principle

The emulator's state — field-side values, channel values, actuator values, settings, the counters of §4.1 — is available on a **control channel**, with one protocol (§8.3) for every client: the browser UI and **external programs** (a Python script, a demo, an automation). All clients are equal: each may set inputs and follows the whole state.

It follows hakkas' principle (`../../../specifications/00-README.md`): **the emulator owns the state, clients only mirror it**. A client sends a request; the emulator applies it and broadcasts the resulting state to every connected client, the sender included, which updates its view only from that. When two clients set the same input, the last one wins and every client sees it.

The control channel is **separate from hakkas' environment link** (*decided*, 2026-10-23): hakkas exchanges channel values on the MCU's side of the modules, while clients work on the **field side** — a voltage on a terminal, a contact closed — and what they set reaches hakkas only through the module's behaviour and the scan cycle (§4), as on the device. (The link protocol could not tell the two kinds of client apart anyway: the emulator sends `describe` first on every connection, `04-drivers.md` §0.1.)

**Application testing is not part of the emulator** (*decided*, 2026-10-23): plant models, scenarios and the like — testing a Colibri application against a simulated real world, e.g. a two-stage furnace regulator whose setpoint comes from a sensor's pulse train — are written outside it, in Python or whatever is at hand, as clients of the control channel. The emulator's job is to make that easy: a simple, documented protocol, on a transport a script reaches without a library.

### 8.2 The UI layout (*proposal*)

The UI knows nothing about Colibri; it renders what the emulator describes:
- **The carrier board**: a photo or drawing of the board (an asset of the carrier's description, §5), with the place of each slot marked (an element with the id `slot-<n>`).
- **The modules**: each module's faceplate (§7.2) drawn in its slot's place on the carrier, with its LEDs and terminal values bound to the state and, where the plugin has them, controls on it.
- **The signal panel**, beside the drawing: one row per field-side input (a slider, toggle, number entry) and per output (an indicator), with the channel's name (`slot1-ain[1]`), unit and value. The UI builds it from the channel list alone, so every module is usable, also one without a faceplate.
- **The settings** (§5) and the counters (§4.1).

The UI is its own package (*proposal*: `emulators/ui/`, with the Colibri emulator serving its built files and the assets over HTTP), so that other emulators reuse it.

### 8.3 The control protocol (*proposal*)

JSON messages, the same on every transport:
- emulator → client: `scene` (the carrier and module assets, the slots' places, bindings and controls, the channel list; sent on connect and after every configuration change), `state` (changed values, by key; all of them on connect), `settings` (the runtime settings and their allowed values: carrier, slot count, cycle time, the module types available per slot), `stats` (the counters of §4.1).
- client → emulator: `set` (an input's new field-side value, by key), `configure` (a setting change, e.g. a module put into slot 4).

**Transports**: a **WebSocket** (the browser UI; also usable from scripts) and a **Unix socket** with newline-delimited JSON, like hakkas' environment link (`04-drivers.md` §0.1) — reachable from a shell (`socat`) or a few lines of Python. Only the framing differs.

Keys name a slot, a value and a channel: `slot1.field.ain.1` (the voltage on AIV terminal 1), `slot1.ain.1` (the channel value hakkas gets), `slot3.relay.1` (the relay's state as hakkas wrote it).

### 8.4 Bindings and controls on a faceplate (*proposal*)

A binding ties an SVG element to a state key, e.g.:
- `{"element": "led1", "attr": "fill", "key": "relay.1", "map": {"true": "#e33", "false": "#422"}}`
- `{"element": "volt1", "text": "ain.1", "format": "%.2f V"}`
- `{"element": "fault2", "visible": "fault.2"}`

A control makes an element operable:
- `{"element": "term1", "control": "slider", "key": "field.ain.1", "min": 0, "max": 10, "step": 0.01}`
- `{"element": "sw1", "control": "toggle", "key": "field.di.1"}`, `"button"` (momentary), `"number"` (typed entry).

A manifest's keys omit the slot; the emulator prefixes it (`slot1.relay.1`), so a manifest is written once, independent of the slot.

## 9. Later

- **Simulated time** through the link's `clock` message: setting the wall clock (e.g. to test a schedule on Monday at 06:59). Running faster than real time would also need hakkas to run on simulated time (its timers, PID and Interval use its monotonic clock) — a change to hakkas.
- **Fault injection**: a broken wire, a dead sensor, an out-of-range value — once it is decided what the firmware reports for them (`04-drivers.md` §0 rule 2: every channel reports a value even when its sensor is dead).
- The MCU and Comm modules.

## 10. Open questions

1. *Decided* (2026-10-23): the built-in I/O slot is slot 0, with nothing to emulate for now (§2).
2. **The channels each module offers** (§2.1): the proposed channel groups, ids, units, ranges and resolutions are guesses from the hardware; they must match what the firmware's drivers offer, since bindings name them.
3. **DII/DIU**: what are count and pulse width exactly — counting which edge, reset (by an actuator channel?) and overflow, pulse width of the high or low phase and its unit, debounce? Does the DIU have 2 or 4 channels?
4. **DIO1**: how is a channel's mode (input/output, and the MAX14906's modes) chosen — fixed in the module's configuration, or by hakkas at runtime? Which faults become sensor channels (per channel, per module)?
5. **PID1**: its parameters and reports, and what it does on its own when the main MCU stops serving it.
6. **Values between two turns of a slot** (§4.1): when hakkas writes a channel several times between turns, does the firmware pass on only the latest, or queue them (with what bound, and what is dropped on overflow)? And on the input side, does a driver report only the state at its turn, or also edges in between?
7. **Environments from a device and their comparison** (§5.1): to be discussed — what is copied (carrier, module types, revisions, serials, calibration?), how the emulator reaches the device (the device's environment is not on hakkas' wire yet, `04-drivers.md` §1; adding it is a protocol change to put to the user), what "compare" reports, and the environment's `name` (`04-drivers.md` §4 refuses a bindings file written for another name).
