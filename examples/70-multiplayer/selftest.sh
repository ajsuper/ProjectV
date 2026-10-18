#!/usr/bin/env bash
# The multiplayer self-test: a server and two clients as separate processes over real sockets on
# localhost, no windows. Ann walks, then throws balls at the tower and blasts it; Bo watches, and both
# check every break they were sent matches the server's. Run from anywhere:
#   examples/70-multiplayer/selftest.sh [build dir, default: build]
set -u
build="${1:-build}"
server="$build/examples/mp_server/mp_server"
client="$build/examples/mp_client/mp_client"
port=$((27200 + RANDOM % 500))
logs="${KEEP_LOGS:-$(mktemp -d)}"
trap 'kill $(jobs -p) 2>/dev/null; [ -n "${KEEP_LOGS:-}" ] || rm -rf "$logs"' EXIT

"$server" --port "$port" --seconds 12 > "$logs/server.log" 2>&1 &
sleep 0.5
"$client" --headless --name ann --port "$port" --walk 0,3 --walk-seconds 3 --throw 3 --aim 15,4,10 --seconds 8 --expect-players 2 --expect-breaks \
    > "$logs/ann.log" 2>&1 &
ann=$!
"$client" --headless --name bo --port "$port" --seconds 8 --expect-players 2 --expect-moved --expect-breaks \
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
