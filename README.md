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
tests/                unit tests (CTest)
third_party/          vendored libraries, pinned and unmodified (cjson/: cJSON, 01 §6)
```

Add new core sources to `HAKKAS_CORE_SOURCES` in `cmake/HakkasSources.cmake`, and POSIX PAL
sources to `HAKKAS_POSIX_PAL_SOURCES` (the `hakkas_pal_posix` library, which tests can link
with `hakkas_add_test(<name> LIBS hakkas_pal_posix)`). The
`portable_core` test fails if any file outside `src/platform/<port>/` includes a POSIX or Zephyr
header.

Vendored libraries live in `third_party/<library>/`, copied unmodified from a pinned release,
with a `README.md` recording the version, checksums and how to update. Those that are part of the
core are listed in `HAKKAS_THIRD_PARTY_SOURCES` (with `HAKKAS_THIRD_PARTY_INCLUDE_DIRS` and
`HAKKAS_THIRD_PARTY_DEFINITIONS`); nothing is fetched at build time.

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
./build/debug/hakkas
```

Each preset builds into `build/<preset>/`; `rm -rf build` removes every build. Without presets,
give CMake a build directory: `cmake -B build/debug -G Ninja && cmake --build build/debug`.
In-source builds (`cmake .`) are refused. Options: `-DHAKKAS_WERROR=ON`,
`-DHAKKAS_SANITIZERS="address;undefined"`, `-DHAKKAS_BUILD_TESTS=OFF`,
`-DHAKKAS_BUILD_POSIX_RUNTIME=OFF`, `-DHAKKAS_QUEUE_CAPACITY=<entries>` (work queue size,
default 1024).

## Using the core from another project

`add_subdirectory()` or `FetchContent` this repository and link `hakkas::core`. As a
subproject, only the core library is built by default. A port with its own build integration,
such as the future Zephyr module, can instead include `cmake/HakkasSources.cmake` and compile
`HAKKAS_CORE_SOURCES` and `HAKKAS_THIRD_PARTY_SOURCES` (with the matching include directories
and definitions) with its own toolchain and flags.

## CLion

Open the repository root. CLion imports `CMakePresets.json`; enable the presets you want under
*Settings → Build, Execution, Deployment → CMake*, and each becomes a profile. Run or debug the
`hakkas` target and the `test_*` targets directly, or use a CTest run configuration.
`.clang-format` and `.clang-tidy` are picked up automatically.
