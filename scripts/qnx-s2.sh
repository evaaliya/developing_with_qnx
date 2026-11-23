#!/bin/sh
# Live BACKGROUND flood for a fixed window. The burst repeats on the
# source period until flood is turned off (D-057). This is not the
# counted pack used by make run-flood.
set -eu

WINDOW_S=2

fail() {
    echo "s2: $1" >&2
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

slay -Q -f supervisor >/dev/null 2>&1 || true
slay -Q -f telemetry >/dev/null 2>&1 || true
slay -Q -f source >/dev/null 2>&1 || true
slay -Q -f relay >/dev/null 2>&1 || true
slay -Q -f sink >/dev/null 2>&1 || true

rm -f /tmp/source.out /tmp/sink.out /tmp/relay-a.out /tmp/relay-b.out

/tmp/sink 64 2000 > /tmp/sink.out 2>&1 &
sink_pid=$!
/tmp/relay signal-relay-b signal-sink > /tmp/relay-b.out 2>&1 &
relay_b=$!
/tmp/relay signal-relay-a signal-relay-b > /tmp/relay-a.out 2>&1 &
relay_a=$!
/tmp/source live signal-relay-a > /tmp/source.out 2>&1 &
source_pid=$!

i=0
while [ "$i" -lt 20 ]; do
    if [ -e /dev/signal-source ]; then
        break
    fi
    i=$((i + 1))
    sleep 0.2
done
if [ ! -e /dev/signal-source ]; then
    fail "path missing"
fi
echo "s2: path /dev/signal-source"

i=0
while [ "$i" -lt 30 ]; do
    if grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.2
done
if ! grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1; then
    fail "sink did not reach NORMAL"
fi

echo "s2: flood on"
echo 'flood on' > /dev/signal-source
echo "s2: window start $(date +%s.%N)"
echo "s2: window ms $((WINDOW_S * 1000))"
sleep "$WINDOW_S"
echo "s2: window end $(date +%s.%N)"
echo 'flood off' > /dev/signal-source
sleep 0.3
echo 'stop' > /dev/signal-source

i=0
while [ "$i" -lt 30 ]; do
    if ! kill -0 "$source_pid" >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.2
done
if kill -0 "$source_pid" >/dev/null 2>&1; then
    fail "source did not exit after stop"
fi

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

if [ "$source_status" -ne 0 ]; then
    exit "$source_status"
fi
if [ "$sink_status" -ne 0 ]; then
    exit "$sink_status"
fi
exit 0
