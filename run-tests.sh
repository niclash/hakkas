#!/usr/bin/env bash
# Build and run the test suite.
#
#   ./run-tests.sh                    debug build, all tests
#   ./run-tests.sh -R version         only tests whose name matches "version"
#   PRESET=asan ./run-tests.sh        run under ASan/UBSan (also: clang, release)
#
# Arguments are passed to ctest.
set -euo pipefail
cd "$(dirname "$0")"

preset="${PRESET:-debug}"

cmake --preset "$preset"
cmake --build --preset "$preset"
ctest --preset "$preset" "$@"
