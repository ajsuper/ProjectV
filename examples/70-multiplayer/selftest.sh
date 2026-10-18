#!/usr/bin/env bash
# The multiplayer self-test: a server and two clients as separate processes over real sockets on
# localhost, no windows. Ann walks; Bo watches her walk. Run from anywhere:
#   examples/70-multiplayer/selftest.sh [build dir, default: build]
set -u
build="${1:-build}"
server="$build/examples/mp_server/mp_server"
client="$build/examples/mp_client/mp_client"
port=$((27200 + RANDOM % 500))
logs="$(mktemp -d)"
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$logs"' EXIT

"$server" --port "$port" --seconds 12 > "$logs/server.log" 2>&1 &
sleep 0.5
"$client" --headless --name ann --port "$port" --walk 0,3 --walk-seconds 3 --seconds 8 --expect-players 2 \
    > "$logs/ann.log" 2>&1 &
ann=$!
"$client" --headless --name bo --port "$port" --seconds 8 --expect-players 2 --expect-moved \
    > "$logs/bo.log" 2>&1 &
bo=$!
wait $ann; a=$?
wait $bo; b=$?
grep -h "MPTEST" "$logs/ann.log" "$logs/bo.log"
grep -h "MPSERVER: 2 client" "$logs/server.log" | tail -1
if [ $a -eq 0 ] && [ $b -eq 0 ] && grep -q "MPSERVER: 2 client" "$logs/server.log"; then
    echo "MPTEST: PASS"; exit 0
fi
echo "MPTEST: FAIL (logs: server, ann, bo below)"
tail -n 20 "$logs/server.log" "$logs/ann.log" "$logs/bo.log"
exit 1
