# Hakkas User's Manual

Hakkas runs control programs drawn as diagrams: blocks wired together into **processes**,
running on a controller such as a Colibri device and edited live from a web browser. This
manual is for automation engineers who have worked with a PLC system before — perhaps with
IEC 61131-3 — and are new to hakkas, and perhaps to programming with diagrams.

It is built around three examples that grow in size:

- **A thermostat with a schedule** (Part II), built step by step: the first process you make.
- **A heating circuit** (Part IV): a curve, a regulator made into a composite block type, and
  what a regulator needs at start-up.
- **An air handling unit** (Part V): a complete unit with seasons, a compensated setpoint and frost
  protection.

Between them, Part III describes everything hakkas offers, and Part VI collects patterns and the
reference of every block type.

**Where to start**: read [Concepts](/getting-started/concepts) first, even if you skim it —
hakkas differs from a PLC in a few ways that matter from the first block on. Then build the
thermostat.
