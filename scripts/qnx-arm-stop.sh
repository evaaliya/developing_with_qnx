#!/bin/sh
# One arm stop on the live network (D-080, D-082). The arm starts at
# home; "arm eye" sends it toward the eye at 250 mm/s; "person in" comes
# at a random moment 200-560 ms into the approach. The sink prints one
# "sink: arm-stop" line with the delay from the event to the brake and
# both paths. Quiet logs (SIGNAL_QUIET=1): no line per signal.
# BINDIR holds source, relay and sink: /tmp for our network, /tmp/naive
# for the naive one (D-084). The run passes when the nodes exit 0, every
# signal is accounted, and the track of the approach ended with a stop
# or a touch of the eye (D-085); the report judges the stop.
#   qnx-arm-stop.sh calm|flood [run-number] [BINDIR]
set -eu

mode=${1:-calm}
case "$mode" in
    calm | flood) ;;
    *)
        echo "arm-stop: mode must be calm or flood" >&2
        exit 2
        ;;
esac
run=${2:-1}
bindir=${3:-/tmp}
for p in source relay sink; do
    [ -x "$bindir/$p" ] || { echo "arm-stop: $bindir/$p is missing" >&2; exit 2; }
done

fail() {
    echo "arm-stop: $1" >&2
    slay -Q -f source >/dev/null 2>&1 || true
    slay -Q -f relay >/dev/null 2>&1 || true
    slay -Q -f sink >/dev/null 2>&1 || true
    echo "--- source ---"
    cat /tmp/source.out 2>/dev/null || true
    echo "--- relay A ---"
    cat /tmp/relay-a.out 2>/dev/null || true
    echo "--- relay B ---"
    cat /tmp/relay-b.out 2>/dev/null || true
    echo "--- sink ---"
    cat /tmp/sink.out 2>/dev/null || true
    exit 1
}

echo "guest: $(uname -a)"
echo "arm-stop: mode $mode run $run bindir $bindir"

slay -Q -f supervisor >/dev/null 2>&1 || true
slay -Q -f telemetry >/dev/null 2>&1 || true
slay -Q -f source >/dev/null 2>&1 || true
slay -Q -f relay >/dev/null 2>&1 || true
slay -Q -f sink >/dev/null 2>&1 || true
rm -f /tmp/source.out /tmp/sink.out /tmp/relay-a.out /tmp/relay-b.out

export SIGNAL_QUIET=1
/tmp/qnx-late-check.sh start 60 || fail "the stall probe did not start"
"$bindir/sink" 64 2000 > /tmp/sink.out 2>&1 &
sink_pid=$!
"$bindir/relay" signal-relay-b signal-sink > /tmp/relay-b.out 2>&1 &
relay_b=$!
"$bindir/relay" signal-relay-a signal-relay-b > /tmp/relay-a.out 2>&1 &
relay_a=$!
"$bindir/source" live signal-relay-a > /tmp/source.out 2>&1 &
source_pid=$!

i=0
while [ "$i" -lt 20 ] && [ ! -e /dev/signal-source ]; do
    i=$((i + 1))
    sleep 0.2
done
[ -e /dev/signal-source ] || fail "path missing"
i=0
while [ "$i" -lt 30 ]; do
    if grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.2
done
grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1 ||
    fail "sink did not reach NORMAL"

echo 'arm home' > /dev/signal-source
if [ "$mode" = flood ]; then
    echo 'flood on' > /dev/signal-source
    echo "arm-stop: flood on"
fi
# The flood fills the relay queues before the approach starts.
sleep 1
# 200..560 ms into the approach; the arm is then about 150..60 mm out.
delay_ms=$((200 + RANDOM % 361))
pause=$(printf '0.%03d' "$delay_ms")
echo "arm-stop: approach, person in after $delay_ms ms"
echo 'arm eye' > /dev/signal-source
sleep "$pause"
echo 'person in' > /dev/signal-source
# The brake takes 200 ms from 250 mm/s; then the person leaves.
sleep 0.6
echo 'person out' > /dev/signal-source
echo 'arm home' > /dev/signal-source
if [ "$mode" = flood ]; then
    echo 'flood off' > /dev/signal-source
fi
sleep 0.5
echo 'stop' > /dev/signal-source

i=0
while [ "$i" -lt 30 ] && kill -0 "$source_pid" >/dev/null 2>&1; do
    i=$((i + 1))
    sleep 0.2
done
kill -0 "$source_pid" >/dev/null 2>&1 && fail "source did not exit after stop"

set +e
wait "$source_pid"
source_status=$?
wait "$sink_pid"
sink_status=$?
slay -Q -f relay >/dev/null 2>&1 || true
wait "$relay_a" >/dev/null 2>&1 || true
wait "$relay_b" >/dev/null 2>&1 || true
set -e

echo "--- source (exit $source_status) ---"
cat /tmp/source.out
echo "--- relay A ---"
cat /tmp/relay-a.out
echo "--- relay B ---"
cat /tmp/relay-b.out
echo "--- sink (exit $sink_status) ---"
cat /tmp/sink.out
/tmp/qnx-late-check.sh check late-check || true

[ "$source_status" -eq 0 ] || exit "$source_status"
[ "$sink_status" -eq 0 ] || exit "$sink_status"
n=$(grep -c '^sink: arm-stop ' /tmp/sink.out || true)
if [ "$n" -gt 1 ]; then
    echo "arm-stop: expected at most one arm-stop line, found $n" >&2
    exit 1
fi
grep -E '^sink: arm-track start_ns .* end (stop|touch) ' /tmp/sink.out >/dev/null 2>&1 ||
    { echo "arm-stop: the track of the approach did not end with a stop or a touch" >&2; exit 1; }
grep 'sink: unaccounted 0' /tmp/sink.out >/dev/null 2>&1 ||
    { echo "arm-stop: sink has no unaccounted 0" >&2; exit 1; }
exit 0
