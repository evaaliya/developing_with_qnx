#!/bin/bash
# Run stall-probe alone on the guest for SECONDS with no network running
# and count the guest stalls it saw: how many over 5, 10, 20, 50 ms,
# the worst, and the total stall time. Used to compare guest set-ups
# (for example QEMU -smp 1 against -smp 2) over the same time.
#   scripts/stall-count.sh SECONDS [LABEL]
# Run make upload first. The probe output is kept in build/series/.
set -u

secs=${1:-300}
label=${2:-idle}
case "$secs" in
    '' | *[!0-9]* | 0)
        echo "stall-count: SECONDS must be a positive integer" >&2
        exit 2
        ;;
esac
case "$label" in
    '' | *[!A-Za-z0-9_-]*)
        echo "stall-count: LABEL may hold letters, digits, - and _" >&2
        exit 2
        ;;
esac
root=$(cd "$(dirname "$0")/.." && pwd)
ssh=(ssh -p 2222 -i "$HOME/.ssh/id_ed25519" -o BatchMode=yes root@localhost)
dir="$root/build/series/stall-count-$label-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$dir"
log="$dir/probe.log"

"${ssh[@]}" 'for p in supervisor telemetry source relay sink stall-probe; do slay -Q -f "$p" >/dev/null 2>&1 || true; done'
"${ssh[@]}" "uname -a; pidin info | grep -E '^(CPU|Processor)'; /tmp/stall-probe $secs 5" > "$log" 2>&1
status=$?
awk -v secs="$secs" -v label="$label" '
    /^stall-probe: woke [0-9]+ ns late/ {
        ms = $3 / 1e6; n++; sum += ms
        if (ms > 10) a++
        if (ms > 20) b++
        if (ms > 50) c++
        if (ms > worst) worst = ms
    }
    /^stall-probe: wakes / { summary = $0 }
    END {
        printf "stall-count %s: %d s, stalls over 5 ms %d, over 10 ms %d, over 20 ms %d, over 50 ms %d, worst %.1f ms, stall time %.0f ms\n", label, secs, n, a, b, c, worst, sum
        if (summary != "") print summary
    }' "$log" | tee "$dir/summary.txt"
echo "stall-count: probe log in ${log#"$root"/}"
exit "$status"
