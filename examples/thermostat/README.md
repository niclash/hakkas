# Example: a thermostat with a schedule

An office's electric heater on a relay, held at 21 °C during office hours and 17 °C otherwise
(hakkas plan X2). It is the User's Manual's first example: Chapter 4 builds it step by step
(`docs/manual/thermostat/`). It runs as it is against the Colibri emulator.

| File | What |
|---|---|
| `Thermostat.process.json` | the process with its bindings: upload it with *Upload…* in the process list (06 §4.16) |
| `thermostat.json` | the Colibri emulator's environment: a Colibri 8 with a Pt1000 module and a relay |

`emulators/colibri/tests/thermostat.test.ts` runs it in the real hakkas against the emulator.

## The I/O

| Slot | Module | Channel | Process port |
|---|---|---|---|
| 1 | Pt1000 | `slot1-ai[1]`, the room temperature | `room_temperature` |
| 2 | Relay | `slot2-dq`, the heater (one channel, no index) | `heater` |

## What it does

- `heating` (`process/Hysteresis`): the heater on below `threshold − 0.5`, off above
  `threshold + 0.5` (`inverted`, since the heater must be on when the room is cold).
- `office_hours` (`timing/MinuteRange`, 07:00–18:00) and `workdays` (`timing/DayOfWeek`,
  Monday–Friday), joined by `occupied` (`logic/And`).
- `setpoint` (`process/SelectIn`): 21 °C while occupied, else 17 °C, into `heating.threshold`.

## Running it

As for the air handling unit (`examples/ahu/README.md`, "Running it"), with `thermostat.json` as
the emulator's environment (`node dist/main.js thermostat` in `emulators/colibri/`). The calendar
blocks use hakkas' `time_zone` (05 §1.1), UTC unless set.
