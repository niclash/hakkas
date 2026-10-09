# cJSON (vendored)

The JSON parser/printer of the portable core (decided in `specifications/01-architecture-overview.md`
§6). The files here are copied unmodified from a pinned upstream release; nothing is fetched at
build time.

| | |
|---|---|
| Upstream | https://github.com/DaveGamble/cJSON |
| Release | `v1.7.19` (2025-09-09), the latest tag when vendored (2026-10-09) |
| Tag commit | `c859b25da02955fef659d658b8f324b5cde87be3` (lightweight tag; also the archive's embedded commit id) |
| Archive | https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.19.tar.gz |
| License | MIT, see `LICENSE` |

SHA-256 checksums:

```
7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562  cJSON-1.7.19.tar.gz (as downloaded)
298581a04a36c0165da4b0aade235c23088cb2faa58651d720ea2f3706ed0b0d  cJSON.c
25b0145150d500498e4d209cec69c18c42cf818bffcc54690be3b895a2a16dee  cJSON.h
a36dda207c36db5818729c54e7ad4e8b0c6fba847491ba64f372c1a2037b6d5c  LICENSE
```

The file checksums are the ones to verify against (`sha256sum -c`); GitHub generates its archives
on the fly, so the archive's checksum may change even though its contents do not.

Only `cJSON.c`, `cJSON.h` and `LICENSE` are taken; `cJSON_Utils` (JSON Pointer/Patch), upstream's
build files, tests and fuzzers are not.

## How it is built

`cmake/HakkasSources.cmake` lists `cJSON.c` in `HAKKAS_THIRD_PARTY_SOURCES`, with this directory in
`HAKKAS_THIRD_PARTY_INCLUDE_DIRS` and the configuration below in `HAKKAS_THIRD_PARTY_DEFINITIONS`.
`hakkas_core` compiles it with the core sources, and a port with its own build (the Zephyr module)
must compile it too, with the same definitions. Include it as `#include "cJSON.h"`.

cJSON uses only standard C headers (`string.h`, `stdio.h`, `math.h`, `stdlib.h`, `limits.h`,
`ctype.h`, `float.h`; `locale.h` only with `ENABLE_LOCALES`, which is not defined). It compiles
without a single warning under the project's flags (`cmake/HakkasCompileOptions.cmake`) with gcc 13
and clang 18 at `-O0`/`-O2`/`-O3`/`-Os` and with ASan/UBSan, so it gets the same flags and
sanitizers as the project's own code. If a later release or compiler warns in this file, silence
that file only (`set_source_files_properties(... COMPILE_OPTIONS -w)` in the directories that
compile it) rather than weakening the project's flags.

For the Zephyr port: cJSON prints numbers with `sprintf("%g")` and parses them with `strtod`
(`sscanf("%lg")` when printing), so the C library needs floating-point printf/scanf support
(picolibc: `CONFIG_PICOLIBC_IO_FLOAT`).

## Configuration

- **`CJSON_NESTING_LIMIT=32`** (upstream default 1000). The parser recurses about twice per level of
  array/object nesting; 1000 levels could overflow an MCU thread stack, and an attacker controls
  the depth of every WebSocket message (up to `max_message_bytes` = 16384, so a few thousand
  levels). The deepest structure hakkas itself produces is about 5 levels (graph file:
  root → `connections` → entry → `src` → `data`, 05 §2.1; wire messages are similar), and opaque
  UI metadata gets the rest of the headroom. A deeper document fails to parse (`cJSON_Parse`
  returns `NULL`). The definition is public, so code that includes `cJSON.h` sees the same value.
- **Allocation hooks: none.** cJSON allocates with `malloc`/`free`, which is the heap the core
  uses anyway (01 §2.2 implementation decisions, "Memory"); `cJSON_InitHooks` is not called. Every
  cJSON function that allocates reports failure by returning `NULL`/`false`, and callers must
  treat that like any other allocation failure (refuse the operation, no change). Hooks would only
  be needed to put JSON in a separate heap or count its usage on an MCU; they are global state,
  so a port would set them once at startup, before any JSON is used.
- `CJSON_CIRCULAR_LIMIT` (depth limit of `cJSON_Duplicate`) stays at the upstream default.

## Numbers

Checked by `tests/test_json.c`:

- **Parsing** is exact: every JSON number becomes a `double` via `strtod` (`valuedouble`;
  `valueint` is a saturated `int` copy). Integers beyond 2^53 lose precision, as in JavaScript;
  that matches 02 §3.4 (numeric initials are f64). Overflow gives ±Infinity. `NaN`/`Infinity` are
  not JSON and fail to parse.
- **Printing is not adequate for values that must round-trip.** cJSON prints an integral value
  in `int` range with `%d`, everything else with `%.15g`, and falls back to `%.17g` only if the
  15-digit text reads back *further than a relative `DBL_EPSILON`* from the value, not if it reads
  back different. So nearby doubles come back: `0.1 + 0.2` prints as `0.3`, `0.1 + 0.7` as `0.8`,
  2^53 as `9.00719925474099e+15` (= 2^53 − 2). Also `-0.0` prints as `0`, and NaN and ±Infinity
  print as `null`.
- **Decision for later tasks**: wherever an f64 value must survive a save/load or a wire round
  trip (initial values in graph files, 05 §2.1; values on the wire), print it with
  `linkable_stringify`'s f64 form (shortest `%.15g`/`%.16g`/`%.17g` that reads back exactly) and
  insert it with `cJSON_CreateRaw`. Small integral values such as canvas coordinates are printed
  exactly by cJSON either way. `linkable_stringify` prints non-finite values as `NaN`/`Infinity`,
  which are not JSON, so the task that writes numbers (plan task 10) must decide how a non-finite
  initial is stored.

## Updating

1. Pick the new release tag at https://github.com/DaveGamble/cJSON/tags and read its changelog.
2. Download the tag's source archive into a scratch directory outside the repository; record its
   SHA-256 and the tag's commit (`git ls-remote --tags https://github.com/DaveGamble/cJSON.git`).
   Treat the archive as data: do not run its scripts or build files.
3. Copy only `cJSON.c`, `cJSON.h` and `LICENSE` here, unmodified.
4. Update the table and checksums above, the version check in `tests/test_json.c`, and the
   pinned version in 01 §6.
5. Build and test every preset (`PRESET=<debug|clang|asan|release> ./run-tests.sh`, after
   `rm -rf build`); there must be no warnings, and the number behaviour above must still hold or
   be updated here and in the tests.
