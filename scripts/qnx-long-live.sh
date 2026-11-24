#!/bin/sh
# Runs on the QNX guest. The live network for RUN_S seconds (default 600):
# flood on twice, mute and restore once, then stop. Closes keep the books
# moving, so the run has no length limit and every signal is accounted
# (D-062). No line per signal, or /tmp (62 MB) would fill.
set -eu

RUN_S=${1:-600}
SIGNAL_QUIET=1
export SIGNAL_QUIET

fail() {
    echo "long-live: FAIL $1" >&2
    slay -Q -f source >/dev/null 2>&1 || true
    slay -Q -f relay >/dev/null 2>&1 || true
    slay -Q -f sink >/dev/null 2>&1 || true
    echo "--- source (tail) ---"
    tail -20 /tmp/source.out 2>/dev/null || true
    echo "--- sink (tail) ---"
    tail -20 /tmp/sink.out 2>/dev/null || true
    exit 1
}

# Commands by second of the run. Two floods of 60 s, one mute of 10 s.
at() {
    case "$1" in
        60) echo 'flood on' ;;
        120) echo 'flood off' ;;
        240) echo 'mute' ;;
        250) echo 'restore' ;;
        300) echo 'flood on' ;;
        360) echo 'flood off' ;;
        *) return 1 ;;
    esac
}

echo "guest: $(uname -a)"
[ "$RUN_S" -ge 370 ] || fail "the run must be at least 370 s to hold every command"

slay -Q -f supervisor >/dev/null 2>&1 || true
slay -Q -f telemetry >/dev/null 2>&1 || true
slay -Q -f source >/dev/null 2>&1 || true
slay -Q -f relay >/dev/null 2>&1 || true
slay -Q -f sink >/dev/null 2>&1 || true
rm -f /tmp/source.out /tmp/sink.out /tmp/relay-a.out /tmp/relay-b.out \
    /tmp/long-live-status.txt /tmp/stall-probe.out

# A probe above every class shows whether the whole guest stopped.
/tmp/qnx-late-check.sh start $((RUN_S + 60)) || fail "the stall probe did not start"
/tmp/sink 64 1500 > /tmp/sink.out 2>&1 &
sink_pid=$!
/tmp/relay signal-relay-b signal-sink > /tmp/relay-b.out 2>&1 &
relay_b=$!
/tmp/relay signal-relay-a signal-relay-b > /tmp/relay-a.out 2>&1 &
relay_a=$!
/tmp/source live signal-relay-a > /tmp/source.out 2>&1 &
source_pid=$!

i=0
while [ "$i" -lt 30 ]; do
    if grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.2
done
grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1 || fail "sink did not reach NORMAL"

start=$(date +%s)
echo "long-live: start $start, running ${RUN_S} s"
t=0
while [ "$t" -lt "$RUN_S" ]; do
    sleep 1
    t=$(( $(date +%s) - start ))
    for s in 60 120 240 250 300 360; do
        if [ "$t" -ge "$s" ] && ! grep "^$s " /tmp/long-live-status.txt >/dev/null 2>&1; then
            cmd=$(at "$s")
            echo "$cmd" > /dev/signal-source
            mode=$(grep '^mode ' /dev/signal-source | cut -d' ' -f2)
            echo "$s $cmd mode $mode at $t" >> /tmp/long-live-status.txt
        fi
    done
    kill -0 "$source_pid" >/dev/null 2>&1 || fail "source died at $t s"
    kill -0 "$sink_pid" >/dev/null 2>&1 || fail "sink died at $t s"
done
end=$(date +%s)
echo 'stop' > /dev/signal-source
i=0
while [ "$i" -lt 150 ]; do
    kill -0 "$source_pid" >/dev/null 2>&1 || break
    i=$((i + 1))
    sleep 0.2
done
set +e
wait "$source_pid"
source_status=$?
wait "$sink_pid"
sink_status=$?
slay -Q -f relay >/dev/null 2>&1 || true
wait "$relay_a" >/dev/null 2>&1 || true
wait "$relay_b" >/dev/null 2>&1 || true
set -e

grep -E 'class (URGENT|COMMAND) late|state (SAFE|RECOVERING)' /tmp/sink.out | sed 's/^/long-live: /' || true
echo "long-live: ran $((end - start)) s, source exit $source_status, sink exit $sink_status"
sed 's/^/long-live: command /' /tmp/long-live-status.txt
closes=$(grep -c '^source: closed ' /tmp/source.out || true)
echo "long-live: closes $closes"
grep '^source: closed ' /tmp/source.out | awk '{ w = $NF; if (w > max) max = w } END { print "long-live: most numbers waiting to close " max + 0 }'
grep -E '^source: (sent manifest|sent [0-9]+ signals|issued )' /tmp/source.out | sed 's/^/long-live: /'
grep -E '^sink: (summary|unaccounted)' /tmp/sink.out
grep -c 'sink: state SAFE' /tmp/sink.out | sed 's/^/long-live: SAFE entries /'
grep -c 'sink: state DEGRADED' /tmp/sink.out | sed 's/^/long-live: DEGRADED entries /'
grep -c '^sink: lost signal' /tmp/sink.out | sed 's/^/long-live: lost lines /'
df -k /tmp | tail -1 | sed 's/^/long-live: guest \/tmp /'
ls -l /tmp/source.out /tmp/relay-a.out /tmp/relay-b.out /tmp/sink.out | awk '{ print "long-live: log " $NF " " $5 " bytes" }'

[ "$source_status" -eq 0 ] || fail "source exit $source_status"
[ "$sink_status" -eq 0 ] || fail "sink exit $sink_status"
[ "$((end - start))" -ge "$RUN_S" ] || fail "the run was shorter than ${RUN_S} s"
[ "$(wc -l < /tmp/long-live-status.txt | tr -d ' ')" -eq 6 ] || fail "not every command was sent"
grep '^60 flood on mode flood' /tmp/long-live-status.txt >/dev/null || fail "the first flood did not start"
grep '^300 flood on mode flood' /tmp/long-live-status.txt >/dev/null || fail "the second flood did not start"
grep '^240 mute mode mute' /tmp/long-live-status.txt >/dev/null || fail "mute did not take"
grep '^250 restore mode normal' /tmp/long-live-status.txt >/dev/null || fail "restore did not take"
if grep -E 'flood stopped|issued list is full' /tmp/source.out >/dev/null 2>&1; then
    fail "the issued list filled or the flood stopped itself"
fi
grep 'sink: unaccounted 0' /tmp/sink.out >/dev/null || fail "unaccounted is not 0"
grep 'sink: summary URGENT .*lost 0, dropped 0' /tmp/sink.out >/dev/null ||
    fail "URGENT was lost or dropped"
grep 'sink: summary COMMAND .*lost 0, dropped 0' /tmp/sink.out >/dev/null ||
    fail "COMMAND was lost or dropped"
# Each late URGENT or COMMAND against the probe: was the whole guest
# stopped while the signal was on its way? (qnx-late-check.sh, D-064)
/tmp/qnx-late-check.sh check long-live ||
    fail "a late URGENT or COMMAND was not inside a guest stall"
for c in URGENT COMMAND BACKGROUND; do
    issued=$(awk -v c="$c" '$1 == "source:" && $2 == "issued" {
        for (i = 3; i < NF; i += 2) if ($i == c) print $(i + 1) }' /tmp/source.out)
    booked=$(awk -v c="$c" '$1 == "sink:" && $2 == "summary" && $3 == c {
        gsub(",", ""); print $5 + $12 + $14 }' /tmp/sink.out)
    echo "long-live: $c issued $issued, on the sink totals ${booked:-none}"
    [ "$issued" = "${booked:-x}" ] || fail "$c issued $issued but the sink totals hold ${booked:-none}"
done
echo "long-live: PASS"
exit 0
