#!/usr/bin/env bash
# Configure (if needed) and build the debug build into build/debug/.
# Extra arguments are passed to `cmake --build`, e.g. `./build-debug.sh --target hakkas`.
set -euo pipefail
cd "$(dirname "$0")"

cmake --preset debug
cmake --build --preset debug "$@"
