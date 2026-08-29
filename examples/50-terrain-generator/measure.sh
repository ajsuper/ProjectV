#!/usr/bin/env bash
set -euo pipefail

count=0
sum=0
skip=0
./terrain_generator | grep --line-buffered '\[FPS\]' | while IFS= read -r line; do
    if [[ $skip -lt 10 ]]; then
        skip=$((skip + 1))
        echo "[SKIP $skip/10] $line"
        continue
    fi
    val=$(echo "$line" | grep -oP '\s+\K[0-9.]+(?=ms\s+avg)')
    if [[ -n "$val" ]]; then
        count=$((count + 1))
        sum=$(echo "$sum + $val" | bc -l)
        avg=$(echo "$sum / $count" | bc -l)
        printf "%s  |  running avg: %5.1fms (n=%d)\n" "$line" "$avg" "$count"
    fi
done

