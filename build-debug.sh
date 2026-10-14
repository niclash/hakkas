#!/usr/bin/env bash
# Build the debug engine (build/debug/) and the frontend (frontend/dist/), and put them
# together in dist-debug/ with dist-debug/run-debug.sh to start them:
#
#   ./build-debug.sh && ./dist-debug/run-debug.sh
#
# dist-debug/bin and dist-debug/web are replaced on every build; dist-debug/conf (users,
# hakkas.json) and dist-debug/data (processes) are kept. Extra arguments are passed to
# `cmake --build`, e.g. `./build-debug.sh --target hakkas`.
set -euo pipefail
cd "$(dirname "$0")"

cmake --preset debug
cmake --build --preset debug "$@"

# The frontend (06 §1). npm ci only when the dependencies are missing or the lock file changed.
(
	cd frontend
	if [ ! -f node_modules/.package-lock.json ] || [ package-lock.json -nt node_modules/.package-lock.json ]; then
		npm ci --no-audit --no-fund
	fi
	npm run build
)

dist=dist-debug
mkdir -p "$dist/bin" "$dist/conf"
cp build/debug/hakkas "$dist/bin/hakkas"
rm -rf "$dist/web"
cp -r frontend/dist "$dist/web"
cp scripts/run-debug.sh "$dist/run-debug.sh"
chmod +x "$dist/run-debug.sh"

# The example process, on the first build only: data/ belongs to the user from then on.
if [ ! -d "$dist/data" ]; then
	mkdir -p "$dist/data/graphs"
	cp examples/example.json "$dist/data/graphs/0826152e-3014-4c89-9380-ec4991a76af2.json"
fi

echo "Built $dist/. Start it with ./$dist/run-debug.sh"
