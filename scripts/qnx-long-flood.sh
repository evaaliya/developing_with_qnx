#!/bin/sh
# Runs on the QNX guest. Live source, flood on for FLOOD_S seconds
# (default 60). Closes keep the issued list short while the network runs,
# so the flood holds until it is turned off, URGENT and COMMAND keep
# going, and every signal is accounted (D-062).
set -eu

FLOOD_S=${1:-60}
# No line per signal: a minute of flood would fill /tmp (D-062).
SIGNAL_QUIET=1
export SIGNAL_QUIET

fail() {
    echo "long-flood: FAIL $1" >&2
    slay -Q -f telemetry >/dev/null 2>&1 || true
    slay -Q -f source >/dev/null 2>&1 || true
    slay -Q -f relay >/dev/null 2>&1 || true
    slay -Q -f sink >/dev/null 2>&1 || true
    echo "--- source (tail) ---"
    grep -Ev 'signal [0-9]* class|flood signal' /tmp/source.out 2>/dev/null | tail -20 || true
    echo "--- sink (tail) ---"
    tail -20 /tmp/sink.out 2>/dev/null || true
    exit 1
}

echo "guest: $(uname -a)"

slay -Q -f supervisor >/dev/null 2>&1 || true
slay -Q -f telemetry >/dev/null 2>&1 || true
slay -Q -f source >/dev/null 2>&1 || true
slay -Q -f relay >/dev/null 2>&1 || true
slay -Q -f sink >/dev/null 2>&1 || true
rm -f /tmp/source.out /tmp/sink.out /tmp/relay-a.out /tmp/relay-b.out \
    /tmp/dev-status.txt /tmp/dev-status-again.txt

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
echo "long-flood: flood on at $start, holding for ${FLOOD_S} s"
echo 'flood on' > /dev/signal-source

i=0
while [ "$i" -lt "$FLOOD_S" ]; do
    sleep 1
    i=$((i + 1))
    kill -0 "$source_pid" >/dev/null 2>&1 || fail "source died during the flood"
done
end=$(date +%s)
echo "long-flood: held ${FLOOD_S} s, $start to $end"

# Only small reads while the network runs: reading the large logs here
# goes through the same /tmp server the nodes print to (B21).
cat /dev/signal-source > /tmp/dev-status.txt
echo 'flood off' > /dev/signal-source
echo 'stop' > /dev/signal-source
i=0
while [ "$i" -lt 100 ]; do
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

echo "long-flood: source exit $source_status, sink exit $sink_status"
echo "long-flood: status at the end of the flood"
cat /tmp/dev-status.txt
grep '^flood on$' /tmp/dev-status.txt >/dev/null || fail "the flood did not hold for ${FLOOD_S} s"
if grep -E 'flood stopped|issued list is full' /tmp/source.out >/dev/null 2>&1; then
    grep -E 'flood stopped|issued list is full' /tmp/source.out | head -3
    fail "the issued list filled or the flood stopped itself"
fi
closes=$(grep -c '^source: closed ' /tmp/source.out || true)
echo "long-flood: closes $closes"
[ "$closes" -gt 0 ] || fail "no close came back"
grep '^source: closed ' /tmp/source.out | awk '{ w = $NF; if (w > max) max = w } END { print "long-flood: most numbers waiting to close " max + 0 }'
grep '^source: sent manifest' /tmp/source.out | sed 's/^/long-flood: /'
grep '^source: issued ' /tmp/source.out | sed 's/^/long-flood: /'
df -k /tmp | tail -1 | sed 's/^/long-flood: guest \/tmp /'
ls -l /tmp/source.out /tmp/relay-a.out /tmp/relay-b.out /tmp/sink.out | awk '{ print "long-flood: log " $NF " " $5 " bytes" }'
grep -E '^sink: (summary|unaccounted)' /tmp/sink.out
# Every issued signal is on the sink totals: received + lost + dropped.
for c in URGENT COMMAND BACKGROUND; do
    issued=$(awk -v c="$c" '$1 == "source:" && $2 == "issued" {
        for (i = 3; i < NF; i += 2) if ($i == c) print $(i + 1) }' /tmp/source.out)
    booked=$(awk -v c="$c" '$1 == "sink:" && $2 == "summary" && $3 == c {
        gsub(",", ""); print $5 + $12 + $14 }' /tmp/sink.out)
    echo "long-flood: $c issued $issued, on the sink totals ${booked:-none}"
    [ "$issued" = "${booked:-x}" ] || fail "$c issued $issued but the sink totals hold ${booked:-none}"
done
grep -c 'sink: state DEGRADED' /tmp/sink.out | sed 's/^/long-flood: DEGRADED entries /'
grep 'class URGENT late' /tmp/sink.out || true
[ "$source_status" -eq 0 ] || fail "source exit $source_status"
[ "$sink_status" -eq 0 ] || fail "sink exit $sink_status"
grep 'sink: unaccounted 0' /tmp/sink.out >/dev/null || fail "unaccounted is not 0"
grep 'sink: summary URGENT .*late 0' /tmp/sink.out >/dev/null || fail "URGENT was late"
grep 'sink: summary COMMAND .*late 0' /tmp/sink.out >/dev/null || fail "COMMAND was late"
grep 'sink: summary URGENT .*lost 0, dropped 0' /tmp/sink.out >/dev/null || fail "URGENT was lost or dropped"
grep 'sink: summary COMMAND .*lost 0, dropped 0' /tmp/sink.out >/dev/null || fail "COMMAND was lost or dropped"
if grep 'dropped signal' /tmp/relay-a.out | grep -v 'class BACKGROUND' >/dev/null 2>&1; then
    fail "a dropped signal was not BACKGROUND"
fi
echo "long-flood: PASS"
exit 0
