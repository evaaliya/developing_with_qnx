#!/bin/bash
# Run scripts/stall-count.sh and, at the same time, sample the Mac: the
# busy share of each core and the CPU time of each QEMU thread every
# PERIOD ms (scripts/host-sample.c). Guest CLOCK_MONOTONIC is tied to
# host wall time before and after by reading `stall-probe now` over one
# ssh session (the smallest host-minus-guest difference is the offset).
# Then each stall the probe saw is matched with the host samples that
# overlap it (F-031).
#   scripts/stall-host.sh SECONDS [LABEL] [PERIOD_MS]
# Run make upload first. Output goes to build/series/stall-host-*/.
set -u

secs=${1:-300}
label=${2:-idle}
period=${3:-20}
case "$secs$period" in
    *[!0-9]*)
        echo "stall-host: SECONDS and PERIOD_MS must be positive integers" >&2
        exit 2
        ;;
esac
case "$label" in
    '' | *[!A-Za-z0-9_-]*)
        echo "stall-host: LABEL may hold letters, digits, - and _" >&2
        exit 2
        ;;
esac
root=$(cd "$(dirname "$0")/.." && pwd)
ssh=(ssh -p 2222 -i "$HOME/.ssh/id_ed25519" -o BatchMode=yes root@localhost)
dir="$root/build/series/stall-host-$label-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$dir"

cc -O2 -Wall -Wextra -o "$root/build/host-sample" "$root/scripts/host-sample.c" || exit 1
pid=$(pgrep -f '^qemu-system-aarch64' | head -1)
if [ -z "$pid" ]; then
    echo "stall-host: no qemu-system-aarch64 process" >&2
    exit 1
fi
ps -o pid=,command= -p "$pid" > "$dir/qemu.txt"

# Each line: host wall ms when the line arrived, then the guest ms.
clock_pairs() {
    "${ssh[@]}" 'i=0; while [ $i -lt 30 ]; do /tmp/stall-probe now; sleep 0.1; i=$((i+1)); done' |
        python3 -I -u -c '
import sys, time
for line in sys.stdin:
    line = line.strip()
    if line.isdigit():
        print(int(time.time() * 1000), line, flush=True)
'
}

clock_pairs > "$dir/clock-before.txt"
"$root/build/host-sample" "$pid" $((secs + 20)) "$period" > "$dir/host.txt" 2> "$dir/host.err" &
sampler=$!
bash "$root/scripts/stall-count.sh" "$secs" "$label" | tee "$dir/stall-count.txt"
probe=$(awk '/probe log in/ { print $NF }' "$dir/stall-count.txt")
kill "$sampler" 2>/dev/null
wait "$sampler" 2>/dev/null
clock_pairs > "$dir/clock-after.txt"
cp "$root/$probe" "$dir/probe.log"

python3 -I "$root/scripts/stall-host.py" "$dir" | tee "$dir/summary.txt"
echo "stall-host: output in ${dir#"$root"/}"
