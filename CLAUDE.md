# Hakkas

A flow-based-programming (FBP) runtime in C: users wire blocks into live dataflow graphs, edited in real time from a browser frontend over a WebSocket protocol. The primary targets are MCUs running Zephyr RTOS (RP2354, STM32H5/H7/N6). A POSIX build covers Raspberry Pi deployment and x86_64 development.

## Specifications are the source of truth

Read `specifications/` before designing or changing anything, in this order: `00-README.md` → `01` → `02` → `03` → `04` → `05` → `06`. `fbp-protocol.html` is the upstream FBP network protocol, for reference only; where it disagrees with `02-wire-protocol.md`, `02` wins.

- When code and spec disagree, the spec wins. If the spec looks wrong, raise it rather than silently diverging.
- Open questions are listed in `00-README.md`. Don't guess an answer in code: ask, record the decision in the spec, then implement it.
- Keep the specs current: a decision made during implementation goes into the relevant spec file in the same change.

## Rules that must not be broken

- **Portable core**: no Zephyr or POSIX headers outside `src/platform/<port>/` (`posix/`, later `zephyr/`). Everything else talks only to the PAL (`01` §0), so the full system, including simulated IO, runs on a desktop.
- **The engine is the single source of truth** (`02` §1.1): the UI never updates its own view. Commands go to the engine, which applies or refuses each one and broadcasts the resulting events to every subscribed client, including the sender. Events describe the state that was applied, not the request.
- **Single-threaded engine with one global FIFO work queue** (`01` §3.2). Timers are core deadlines fired from the engine loop (no PAL timers); IO workers only enqueue and never touch block state.
- **No fan-in**: an input has at most one source, an edge or an initial. A severed input reverts to its port's default (`01` §2.2).
- **Only changes propagate**, filtered by the change threshold (`01` §3.3).
- **One generic block runtime** driven by `BlockTypeDef` data records, not one implementation per block type (`01` §2.3).
- No credentials compiled into any build, and secrets are never logged (`02` §2).
- **The wire protocol stays as close to the upstream FBP protocol as possible** (`00-README.md`, "Protocol compatibility policy"): ask the user before any change to it, one change at a time; acceptable ones are typically additions. The frontend being in this repository does not change that.

## Building

CMake is the canonical build system. The repository is a plain CMake project with no Zephyr dependency: the portable core library (`hakkas_core`), the POSIX runtime (`hakkas`), and the unit tests. Zephyr integration comes later as a separate layer over the same core sources. See `README.md`.

- `cmake --preset debug && cmake --build --preset debug && ctest --preset debug` (also `clang`, `asan`, `release`)
- The frontend (`frontend/`, `06`) is built separately: `npm install && npm run build` in `frontend/` gives `frontend/dist`, which a development build's `webdir` defaults to. Its tests: `npm test` (unit) and `npm run test:e2e` (browser + a real engine per test, needs `build/debug/hakkas`; `06` §6).
- New core sources go in `HAKKAS_CORE_SOURCES` (`cmake/HakkasSources.cmake`). Platform code lives only under `src/platform/<port>/`. The `portable_core` CTest enforces the header rule above.

## Git workflow

There is one developer. For now, commit directly on `main`; don't create feature branches.

## Terminology

"Graph" and "process" mean the same thing. "Group" and "composite block type" mean the same thing (`00-README.md`, Terminology).
