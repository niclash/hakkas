# Colibri Emulator — Specification

> **Draft** (started 2026-10-23; settled for implementation in plan task E0, 2026-10-25). Nothing is implemented yet; the order of the work is `../docs/plan.md`. Decisions are marked *decided* with their date. Values marked **provisional** are guesses from the hardware, to be adjusted when the firmware's are known (§10 question 2). Open questions are in §10.

## 1. What this is

The Colibri emulator lets a user test a hakkas program (processes and their bindings files) on a desktop before it is loaded into a Colibri device. It emulates a Colibri system's **I/O side**: a carrier board with its I/O module slots and the modules in them. The user sees each module's faceplate in a browser, sets its inputs (a voltage on an AIV terminal, a contact on a DII), and watches its outputs (an SSR's LED, an AQV's voltage) follow what the hakkas processes do.

It is a **separate project** in `emulators/colibri/`, with its own build and tests; hakkas does not depend on it, and it depends on hakkas only through the environment link protocol (`specifications/04-drivers.md` §0.1) and, to copy an environment from a device, hakkas' wire protocol (§5.3).

**In scope**: the carrier board's I/O slots; the I/O modules, as plugins (§7); the slot scan cycle and its timing (§4); a library of environments, made from scratch, copied from a device (§5); a control channel (§8), on which a browser UI and external programs set inputs and follow outputs; the UI is generic, so other emulators can use it too.

**Not part of the emulator**: application testing — plant models, scenarios — which external programs do through the control channel (§8.1).

**Out of scope for now**: emulating the MCU module and the Comm module (Ethernet, RS-485, WiFi, LoRaWAN); hakkas itself runs as its POSIX build on the desktop instead of the MCU. The carrier board's power supply and its I2C/SPI infrastructure are not emulated (§2).

**Implementation** (*decided*, 2026-10-23, completed 2026-10-25): TypeScript on Node.js 24, compiled with `tsc`; tests with vitest, and Playwright for the UI. Module plugins are data files, not code (§7). The UI is the generic emulator UI, `emulators/ui/` (Vue 3 and Vite, like hakkas' frontend; `../../specifications/00-README.md`), and the control protocol is the shared one, `../../specifications/01-control-protocol.md`.

## 2. The emulated system

A Colibri system is, conceptually:

- one **carrier board**: power supply, the I2C and SPI infrastructure (I2C multiplexed per slot, one SPI chip select per slot, plus two more for a module with several SPI devices; each slot powered and addressed individually), **one I/O slot built into the board**, and the slots below;
- **1…n I/O module slots** (M.2 connectors) — Colibri 8 has 7, Colibri-16pi 15;
- one **Comm module slot** (not emulated);
- one **MCU module slot** (not emulated: hakkas runs on the desktop).

The built-in I/O slot exists so that the firmware's (Zephyr's) architecture treats the carrier board's own I/O like any module's (*decided*, 2026-10-23). Colibri 8 thus has **8 I/O slots** in the scan cycle (§4). The built-in slot is **slot 0**; nothing in it is emulated for now (*decided*, 2026-10-23): it takes its turn in the scan cycle with no channels, like an empty slot. A built-in slot holds an **I/O module that cannot be changed** (*decided*, 2026-10-24): it comes with the carrier's definition (§5.4) and is listed among the modules, marked built-in. Slot 0's module has no real name (*decided*, 2026-10-25): its type is **`carrier`**, and it has no human-readable name.

On the device, the firmware identifies each I/O module at boot from its EEPROM and runs the module's driver cyclically (§4); it then offers the modules' channels to hakkas as the environment (`04-drivers.md` §1). The emulator takes the firmware's place towards hakkas: it is the **environment program** of `04-drivers.md` §0.1.

**Fidelity** (*decided*, 2026-10-23): the emulator models each module at the level of its **channels** — the values the firmware's driver delivers to and takes from hakkas — plus the module's **field side**, the physical terminals the user manipulates in the UI. There is no emulation of I2C/SPI registers or of the driver code in a module's EEPROM. What the emulator does reproduce is the **timing** of the firmware's slot scan, including what happens when values arrive faster than a slot is served (§4).

### 2.1 The module catalogue

Collected 2026-10-23 from the CurrentMakers catalogue (https://currentmakers.com/products/catalog/?category=colibri), its product pages and the STM32World wiki pages they link. The pages give hardware, not the channels a driver offers.

**Type names** (*decided*, 2026-10-25): a module's machine-readable type and its human-readable name are both in its EEPROM from the factory and never change. The type is `colibri_<module>` in lower case, and the name includes "Colibri": `colibri_pt1000` is "Colibri Pt1000". Carriers and communication modules are named the same way (`colibri_8`, "Colibri 8"; §5.4).

**Channel names** (*decided*, 2026-10-23): `ai` analog input, `aq` analog output, `di` digital input, `dq` digital output — whatever the hardware (a relay, a FET, a triac is a `dq`); channels that are none of these keep their own names (`freq`, `duty`, `count`, `fault`, PID1's parameters). Temperatures and pressures are `ai` too, the unit telling what they measure.

| Type | Name | Hardware (from the pages) |
|---|---|---|
| `colibri_aiv` | Colibri AIV | 2 × 0–10 V in, ADS1115 16-bit ADC, I2C |
| `colibri_aic` | Colibri AIC | 2 × 0–20 mA in, ADS1115 16-bit ADC, I2C |
| `colibri_aqv` | Colibri AQV | 2 × 0–10 V out, MCP4725 12-bit DAC, I2C |
| `colibri_pt1000` | Colibri Pt1000 | 2 × Pt1000 (also Ni1000 DIN, Ni1000 Tk5000), MAX31865 per input (SPI), open/short detection |
| `colibri_dp1` | Colibri DP1 | 1 × 0–500 Pa differential pressure (SM7391, I2C) |
| `colibri_dp2`, `colibri_dp3` | Colibri DP2, Colibri DP3 | 0–10 kPa / 0–100 kPa (wiki only: planned) |
| `colibri_dii` | Colibri DII | 2 isolated digital inputs, on-board STM32C071 |
| `colibri_diu` | Colibri DIU | 4 digital inputs, not isolated, on-board STM32C071 (*decided*, 2026-10-23; the product page's 2 is wrong) |
| `colibri_dio1` | Colibri DIO1 | IEC 61131-2, MAX14906 (4 channels per its datasheet), each channel input or output, current limit set in software (100 mA advised) |
| `colibri_fet` | Colibri FET | 4 × N-channel MOSFET sink, 1 A, 60 V, PCA9536, I2C |
| `colibri_ssr` | Colibri SSR | 2 × solid-state relay, 80 mA, 60 V DC, PCA9536 |
| `colibri_triac1` | Colibri TRIAC1 | 4 × triac, 500 mA AC, zero-cross detection, PCA9536 |
| `colibri_fallback` | Colibri Fallback | 1 × SPDT relay (wiki only) |
| `colibri_pid1` | Colibri PID1 | 1 × Pt1000 in (MAX31865), 1 × 0–10 V out (MCP4725 12-bit), STM32F030 running a full PID regulator |
| `colibri_rs485i`, `colibri_rs485u` | Colibri RS485I, Colibri RS485U | RS-485 — not defined yet; outside the scope of the emulator and of hakkas (*decided*, 2026-10-23) |

The catalogue also lists MCU modules (MCU1 STM32WLE5, MCU2 STM32H562RG, MCU3 STM32F405RG, MCU5 STM32F103RE, MCU6 STM32H735RG, MCU7 RP2350) and the carriers Colibri 3a and Colibri 8; neither kind is emulated (the Colibri 8 carrier board is, §5.4).

**The channels** (*decided*, 2026-10-25, the values **provisional**): per module, its channel groups. Direction *in* is a module input (a sensor to hakkas), *out* a module output (an actuator). A group of one channel is a plain port; several make an addressable port (§6). *Res.* is the resolution values are rounded to, *Noise* the default noise (§7.2), *UI* the default control or indicator.

| Module | Channel | Dir. | Type | Count | Unit | Range | Res. | Default | Noise | UI |
|---|---|---|---|---|---|---|---|---|---|---|
| AIV | `ai` | in | number | 2 | V | 0–10 | 0.001 | 0 | 0.005 | slider |
| AIC | `ai` | in | number | 2 | mA | 0–20 | 0.001 | 0 | 0.005 | slider |
| AQV | `aq` | out | number | 2 | V | 0–10 | | 0 | | bar |
| Pt1000 | `ai` | in | number | 2 | °C | −50–150 | 0.01 | 21 | 0.05 | slider |
| | `fault` | in | bool | 2 | | | | false | | toggle |
| DP1 | `ai` | in | number | 1 | Pa | 0–500 | 0.1 | 0 | 0.5 | slider |
| DP2 | `ai` | in | number | 1 | Pa | 0–10000 | 1 | 0 | 5 | slider |
| DP3 | `ai` | in | number | 1 | Pa | 0–100000 | 10 | 0 | 50 | slider |
| DII (DIU: count 4) | `di` | in | bool | 2 | | | | false | | (measured) |
| | `freq` | in | number | 2 | Hz | 0–10000 | 0.01 | 0 | 0 | (measured) |
| | `duty` | in | number | 2 | % | 0–100 | 0.1 | 0 | 0 | (measured) |
| | `count` | in | number | 2 | | 0–4294967295 | 1 | 0 | | (measured) |
| DIO1 | `dir` | out | bool | 4 | | | | false | | lamp |
| | `dq` | out | bool | 4 | | | | false | | lamp |
| | `di` | in | bool | 4 | | | | false | | toggle |
| | `fault` | in | bool | 4 | | | | false | | toggle |
| FET | `dq` | out | bool | 4 | | | | false | | lamp |
| SSR | `dq` | out | bool | 2 | | | | false | | lamp |
| TRIAC1 | `dq` | out | bool | 4 | | | | false | | lamp |
| Fallback | `dq` | out | bool | 1 | | | | false | | lamp |
| PID1 | `ai` | in | number | 1 | °C | −50–150 | 0.01 | 21 | 0.05 | slider |
| | `output` | in | number | 1 | V | 0–10 | 0.001 | 0 | 0 | slider |
| | `saturated` | in | bool | 1 | | | | false | | toggle |
| | `setpoint` | out | number | 1 | °C | −50–150 | | 0 | | number |
| | `kp`, `ki`, `kd`, `max_dgain` | out | number | 1 each | (`ki` 1/s, `kd` s) | | | 0 | | number |
| | `min`, `max`, `manual` | out | number | 1 each | V | 0–10 | | 0 | | number |
| | `reverse`, `auto` | out | bool | 1 each | | | | false | | lamp |
| | `sample_ms` | out | number | 1 | ms | | | 0 | | number |
| RS485I, RS485U, `carrier` | (none) | | | | | | | | | |

- **DII and DIU** (*decided*, 2026-10-25): the on-board STM32C071 measures each input and reports, per input, its **state** (`di`), **frequency** (`freq`), **duty cycle** (`duty`, 0–100 %) and **pulse count** (`count`, a 32-bit unsigned counter that rolls over) every scan turn of its slot (80 ms on Colibri 8). The emulator gives each input terminal one field-side **signal**, `in`, a pulse train (§7.2), from which the four channels are measured.
- **DIO1** (*decided*, 2026-10-23 and 2026-10-25): the direction is set by the user program through `dir` (true makes the channel an output). `fault` is one **accumulated** flag per channel, true while any fault of that channel or of the whole module is reported; each kind of fault the MAX14906 reports becomes a separate flag later. In the emulator `di` and `dq` are not connected: `dir` and `dq` are shown, and `di` and `fault` are set, independently.
- **PID1** (*decided*, 2026-10-23): a full PID regulator whose parameters are output channels named as hakkas' `process/PID`'s inputs (`specifications/03-block-types.md` §6.1); its Pt1000 measurement, its output and `saturated` are inputs. The emulator does not regulate: `output` and `saturated` are settable inputs like any other.
- **RS485I/RS485U** have no channels: the emulator places the module's picture in its slot, with no controls or indicators, and they are described to hakkas as modules without ports.

## 3. Place in the system

```
 browser UI (§8)            external programs: scripts, demos, automation
    │  WebSocket             │  Unix socket — both the control protocol (§8)
 colibri-emulator ── module plugins (§7), the scan cycle (§4), environments (§5)
    │  Unix socket, newline-delimited JSON (04-drivers.md §0.1)
 hakkas (POSIX build) ── processes + bindings files
    │  WebSocket, the FBP wire protocol (02-wire-protocol.md)
 browser ── hakkas frontend
```

- **Towards hakkas** the emulator is the server of the environment link: it listens on a Unix socket (hakkas' `environment.socket`), sends its `describe` (§6) first on every connection, then all current sensor values, then changes; it takes hakkas' `write` messages for the actuators. It follows every rule of `04-drivers.md` §0.1, including re-sending all current sensor values after a reconnect. Decided 2026-10-25:
  - **One hakkas per emulator**, one-to-one in both directions: while one connection is open, another is closed at once, without a `describe`, with a warning in the log and a `notice` to the clients (§8).
  - hakkas' `{"hello": {"version": 1}}` is checked: any other version closes the connection, with a warning. A `write` arriving before the greeting is accepted (the greeting is sent first by hakkas anyway).
  - A line that is not valid, or a `write` for an unknown port, a sensor port, a channel the port does not have or a value of another type, is logged as a warning (at most 8 per connection, as hakkas does) and ignored. A line over 256 KiB closes the connection.
  - No `clock` messages for now (§9): hakkas uses its host's clock.
- **Towards the user and external programs** it serves the generic UI and offers the control channel (§8).
- The two programs are started separately; the emulator does not start or stop hakkas (launching them as a pair comes later, §9).

### 3.1 Running it

(*Decided*, 2026-10-25.)

```
colibri-emulator [--library <dir>] [--plugins <dir>]… [--socket <path>] [--control-socket <path>]
                 [--http <port>] [--seed <n>] [--log-level <level>] [-h|--help] [--version]
                 [<environment>]
```

- `--library`: the library directory (§5.1), default `$XDG_DATA_HOME/colibri-emulator/` (`~/.local/share/colibri-emulator/`); with neither `XDG_DATA_HOME` nor `HOME` set it must be given.
- `--plugins`: a further plugin directory (§7), repeatable, after the built-in one.
- `--socket`: the environment link's socket, default `$XDG_RUNTIME_DIR/colibri-emulator/environment.sock` (the path hakkas' `environment.socket` names); `--control-socket`: the control channel's, default `…/control.sock`. Without `XDG_RUNTIME_DIR` the directory is `colibri-emulator-<uid>` in the system's temporary directory. A socket path must fit a Unix socket address (107 bytes on Linux), and the two must differ. A stale socket file is replaced; a socket another emulator is listening on refuses startup.
- `--http`: the HTTP port for the UI, its assets and the control WebSocket, on localhost only; default 8090, `0` for any free port (tests).
- `--seed`: seeds the noise's random generator, so a run is reproducible (tests), 0 to 4294967295; without it the seed is random.
- `--log-level`: `fine`, `info` (the default), `warn` or `error`, hakkas' levels (05 §4).
- `-h`/`--help` prints the usage, `--version` the version, both on standard output, exit 0.
- `<environment>`: the environment to run; without it, the one that ran last (§5.1).

The emulator logs to standard error, one line per event, `<level> <message>` (line breaks in a message become spaces; no timestamp, as hakkas'). `SIGINT`/`SIGTERM` stop it, closing its sockets and removing their files. Exit codes (implemented in plan task E1): 0 after a stop request, 1 when the emulator cannot start (logged as errors, then `the emulator did not start`), 2 for a usage error (the message and the usage on standard error).

## 4. The slot scan cycle

The firmware serves its I/O slots **one at a time, in turn**, and so does the emulator: every **cycle time** (default **10 ms**) it serves the next slot — slot 0 (the built-in slot), then slot 1, 2, …, n, then slot 0 again. A module's channels are therefore refreshed once every (n + 1) × cycle time: **80 ms** on Colibri 8 with the default.

**Every slot takes its turn** (*decided*, 2026-10-23), filled or not: on the device a dummy driver is loaded for an empty slot, so the runtime's critical path does not depend on which slots are filled. The emulator gives an empty slot a dummy module with no channels.

**Serving a slot** (*decided*, 2026-10-25):
1. hand the module the actuator values queued for its channels since the slot was last served (§4.1), in order; each output channel's value is the last one applied;
2. compute every input channel's value from its source and noise, and every measured channel's from its signal (§7.2);
3. send every sensor value that changed to hakkas, and every changed state to the clients (§8).

So a change on the field side reaches hakkas, and a write from hakkas reaches the field side, only when its slot is served — the latency a program sees on the device. The turns are **deadline-based**: turn *k* is due at start + *k* × cycle time, so the cycle does not drift; a turn that comes late (a busy desktop) is served at once, and turns more than a whole cycle (all slots) behind are skipped, with a warning at most once a minute. Changing the cycle time takes effect from the next turn.

### 4.1 Values arriving faster than the cycle

The point of emulating the cycle is the timing (*decided*, 2026-10-23): a program that writes an actuator more often than its slot is served, or that depends on seeing every edge of a fast input, must show the same behaviour on the desktop as on the device. That requires the emulator to treat the values between two turns of a slot **as the firmware does**: the firmware **queues** the values hakkas writes (*decided*, 2026-10-23), and so does the emulator — every write reaches the module, in order, none is coalesced. (*Decided*, 2026-10-23:)
- **One queue per I/O module**, for the writes to all its output channels.
- **A turn applies every queued value**, in order, and leaves the queue empty.
- **The queue holds 10 values** (its size on the device is not determined yet; it is a setting, §5).
- **A full queue drops the new value.** So when a program writes a module's outputs more than 10 times within one scan cycle, the last writes are lost and the outputs keep older values — until hakkas writes them again, which it does only when the value changes. **On the input side**, a driver forwards a value to its channel only in its slot's turn (*decided*, 2026-10-23); nothing between turns is reported. An input's source is therefore sampled at its slot's turn, so a `square` faster than the cycle shows only the samples a turn hits; a `counter` or a pulse train's `count` (§7.2) counts at its rate regardless.

The emulator counts, per channel, the writes received, the writes queued, the writes **dropped** on overflow and the sensor changes sent, and per module the queue's current and highest length, and shows them in the UI (the control protocol's `stats`), so the user can see where a program outruns the cycle. A write for a slot whose module was removed by a power cycle cannot arrive: the power cycle closed the link (§5.1).

## 5. Environments

An **environment** is one emulated Colibri system:
- **Carrier board**: which carrier definition (§5.4: Colibri 8, …) — the number of I/O slots, the built-in modules, the power specification and the communication slots follow from it.
- **Communication modules**: per communication slot, the module's type and its parameters (§5.4); not emulated, only described to hakkas.
- **I/O module cycle time**: default 10 ms (1–1000).
- **Write queue size** per module (§4.1): default 10 (1–1000).
- **Modules**: which module type (plugin) sits in which slot, and its serial number; a slot may be empty.
- **The inputs' sources and noise** (§7.2), so a setup comes back as it was left.

Each environment is a file, `<name>.json`, its file name being the environment's name (§6). Example:

```json
{"carrier": "colibri_8", "cycleMs": 10, "queueSize": 10,
 "comm": [{"slot": 1, "type": "colibri_ethernet",
           "ip": {"mode": "dhcp", "address": "192.168.1.40", "prefix": 24, "gateway": "192.168.1.1",
                  "dns": ["192.168.1.1"], "mac": "02:00:00:00:00:01"}}],
 "modules": [{"slot": 1, "type": "colibri_aiv", "serial": "EMU-0001"},
             {"slot": 3, "type": "colibri_ssr"}],
 "inputs": {"slot1.ai.1": {"source": 5.0, "noise": 0.02},
            "slot2.in.1": {"source": {"pulse": {"frequency": 10, "duty": 50}}}}}
```

- `carrier` is a carrier type, or a whole carrier definition (§5.4) for a carrier copied from a device that the emulator has no definition for (§5.3).
- `modules` lists the changeable slots only; a built-in slot's module comes from the carrier.
- `inputs` holds, by key (§8), the source and noise of each input and signal that differ from the plugin's defaults.
- A member the emulator does not know is kept as it is when the file is written again. A file that does not parse, or names a slot the carrier does not have, cannot be run: the UI shows why. A module type without a plugin leaves its slot empty, with a warning (the file keeps it, so the slot comes back when the plugin is there).

### 5.1 The environment library

(*Decided*, 2026-10-23; persistence revised 2026-10-25.) The user makes **many environments**; they are listed and selectable in the UI, and kept in a **central library** so they are reused between projects:
- The library is a directory, `$XDG_DATA_HOME/colibri-emulator/` (`~/.local/share/colibri-emulator/`), overridden with `--library <dir>` (§3.1), holding `environments/` (the environment files), `bindings/` (bindings files the user keeps beside them, §5.2; the emulator itself does not read them) and `state.json` (which environment ran last).
- The UI lists the library's environments and lets the user **create** one from scratch (the Colibri 8 carrier, no modules, no communication module), **duplicate**, **rename** and **delete** one, and **select** the one the emulator runs. One environment runs at a time; the emulator starts with the one that ran last, or the one named on the command line. With an empty library it creates `default` and runs it. Library operations act on the files at once.
- **Persisting** (*decided*, 2026-10-25, replacing "every change is written back at once"): every change a client makes takes effect at once in the running emulator, and the environment file is written only by the control protocol's **`persist`** request — the whole environment: structure, settings, every source and noise. Until then the environment is **unsaved**, which the UI shows. Selecting another environment with unsaved changes asks the user first. A file is written atomically (a temporary file in the same directory, renamed over it). This is the emulator's: a real device's environment cannot be changed.

Changing the cycle time or the queue size takes effect at once. **Selecting another environment, renaming the running one, or changing the carrier, a module or its serial, or a communication module's type, is a power cycle** of the emulated device (*decided*, 2026-10-23): it changes the environment description, which hakkas accepts only at its startup (`04-drivers.md` §0.1), so the emulator closes the link, and the UI tells the user to restart hakkas (the control protocol's `link` state `restart`). At a power cycle the queues are emptied, outputs return to their defaults, and the inputs keep their sources where their keys still exist. A communication module's parameters are view-only values that hakkas does not compare (`04-drivers.md` §0.1): changing them is no power cycle, and hakkas sees them at its next start.

### 5.2 Bindings are made in hakkas' UI

(*Decided*, 2026-10-23.) Bindings belong to a device, not to the emulator: hakkas' frontend gets the environment's ports from the device it is connected to — a Colibri device, or hakkas on the desktop running against the emulator — and offers a page for binding the processes' ports to them. On that page the user **selects a bindings file from the local file system**, typically in the library's `bindings/`; the user names the files and so controls which bindings go with which environment and device. When a process is deployed to a device, the user selects its bindings file then, and a mapping dialog resolves the entries that do not match (§5.3). That is hakkas' side (`specifications/04-drivers.md` §4, `06-frontend.md` §4.14, §7).

### 5.3 Copying an environment from a device

(*Decided*, 2026-10-24; how, 2026-10-25.) The emulator UI can **connect to a real Colibri device**, download its environment description (`specifications/04-drivers.md` §1: the carrier board, the modules per slot, the ports) over hakkas' wire protocol, and **save it as an emulator environment under a name the user gives**. The user can then select it (§5.1) and develop against the device's twin.

**The emulator's server does the copying, not the browser** (*decided*, 2026-10-25), so there is no cross-origin access:
1. The UI sends the control protocol's `copy` request (a `features` entry, `01-control-protocol.md` §4.5): `{"copy": {"address": "http://<host>[:<port>]/", "user": "…", "password": "…", "name": "<new environment>"}}`. `address` is the device's web address, as the user opens it in a browser.
2. The emulator connects to the device's WebSocket, `ws://<host>:<port + 1>/`, sending the device's own origin `http://<host>:<port>` as `Origin`, as a non-browser client may (`02-wire-protocol.md` §0).
3. It logs in with `auth:challenge` and `auth:login` (`02-wire-protocol.md` §2.3) and checks the `server_signature`; the password is used for that only — never stored, logged, or sent to any client.
4. It asks `environment:describe` (§3.7), then `auth:logout`, and closes the connection.
5. It builds the environment: the carrier definition of the device's carrier type (an unknown type gives a carrier definition made from the description — its type, name, slot count, built-in slots, power and communication slots — stored in the environment file, with a warning); the communication modules with their parameters as described; per changeable slot, the plugin of the device's module type, with its serial (an unknown type leaves the slot empty, with a warning); inputs at their plugins' defaults. The file is written at once (it is a new environment, not a change to the running one); it is not selected.

The answer is `ok` with `{"name", "warnings": […]}`, or an error: `Cannot connect: <address>`, `Login failed`, `No environment` (the device has none), `Not a Colibri environment` (no carrier in its description), `Environment exists: <name>`, `Timeout` (the whole copy has 15 s). It is tested against hakkas on the desktop running against a second emulator.

**Comparing** happens where it matters: when a process is deployed to a device, hakkas' frontend matches the chosen bindings file against the device's environment and lets the user map what does not match (`specifications/06-frontend.md` §7). Names are never enforced; mismatches are warnings (`04-drivers.md` §4).

### 5.4 Carrier definitions and communication modules

(*Decided*, 2026-10-24: the carrier board is described like the modules, `04-drivers.md` §1.) A carrier board type is defined by data, like a module plugin (§7), in a directory with `carrier.json` and the board's picture (an SVG or a photo; each slot's place an element with the id `slot-<n>`, `01-control-protocol.md` §3.4). The Colibri 8 (*decided*, 2026-10-25):

```json
{"type": "colibri_8", "name": "Colibri 8", "vendor": "CurrentMakers",
 "documentation": "https://currentmakers.com/products/colibri/colibri_8/",
 "slots": 7,
 "builtin": [{"slot": 0, "type": "carrier"}],
 "power": {"voltage": {"min": 7, "max": 30}, "ac": true, "dc": true, "current": 2.0},
 "comm": [{"slot": 1, "types": ["colibri_ethernet", "colibri_wifi", "colibri_rs485", "colibri_lorawan"]}],
 "picture": "board.svg"}
```

- **Type and name**: machine-readable and human-readable (§2.1).
- **Number of I/O slots**, the changeable ones (`slots`; hakkas' `HakkasEnvCarrier.slots`, `04-drivers.md` §1); the built-in ones come first: Colibri 8 has slot 0 built in and slots 1–7.
- **Built-in slots**: a list, each entry with its slot number and the module type it always holds (a module plugin, §7), which the user cannot change or remove (*decided*, 2026-10-24). Whether a carrier will ever have more than one is not known; the list allows it.
- **Power specification**: supply voltage range, AC and/or DC, supply current (for the Colibri 8: 7–30 V, AC or DC, 2 A recommended) — described, not emulated.
- **Communication slots**: the module types each accepts — the Colibri 8 has one slot, taking Ethernet, WiFi, RS-485 (Modbus/RTU) or LoRaWAN. The environment chooses one per slot with its **parameters, which differ per type** and are **view only** (*decided*, 2026-10-24): Ethernet and WiFi DHCP or a static address (with WiFi's SSID), RS-485 bitrate and parity, LoRaWAN (OTAA only) App EUI, Dev EUI and whether an App Key is set (never the key) — the sets of `04-drivers.md` §1. Communication modules are not emulated (§1); they are described to hakkas and copied from a device, and the emulator's UI lets the user enter them for an environment made from scratch. No secret (a WiFi passphrase) is kept in an environment or described.

A **communication module type** is data too, a directory with `comm.json` (*decided*, 2026-10-25), so new types are added by adding a file:

```json
{"type": "colibri_wifi", "name": "Colibri WiFi", "vendor": "CurrentMakers", "params": ["ip", "wifi"]}
```

`params` names the parameter groups of `04-drivers.md` §1 the type has: `colibri_ethernet` `["ip"]`, `colibri_wifi` `["ip", "wifi"]`, `colibri_rs485` (Modbus/RTU) `["rs485"]`, `colibri_lorawan` `["lorawan"]`.

## 6. The environment description

The emulator describes itself to hakkas per `04-drivers.md` §1:

- `name`: the environment's name (§5); `description`: `"Colibri emulator: <carrier name>"`; `icon`: `"chip"`.
- **Ports are named per slot**: `slot<n>-<channel group>`, e.g. `slot1-ai` (AIV in slot 1), `slot3-dq` (SSR in slot 3). A group of several identical channels is one addressable port with `count`; a group of one is a plain port (DP1 in slot 4: `slot4-ai`, bound without an index). A module with several groups has several ports (DII in slot 2: `slot2-di`, `slot2-freq`, `slot2-duty` and `slot2-count`). A port's `type`, `unit`, `min`/`max` and `description` come from the module's plugin (§7.1); the field-side signals (§7.2) are not ports. Ports are listed by slot, then in the plugin's channel order; `outPorts` are the module inputs (sensors), `inPorts` the module outputs (actuators).
- **Modules**: one entry per filled slot, with `slot`, `type`, `name`, `vendor`, `revision`, `serial`, `documentation` from the plugin and the environment, and the ids of its ports; built-in modules marked `builtin` (slot 0's `carrier`: no name, no ports).
- **Carrier**: the carrier board of `04-drivers.md` §1 — type, name and metadata, number of I/O slots, the built-in slots, power specification, communication slots with their modules' types, names and parameters — from the carrier definition and the environment (§5.4).

The human-readable `name` members of the carrier, the modules and the communication slots are an addition to hakkas' description (*decided*, 2026-10-25; hakkas plan task 68, `04-drivers.md` §1).

The emulator gives the same ports, port ids and module list that the firmware gives for the same modules in the same slots, so that one bindings file works on both. The naming of `04-drivers.md` §1 (decided 2026-10-23) is what both follow; the channels themselves are provisional until the firmware's are known (§10 question 2).

## 7. Module plugins

Every I/O module type is a **plugin**, also the built-in ones, so that others can make modules for Colibri and supply their emulation with them. **A plugin is data, not code** (*decided*, 2026-10-23): the emulation of a module is nearly nothing — channels with their ranges, a value source per input, noise, indicators and controls — so it is described in its own file, the module's **descriptor**, rather than programmed, and a module is added or changed by editing that file (*decided*, 2026-10-25):

```
modules/colibri_pt1000/
  module.json      the descriptor (§7.1)
  faceplate.svg    optional: the module's picture (§7.4)
```

**Plugin directories** (*decided*, 2026-10-25): the emulator's built-in data directory (`emulators/colibri/data/`) and each `--plugins <dir>` (§3.1), in that order. Each may hold `modules/`, `carriers/` (§5.4) and `comm/` (§5.4), one subdirectory per type. A type defined again in a later directory replaces the earlier definition, with a warning, so a user can override a built-in one. A descriptor that does not parse or validate is skipped, with an error naming the file and the problem; the emulator still starts. When slot 0 gets channels to emulate, they come from the `carrier` plugin.

### 7.1 The descriptor

```json
{"type": "colibri_pt1000", "name": "Colibri Pt1000", "vendor": "CurrentMakers", "revision": "1",
 "description": "2 × Pt1000 temperature inputs",
 "documentation": "https://currentmakers.com/products/colibri/colibri-pt1000/",
 "channels": [
   {"id": "ai", "direction": "input", "type": "number", "count": 2,
    "unit": "°C", "min": -50, "max": 150, "resolution": 0.01,
    "default": 21, "noise": 0.05, "control": "slider", "description": "Pt1000 temperature"},
   {"id": "fault", "direction": "input", "type": "bool", "count": 2,
    "control": "toggle", "description": "Sensor open or shorted"}
 ],
 "faceplate": "faceplate.svg",
 "ui": {"bindings": […], "controls": […]}}
```

- `type`, `name` (§2.1), `vendor`, `revision`, `description`, `documentation`: the module's metadata, given to hakkas in the description's module list (§6). `type` is required and is the directory's name (1–64 lower-case letters, digits and `_`); `name` is there for every module that has one (not slot 0's `carrier`). Each string is at most 1024 bytes, hakkas' limit.
- `signals` (optional): the module's field-side signals that are not channels (§7.2), each with `id`, `type` (`pulse`), `count`, `default` (a source; without one, `{"pulse": {"frequency": 0, "duty": 0}}`) and `description`.
- `channels`, each with:
  - `id`, `type` (`number` or `bool`), `description`, `unit`, `min`/`max` — they become the environment description's port (§6); `count` > 1 makes the port addressable (default 1).
  - `direction`: `input` (a module input: a sensor to hakkas, an environment output port) or `output` (a module output: an actuator, an environment input port). A module may have both (DIO1, PID1).
  - **Inputs**: `resolution` (values are rounded to it), `default` (the source at power-up, of a kind the channel accepts, §8.3; without one, false, or 0 brought into the channel's range), `noise` (§7.2, default 0), `control` (the UI's default control: `slider` or `number` for a number — a slider needs `min` and `max`; without a control, a slider when the channel has both, else a number field — and `toggle` or `button`, momentary, for a bool, `toggle` by default), or `measure` instead of a source (below).
  - **Outputs**: `default` (the value until hakkas writes one: 0 or false by default) and `indicator` — how the UI shows the value hakkas writes: `lamp` (bool), `number`, `bar` or `meter` (number, between `min` and `max`, which they need). Without one: `lamp` for a bool, `number` for a number. An output has no `resolution`, `noise`, `control` or `measure`.
  - A bool channel has no `min`, `max`, `resolution` or `noise`.
  - **Measured inputs**: `{"measure": {"signal": "in", "quantity": "state" | "frequency" | "duty" | "count"}}` — the channel is measured from the signal with the same index (§7.2), so it has the signal's `count`; `state` is a bool, the others numbers. It has no `default` or `control`.
- A module with **no channels** (RS485I, RS485U, `carrier`) is only placed with its picture: no controls, no indicators, no ports.
- Ids are 1–32 characters of lower-case letters, digits and `_`, unique among the module's channels and signals, so the port ids (§6) and keys (§8) are well formed.
- `faceplate` is a picture file in the descriptor's directory (`.svg`, `.png`, `.jpg`, `.webp`), a plain file name; a picture that is not there is left out, with a warning. The `ui` section's keys must name the module's channels and signals: a binding's key a channel or signal (with its index when its group has several), its `source` (an input with a source, or a signal) or its `noise` (a number input); a control's key a `source`.
- A member the descriptor format does not know is warned about and ignored (most likely a typing error).

**Loading** (implemented in plan task E2, `src/catalog.ts`): the plugin directories are read in order, each type directory's descriptor checked as above; a descriptor that cannot be read or is invalid is skipped with an error per problem, naming the file and the member (`modules/colibri_x/module.json: channels[1].min: greater than max`). Then what the carriers name is checked: a carrier whose built-in module type is not in the catalog is skipped (its slot could not be filled); a communication module type that is not in the catalog is left out of its slot's list, with a warning. A carrier's `carrier.json` is checked like hakkas checks a carrier (04 §0: 1–31 I/O slots with the built-in ones, built-in slot numbers among them and none twice, at most 4 communication slots numbered 1–4); its `picture` like a faceplate. The info line `catalog: module types <n>, carrier types <m>, communication module types <k>` follows.

### 7.2 Inputs: sources, signals and noise

At each turn of its slot (§4), every input channel produces a value, which goes to hakkas if it changed:

> value = round(clamp(source(t) + noise, min, max), resolution)

- **The source** is what the user or an external program set (§8): a **constant** (a slider, a number, a toggle, a button) or a **function** of the emulator's time — `sine`, `square`, `ramp`, `counter` (an increasing integer at a rate per second that **rolls over** past the channel's `max` to its `min`), `sequence` — as the control protocol defines them (`01-control-protocol.md` §5).
- **Noise** (numbers only): a uniformly distributed random value in ±`noise` (e.g. ±0.05 °C on a Pt1000), added anew at every turn. Its default comes from the descriptor; it is set per channel at runtime, 0 turns it off.
- **Signals** (*decided*, 2026-10-25): a field-side signal has a source but is no channel; the module's measured channels are derived from it. The one kind so far is the **pulse train** of a DII/DIU input, `{"pulse": {"frequency", "duty"}}` (each a number or a function of time), from which, at each turn:
  - `count` is the number of pulses (rising edges) since power-up — the frequency integrated over time, so it counts at its rate however fast the pulses are — modulo 2³²;
  - `di` (state) is the level at the turn: high in the first `duty` % of each period;
  - `freq` is the frequency, `duty` the duty cycle, rounded and with their noise (0 by default);
  - at frequency 0 the input is a steady level, high if `duty` ≥ 50: `freq` is 0, `duty` 100 or 0, and `count` does not change.

So a noisy Pt1000 channel sends a new value every 80 ms on Colibri 8. Hakkas' change threshold (`01-architecture-overview.md` §3.3, engine-wide) filters it like real sensor noise; a `process/Deadband` filters a channel differently.

The source and noise of every input and signal are part of the emulator's state (§8) and are kept in the environment's file when it is persisted (§5.1).

### 7.3 Outputs

An output channel shows the value that arrives on it from hakkas, at its slot's turn (§4.1), on its indicator. Nothing else happens to it.

### 7.4 Graphics assets

A plugin's faceplate is an optional **SVG** whose elements carry ids; the descriptor's `ui` section binds those elements to channel values (an LED's colour to a relay channel, a label's text to a voltage) and may make elements operable (a slider on an analog input's terminal), in the form of `01-control-protocol.md` §6, with keys **relative to the module** (`dq.1`, `ai.1.source`): the emulator prefixes the slot (`slot3.dq.1`), so a descriptor is written once, independent of the slot. The emulator does not draw: it hands the assets and bindings to the generic UI (§8). Every channel is usable without a faceplate, from the signal panel.

**Pictures** (*decided*, 2026-10-25): the signal panel comes first; then simple schematic SVGs, drawn for this project, of the Colibri 8 board and each module. Photos or vendor drawings may replace them later, as data, without code changes.

## 8. The UI and the control channel

### 8.1 Principle

The emulator's state — the inputs' sources and noise, the channel values, the output values, the settings, the counters of §4.1 — is available on a **control channel**, with one protocol for every client: the browser UI and **external programs** (a Python script, a demo, an automation). All clients are equal: each may set inputs and follows the whole state. The protocol is shared by all emulators: `../../specifications/01-control-protocol.md` (*decided*, 2026-10-25).

It follows hakkas' principle (`../../../specifications/00-README.md`): **the emulator owns the state, clients only mirror it**. A client sends a request; the emulator applies it and broadcasts the resulting state to every connected client, the sender included, which updates its view only from that. When two clients set the same input, the last one wins and every client sees it.

The control channel is **separate from hakkas' environment link** (*decided*, 2026-10-23): hakkas exchanges channel values on the MCU's side of the modules, while clients work on the **field side** — a voltage on a terminal, a contact closed — and what they set reaches hakkas only through the module's behaviour and the scan cycle (§4), as on the device. (The link protocol could not tell the two kinds of client apart anyway: the emulator sends `describe` first on every connection, `04-drivers.md` §0.1.)

**Application testing is not part of the emulator** (*decided*, 2026-10-23): plant models, scenarios and the like — testing a Colibri application against a simulated real world, e.g. a two-stage furnace regulator whose setpoint comes from a sensor's pulse train — are written outside it, in Python or whatever is at hand, as clients of the control channel. The emulator's job is to make that easy: a simple, documented protocol, on a transport a script reaches without a library. The project includes an example script.

### 8.2 The UI

(*Decided*, 2026-10-25.) The generic UI (`../../specifications/00-README.md`) knows nothing about Colibri; it renders what the emulator describes:
- **The carrier board**: the carrier definition's picture (§5.4), with the place of each slot marked (an element with the id `slot-<n>`).
- **The modules**: each module's faceplate (§7.4) drawn in its slot's place on the carrier, with its LEDs and terminal values bound to the state and, where the plugin has them, controls on it.
- **The signal panel**, beside the drawing: one row per input, signal, measured channel and output, with its control or source editor and noise, or its indicator, the channel's name (`slot1-ai[1]`), unit and value. The UI builds it from the rows alone, so every module is usable, also one without a faceplate.
- **The environment library** (§5.1): the environments, which one runs, unsaved changes and Persist; the settings; the counters (§4.1); the link to hakkas, with the restart notice; copying from a device (§5.3).

The Colibri emulator serves the UI's built files and the assets over HTTP (§3.1).

### 8.3 What the Colibri emulator puts on the control channel

(*Decided*, 2026-10-25.) The protocol's generic messages, filled in as follows:
- **`hello`**: `emulator` `"colibri"`, `features` `["copy"]`.
- **Parts**: one per I/O slot, filled or not, with the id `slot<n>` and the place `slot-<n>`.
- **Keys**: `slot<n>.<channel>[.<index>]` for a channel (the index only for a group of several), `slot<n>.<signal>.<index>` for a signal, each with `.source` and `.noise` where they apply: `slot1.ai.1`, `slot1.ai.1.source`, `slot4.ai.noise` (DP1), `slot2.in.1.source` (a DII terminal's pulse train), `slot2.count.1`. A row's label is the hakkas port and channel, `slot1-ai[1]` (`slot4-ai` without an index).
- **Rows**: an input channel with a source is `input`; a signal `signal`; a measured channel `measured`; an output channel `output`. Number inputs accept the sources `constant`, `sine`, `square`, `ramp`, `sequence`, and also `counter` when the channel's resolution is 1; bool inputs `constant`, `square` and `sequence`; signals `pulse`.
- **Settings fields**: `carrier` (enum of the carrier definitions, restart); `cycleMs` (integer 1–1000, ms); `queueSize` (integer 1–1000); per I/O slot `slot<n>.type` (enum: empty, or a module type; read-only for a built-in slot; restart) and `slot<n>.serial` (text; restart); per communication slot `comm<n>.type` (enum: empty, or one of the slot's types; restart) and its type's parameters: `comm<n>.ip.mode` (`dhcp`/`static`), `.address`, `.prefix`, `.gateway`, `.dns` (list), `.mac`; `comm<n>.wifi.ssid`; `comm<n>.rs485.bitrate` (1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200), `.parity` (`none`/`even`/`odd`); `comm<n>.lorawan.appEui`, `.devEui` (16 hex digits), `.appKeySet` (bool).
- **Library**: a new environment has the Colibri 8 carrier, no modules and no communication module.
- **`copy`**: §5.3.
- **The control socket** is `--control-socket` and the WebSocket `ws://localhost:<--http>/control` (§3.1).

## 9. Later

- **Simulated time** through the link's `clock` message: setting the wall clock (e.g. to test a schedule on Monday at 06:59). Running faster than real time would also need hakkas to run on simulated time (its timers, PID and Interval use its monotonic clock) — a change to hakkas.
- **Fault injection**: a broken wire, a dead sensor, an out-of-range value — once it is decided what the firmware reports for them (`04-drivers.md` §0 rule 2: every channel reports a value even when its sensor is dead); DIO1's separate fault flags (§2.1).
- **"Deploy on desktop"** (*decided* as a later step, 2026-10-25): launching hakkas and the emulator as a pair.
- The MCU and Comm modules; the RS-485 modules' channels; the Colibri-16pi carrier (15 + 1 slots) once its values are known.

## 10. Open questions

1. *Decided* (2026-10-23): the built-in I/O slot is slot 0, with nothing to emulate for now (§2). Its module's type is `carrier`, with no name (2026-10-25).
2. **The channels each module offers** (§2.1; names decided 2026-10-23): the units, ranges and resolutions are guesses from the hardware; they must match what the firmware's drivers offer. *Decided* (2026-10-25): build with the guesses, marked provisional, and adjust them later.
3. *Decided* (2026-10-23, completed 2026-10-25): DII/DIU deliver per input its state, frequency, duty cycle (0–100 %) and a 32-bit unsigned counter that rolls over, measured by the on-board STM32C071 every scan turn; the DIU has 4 inputs, the DII 2; in the emulator each input is a pulse-train signal (§2.1, §7.2).
4. *Decided* (2026-10-23, completed 2026-10-25): DIO1's direction is set by the user program through an output channel (`dir`, true = output); its faults are one accumulated `fault` flag per channel for now, each kind of fault a separate flag later (§2.1).
5. *Decided* (2026-10-23): PID1 is a full PID regulator whose parameters, named as hakkas' `process/PID`'s, are output channels, with a Pt1000 input; the emulator does not regulate (§2.1).
6. *Decided* (2026-10-23): the write queue (§4.1) — one per module, emptied at each turn, 10 values for now, a full queue drops the new value; on the input side a driver forwards a value to its channel only in its slot's turn. The firmware's behaviour is to be reviewed later (the user).
7. *Decided* (2026-10-24): no enforcement by names, warnings instead — hakkas logs a mismatching bindings file's `environment` and applies it (`04-drivers.md` §4); the user picks the bindings file at deployment, with a mapping dialog for mismatches (§5.2, §5.3).
8. *Decided* (2026-10-24): the environment description gains the carrier board — number of slots, built-in slots (unchangeable I/O modules), power specification, communication slots with per-type parameters (§5.4, `04-drivers.md` §1).
9. *Decided* (2026-10-24): the carrier lists its built-in slots, each entry with a slot number (§5.4).
10. *Decided* (2026-10-24): communication parameters are view only, for now: Ethernet/WiFi DHCP or static, RS-485 bitrate and parity, LoRaWAN OTAA App EUI and Dev EUI (§5.4, `04-drivers.md` §1).
11. *Decided* (2026-10-24): the LoRaWAN App Key is a secret; the description shows only whether one is set (`appKeySet`), never the key and no fingerprint (`04-drivers.md` §1).
12. *Decided* (2026-10-25): type names `colibri_<module>`, human-readable names "Colibri <Module>", both from the module's EEPROM and unchanging; the description carries the name (§2.1, §6; hakkas plan task 68).
13. *Decided* (2026-10-25): the Colibri 8 carrier — 7 + 1 I/O slots, 7–30 V AC or DC, 2 A recommended, one communication slot taking Ethernet, WiFi, RS-485 (Modbus/RTU) or LoRaWAN (§5.4).
14. *Decided* (2026-10-25): the environment file is written only by `persist` (§5.1); one hakkas per emulator (§3); the emulator's server copies from a device (§5.3); the control protocol is shared by all emulators (§8).
15. **DIO1's faults and the module's fault flags in general**: which faults the firmware reports, per channel and per module — needed for the separate flags (§9).
