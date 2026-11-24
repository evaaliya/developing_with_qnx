#!/bin/sh
# Live network for a fixed number of seconds after NORMAL, then stop.
# Two runs with the same argument have the same live length (S4).
set -eu

seconds=${1:-}
case "$seconds" in
    ''|*[!0-9]*|0)
        echo "usage: qnx-s4.sh SECONDS" >&2
        exit 2
        ;;
esac

echo "guest: $(uname -a)"

slay -Q -f supervisor >/dev/null 2>&1 || true
slay -Q -f telemetry >/dev/null 2>&1 || true
slay -Q -f source >/dev/null 2>&1 || true
slay -Q -f relay >/dev/null 2>&1 || true
slay -Q -f sink >/dev/null 2>&1 || true
slay -Q -f -s SIGKILL qconn >/dev/null 2>&1 || true
i=0
while [ "$i" -lt 20 ]; do
    if ! netstat -an | grep -q '[*][.]8000'; then
        break
    fi
    i=$((i + 1))
    sleep 0.1
done

rm -f /tmp/supervisor.out /tmp/sink.out /tmp/relay-a.out /tmp/relay-b.out \
    /tmp/source.out /tmp/telemetry.out

/tmp/qnx-late-check.sh start 120 || exit 1
/tmp/supervisor -k 3 -u 12 -c 6 -b 6 -w 2000 -L >/tmp/supervisor.out 2>&1 &
sup=$!

i=0
while [ "$i" -lt 50 ]; do
    if grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.2
done
if ! grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1; then
    echo "s4: sink did not reach NORMAL" >&2
    kill -KILL "$sup" >/dev/null 2>&1 || true
    wait "$sup" >/dev/null 2>&1 || true
    echo "--- supervisor ---"
    cat /tmp/supervisor.out 2>/dev/null || true
    echo "--- sink ---"
    cat /tmp/sink.out 2>/dev/null || true
    exit 1
fi

if [ ! -e /dev/signal-source ]; then
    echo "live: path missing" >&2
    kill -KILL "$sup" >/dev/null 2>&1 || true
    wait "$sup" >/dev/null 2>&1 || true
    echo "--- supervisor ---"
    cat /tmp/supervisor.out 2>/dev/null || true
    echo "--- source ---"
    cat /tmp/source.out 2>/dev/null || true
    exit 1
fi
echo "live: path /dev/signal-source"
ls -l /dev/signal-source

echo "s4: live seconds $seconds"
sleep "$seconds"
echo "s4: stop"
if [ -e /dev/signal-source ]; then
    echo stop > /dev/signal-source || true
fi

set +e
wait "$sup"
status=$?
set -e

echo "--- supervisor (exit $status) ---"
cat /tmp/supervisor.out 2>/dev/null || true
echo "--- telemetry ---"
cat /tmp/telemetry.out 2>/dev/null || true
echo "--- source ---"
cat /tmp/source.out 2>/dev/null || true
echo "--- relay A ---"
cat /tmp/relay-a.out 2>/dev/null || true
echo "--- relay B ---"
cat /tmp/relay-b.out 2>/dev/null || true
echo "--- sink ---"
cat /tmp/sink.out 2>/dev/null || true
# The scenario report reads these lines; its rows decide (D-064).
/tmp/qnx-late-check.sh check late-check || true
exit "$status"
