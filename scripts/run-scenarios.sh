#!/bin/bash
# One command: S1, S2, S3, S4, then the table. A non-zero guest status
# is recorded in the log and does not hide the later scenarios.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
ssh=(ssh -p 2222 -i "$HOME/.ssh/id_ed25519" -o BatchMode=yes)
scp=(scp -P 2222 -i "$HOME/.ssh/id_ed25519" -o BatchMode=yes)
out="$root/build/scenarios"
mkdir -p "$out"
rm -f "$out"/s1.log "$out"/s2.log "$out"/s3.log "$out"/s4.log \
    "$out"/s4-marks.txt "$out"/panel-s4.log

"${scp[@]}" \
    "$root/scripts/qnx-scenario.sh" \
    "$root/scripts/qnx-s2.sh" \
    "$root/scripts/qnx-s3.sh" \
    "$root/scripts/qnx-s4.sh" \
    root@localhost:/tmp/

guest_setup='chmod +x /tmp/qnx-scenario.sh /tmp/qnx-s2.sh /tmp/qnx-s3.sh /tmp/qnx-s4.sh /tmp/supervisor /tmp/source /tmp/sink /tmp/relay /tmp/telemetry'

run_guest() {
    label=$1
    shift
    echo "=== $label ==="
    set +e
    "${ssh[@]}" root@localhost "$guest_setup; $*" > "$out/$label.log"
    status=$?
    set -e
    echo "scenario-host: guest exit $status" >> "$out/$label.log"
    echo "$label guest exit $status"
}

run_guest s1 "/tmp/qnx-s4.sh 4"
run_guest s2 /tmp/qnx-s2.sh
run_guest s3 /tmp/qnx-s3.sh

echo "=== s4 ==="
/usr/bin/ruby "$root/scripts/host-panel.rb" --no-browser \
    --log="$out/panel-s4.log" >/dev/null 2>&1 &
panel_pid=$!
set +e
"${ssh[@]}" root@localhost "$guest_setup; /tmp/qnx-s4.sh 4" > "$out/s4.log" &
ssh_pid=$!
set -e
i=0
while [ "$i" -lt 80 ]; do
    if grep -q 's4: live seconds' "$out/s4.log" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$ssh_pid" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
    sleep 0.2
done
sleep 2
if kill -0 "$panel_pid" 2>/dev/null; then
    kill "$panel_pid" 2>/dev/null || true
    echo "s4: panel killed" > "$out/s4-marks.txt"
else
    echo "s4: panel was not running" > "$out/s4-marks.txt"
fi
wait "$panel_pid" 2>/dev/null || true
"${ssh[@]}" root@localhost 'slay -Q -f -s SIGKILL telemetry >/dev/null 2>&1 || true'
echo "s4: telemetry killed" >> "$out/s4-marks.txt"
set +e
wait "$ssh_pid"
s4_status=$?
set -e
echo "scenario-host: guest exit $s4_status" >> "$out/s4.log"
cat "$out/s4-marks.txt" >> "$out/s4.log"
echo "s4 guest exit $s4_status"

/usr/bin/ruby "$root/scripts/scenario_report.rb" \
    "$out/s1.log" "$out/s2.log" "$out/s3.log" "$out/s4.log"
