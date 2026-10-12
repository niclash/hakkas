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
examples/             the sister project's files, kept unmodified; its graph files load as
                      they are (05 §2.1), e.g. `hakkas describe topology examples/example.json`
                      prints one in hakkas' canonical form
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

`hakkas run daemon` starts the engine with every graph saved in `<basedir>/graphs/`;
`hakkas run process a.json:b.json` with the given graph files instead, and
`hakkas run processes processes.json` with the processes of a process list: either a plain
array of graph file paths, like `examples/processes.json`, or
`{"processes":[{"graph":"a.json","start":true}]}`, the paths relative to the list's directory
(05 §1.2). The engine itself starts no process: `run` is the host, which loads the processes
through the local C API (`src/hakkas.h`, 02 §6) and starts those whose list entry says
`"start": true` or, without one, whose graph file was persisted running. For a development
setup in a scratch directory:

```sh
mkdir -p conf data
./build/debug/hakkas --confdir conf user add alice --role editor   # asks for a password
./build/debug/hakkas --info --confdir conf --basedir data run daemon
```

Then open `http://localhost:8080/`; the FBP WebSocket is on port 8081 (`port + 1`). The first
`run` writes a generated `auth.server_secret` into `conf/hakkas.json`, so the configuration
directory must be writable. `run`'s options override the file: `--host <addr>` (a numeric
address, default `0.0.0.0`), `--port <n>`, `--webdir <dir>`, `--users <file>`, `--id <id>`.
The frontend is a separate artifact: `webdir` defaults to `<working directory>/frontend/src`,
so point `--webdir` at a frontend build (any directory with an `index.html` will do for a quick
check). `SIGINT` (Ctrl-C) or `SIGTERM` stops the engine cleanly (exit code 0); a failed startup
exits 1 with the cause logged. Running graphs are not saved at shutdown: a graph's file changes
only when a client persists it.

## Using the core from another project

`add_subdirectory()` or `FetchContent` this repository and link `hakkas::core`; the embedding
interface is `#include "hakkas.h"` (`src/hakkas.h`, 02 §6): create the runtime from a
`Config`, load and start processes, edit them, and subscribe to their events. As a
subproject, only the core library is built by default. A port with its own build integration,
such as the future Zephyr module, can instead include `cmake/HakkasSources.cmake` and compile
`HAKKAS_CORE_SOURCES` and `HAKKAS_THIRD_PARTY_SOURCES` (with the matching include directories
and definitions) with its own toolchain and flags.

## CLion

Open the repository root. CLion imports `CMakePresets.json`; enable the presets you want under
*Settings → Build, Execution, Deployment → CMake*, and each becomes a profile. Run or debug the
`hakkas` target and the `test_*` targets directly, or use a CTest run configuration.
`.clang-format` and `.clang-tidy` are picked up automatically.
