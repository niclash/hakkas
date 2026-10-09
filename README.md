# Hakkas

A flow-based-programming runtime in C. The portable core runs on any hosted C toolchain; the
POSIX runtime runs it on Linux (x86_64 desktop, Raspberry Pi), and a Zephyr port will run it on
MCUs. The design lives in [`specifications/`](specifications/00-README.md).

## Layout

```
CMakeLists.txt        plain CMake project: hakkas_core library, hakkas POSIX runtime, tests
CMakePresets.json     build configurations (also CLion profiles)
cmake/                source lists, compile options, version header, portability check
src/<module>/         portable core: graphs, blocktypes, protocol, system, app, drivers, engine (01 §6)
src/platform/<port>/  PAL implementations; the only place OS/RTOS headers may appear
tests/                unit tests (CTest)
```

Add new core sources to `HAKKAS_CORE_SOURCES` in `cmake/HakkasSources.cmake`. The
`portable_core` test fails if any file outside `src/platform/<port>/` includes a POSIX or Zephyr
header.

## Building

Needs CMake ≥ 3.25, Ninja, and gcc and/or clang. ccache is used if present.

| Preset | What |
|---|---|
| `debug` | gcc, Debug — the everyday development build |
| `clang` | clang, Debug |
| `asan` | Debug with AddressSanitizer + UndefinedBehaviorSanitizer |
| `release` | RelWithDebInfo |

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
./build/debug/hakkas
```

Each preset builds into `build/<preset>/`. Options: `-DHAKKAS_WERROR=ON`,
`-DHAKKAS_SANITIZERS="address;undefined"`, `-DHAKKAS_BUILD_TESTS=OFF`,
`-DHAKKAS_BUILD_POSIX_RUNTIME=OFF`.

## Using the core from another project

`add_subdirectory()` or `FetchContent` this repository and link `hakkas::core`. As a
subproject, only the core library is built by default. A port with its own build integration,
such as the future Zephyr module, can instead include `cmake/HakkasSources.cmake` and compile
`HAKKAS_CORE_SOURCES` with its own toolchain and flags.

## CLion

Open the repository root. CLion imports `CMakePresets.json`; enable the presets you want under
*Settings → Build, Execution, Deployment → CMake*, and each becomes a profile. Run or debug the
`hakkas` target and the `test_*` targets directly, or use a CTest run configuration.
`.clang-format` and `.clang-tidy` are picked up automatically.
