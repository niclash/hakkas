# Colibri emulator

Emulates a Colibri system's I/O side — the carrier board, its I/O module slots and the modules
in them — so a hakkas program can be tested on the desktop before it goes onto a device. hakkas
runs as its POSIX build and talks to the emulator as its environment program
(`../../specifications/04-drivers.md` §0.1).

The specification is `specifications/00-README.md`; the order of the work, and what is done, is
`docs/plan.md`. The control protocol and the generic UI are shared with other emulators:
`../specifications/`.

> Work in progress: the command line, logging, the module catalog (`data/`), the environment
> library, the emulated system's model, the scan cycle, the environment description, the link to
> hakkas (tested with hakkas), the control channel and the UI's signal panel exist (plan tasks
> E1–E12); the rest of the UI follows.

## Building and testing

Needs Node.js 24. To build the emulator with its UI and start it, from `emulators/`:

```sh
./build-debug.sh && ./run-debug.sh
```

```sh
npm install
npm run build        # dist/
npm test             # unit tests (vitest)
npm run type-check
```

`npm test` includes a test with the real hakkas (`tests/hakkas.test.ts`): it runs
`../../build/debug/hakkas` (or `HAKKAS_BIN`), so build hakkas first
(`cmake --preset debug && cmake --build --preset debug` in the repository's root); without it
that test is skipped.

## Running

```sh
node dist/main.js [options] [<environment>]
```

| Option | Default |
|---|---|
| `--library <dir>` | `$XDG_DATA_HOME/colibri-emulator` (`~/.local/share/colibri-emulator`) |
| `--plugins <dir>` (repeatable) | only the built-in `data/` |
| `--socket <path>` | `$XDG_RUNTIME_DIR/colibri-emulator/environment.sock` |
| `--control-socket <path>` | `$XDG_RUNTIME_DIR/colibri-emulator/control.sock` |
| `--http <port>` | `8090` (localhost only; `0` for any free port) |
| `--mcu <type>` | `colibri_mcu6` (the MCU module named to hakkas: `colibri_mcu2`, `colibri_mcu6` or `colibri_mcu7`) |
| `--seed <n>` | random |
| `--log-level <level>` | `info` (`fine`, `info`, `warn`, `error`) |

`SIGINT` (Ctrl+C) or `SIGTERM` stops it. The log goes to standard error.

### The UI

With the UI built (`npm install && npm run build` in `../ui`), the emulator serves it at
`http://localhost:<--http>/` (default 8090): every module's channels, their sources and noise,
and the outputs as hakkas writes them.

### The control channel

Every client — the UI, a script — sees and changes the emulated system through the control
protocol (`../specifications/01-control-protocol.md`): newline-delimited JSON on the
`--control-socket`, or a WebSocket at `ws://localhost:<--http>/control`. For example, from a
shell:

```sh
echo '{"set": {"key": "slot1.ai.1.source", "value": 7.5}}' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/colibri-emulator/control.sock
```

`examples/follow.py` sets an input to a sine and prints an output's values, with nothing but
Python 3. Changes take effect at once; `{"persist": {}}` writes them to the environment's
file.

### With hakkas

Start the emulator first, then hakkas with its `environment.socket` set to the emulator's
`--socket` path, in `hakkas.json`:

```json
{"environment": {"socket": "/run/user/1000/colibri-emulator/environment.sock"}}
```

hakkas waits for the emulator's description at its startup (`04-drivers.md` §0.1). After the
emulated system changes its structure (another environment, a module put into a slot), hakkas
has to be restarted.

Bindings are made in hakkas' frontend, on **process ports**: *Bind to environment…* in a
process port marker's menu, or in a block port's menu, which exposes the port as a process port
first (hakkas `06-frontend.md` §4.14).
