#!/bin/bash
# Run scenario S2 (live flood, 5 s window) alone N times in a row and
# keep every run's log. For each run: URGENT n and median from
# latency-report, the late check line, and how evenly source issued
# URGENT during the flood: the gaps between URGENT arrivals at relay A
# (the schedule is one URGENT every 20 ms on a 10 ms tick).
#   scripts/s2-series.sh N
# Run make upload first; this script copies only the S2 guest scripts.
set -u

n=${1:-10}
case "$n" in
    '' | *[!0-9]* | 0)
        echo "s2-series: N must be a positive integer" >&2
        exit 2
        ;;
esac
root=$(cd "$(dirname "$0")/.." && pwd)
ssh=(ssh -p 2222 -i "$HOME/.ssh/id_ed25519" -o BatchMode=yes root@localhost)
scp=(scp -P 2222 -i "$HOME/.ssh/id_ed25519" -o BatchMode=yes)
dir="$root/build/series/s2-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$dir"
echo "s2-series: logs in ${dir#"$root"/}" | tee "$dir/summary.txt"

"${scp[@]}" "$root/scripts/qnx-s2.sh" "$root/scripts/qnx-late-check.sh" \
    root@localhost:/tmp/ || exit 1
"${ssh[@]}" 'chmod +x /tmp/qnx-s2.sh /tmp/qnx-late-check.sh /tmp/stall-probe /tmp/source /tmp/sink /tmp/relay' || exit 1

low=0
i=1
while [ "$i" -le "$n" ]; do
    tag=$(printf '%02d' "$i")
    log="$dir/$tag.log"
    "${ssh[@]}" /tmp/qnx-s2.sh > "$log" 2>&1
    status=$?
    urgent=$("$root/build/latency-report" "$log" |
        awk '/^latency class URGENT total/ { print "n " $6 " median " $8 " ns max " $14 " ns" }')
    count=$(echo "$urgent" | awk '{ print $2 }')
    gaps=$(awk '
        /^source: flood signal [0-9]+ at/ { t = $6 + 0; if (!f0) f0 = t; f1 = t }
        /^relay signal-relay-a: sent signal [0-9]+ class URGENT/ {
            for (k = 1; k <= NF; k++) if ($k == "arrived") a = $(k + 1) + 0
            m++; arr[m] = a
        }
        END {
            c = 0; prev = 0; big = 0; max = 0
            for (k = 1; k <= m; k++) {
                if (arr[k] < f0 || arr[k] > f1) continue
                c++
                if (prev) { g = (arr[k] - prev) / 1e6; if (g > max) max = g; if (g > 40) big++ }
                prev = arr[k]
            }
            printf "in flood %d, gaps over 40 ms %d, longest gap %.1f ms", c, big, max
        }' "$log")
    late=$(grep -E '^late-check: late URGENT or COMMAND' "$log" | head -1)
    if [ -z "$count" ] || [ "$count" -lt 100 ]; then
        low=$((low + 1))
    fi
    echo "s2-series: run $tag exit $status; URGENT $urgent; $gaps; ${late:-no late-check line}" |
        tee -a "$dir/summary.txt"
    i=$((i + 1))
done
echo "s2-series: runs with URGENT n below 100: $low of $n" | tee -a "$dir/summary.txt"
[ "$low" -eq 0 ]
