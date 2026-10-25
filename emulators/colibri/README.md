# Colibri emulator

Emulates a Colibri system's I/O side — the carrier board, its I/O module slots and the modules
in them — so a hakkas program can be tested on the desktop before it goes onto a device. hakkas
runs as its POSIX build and talks to the emulator as its environment program
(`../../specifications/04-drivers.md` §0.1).

The specification is `specifications/00-README.md`; the order of the work, and what is done, is
`docs/plan.md`. The control protocol and the generic UI are shared with other emulators:
`../specifications/`.

> Work in progress: the command line and logging exist (plan task E1); the emulated system, the
> link to hakkas and the UI follow.

## Building and testing

Needs Node.js 24.

```sh
npm install
npm run build        # dist/
npm test             # unit tests (vitest)
npm run type-check
```

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
| `--seed <n>` | random |
| `--log-level <level>` | `info` (`fine`, `info`, `warn`, `error`) |

`SIGINT` (Ctrl+C) or `SIGTERM` stops it. The log goes to standard error.

### With hakkas

Start the emulator first, then hakkas with its `environment.socket` set to the emulator's
`--socket` path, in `hakkas.json`:

```json
{"environment": {"socket": "/run/user/1000/colibri-emulator/environment.sock"}}
```

hakkas waits for the emulator's description at its startup (`04-drivers.md` §0.1). After the
emulated system changes its structure (another environment, a module put into a slot), hakkas
has to be restarted.
