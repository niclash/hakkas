#!/usr/bin/env bash
# Build the Colibri emulator (colibri/dist/) and the emulators' UI (ui/dist/, which the
# emulator serves), then start it with ./run-debug.sh:
#
#   ./build-debug.sh && ./run-debug.sh
#
# Each package gets npm ci only when its dependencies are missing or its lock file changed.
set -euo pipefail
cd "$(dirname "$0")"

for package in colibri ui; do
	(
		cd "$package"
		if [ ! -f node_modules/.package-lock.json ] || [ package-lock.json -nt node_modules/.package-lock.json ]; then
			npm ci --no-audit --no-fund
		fi
		npm run build
	)
done

echo "Built colibri/dist/ and ui/dist/. Start the emulator with ./run-debug.sh"
