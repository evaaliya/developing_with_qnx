#!/bin/sh
# Live BACKGROUND flood for a fixed window. Each batch is larger than
# the ordinary relay queue and repeats until flood is turned off (D-059).
# This is not the counted pack used by make run-flood.
set -eu

# 5 s at a 20 ms URGENT period is at least 100 URGENT, so p99 has a sample.
WINDOW_S=5

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

/tmp/qnx-late-check.sh start 120 || fail "the stall probe did not start"
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

# Receive threads wait for the next message at URGENT priority (D-068,
# D-073). A thread that waits at 10 stays READY behind the flood work at
# 12 before it reaches MsgReceive, and an URGENT sent to it waits too.
k=0
low=""
while [ "$k" -lt 5 ]; do
    low="$low$(pidin | awk -v a="$relay_a" -v b="$relay_b" -v s="$source_pid" \
        '($1 == a || $1 == b || $1 == s) && $2 == 1 && $4 ~ /^10/ {
            who = ($1 == a) ? "relay A" : ($1 == b) ? "relay B" : "source"
            printf " %s %s %s;", who, $4, $5 }')"
    k=$((k + 1))
    sleep 0.1
done
if [ -n "$low" ]; then
    fail "a receive thread rests at priority 10:$low"
fi
echo "s2: receive threads of source and both relays rest above priority 10 (5 snapshots)"

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
# The scenario report reads these lines; its rows decide (D-064).
/tmp/qnx-late-check.sh check late-check || true

if [ "$source_status" -ne 0 ]; then
    exit "$source_status"
fi
if [ "$sink_status" -ne 0 ]; then
    exit "$sink_status"
fi
exit 0
