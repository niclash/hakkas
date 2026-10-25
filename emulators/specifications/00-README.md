# Emulators — shared specification

> **Draft** (started 2026-10-25, emulator plan task E0). Nothing is implemented yet.

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

**Principle**, as in hakkas (`../../specifications/00-README.md`): the emulator owns the state;
clients only mirror it. A client sends a request; the emulator applies or refuses it and
broadcasts the resulting state to every connected client, the sender included, which updates
its view only from that.
