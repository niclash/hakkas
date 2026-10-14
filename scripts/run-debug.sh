#!/usr/bin/env bash
# Starts the debug build that ./build-debug.sh put together in this directory (dist-debug/):
# the engine (bin/hakkas) serving the frontend (web/), with its configuration in conf/ and
# its processes in data/graphs/. On the first run it asks for a user to log in with.
#
#   ./run-debug.sh                    http://localhost:8080/, WebSocket on 8081
#   ./run-debug.sh --port 9000        options of `hakkas run` (05 §1.2) are passed on
#   LOG=--fine ./run-debug.sh         log level: --fine, --info (default), --warn
#
# Ctrl-C stops it. Changes made in the UI are kept only if a client persists the process.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
hakkas="$here/bin/hakkas"

if [ ! -s "$here/conf/users.json" ]; then
	if [ ! -t 0 ]; then
		echo "No users yet: run $hakkas --confdir $here/conf user add <name> --role admin" >&2
		exit 1
	fi
	read -rp "No users yet. User name to create [admin]: " name
	"$hakkas" --confdir "$here/conf" user add "${name:-admin}" --role admin
fi

exec "$hakkas" "${LOG:---info}" --confdir "$here/conf" --basedir "$here/data" \
	run --webdir "$here/web" "$@" daemon
