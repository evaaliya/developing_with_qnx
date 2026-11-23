#!/bin/sh
# Runs on the QNX guest. Live source: path, commands, flood, mute.
set -eu

fail() {
    echo "scenario: $1" >&2
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

show_status() {
    label=$1
    cat /dev/signal-source > /tmp/dev-status.txt
    echo "scenario: $label"
    cat /tmp/dev-status.txt
}

echo "guest: $(uname -a)"

slay -Q -f supervisor >/dev/null 2>&1 || true
slay -Q -f source >/dev/null 2>&1 || true
slay -Q -f relay >/dev/null 2>&1 || true
slay -Q -f sink >/dev/null 2>&1 || true
rm -f /tmp/source.out /tmp/sink.out /tmp/relay-a.out /tmp/relay-b.out /tmp/dev-status.txt /tmp/bad-command.err

/tmp/sink 64 1500 > /tmp/sink.out 2>&1 &
sink_pid=$!
/tmp/relay signal-relay-b signal-sink > /tmp/relay-b.out 2>&1 &
relay_b=$!
/tmp/relay signal-relay-a signal-relay-b -q 4 > /tmp/relay-a.out 2>&1 &
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
    fail "/dev/signal-source is missing after start"
fi
echo "scenario: path after start"
ls -l /dev/signal-source
show_status "cat after start"
grep '^emission live$' /tmp/dev-status.txt >/dev/null
grep '^mode normal$' /tmp/dev-status.txt >/dev/null

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

echo "scenario: command flood on"
echo 'flood on' > /dev/signal-source
show_status "cat during flood"
grep '^mode flood$' /tmp/dev-status.txt >/dev/null
grep '^flood on$' /tmp/dev-status.txt >/dev/null

i=0
while [ "$i" -lt 40 ]; do
    if grep 'dropped signal' /tmp/relay-a.out >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done
if ! grep 'dropped signal' /tmp/relay-a.out >/dev/null 2>&1; then
    fail "flood dropped nothing"
fi
if grep 'dropped signal' /tmp/relay-a.out | grep -v 'class BACKGROUND' >/dev/null 2>&1; then
    fail "a dropped signal during flood was not BACKGROUND"
fi

echo "scenario: command flood off"
echo 'flood off' > /dev/signal-source
show_status "cat after flood off"
grep '^flood off$' /tmp/dev-status.txt >/dev/null
grep '^mode normal$' /tmp/dev-status.txt >/dev/null

sleep 0.4
mark=$(wc -l < /tmp/relay-a.out | tr -d ' ')
sleep 1
if tail -n +$((mark + 1)) /tmp/relay-a.out | grep 'dropped signal' >/dev/null 2>&1; then
    fail "drops continued after flood off"
fi
if grep 'dropped signal' /tmp/relay-a.out | grep -v 'class BACKGROUND' >/dev/null 2>&1; then
    fail "a dropped signal was not BACKGROUND"
fi
if grep 'sink: state SAFE' /tmp/sink.out >/dev/null 2>&1; then
    fail "SAFE before urgent was stopped"
fi
if ! kill -0 "$source_pid" >/dev/null 2>&1; then
    fail "source died before urgent was stopped"
fi

echo "scenario: command urgent off"
echo 'urgent off' > /dev/signal-source
show_status "cat while urgent is off"
grep '^mode mute$' /tmp/dev-status.txt >/dev/null
grep '^urgent off$' /tmp/dev-status.txt >/dev/null

i=0
while [ "$i" -lt 24 ]; do
    if grep 'sink: state SAFE' /tmp/sink.out >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.25
done
if ! grep 'sink: state SAFE' /tmp/sink.out >/dev/null 2>&1; then
    fail "stopping urgent did not reach SAFE"
fi
if ! kill -0 "$source_pid" >/dev/null 2>&1; then
    fail "source died when urgent stopped"
fi
echo "scenario: source still alive pid $source_pid"

echo "scenario: command restore"
echo 'restore' > /dev/signal-source
show_status "cat after restore"
grep '^urgent on$' /tmp/dev-status.txt >/dev/null

i=0
restored=0
while [ "$i" -lt 40 ]; do
    if awk '
        /sink: state SAFE/ { safe = 1 }
        safe && /sink: state NORMAL/ { found = 1 }
        END { exit found ? 0 : 1 }
    ' /tmp/sink.out; then
        restored=1
        break
    fi
    i=$((i + 1))
    sleep 0.2
done
if [ "$restored" -ne 1 ]; then
    fail "restore did not reach NORMAL"
fi

echo "scenario: bad command"
if echo 'nope' > /dev/signal-source 2>/tmp/bad-command.err; then
    fail "bad command was accepted"
fi
echo "scenario: bad command reply"
cat /tmp/bad-command.err
show_status "cat after bad command"
if ! grep '^error unknown command$' /tmp/dev-status.txt >/dev/null; then
    fail "status did not show the unknown command"
fi
if ! kill -0 "$source_pid" >/dev/null 2>&1; then
    fail "source died after a bad command"
fi
echo "scenario: source still alive after the bad command pid $source_pid"

echo "scenario: command stop"
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
if ! grep 'sink: unaccounted 0' /tmp/sink.out >/dev/null; then
    fail "unaccounted is not 0"
fi
if ! grep 'sink: summary URGENT .*late 0' /tmp/sink.out >/dev/null; then
    fail "URGENT was late"
fi
if ! grep 'sink: summary COMMAND .*late 0' /tmp/sink.out >/dev/null; then
    fail "COMMAND was late"
fi
if grep 'dropped signal' /tmp/relay-a.out | grep -v 'class BACKGROUND' >/dev/null 2>&1; then
    fail "a dropped signal was not BACKGROUND"
fi

exit 0
