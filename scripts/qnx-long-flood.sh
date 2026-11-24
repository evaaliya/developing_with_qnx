#!/bin/sh
# Runs on the QNX guest. Live source, flood on for at least 20 s.
# The flood stops itself when it reaches its share of the issued list;
# URGENT and COMMAND keep going and every signal is accounted (D-061).
set -eu

FLOOD_S=${1:-20}

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
if echo 'flood on' > /dev/signal-source 2>/dev/null; then
    again=accepted
else
    again=refused
fi
cat /dev/signal-source > /tmp/dev-status-again.txt
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
budget=$(grep 'flood budget used' /tmp/source.out || true)
[ -n "$budget" ] || fail "the flood never reached its budget"
echo "long-flood: $budget"
echo "long-flood: status after the budget"
cat /tmp/dev-status.txt
grep '^flood off$' /tmp/dev-status.txt >/dev/null || fail "flood is still on after the budget"
echo "long-flood: flood on after the budget $again; $(grep '^error ' /tmp/dev-status-again.txt)"
[ "$again" = refused ] || fail "flood on was accepted after the budget"
grep '^error flood budget used$' /tmp/dev-status-again.txt >/dev/null ||
    fail "status does not say the budget is used"
# URGENT after the budget line: the schedule did not stop with the flood.
after=$(awk '/flood budget used/ { seen = 1; next }
             seen && /sent signal [0-9]* class URGENT/ { n++ }
             END { print n + 0 }' /tmp/source.out)
echo "long-flood: URGENT sent after the budget $after"
[ "$after" -gt 0 ] || fail "no URGENT after the budget"
if grep 'issued list is full' /tmp/source.out >/dev/null 2>&1; then
    fail "the issued list filled"
fi
grep -c 'class BACKGROUND' /tmp/source.out | sed 's/^/long-flood: BACKGROUND sent lines /'
grep 'flood batch' /tmp/source.out | wc -l | sed 's/^ */long-flood: flood batches /'
grep 'sent manifest' /tmp/source.out || true
grep -E '^sink: (summary|unaccounted)' /tmp/sink.out
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
