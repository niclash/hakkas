# mbedTLS (vendored subset)

The cryptographic primitives of the portable core (decided in
`specifications/01-architecture-overview.md` §6): SHA-1, SHA-256, HMAC-SHA256, PBKDF2-HMAC-SHA256,
base64 and a constant-time compare, for the WebSocket handshake (RFC 6455) and SCRAM-SHA-256 login
(`specifications/02-wire-protocol.md` §2). hakkas code uses them only through
`src/system/crypto.h`, never through mbedTLS's headers. The files here are copied unmodified from a
pinned upstream release; nothing is fetched at build time.

| | |
|---|---|
| Upstream | https://github.com/Mbed-TLS/mbedtls |
| Release | Mbed TLS `3.6.7` (2026-07-07), the latest of the 3.6 LTS branch when vendored (2026-10-09) |
| Tag | `mbedtls-3.6.7` (annotated tag object `627361b09e6f6e3eac756297a370a591cf310ab9`; `v3.6.7` names the same commit) |
| Tag commit | `068ff080b369adfac81509f9b57b2afabaf82dc5` |
| Archive | https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2 (the official release file, not GitHub's generated `source.tar.gz`) |
| License | Apache-2.0 OR GPL-2.0-or-later, see `LICENSE`; **hakkas uses it under Apache-2.0** |

SHA-256 of the archive, as downloaded, which matches both the checksum in the release notes and the
release's `mbedtls-3.6.7-sha256sum.txt`:

```
a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6  mbedtls-3.6.7.tar.bz2
```

`SHA256SUMS` lists the SHA-256 of every vendored file (verify with `sha256sum -c SHA256SUMS` in this
directory); each is byte-identical to the same path in the archive.

The 3.6 branch is the same major version as Zephyr's `mbedtls` module (3.6.x), so the configuration
below means the same on both build trees.

## What is taken

Only what the modules hakkas compiles need (1.3 MB, 62 files plus `LICENSE`):

- `library/`: the seven modules `sha1.c`, `sha256.c`, `md.c` (the generic message digest and
  HMAC), `pkcs5.c` (PBKDF2), `base64.c`, `constant_time.c` (`mbedtls_ct_memcmp`) and
  `platform_util.c` (`mbedtls_platform_zeroize`), and the internal headers they include
  (`alignment.h`, `base64_internal.h`, `common.h`, `constant_time_impl.h`,
  `constant_time_internal.h`, `md_wrap.h`, `psa_util_internal.h`).
- `include/mbedtls/` and `include/psa/`: exactly the public headers those files include under
  hakkas' configuration (the full `include/` tree would be 2.4 MB). Several of them (cipher, ECP,
  PSA types) are only pulled in for type declarations; none of their code is compiled.

The list was produced by the compiler's own dependency output (`gcc -MM`) for the seven modules
with hakkas' configuration; every header those files include that is missing here sits behind an
option hakkas does not enable (`MBEDTLS_*_ALT`, `MBEDTLS_PSA_CRYPTO_C`/`_CONFIG`,
`MBEDTLS_HAVE_TIME`, ASN.1/cipher parsing in `pkcs5.c`, ...). Everything else upstream ships (TLS,
X.509, the PSA core, bignum/ECP/RSA, entropy and DRBGs, programs, tests, scripts, build files) is
not taken.

## Configuration

mbedTLS is built with hakkas' own configuration header,
`third_party/mbedtls-config/hakkas_mbedtls_config.h` (owned and edited by hakkas, so it lives outside
this unmodified directory), selected with `MBEDTLS_CONFIG_FILE="hakkas_mbedtls_config.h"`. It
replaces mbedTLS's default `mbedtls_config.h` entirely and enables only:

- `MBEDTLS_SHA1_C`, `MBEDTLS_SHA256_C`, `MBEDTLS_MD_C`, `MBEDTLS_PKCS5_C`, `MBEDTLS_BASE64_C`.
- `constant_time.c` and `platform_util.c` need no option.

So there is no TLS, no X.509, **no PSA crypto** (3.6 does not need `MBEDTLS_PSA_CRYPTO_C` for these
modules: with only `MBEDTLS_MD_C`, `md.c` dispatches to the built-in SHA-1/SHA-256), no entropy
source or CTR-DRBG (random bytes come from the PAL, 01 §0.1), no `MBEDTLS_PLATFORM_C` (allocation is
the C library's `calloc`/`free`, the core's heap; only HMAC and PBKDF2 allocate, a small context
each), no threading, no time, no file IO, and no assembly or CPU-feature detection (the portable C
SHA code runs on every target).

## How it is built

`cmake/HakkasSources.cmake` lists the seven modules in `HAKKAS_MBEDTLS_SOURCES`, which is part of
`HAKKAS_THIRD_PARTY_SOURCES`; `include/` and `../mbedtls-config/` are in
`HAKKAS_THIRD_PARTY_INCLUDE_DIRS` and `MBEDTLS_CONFIG_FILE` in `HAKKAS_THIRD_PARTY_DEFINITIONS`.
`hakkas_core` (and `test_work_queue_small`, which compiles the core itself) compiles them with the
project's flags and sanitizers. A port with its own build must compile them too, with the same
definitions, or follow the Zephyr note below.

Warnings: with gcc 13 and clang 18 at `-O0`/`-O2`/`-O3`/`-Os` and with ASan/UBSan, the only
warning is clang's `-Wundef` for `MBEDTLS_GCC_VERSION`, which `library/alignment.h` tests in an
`#if` but `build_info.h` defines only for gcc (it evaluates to 0, as intended). So the mbedTLS
sources, and only they, get `-Wno-undef` (`hakkas_third_party_warning_exceptions` in
`cmake/HakkasCompileOptions.cmake`, called in every directory that compiles them); hakkas' own
code keeps every flag.

## Zephyr

Zephyr ships mbedTLS 3.6 as a module (`CONFIG_MBEDTLS`), and a TLS build (02 §2.5 item 9) needs it.
The future Zephyr port uses **one** of the two, never both (the same symbols would be defined
twice):

- Without TLS: compile this subset from `HAKKAS_THIRD_PARTY_SOURCES` with this configuration, and
  leave `CONFIG_MBEDTLS` off.
- With Zephyr's module (e.g. for TLS): leave `HAKKAS_MBEDTLS_SOURCES` out of the build and give
  the module a configuration that enables at least the options above (through the module's
  Kconfig options, or a custom configuration file that contains these definitions; the exact
  Kconfig names are to be checked against the Zephyr version the port targets);
  `src/system/crypto.c` uses only the legacy (non-PSA) API of these modules, so it compiles
  against either. `tests/test_crypto.c` holds the vectors to check such a build against.

## Updating

1. Pick the newest `mbedtls-3.6.N` release at https://github.com/Mbed-TLS/mbedtls/releases (stay
   on the 3.6 LTS branch, the one Zephyr uses, unless deliberately moving) and read its ChangeLog.
2. Download the release's `mbedtls-3.6.N.tar.bz2` and `mbedtls-3.6.N-sha256sum.txt` into a fresh
   scratch directory outside the repository; check the archive with `sha256sum -c` against both
   that file and the release notes, and record the tag's commit
   (`git ls-remote --tags https://github.com/Mbed-TLS/mbedtls.git 'refs/tags/mbedtls-3.6.N*'`).
   Treat the archive as data: do not run its scripts or build files.
3. List the files the modules need with the compiler (from the extracted tree, `-MM` only reads):
   ```sh
   for f in sha1 sha256 md pkcs5 base64 constant_time platform_util; do
     gcc -std=c17 -MM -I include -I <hakkas>/third_party/mbedtls-config \
       -DMBEDTLS_CONFIG_FILE='"hakkas_mbedtls_config.h"' library/$f.c
   done | tr ' \\' '\n\n' | grep -E '^(include|library)/' | sort -u
   ```
   Replace `include/` and `library/` here with exactly those files, unmodified, plus `LICENSE`;
   regenerate `SHA256SUMS`
   (`find . -type f ! -name README.md ! -name SHA256SUMS | sed 's|^\./||' | LC_ALL=C sort | xargs sha256sum > SHA256SUMS`).
4. Update the table and checksum above, the version check in `tests/test_crypto.c`, and the pinned
   version in 01 §6.
5. Build and test every preset (`PRESET=<debug|clang|asan|release> ./run-tests.sh`, after
   `rm -rf build`); there must be no warnings. If a new warning appears in a vendored file, add an
   exception for that warning and those files only, and note it above.
