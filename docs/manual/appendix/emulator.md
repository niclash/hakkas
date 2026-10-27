# D. The Colibri emulator

The Colibri emulator behaves like a Colibri controller's I/O, on your desktop: a carrier board
with modules in its slots, whose inputs you set by hand and whose outputs you watch. Hakkas runs
against it exactly as against a device. Every example in this manual comes with an environment
file for it.

## D.1 Starting the two

In the repository (it needs Node.js 24 for the emulator):

1. **Build** hakkas and its editor (`./build-debug.sh` in the root), and the emulator with its own
   page (`./build-debug.sh` in `emulators/`).
2. **Put the example's environment** into the emulator's library:

   ```sh
   mkdir -p ~/.local/share/colibri-emulator/environments
   cp examples/thermostat/thermostat.json ~/.local/share/colibri-emulator/environments/
   ```
3. **Start the emulator** with it: `./run-debug.sh thermostat` in `emulators/`. It prints its
   environment socket, and serves its own page at `http://localhost:8090/`.
4. **Point hakkas at it**: in hakkas' configuration (for the debug build,
   `dist-debug/conf/hakkas.json`):

   ```json
   {"environment": {"socket": "/run/user/1000/colibri-emulator/environment.sock"},
    "time_zone": "CET-1CEST,M3.5.0,M10.5.0/3"}
   ```

   Use the socket path the emulator printed.
5. **Start hakkas** (`./dist-debug/run-debug.sh`) after the emulator. It waits for the emulator if
   it is not there yet. The editor is at `http://localhost:8080/`.

Then upload the example's process file with *Upload…*. Its bindings name the example's
environment, so they apply at once. Start the process.

If you change the emulated hardware, for example by putting another module in a slot or choosing
another environment, restart hakkas: it reads the environment's description when it starts.

## D.2 Setting inputs and watching outputs

The emulator's page lists every module's channels:

- **An input** has a **source**: a constant, or a function of time: a sine, a square wave, a
  ramp, a counter or a sequence. A DII's or DIU's input is a pulse train. It can also have **noise**, as a real sensor has. Set a temperature, or press a DIU's
  input, there.
- **An output** shows what hakkas writes to it.

In hakkas' editor, the **device window** (Section 8.2) shows the same controller with every
channel's value. The emulator provides the pictures.

Scripts can set inputs too, through the emulator's control socket. `emulators/colibri/README.md`
describes it.

## D.3 Your own environment

An environment file describes a controller: the carrier, and the module in each slot:

```json
{
  "carrier": "colibri_8",
  "modules": [
    {"slot": 1, "type": "colibri_pt1000", "serial": "TEST-0001"},
    {"slot": 2, "type": "colibri_relay", "serial": "TEST-0002"}
  ],
  "inputs": {"slot1.ai.1": {"source": 19.5, "noise": 0.05}}
}
```

The module types are the emulator's catalog (`emulators/colibri/data/modules/`):
- **Analog inputs**: `colibri_pt1000`, `colibri_aiv`, `colibri_aic`;
- **Analog outputs**: `colibri_aqv`;
- **Digital**: `colibri_dio1`, `colibri_dii`, `colibri_diu`, `colibri_relay`, `colibri_ssr`,
  `colibri_triac1`, `colibri_fet`;
- **Others**: `colibri_dp1` (differential pressure), `colibri_pid1` (a module with its own PID);
  `colibri_rs485i` and `colibri_rs485u` are described, but not emulated.

Name the file after the environment, and name the controller the same in hakkas' bindings.
Describe the device you will deploy to, slot by slot, and the bindings will apply there as
they are (Section 9.1).

## D.4 Testing a process automatically

The examples are tested by programs that start hakkas with the example's process, against the
emulator, then set inputs and check outputs (Section 13.10). See
`emulators/colibri/tests/thermostat.test.ts` for the smallest, and `tests/example-rig.ts` for the
rig they share. A test like that, run after every change, is the closest thing to a commissioning
protocol that runs by itself.
