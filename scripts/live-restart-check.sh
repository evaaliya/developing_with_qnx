#!/bin/bash
# Host side of run-live-restart (D-063). Starts the panel, runs the
# guest script, and reads the panel's /text every 2 s while the network
# runs. Afterwards it checks that the panel showed each restart, books
# kept and not reset, and at the end issued equal to the sink totals.
#   live-restart-check.sh RUN_S PLAN
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUN_S=${1:-300}
PLAN=${2:-all}
PAGE="http://127.0.0.1:8765"
SSH=(ssh -p 2222 -i "$HOME/.ssh/id_ed25519" -o BatchMode=yes root@localhost)
OUT="$ROOT/build/live-restart"
mkdir -p "$OUT"
rm -f "$OUT/panel-samples.txt" "$OUT/guest.log" "$OUT/panel.out"

panel_pid=
if curl -s -m 1 "$PAGE/text" >/dev/null 2>&1; then
    echo "live-restart-check: a panel is already serving $PAGE; using it"
else
    /usr/bin/ruby "$ROOT/scripts/host-panel.rb" > "$OUT/panel.out" 2>&1 &
    panel_pid=$!
    for _ in $(seq 1 50); do
        curl -s -m 1 "$PAGE/text" >/dev/null 2>&1 && break
        sleep 0.1
    done
fi

"${SSH[@]}" "chmod +x /tmp/qnx-live-restart.sh /tmp/stall-probe /tmp/supervisor /tmp/telemetry /tmp/source /tmp/sink /tmp/relay; /tmp/qnx-live-restart.sh $RUN_S $PLAN" \
    > "$OUT/guest.log" 2>&1 &
guest=$!

while kill -0 "$guest" 2>/dev/null; do
    {
        echo "=== $(date '+%H:%M:%S')"
        curl -s -m 2 "$PAGE/text" || echo "panel did not answer"
    } >> "$OUT/panel-samples.txt"
    sleep 2
done
wait "$guest"
guest_status=$?
# The final reports of source and sink reach the panel after the stop.
sleep 1
{
    echo "=== $(date '+%H:%M:%S') after the stop"
    curl -s -m 2 "$PAGE/text" || echo "panel did not answer"
} >> "$OUT/panel-samples.txt"
if [ -n "$panel_pid" ]; then
    kill "$panel_pid" 2>/dev/null
    wait "$panel_pid" 2>/dev/null
fi

cat "$OUT/guest.log"

failures=0
check() {
    if [ "$1" -eq 0 ]; then
        echo "live-restart-check: ok   $2"
    else
        echo "live-restart-check: FAIL $2"
        failures=$((failures + 1))
    fi
}

samples=$(grep -c '^===' "$OUT/panel-samples.txt")
echo "live-restart-check: panel samples $samples in $OUT/panel-samples.txt"
if [ "$PLAN" = all ]; then nodes="relay-a relay-b sink source"; else nodes=$PLAN; fi
for n in $nodes; do
    case "$n" in
        relay-a) label="relay A" ;;
        relay-b) label="relay B" ;;
        *) label=$n ;;
    esac
    first=$(grep -n -E "(^map: |→ )$label \((alive|dead), restarts 1\)" "$OUT/panel-samples.txt" | head -1 | cut -d: -f1)
    [ -n "$first" ]; check $? "the panel showed $label with restarts 1"
done
grep -q 'books reset' "$OUT/panel-samples.txt"; [ $? -ne 0 ]; check $? "the panel never showed books reset"
if [ "$PLAN" = all ] || [ "$PLAN" = source ]; then
    grep -q 'books kept across restarts: source 1,' "$OUT/panel-samples.txt"; check $? "the panel showed the source book kept"
fi
if [ "$PLAN" = all ] || [ "$PLAN" = sink ]; then
    grep -q 'books kept across restarts: source [0-9]*, sink 1;' "$OUT/panel-samples.txt"; check $? "the panel showed the sink books kept"
fi
# Telemetry stops with the network, so the last sample may be stale.
# The last current one must hold every issued number: accounted, or
# still waiting to be closed.
last=$(awk '/^=== / { if (cur != "" && cur ~ /\nmap: /) keep = cur; cur = $0; next }
            { cur = cur "\n" $0 }
            END { if (cur ~ /\nmap: /) keep = cur; print keep }' "$OUT/panel-samples.txt")
echo "--- last current panel sample ---"
echo "$last"
waiting=$(echo "$last" | sed -n 's/.*; \([0-9]*\) issued numbers wait to be closed/\1/p')
# source publishes its issued count after MsgSend returns, and relay A
# answers on receipt, so the sink may publish a signal as accounted
# before source publishes it as issued. Each sending thread has at most
# its last send unpublished: URGENT and COMMAND come from the schedule
# thread (1), BACKGROUND also from one flood batch (72 + 1) (D-067).
for c in URGENT COMMAND BACKGROUND; do
    case "$c" in BACKGROUND) lead=73 ;; *) lead=1 ;; esac
    line=$(echo "$last" | grep "^class $c issued ")
    # class X issued I on time O late L lost S dropped D ...
    gap=$(echo "$line" | awk '{ print $4 - ($7 + $9 + $11 + $13) }')
    echo "live-restart-check: panel $c issued minus accounted ${gap:-none}, waiting ${waiting:-none}, accounted may lead by $lead"
    [ -n "$gap" ] && [ -n "$waiting" ] && [ "$gap" -ge "-$lead" ] && [ "$gap" -le "$waiting" ]
    check $? "panel: every issued $c is accounted or waits to be closed"
done

if [ "$guest_status" -ne 0 ] || [ "$failures" -ne 0 ]; then
    echo "live-restart-check: FAIL (guest $guest_status, host checks failed $failures)"
    exit 1
fi
echo "live-restart-check: PASS"
exit 0
