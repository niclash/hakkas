# Example: a heating circuit

A radiator circuit with a shunt valve and a circulation pump, for a Colibri 8 (hakkas plan X3).
It is the User's Manual's second example (Chapter 12, `docs/manual/intermediate/`), between the
thermostat and the air handling unit, and the one that introduces a composite block type. It runs
as it is against the Colibri emulator.

| File | What |
|---|---|
| `Heating Circuit.process.json` | the process with its bindings and the composite type `heating/ShuntGroup`: upload it with *Upload…* in the process list (06 §4.16) |
| `heating.json` | the Colibri emulator's environment: a Colibri 8 with a Pt1000, an AQV and a relay |

`emulators/colibri/tests/heating-circuit.test.ts` runs it in the real hakkas against the emulator.

## The I/O

| Slot | Module | Channel | Process port |
|---|---|---|---|
| 1 | Pt1000 | `slot1-ai[1]`, outdoor | `outdoor_temperature` |
| 1 | Pt1000 | `slot1-ai[2]`, supply water | `supply_temperature` |
| 2 | AQV (0–10 V) | `slot2-aq[1]`, shunt valve | `shunt_valve` |
| 3 | Relay | `slot3-dq`, circulation pump | `pump` |

## What it does

- **Sensors** through a 0.1 °C deadband (`outdoor`, `supply`).
- **Supply water setpoint**: a curve over the outdoor temperature (`curve`: −20 °C → 60 °C,
  0 °C → 45 °C, +15 °C → 25 °C), 5 °C lower outside 06:00–22:00 (`comfort_hours`, `setback`),
  added in `supply_setpoint`.
- **The shunt group** (`shunt_group`, the composite type `heating/ShuntGroup`): a PID (Kp 3 %/K,
  Ki 0.05 %/K·s, 0–100 %, sampled every second) and its output as 0–10 V; in automatic while
  `enable`, else in manual at 0 % (closed).
- **Summer stop** (`heating_needed`): heating below 16 °C outdoors, none above 18 °C.
- **Start delay** (`start_time`, `start_delay`, `regulate`): the regulator goes to automatic
  10 s after the start, once both sensors have delivered their values: a fixed `true` into a
  `process/Delay` with mode 1 (an on-delay), its time `10 → timing/Seconds`.
- **The pump** (`pump`): while heating is needed, and every Monday 10:00–10:05 (`exercise`).

## Running it

As for the air handling unit (`examples/ahu/README.md`, "Running it"), with `heating.json` as the
emulator's environment (`node dist/main.js heating` in `emulators/colibri/`). The calendar blocks
use hakkas' `time_zone` (05 §1.1), UTC unless set. The PID settings suit the emulator; a real
circuit needs its own tuning.
