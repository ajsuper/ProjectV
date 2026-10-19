#!/usr/bin/env bash
# The party self-test: a server and three clients as separate processes over real sockets on
# localhost, no windows, a bot in the fourth seat. Each client wanders about its arena, shoves, lays
# blocks and throws bombs. Twice: a match from the start (bomb tag, then sky dash -- its crumbling
# strip growing back, its sweepers throwing), then a server playing Blueprint alone, where each client
# lays cubes on its pad by aiming, as a first-person player does. Each client checks it joined, took a
# seat, was told how things stand, saw the rounds begin, and that every change to the world it was
# sent matches the server's (and in Blueprint, that its aimed cubes were laid). Run from
# anywhere:
#   examples/80-party/selftest.sh [build dir, default: build]
set -u
build="${1:-build}"
server="$build/examples/party_server/party_server"
client="$build/examples/party_client/party_client"
logs="${KEEP_LOGS:-$(mktemp -d)}"
trap 'kill $(jobs -p) 2>/dev/null; [ -n "${KEEP_LOGS:-}" ] || rm -rf "$logs"' EXIT

# play <label> <server seconds> <client seconds> <rounds expected> <client mode> [server options...]
play() {
    local label=$1 serverSeconds=$2 clientSeconds=$3 rounds=$4 how=$5
    shift 5
    local port=$((27600 + RANDOM % 300))
    "$server" --port "$port" --seconds "$serverSeconds" "$@" > "$logs/$label-server.log" 2>&1 &
    sleep 1
    local pids=() ok=1
    for name in amy ben cal; do
        "$client" --headless --host 127.0.0.1 --port "$port" --name "$name" "$how" --seconds "$clientSeconds" \
            --expect-rounds "$rounds" > "$logs/$label-$name.log" 2>&1 &
        pids+=($!)
        sleep 0.3
    done
    for pid in "${pids[@]}"; do wait "$pid" || ok=0; done
    grep -h "PARTYTEST" "$logs/$label"-amy.log "$logs/$label"-ben.log "$logs/$label"-cal.log
    grep -h "PARTYSERVER" "$logs/$label-server.log" | tail -1
    if [ $ok -ne 1 ]; then
        echo "PARTYTEST: $label FAILED (logs below)"
        tail -n 20 "$logs/$label"-*.log
        return 1
    fi
}

play match 140 120 2 --wander && play blueprint 45 30 1 --build --mode blueprint || exit 1
echo "PARTYTEST: PASS"
