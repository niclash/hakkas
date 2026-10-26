# 01 — The control protocol

> Drafted in emulator plan task E0; implemented by the Colibri emulator in its tasks E9–E11
> (2026-10-25, `../colibri/src/core.ts`, `transports.ts`, `settings.ts`, `scene.ts`).

The control protocol connects an emulator to its **clients**: the generic browser UI
(`00-README.md`) and external programs (a Python script, a demo, a plant model). All clients are
equal: each may set inputs and change the configuration, and each follows the whole state. When
two clients set the same thing, the last one wins and every client sees it.

The protocol is generic. What it carries for one particular emulator (its parts, keys, settings
and messages of its own) is in that emulator's specification (Colibri: `../colibri/specifications/00-README.md` §8).

It is the emulators' own protocol, not hakkas' FBP wire protocol; it has no upstream to stay
compatible with. It is versioned (§3.1) so it can change.

## 1. Transports

The same messages on two transports (*decided*, 2026-10-23); only the framing differs:

- **WebSocket**, for the browser UI (also usable from scripts): `ws://localhost:<port>/control`
  on the emulator's HTTP server, which also serves the UI and the assets (§3.4). Text frames,
  one message per frame. The UI is served from the same origin, so there is no cross-origin
  access. A request with an `Origin` header other than the server's own (`http://localhost:<port>`
  or `http://127.0.0.1:<port>`) is refused with HTTP 403. A request without one, from a script,
  is accepted.
- **Unix domain socket**, newline-delimited JSON, like hakkas' environment link
  (`../../specifications/04-drivers.md` §0.1): one message per line, UTF-8. It can be reached
  from a shell (`socat - UNIX-CONNECT:<path>`) or a few lines of Python.

(*Decided* in the Colibri emulator's task E10, 2026-10-25.) The HTTP server listens on `127.0.0.1`
and, where there is one, `::1`, so `localhost` works whichever it resolves to; `http://[::1]:<port>`
is an allowed `Origin` too. It answers `GET` and `HEAD` only (405 otherwise); a path that names no
file is 404, except `/` without a built UI, which is a short page saying how to build it. The UI's
files and the assets are served with `Cache-Control: no-cache`.

Both listen on the local machine only (localhost, and a socket in the user's runtime directory)
and need no login (*decided*, 2026-10-25): an emulator is a desktop tool. A message is at most
**256 KiB**; a longer one closes the connection (the WebSocket with close code 1009). There is
no limit on the number of clients.

## 2. Messages

A message is a JSON object with **one member**, whose name is the message and whose value (an
object) is its payload: `{"set": {"key": "slot1.ai.1.source", "value": 5}}`.

- **Requests** (client → emulator, §4) may carry an `id` (any JSON number or string) in their
  payload. The emulator answers every request with `ok` or `error` (§3.9), to the sender only,
  repeating the `id` when there is one. The resulting state changes are broadcast (§3) to every
  client before the `ok` is sent, so a client that gets `ok` already has the new state.
- A message that is not a JSON object with one member, or a request with an unknown name, is
  answered with `error` (`"Invalid message"`, `"Unknown request: <name>"`). A member the
  receiver does not know inside a known payload is ignored (forward compatibility).
- (*Decided*, 2026-10-25.) A request whose payload is not an object is `"Invalid message"` too,
  with its `request` named; a payload member that a request needs and that is missing or of the
  wrong type is `"Invalid field: <member>"` (e.g. `set` without `key` or `values`). Any request
  that needs a running environment, when none runs, is `"No environment"`.
- Requests are applied one at a time, in the order they arrive (per client, and across clients
  in arrival order).

## 3. Emulator → client

On connect the emulator sends, in this order: `hello`, `library`, `settings`, `scene`, `state`
(every key), `stats` (every counter) and `link`. After that, each message whenever what it
describes changes.

### 3.1 `hello`

```json
{"hello": {"protocol": 1, "emulator": "colibri", "version": "0.1.0", "features": ["copy"]}}
```

`protocol` is this document's version (1). `emulator` names the emulator, and `features` lists
the optional requests it supports (e.g. `copy`, Colibri's copy from a device). A client that does
not know the protocol version should not go on.

### 3.2 `library`

The emulator's environments (an **environment** is one emulated system, kept in a file):

```json
{"library": {"environments": [{"name": "lab"}, {"name": "pump-room"}],
             "running": "lab", "unsaved": true}}
```

`running` is the environment that runs, or `null` when there is none (the library is empty, or
the environment to run cannot be run: then `settings` has no fields, `scene` no board, parts or
rows, and `state` and `stats` are empty, until one is selected).
`unsaved` is true when the running environment has changes that `persist` (§4.3) has not
written yet.

### 3.3 `settings`

The running environment's configuration, as a form that the UI renders without knowing the
emulator: a list of **fields** and their values.

```json
{"settings": {
  "fields": [
    {"id": "carrier", "label": "Carrier board", "type": "enum", "restart": true,
     "options": [{"value": "colibri_8", "label": "Colibri 8"}]},
    {"id": "cycleMs", "label": "Cycle time", "type": "integer", "unit": "ms", "min": 1, "max": 1000},
    {"id": "slot0.type", "label": "Slot 0", "group": "Slots", "type": "enum", "readonly": true,
     "options": [{"value": "carrier", "label": "carrier"}]}],
  "values": {"carrier": "colibri_8", "cycleMs": 10, "slot0.type": "carrier"}}}
```

A field has an `id` (a key of `values`), a `label`, and a `type`:
- `enum` (one of `options`, each a `value` and a `label`);
- `number` or `integer`, with optional `min`, `max` and `unit`;
- `text`;
- `bool`;
- `list` (a list of strings).

Optional members: `group` (a heading the UI puts the field under; fields of a group are listed
together, in order), `description`, `readonly` (shown, not changeable), and `restart`. A
`restart` field is one whose change is a **power cycle** of the emulated device: the
description the runtime got at its start no longer holds, so the emulator closes the runtime
link and `link` says so (§3.7). The emulator sends the whole `settings` again whenever a field
or a value changes. Fields can appear and go: choosing another module for a slot brings that
module's fields.

A value is `null` for a field that is optional and not set (*decided*, 2026-10-25: e.g. a
communication module's static address under DHCP); `configure` sets it to `null`, or a `text` to
`""` or a `list` to `[]`, to clear it. An `enum`'s `options` may have values of any JSON type
(numbers for a bitrate).

### 3.4 `scene`

What the UI draws: the board, the parts on it, and one **row** per channel.

```json
{"scene": {
  "board": {"asset": "assets/carriers/colibri_8/board.svg"},
  "parts": [{"id": "slot1", "place": "slot-1", "type": "colibri_aiv", "name": "Colibri AIV",
             "asset": "assets/modules/colibri_aiv/faceplate.svg",
             "bindings": [{"element": "volt1", "text": "slot1.ai.1", "format": "%.2f V"}],
             "controls": [{"element": "term1", "control": "slider", "key": "slot1.ai.1.source",
                           "min": 0, "max": 10, "step": 0.01}]}],
  "rows": [{"key": "slot1.ai.1", "part": "slot1", "label": "slot1-ai[1]", "kind": "input",
            "type": "number", "unit": "V", "min": 0, "max": 10, "resolution": 0.001,
            "control": "slider", "sources": ["constant", "sine", "square", "ramp", "sequence"],
            "description": "0-10 V input"}]}}
```

- **`board`**: a picture (SVG or a photo) in which each part's place is an element with an id,
  or `null`. Without a board, the UI lays the parts out in a row. A board drawn from pictures
  (added 2026-10-26, Colibri plan task E14) gives the places as rectangles instead — each part's
  `rect` — and may have `extras`, `[{"name", "rect", "asset"?}]`: further pictures on it that are
  not parts (a communication module), each in its rectangle. Rectangles and points are fractions
  (0–1) of the board picture's width and height, from its top left corner.
- **`parts`**: each has an `id` (the first component of its keys, §3.5), its `place` (an element
  id in the board's picture), `type` and `name` (both `null` for an empty place), `builtin: true`
  for one the user cannot change, and optionally its picture `asset` with `bindings` and
  `controls` (§6). A part without a picture is drawn as a labelled box. On a board drawn from
  pictures a part has its `rect`, and its `asset` is a picture drawn into it (no bindings).
- **`rows`**: the signal panel (*decided*, 2026-10-23: every channel is usable without a picture).
  A row's `kind` is one of:
  - `input`: a field-side input whose value is a channel (an analog input). It has a source and
    noise;
  - `signal`: a field-side signal with a source but no channel value of its own; it drives
    `measured` rows (a pulse train on a digital input terminal);
  - `measured`: an input channel derived from a signal (the terminal's frequency, its count). It
    has noise but no source;
  - `output`: a channel the runtime writes.

  A row also has its `type` (`number`, `bool` or `pulse`), and optionally `unit`, `min`, `max`,
  `resolution`, `description`, `control` (an input's or signal's default control: `slider`,
  `number`, `toggle` or `button`, momentary) or `indicator` (an output's: `lamp`, `number`,
  `bar` or `meter`), `sources` (the source kinds the row accepts, §5), and `at` `{"x", "y"}`:
  where the channel is on a board drawn from pictures, so the UI can show and operate it there.

**Assets** are paths relative to the HTTP server's root, so a browser fetches them from the same
origin. A Unix socket client gets the same paths and can fetch them over HTTP.

The emulator sends `scene` again after every change of structure (a power cycle).

### 3.5 `state`

The values, by **key**. A key names a part, a channel, the channel's index when its group has
several (`1…count`; no index when it has one), and optionally an attribute:

| Key | Value |
|---|---|
| `<part>.<channel>[.<index>]` | the channel's value: for an input, signal-derived or not, what the runtime gets; for an output, what the runtime wrote and the part applied. `null` before there is one. Not set for a `signal` row. |
| `….source` | an `input`'s or `signal`'s source (§5) |
| `….noise` | an `input`'s or `measured` row's noise (§5) |

```json
{"state": {"values": {"slot1.ai.1": 5.012, "slot1.ai.1.source": 5, "slot1.ai.1.noise": 0.02}}}
```

On connect, `state` holds every key. After that it holds the keys that changed, each with its
latest value. It is sent **at most every 50 ms** (a channel can change at every scan turn, and a
slow client must not build up a backlog). So a value that lasts less than that may never be seen
by a client; the stats (§3.6) count every one. (*Decided*, 2026-10-25.) The changes a request
makes are sent at once, before its `ok`, with whatever else has changed; a power cycle sends
`state` with every key.

### 3.6 `stats`

Counters, so the user sees where a program outruns the emulated device:

```json
{"stats": {"parts": {"slot3": {"queue": 0, "queueMax": 4, "dropped": 0}},
           "channels": {"slot3.dq.1": {"received": 12, "queued": 12, "dropped": 0},
                        "slot1.ai.1": {"sent": 840}}}}
```

- per part: `queue`, the write queue's current length; `queueMax`, its highest; `dropped`, the
  writes it dropped because it was full;
- per output channel: the writes `received` from the runtime, `queued` and `dropped`;
- per input channel: the values `sent` to the runtime.

The meaning of the queue is the emulator's (Colibri: §4.1 of its specification). `stats` holds
only what changed and is sent at most once a second. `resetstats` (§4.6) sets every counter to 0.

### 3.7 `link`

The connection to the runtime (hakkas):

```json
{"link": {"state": "connected"}}
```

`state` is one of:
- `waiting`: no runtime connected;
- `connected`;
- `restart`: the configuration changed since the runtime started (a power cycle, §3.3), so the
  runtime must be restarted to see it. A connection that stays open for **2 seconds** clears
  it. hakkas closes a reconnection whose description changed at once (`04-drivers.md` §0.1),
  so only a restarted hakkas stays.

### 3.8 `notice`

`{"notice": {"level": "warning", "message": "…"}}`, with `level` `info`, `warning` or `error`:
something every client should show, e.g. a second runtime connection refused, or a failed
`persist`. Notices are not kept: a client that connects later does not get earlier ones.

### 3.9 `ok` and `error`

The answer to a request, to the sender only: `{"ok": {"id": 7}}`, with the request's result
where it has one, or `{"error": {"id": 7, "request": "set", "message": "Unknown key: slot9.ai.1"}}`.
A refused request changes nothing.

## 4. Client → emulator

### 4.1 `set` — sources and noise

```json
{"set": {"key": "slot1.ai.1.source", "value": {"sine": {"offset": 5, "amplitude": 2, "period": 60}}}}
{"set": {"values": {"slot1.ai.1.source": 4, "slot1.ai.2.noise": 0}}}
```

Sets one key, or several at once (`values`, applied all or none). Only `source` and `noise`
keys can be set: a channel's value is the emulator's, and an output's is the runtime's.
Refusals: `Unknown key: <key>` (also a `noise` key of a row that takes no noise),
`Not settable: <key>` (a channel's value, or an output's `source` or `noise`),
`Invalid source: <key>: <problem>` (e.g. `Invalid source: slot1.ai.1.source: sine.period:
expected a number > 0`), `Source not accepted: <key>` (a kind the row's `sources` does not list),
`Invalid noise: <key>` (not a number ≥ 0). The change takes effect at once, and its effect reaches the runtime at the part's next
scan turn. It makes the environment unsaved.

### 4.2 `configure` — settings

`{"configure": {"values": {"cycleMs": 20, "slot4.type": "colibri_ssr"}}}`: changes settings
(§3.3), all or none, then broadcasts `settings`, and after a power cycle also `scene`, `state`
and `link`. Several `restart` fields in one request are one power cycle. Refusals:
`Unknown setting: <id>`, `Read-only: <id>`, `Invalid value: <id>`. It makes the environment
unsaved.

(*Decided*, 2026-10-25.) The fields that bring others — the emulator's (Colibri: `carrier` and
the `….type` fields) — are applied first, so a request can set a field it brings (a slot's type
and its serial number together); the others in their order. A value equal to the current one
changes nothing (also of a `readonly` field) and does not make the environment unsaved. A power
cycle to a configuration that cannot be run is refused, `Cannot be run: <name>: <problem>`.

### 4.3 `persist`

`{"persist": {}}` writes the running environment to its file, all of it: its structure, settings,
and every source and noise (*decided*, 2026-10-25). Until then, no change is written: everything
a client changes takes effect at once in the running emulator, and `library`'s `unsaved` says
that the file differs. Refusal: `No environment`, or the write's failure (also a `notice`,
`Persist failed: <the failure>`).

### 4.4 The library

Each acts on the files at once:
- `{"create": {"name": "lab2"}}`: a new environment with the emulator's defaults. With
  `"from": "lab"`, a copy of that environment's file (its saved state, not unsaved changes).
- `{"rename": {"name": "lab", "to": "lab-old"}}`. Renaming the running environment changes the
  name the runtime knows it by, so it is a power cycle.
- `{"delete": {"name": "lab2"}}`. The running environment cannot be deleted.
- `{"select": {"name": "pump-room"}}`: runs that environment instead, a power cycle. With unsaved
  changes it is refused (`Unsaved changes`) unless the request has `"discard": true`, so the UI
  asks the user first. One that cannot be run is refused, `Cannot be run: <name>: <problem>`,
  and the running one goes on. Selecting the running environment runs it again from its file.

A name is 1–64 characters of letters, digits, `.`, `_` and `-`, not starting with `.` (it is a
file name and the runtime's environment name). Refusals: `Invalid name`,
`Environment exists: <name>`, `Unknown environment: <name>`, `Environment running: <name>`,
`Unsaved changes`, `Cannot be run: <name>: <problem>`.

### 4.5 Emulator-specific requests

An emulator may add requests and lists them in `hello`'s `features`: Colibri's `copy`
(`../colibri/specifications/00-README.md` §5.3).

### 4.6 `resetstats`

`{"resetstats": {}}` sets every counter of §3.6 to 0 and broadcasts them.

## 5. Sources and noise

An input's or signal's **source** is a constant or a function of time:

| Source | Form | Rows |
|---|---|---|
| constant | a number, or a bool for a bool row | number, bool |
| `sine` | `{"sine": {"offset", "amplitude", "period"}}` | number |
| `square` | `{"square": {"low", "high", "period", "duty"}}`, `duty` 0–100 % | number, bool (`low`/`high` default to `false`/`true`) |
| `ramp` | `{"ramp": {"from", "to", "period", "repeat"}}`, a sawtooth when `repeat` is true, else it holds `to` | number |
| `counter` | `{"counter": {"rate"}}`, an integer counting up from the row's `min` (0 without one) at `rate` per second, rolling over past its `max` to its `min` | number |
| `sequence` | `{"sequence": {"steps": [[<seconds>, <value>], …], "repeat"}}`, each step's value held for its seconds; after the last, it starts over or holds the last value | number, bool |
| `pulse` | `{"pulse": {"frequency", "duty"}}`, a pulse train in Hz, `duty` 0–100 %; each is a number or a number source above (e.g. a `ramp` of the frequency); a function's frequency below 0 counts as 0, its duty cycle is kept within 0–100 | pulse |

Periods and steps are in seconds. A function's time `t` starts at 0 when the source is set, or
when the emulator starts with a source from the environment's file. The emulator evaluates it at
each turn of its part (the scan cycle), so a function faster than the cycle shows only the
samples the turns hit (`counter` and `pulse` count at their rate regardless).

**Noise** applies to number rows only: a uniformly distributed random value in ±`noise`, added
anew at each turn; 0 turns it off. A row's value is

> value = round(clamp(source(t) + noise, min, max), resolution)

kept within `min` and `max` afterwards too (a bound that is not a multiple of the resolution),
except a `counter`, which rolls over instead of clamping and has no noise. Which rows take noise
is the emulator's (Colibri: number inputs, but not a pulse count).

## 6. Pictures: bindings and controls

A part's or the board's picture is an SVG whose elements carry ids. A **binding** ties an
element to a state key:
- `{"element": "led1", "attr": "fill", "key": "slot3.dq.1", "map": {"true": "#e33", "false": "#422"}}`:
  an attribute, from the value through `map` (or the value itself without one);
- `{"element": "volt1", "text": "slot1.ai.1", "format": "%.2f V"}`: the element's text, with a
  `printf`-like format (`%d`, `%.<n>f`, `%s`);
- `{"element": "fault2", "visible": "slot1.fault.2"}`: shown while the value is true.

A **control** makes an element operable; it sets a `source` key with `set`:
- `{"element": "term1", "control": "slider", "key": "slot1.ai.1.source", "min": 0, "max": 10, "step": 0.01}`;
- `"toggle"`, `"button"` (momentary: true while pressed) and `"number"` (typed entry).

The keys in a `scene` are complete. An emulator's own data files may write them relative to the
part (Colibri's manifests do, its spec §7.4), and the emulator completes them.
