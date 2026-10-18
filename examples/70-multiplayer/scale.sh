#!/usr/bin/env bash
# The scale check: a server and a hundred bot clients (one process, real sockets) on localhost for
# twenty seconds. Passes when every bot joined and the server's frames stayed within a 60 Hz step.
#   examples/70-multiplayer/scale.sh [build dir, default: build] [bots, default: 100]
set -u
build="${1:-build}"
count="${2:-100}"
port=$((27700 + RANDOM % 200))
logs="$(mktemp -d)"
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$logs"' EXIT

"$build/examples/mp_server/mp_server" --port "$port" --seconds 26 --max-clients $((count + 8)) > "$logs/server.log" 2>&1 &
sleep 0.5
"$build/examples/mp_bots/mp_bots" --port "$port" --count "$count" --seconds 22 > "$logs/bots.log" 2>&1
bots=$?
sleep 1
grep -h "BOTS:" "$logs/bots.log"
# The server's last seconds with every bot in.
grep "MPSERVER: $count client" "$logs/server.log" | tail -5 | sed 's/ player .*//'
worst=$(grep "MPSERVER: $count client" "$logs/server.log" | tail -10 | sed -E 's/.*frames [0-9.]+ ms average ([0-9.]+) ms worst.*/\1/' | sort -g | tail -1)
echo "SCALE: worst server frame with $count clients: ${worst:-none} ms"
if [ $bots -eq 0 ] && [ -n "${worst:-}" ] && awk "BEGIN{exit !($worst < 16.7)}"; then
    echo "SCALE: PASS"; exit 0
fi
echo "SCALE: FAIL"
tail -n 20 "$logs/server.log" "$logs/bots.log"
exit 1
