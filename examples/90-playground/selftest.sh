#!/usr/bin/env bash
# The playground self-test: a server and two clients as separate processes over real sockets on
# localhost, no windows. Each client walks to the castle's gate and uses every tool on its south wall
# in turn -- cannon, blast, TNT, crate, ball, build, dig, push; one asks for the Moon's gravity, the
# other puts the world back part way through. Each checks it joined, took a place, was told how things
# stand, that the world changed, and that every change it was sent matches the server's. Run from
# anywhere:
#   examples/90-playground/selftest.sh [build dir, default: build]
set -u
build="${1:-build}"
server="$build/examples/playground_server/playground_server"
client="$build/examples/playground_client/playground_client"
logs="${KEEP_LOGS:-$(mktemp -d)}"
trap 'kill $(jobs -p) 2>/dev/null; [ -n "${KEEP_LOGS:-}" ] || rm -rf "$logs"' EXIT

port=$((27900 + RANDOM % 90))
"$server" --port "$port" --seconds 75 > "$logs/server.log" 2>&1 &
sleep 1
ok=1
"$client" --headless --host 127.0.0.1 --port "$port" --name amy --play --gravity --seconds 60 > "$logs/amy.log" 2>&1 &
amy=$!
sleep 0.3
"$client" --headless --host 127.0.0.1 --port "$port" --name ben --play --reset --seconds 60 > "$logs/ben.log" 2>&1 &
ben=$!
wait "$amy" || ok=0
wait "$ben" || ok=0
grep -h "PLAYTEST" "$logs/amy.log" "$logs/ben.log"
grep -h "PLAYSERVER" "$logs/server.log" | tail -1
if [ $ok -ne 1 ]; then
    echo "PLAYTEST: FAILED (logs below)"
    tail -n 20 "$logs"/*.log
    exit 1
fi
echo "PLAYTEST: PASS"
