# Hakkas

A flow-based-programming runtime in C. The portable core runs on any hosted C toolchain; the
POSIX runtime runs it on Linux (x86_64 desktop, Raspberry Pi), and a Zephyr port will run it on
MCUs. The design lives in [`specifications/`](specifications/00-README.md).

## Layout

```
CMakeLists.txt        plain CMake project: hakkas_core library, POSIX PAL + runtime, tests
CMakePresets.json     build configurations (also CLion profiles)
cmake/                source lists, compile options, version header, portability check
src/<module>/         portable core: graphs, blocktypes, protocol, system, app, drivers, engine (01 §6)
src/platform/pal.h    the PAL interface (portable)
src/platform/<port>/  PAL implementations; the only place OS/RTOS headers may appear
tests/                unit tests and end-to-end protocol tests (CTest; e2e.h: the
                      engine in-process with real WebSocket clients)
library/schedules/    the schedule standard library: composite type files (schedule/DaySchedule,
                      WeekSchedule, MonthSchedule, YearSchedule; 03 §6.2), not built in
docs/manual/          the User's Manual (VitePress; its README.md)
examples/             the sister project's files, kept unmodified; its graph files load as
                      they are (05 §2.1), e.g. `hakkas describe topology examples/example.json`
                      prints one in hakkas' canonical form; also hakkas' own example
                      processes: shunt-group.json (a shunt valve PID with its pump),
                      ahu/ (an air handling unit for a Colibri 8, with its emulator
                      environment; ahu/README.md)
third_party/          vendored libraries, pinned and unmodified (01 §6): cjson/ (cJSON),
                      mbedtls/ (a subset of mbedTLS 3.6; its configuration in mbedtls-config/)
```

Add new core sources to `HAKKAS_CORE_SOURCES` in `cmake/HakkasSources.cmake`, and POSIX PAL
sources to `HAKKAS_POSIX_PAL_SOURCES` (the `hakkas_pal_posix` library, which tests can link
with `hakkas_add_test(<name> LIBS hakkas_pal_posix)`). The
`portable_core` test fails if any file outside `src/platform/<port>/` includes a POSIX or Zephyr
header.

Vendored libraries live in `third_party/<library>/`, copied unmodified from a pinned release,
with a `README.md` recording the version, checksums and how to update. Those that are part of the
core are listed in `HAKKAS_THIRD_PARTY_SOURCES` (with `HAKKAS_THIRD_PARTY_INCLUDE_DIRS` and
`HAKKAS_THIRD_PARTY_DEFINITIONS`); nothing is fetched at build time. mbedTLS is a subset (only
the modules the core compiles) built with hakkas' own configuration header
(`third_party/mbedtls-config/`); hakkas code uses it only through `src/system/crypto.h`.

## Building

Needs CMake ≥ 3.25, Ninja, and gcc and/or clang. ccache is used if present.

| Preset | What |
|---|---|
| `debug` | gcc, Debug — the everyday development build |
| `clang` | clang, Debug |
| `asan` | Debug with AddressSanitizer + UndefinedBehaviorSanitizer |
| `release` | RelWithDebInfo |

The shortcuts:

```sh
./build-debug.sh              # configure + build into build/debug/
./run-tests.sh                # build + run every test (args go to ctest, e.g. -R version)
PRESET=asan ./run-tests.sh    # same, with another preset
```

The underlying commands:

```sh
cmake --preset debug          # configure, into build/debug/
cmake --build --preset debug  # compile
ctest --preset debug          # run the tests
./build/debug/hakkas --help   # the command line (specifications/05 §1.2)
```

For example `./build/debug/hakkas --confdir . list types` or
`./build/debug/hakkas --confdir . describe type math/Add2`. The configuration file is
`<confdir>/hakkas.json` (default `/etc/hakkas`; a missing file means all defaults, 05 §1.1).
Users are provisioned locally, never over the network:
`./build/debug/hakkas --confdir . user add alice --role editor` asks for the password (twice, with
echo off; from a pipe it reads one line) and writes `<confdir>/users.json`; `user passwd`,
`user del` and `user list` do the rest (02 §2.2, 05 §1.2). Without users every login is refused.

Each preset builds into `build/<preset>/`; `rm -rf build` removes every build. Without presets,
give CMake a build directory: `cmake -B build/debug -G Ninja && cmake --build build/debug`.
In-source builds (`cmake .`) are refused. Options: `-DHAKKAS_WERROR=ON`,
`-DHAKKAS_SANITIZERS="address;undefined"`, `-DHAKKAS_BUILD_TESTS=OFF`,
`-DHAKKAS_BUILD_POSIX_RUNTIME=OFF`, `-DHAKKAS_QUEUE_CAPACITY=<entries>` (work queue size,
default 1024).

## Running

The quickest way to try the whole system — engine and frontend:

```sh
./build-debug.sh && ./dist-debug/run-debug.sh
```

`build-debug.sh` builds the debug engine and the frontend (it needs Node.js 18 or later) and
puts them together in `dist-debug/` (not committed); `run-debug.sh` asks for a user to create
on its first run, then serves `http://localhost:8080/` with the example process. Options of
`run` are passed on (`./dist-debug/run-debug.sh --port 9000`), `LOG=--fine` sets the log
level. A rebuild replaces `dist-debug/bin` and `dist-debug/web` and keeps `conf/` and `data/`.
The sections below show what it does by hand.

`hakkas run` starts the engine with every process saved in the data directory (`<basedir>` by
default): the graphs in `graphs/`, each with its environment bindings from `bindings/<id>.json`,
and the links between processes from `links.json` (05 §1.2, §2.3). Saving a process from the
frontend writes all three. The engine itself starts no process: `run` is the host, which loads
the processes through the local C API (`src/hakkas.h`, 02 §6) and starts those whose graph file
was saved running. To run a graph file you have, copy it into `graphs/` (named after its id, or
it is loaded with a warning). For a development setup in a scratch directory:

```sh
mkdir -p conf data
./build/debug/hakkas --confdir conf user add alice --role editor   # asks for a password
./build/debug/hakkas --info --confdir conf --basedir data run
```

Then open `http://localhost:8080/`; the FBP WebSocket is on port 8081 (`port + 1`). The first
`run` writes a generated `auth.server_secret` into `conf/hakkas.json`, so the configuration
directory must be writable. `run`'s options override the file: `--host <addr>` (a numeric
address, default `0.0.0.0`), `--port <n>`, `--webdir <dir>`, `--users <file>`, `--id <id>`.
`webdir` defaults to this source tree's `frontend/dist` (below), whatever the working
directory. `SIGINT` (Ctrl-C) or `SIGTERM` stops the engine cleanly (exit code 0); a failed startup
exits 1 with the cause logged. Running graphs are not saved at shutdown: a graph's file changes
only when a client persists it.

## The frontend

`frontend/` is the browser editor (Vue 3, Rete), built with Vite into `frontend/dist`, which
the engine's web server serves:

```sh
cd frontend && npm install && npm run build && cd ..
./build/debug/hakkas --info --confdir conf --basedir data run
```

If `frontend/dist` is missing, the engine warns at startup (`web: no frontend in …`) and the
browser gets `404`. A package build serves the installed frontend instead: configure with
`-DHAKKAS_PACKAGE=ON` (and the install prefix), and `cmake --install` puts the executable in
`<prefix>/bin` and copies `frontend/dist` to `<prefix>/share/hakkas/web`, the default `webdir`
of that build (06 §1).

For a device's flash, `npm run build:device` also writes `frontend/dist-device`: every file
stored only as `<file>.gz` (about 0.5 MB), which the web server serves compressed (06 §5).

It reaches the WebSocket on the page's own port + 1 (02 §0) and changes its view only from the
engine's events (02 §1.1). For live reloading, `npm run dev` serves it from Vite instead; then
tell it where the WebSocket is and let the engine accept Vite's origin:

```sh
VITE_HAKKAS_WS=ws://localhost:8081/ npm run dev       # in frontend/
# conf/hakkas.json: {"allowed_origins": ["http://localhost:5173"]}
```

Its tests (06 §6), in `frontend/`: `npm test` runs the unit tests (Node only);
`npm run test:e2e` builds the frontend and runs the end-to-end tests in a browser, each against
its own engine started from `build/debug/hakkas` (build it first; `HAKKAS_BIN` names another).
The browser is Playwright's Chromium (`npx playwright install chromium` once) or the
Chromium-based browser named by `HAKKAS_BROWSER`:

```sh
HAKKAS_BROWSER=/usr/bin/google-chrome npm run test:e2e
npx playwright show-report test-results/report      # after a failure: screenshots, traces
```

## The environment: inputs and outputs

Hakkas reads sensors and drives actuators through its **environment** (`specifications/04`): on
Zephyr the device firmware, on POSIX a separate **environment program** — a simulator, a separate
project — that hakkas talks to over a Unix socket. Point hakkas at it in `hakkas.json`:

```json
{"environment": {"socket": "/run/sim/environment.sock"}}
```

(a relative path is relative to the configuration directory). The program is the server; `run`
connects, logs `waiting for environment at <path>` and retries every second until the program
has described its ports, and only then loads the processes (Ctrl-C stops the wait). It does not
wait for sensor values: processes start from their inputs' defaults, and values propagate as the
program sends them. If the program goes away, inputs keep their last values and hakkas
reconnects by itself; the program must then describe itself identically. The protocol —
newline-delimited JSON (`describe`, `value`, `clock` from the program; `hello`, `write` from
hakkas) — is in `specifications/04-drivers.md` §0.1. For a quick look without a simulator, any
program that serves the socket will do, e.g. `socat` replaying a description and the current
values (two analog inputs, one relay) and printing what hakkas writes:

```sh
cat > sim.ndjson <<'EOF'
{"describe":{"name":"sim","outPorts":[{"id":"ain","type":"number","addressable":true,"count":2}],"inPorts":[{"id":"relay","type":"bool"}]}}
{"value":{"port":"ain","index":1,"value":2.5}}
{"value":{"port":"ain","index":2,"value":0}}
EOF
socat UNIX-LISTEN:/run/sim/environment.sock,fork SYSTEM:'cat sim.ndjson; cat >&2'
```

A process is connected to the environment by its **bindings**, made in the frontend (a port's
context menu, or a bindings file loaded there) and saved with the process in
`<datadir>/bindings/<process id>.json`, a **bindings file**:

```json
{"environment": "sim",
 "bindings": [{"block": "add", "port": "in1", "env": "ain", "index": 1},
              {"block": "cmp", "port": "out", "env": "relay"}]}
```

A sensor (an environment output port) binds a block input, an actuator (an environment input
port) a block output; `index` is required exactly for an addressable port. Bindings never stop
startup (04 §4): an entry that does not fit the process or the environment (an unknown block,
port or channel, a wrong direction, an input that already has a source, a second driver for an
actuator) is skipped with a warning and the rest applied, and a file written for another
environment is applied after a warning. Without `environment.socket` there is no environment,
bindings files are not applied, and the wall clock is the host's.

## The schedule standard library

The calendar blocks (`timing/MinuteRange`, `DayOfWeek`, `DayOfMonth`, `MonthOfYear`, `DayOfYear`,
`WeekOfYear`) and `logic/And`/`Or`/`Xor`/`Not` are built in; the standard schedule types made of
them are not. To install them, copy the type files into the data directory's `types/`, named
as the engine names a type's file (the encoded type name, `schedule%2F<file>`, 05 §2.3):

```sh
mkdir -p data/types
for f in library/schedules/*.json; do cp "$f" "data/types/schedule%2F$(basename "$f")"; done
```

They load at the next startup as ordinary user types. The calendar blocks evaluate in local time
from `time_zone` in `hakkas.json`, a POSIX TZ string (default `"UTC0"`; Sweden is
`"CET-1CEST,M3.5.0,M10.5.0/3"`, 05 §1.1).

## Using the core from another project

`add_subdirectory()` or `FetchContent` this repository and link `hakkas::core`; the embedding
interface is `#include "hakkas.h"` (`src/hakkas.h`, 02 §6): create the runtime from a
`Config`, load and start processes, edit them, and subscribe to their events. A host with
inputs and outputs implements the environment interface of `src/hakkas_env.h` (04 §0) and hands
it to `hakkas_create` (`HakkasParams.environment`). As a
subproject, only the core library is built by default. A port with its own build integration,
such as the future Zephyr module, can instead include `cmake/HakkasSources.cmake` and compile
`HAKKAS_CORE_SOURCES` and `HAKKAS_THIRD_PARTY_SOURCES` (with the matching include directories
and definitions) with its own toolchain and flags.

## CLion

Open the repository root. CLion imports `CMakePresets.json`; enable the presets you want under
*Settings → Build, Execution, Deployment → CMake*, and each becomes a profile. Run or debug the
`hakkas` target and the `test_*` targets directly, or use a CTest run configuration.
`.clang-format` and `.clang-tidy` are picked up automatically.
