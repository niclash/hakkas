#!/usr/bin/env bash
# Starts the Colibri emulator that ./build-debug.sh built, with its UI:
#
#   ./run-debug.sh                    the environment that ran last; the UI on http://localhost:8090/
#   ./run-debug.sh lab                the environment "lab" of the library
#   ./run-debug.sh --http 9000        options of colibri-emulator (colibri/README.md) are passed on
#   LOG=fine ./run-debug.sh           log level: fine, info (default), warn, error
#
# The library (the environments) is ~/.local/share/colibri-emulator/ unless --library says
# otherwise. To run hakkas against the emulator, give hakkas' configuration (for the debug
# build, ../dist-debug/conf/hakkas.json) the environment socket this prints, and start hakkas
# after the emulator. Ctrl-C stops it.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
main="$here/colibri/dist/main.js"

if [ ! -f "$main" ] || [ ! -f "$here/ui/dist/index.html" ]; then
	echo "Not built yet: run $here/build-debug.sh first" >&2
	exit 1
fi

# The emulator's default environment socket (colibri spec §3.1), named here so it can be shown.
if [ -n "${XDG_RUNTIME_DIR:-}" ]; then
	socket="$XDG_RUNTIME_DIR/colibri-emulator/environment.sock"
else
	socket="${TMPDIR:-/tmp}/colibri-emulator-$(id -u)/environment.sock"
fi
# One given on the command line is the one shown.
args=("$@")
for i in "${!args[@]}"; do
	case "${args[$i]}" in
	--socket=*) socket="${args[$i]#--socket=}" ;;
	--socket) socket="${args[$((i + 1))]:-$socket}" ;;
	esac
done

echo "hakkas.json for hakkas to use this emulator: {\"environment\": {\"socket\": \"$socket\"}}"
exec node "$main" --log-level "${LOG:-info}" --socket "$socket" "$@"
