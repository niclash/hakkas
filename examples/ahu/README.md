# Example: Air Handling Unit

An air handling unit's controls as one hakkas process, for a Colibri 8 (hakkas plan X1, designed
with someone who knows these units from practice). It runs as it is against the Colibri emulator.

| File | What |
|---|---|
| `Air Handling Unit.process.json` | the process with its bindings: upload it with *Upload…* in the process list (06 §4.16) |
| `ahu.json` | the Colibri emulator's environment: the Colibri 8 with its seven modules |

`emulators/colibri/tests/ahu.test.ts` runs it in the real hakkas against the emulator: each
season, a stopped unit, frost protection and Extended operation.

## The unit

Supply fan and exhaust fan (on/off; the dampers follow the fans), a rotary heat exchanger (an
analog signal: the rotor's speed, or a shunt group for a run-around coil), a heating coil with
its valve, pump and a water temperature sensor for frost protection, a cooling coil with its
valve and pump, temperature sensors in the four air streams and in the room, and an Extended
operation button.

| Slot | Module | Channel 1 | Channel 2 | Channels 3–4 |
|---|---|---|---|---|
| 1 | Pt1000 | outdoor air | supply air | |
| 2 | Pt1000 | extract air (from the room) | exhaust air (out of the building) | |
| 3 | Pt1000 | heating coil water (frost) | room | |
| 4 | AQV (0–10 V) | heating valve | cooling valve | |
| 5 | AQV (0–10 V) | heat exchanger | *spare* | |
| 6 | DIO1 | supply fan | exhaust fan | heating pump, cooling pump |
| 7 | DIU | Extended operation button | frost reset button | *spare* |

The process ports carry these names (`outdoor_temperature`, `heating_valve`, …); the bindings in
the file put them on the channels above, for an environment called `ahu`. On a device with other
slots, the mapping dialog asks where each one goes (06 §4.14). The DIO1's channels are outputs
because the process sets its `dir` channels (`dio1_direction`, always true).

Left out on purpose: fire and smoke (outside the AHU controls by regulation in many
jurisdictions), filter changes (service companies, on a yearly schedule), run and fault
indications (they show only some faults; alarms go on what the unit is for: temperatures and
efficiency), a hard-wired frost thermostat (outside the automation if a project wants one) and
flow control (no inverters, no flow sensors in this unit).

## What it does

The blocks are named after what they do; the process reads from left to right.

- **Sensors**: every temperature enters through a `process/Deadband` of 0.1 °C (`outdoor_air`,
  `supply_air`, …), which filters its noise and gives the value one block to be wired from.
- **Operation**: the week schedule, Monday–Friday 06:00–18:00 (`workdays`, `work_hours`,
  `schedule`), or **Extended operation**: a press of the button runs the unit for 2 hours
  (`extended`, a `process/Trigger` set by the press and reset by `extended_timer` after
  2 hours; `extended_wait` makes the timer fall back at once afterwards, so that the next press
  starts a new 2 hours). `running` is either.
- **Start delay**: the unit starts 10 s after the process does (`power_up`, a fixed `true` into
  a `process/Delay` with mode 1, its time from `power_up_time`), once every sensor
  has delivered its first value. Without it, a regulator switched to automatic would start from
  a not yet delivered 0 °C and take minutes to recover.
- **Fans** (`fans`): while running, after the start delay, and not locked by frost.
- **Seasons** by date, which do not overlap: the heating season 15 September – 15 May (whole
  months `heating_months` October–April, plus `late_september` and `early_may`), the cooling
  season June–August (`cooling_season`), and neither in between (`neither_season`).
- **Supply air setpoint**: a curve over the outdoor temperature (`supply_curve`: −20 °C → 22 °C,
  0 °C → 20 °C, +20 °C → 18 °C), plus the room's compensation (`room_compensation`, a PID
  holding the room at 21 °C, its output limited to ±3 °C), added in `supply_setpoint`.
- **Supply air regulators**, one per season, each a `process/PID` in automatic only in its season
  while the fans run (in manual otherwise, which holds its `manual` value; switching is
  bumpless):
  - heating season: `heating_regulator` on the heating valve; the heat exchanger at 100 %;
  - neither: `exchanger_regulator` on the heat exchanger, for heating only, 0–100 %; both valves
    closed;
  - cooling season: `cooling_regulator` (reverse acting) on the cooling valve; the heat
    exchanger at 100 %.

  The heat exchanger's 100 % outside its season comes from its regulator's manual value,
  `exchanger_manual`: 100 % while the fans run, 0 % when they do not. The regulators work in
  percent; `heating_valve_volts`, `exchanger_volts` and `cooling_valve_volts` turn that into
  0–10 V for the AQV outputs.
- **Frost protection**: when the heating coil's water stays below 5 °C for 5 s (`frost`, a
  `process/Hysteresis` on the negated temperature, so it holds from below 5 °C until above
  20 °C; `frost_confirmed`, the 5 s), `frost_alarm` (a `process/Alarm`, class B) is raised and
  the unit is **locked** (`frost_lock`): the fans stop, the heating pump runs, and
  `frost_regulator` holds the water at 20 °C. The heating valve takes the larger of the two
  regulators (`heating_demand`, a `math/Max`), so frost protection always wins. The unit
  restarts only when the alarm is acknowledged with the frost reset button **after** the water
  has reached 20 °C (`frost_acknowledge`: an acknowledgement while it is still cold does not
  count).
- **Pumps**: the heating pump runs in the heating season while the unit runs, and during a frost
  lock; the cooling pump in the cooling season while the unit runs. Both run every Monday
  10:00–10:05, whatever the season (`exercise`), so they do not seize.

## Running it

1. Build hakkas and the frontend (`README.md` in the repository's root) and the emulator
   (`emulators/colibri/README.md`).
2. Copy `ahu.json` into the emulator's library, `~/.local/share/colibri-emulator/environments/`,
   and start the emulator with it: `node dist/main.js ahu` in `emulators/colibri/`.
3. Start hakkas with its `environment.socket` set to the emulator's socket (the emulator's
   README, "With hakkas"), log in as an editor and upload `Air Handling Unit.process.json` with
   *Upload…*. It arrives stopped: start it.
4. Change the temperatures in the emulator's UI (`http://localhost:8090/`) and watch the outputs;
   *Watch all connections* in the editor's menu shows them inside the process. Press the DIU's
   input 1 for Extended operation, set the heating coil's water below 5 °C for frost.

The calendar blocks use hakkas' `time_zone` (05 §1.1), UTC unless set: for Sweden,
`"time_zone": "CET-1CEST,M3.5.0,M10.5.0/3"` in `hakkas.json`.

## Not yet

- **Alarms on the unit's goals** — the supply and room temperature deviations, the heat
  exchanger's efficiency (from the extract and exhaust air: η = (extract − exhaust) /
  (extract − outdoor)), and both seasons at once (a configuration error) — are to be designed
  with the user (plan X1). The frost alarm is part of frost protection.
- The PID settings are placeholders that behave reasonably against the emulator; a real unit
  needs its own tuning.
- Making composite block types out of parts of it (the seasons, Extended operation, frost
  protection) is a later exercise.
