# Emulators — shared specification

> Started 2026-10-25 (Colibri emulator plan task E0). The control protocol is implemented by the
> Colibri emulator (its tasks E9–E11); the generic UI's signal panel, library, settings and
> counters by its tasks E12–E13.

An emulator stands in for a device's I/O side on the desktop, so hakkas programs can be tested
without hardware. Each emulator is its own project in `emulators/<name>/`, with its own
specification (the first: `colibri/specifications/00-README.md`). The pieces described here are
shared by all of them:

| File | Contents |
|---|---|
| `00-README.md` | This file |
| `01-control-protocol.md` | The **control protocol**: how a browser UI and external programs (scripts, demos, automation) follow an emulator's state, set its inputs and change its configuration |

**The generic UI** (*decided*, 2026-10-25): one browser UI for every emulator, in its own package
`emulators/ui/`, written with Vue 3 and Vite like hakkas' frontend (Rete.js where it helps). It
knows nothing about any particular device: it draws what the emulator describes over the control
protocol (its `scene` and `settings`, `01-control-protocol.md` §3), and every emulator serves its
built files.

**The UI, as built** (*decided* in the Colibri emulator's task E12, 2026-10-25; `../ui/`):
- **The signal panel** shows each filled part as a card (its id, name, *built in*), one table row
  per scene row: the label and description; the value with the row's unit and its resolution's
  decimals (*on*/*off* for a bool); for an `input` with a constant source its control (a slider
  with a number field when the row has `min` and `max` and its control is not `number`; a
  checkbox for `toggle`; a momentary button for `button`), for a function source a short
  description of it; for an `output` its indicator (a lamp, a bar or meter, a number); the noise
  as a number field for the rows whose state has a `….noise` key. Empty parts are listed in one
  line below. A part without rows says *Nothing to emulate*.
- **The source editor** (the *ƒ* button of an `input` or `signal` row) edits a **draft** — the
  kinds the row's `sources` list, with each kind's parameters, a sequence's steps added and
  removed, a pulse train's frequency and duty cycle each a constant or a function — and sends it
  only on *Apply*; *Cancel* sends nothing. A new kind starts within the row's range.
- **Nothing in the view changes until the emulator says so**: controls show the state. A slider
  being dragged shows its own position and sends at most every 50 ms, then its last position
  when let go. A number field being edited keeps what is typed (the state of a noisy input
  changes at every scan turn) and commits it on Enter or when left; once that request is
  answered it shows the emulator's value again, also after a refusal.
- A refusal is shown as a notice, like the emulator's `notice`s, until dismissed. The header
  shows the emulator, the running environment with an unsaved mark, and the link to hakkas
  (*restart needed* stands out); a lost connection shows a banner and the client reconnects
  every second, starting over from `hello`; another protocol version stops it with a message.
- The build's own files are under `ui/` (not `assets/`, the emulator's pictures); `npm run dev`
  passes `/control` and `/assets` to a running emulator.
- (*Decided* in the Colibri emulator's task E13, 2026-10-25.) **The layout**: the environment
  library in a column on the left; the rest in tabs — *Signals* (the signal panel), *Settings*,
  *Statistics* — the tab chosen kept per browser (`localStorage`, where it can be). The header has
  **Persist**, enabled while the environment is unsaved, beside the environment's name and its
  unsaved mark.
- **The library** lists the environments, the running one marked; each has *Run* (not the running
  one), *Duplicate* (a name offered, `<name>-copy`), *Rename* (the running one's power cycle
  said in the dialog) and *Delete* (not the running one, after a confirmation). *New…* asks for a
  name. A refusal is shown in its dialog. *Run* with unsaved changes asks first: *Persist, then
  run*, *Discard* or *Cancel*.
- **The settings** are the emulator's form as it sends it: the fields without a group first,
  then each group; an `enum` a choice (*— not set —* while it has no value), a number a number
  field, `text` and `list` (comma-separated) a text field, `bool` a checkbox, a `readonly` field
  shown but disabled, a `restart` field marked *restart*. A change is a `configure` of that one
  field, sent when it is made (a choice at once, a typed value on Enter or when the field is left,
  kept while it is being typed); a refusal is a notice and the field shows the emulator's value
  again.
- **A power cycle** (`link` `restart`) shows a banner across the page: restart hakkas so that it
  sees the new environment.
- **The counters**: per filled part its queue's length and highest length, and per channel the
  writes received, queued and dropped (outputs) or the values sent (inputs); a part or channel
  that dropped writes stands out (in the error color, with a *dropped* badge), and the
  *Statistics* tab shows the number dropped in all. *Reset counters* sends `resetstats`.

**Principle**, as in hakkas (`../../specifications/00-README.md`): the emulator owns the state;
clients only mirror it. A client sends a request; the emulator applies or refuses it and
broadcasts the resulting state to every connected client, the sender included, which updates
its view only from that.
