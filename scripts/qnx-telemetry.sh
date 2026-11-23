#!/bin/sh
# Live network with telemetry. The host creates /tmp/telemetry-done
# after it has read snapshots, killed telemetry, and continued it.
set -eu

echo "guest: $(uname -a)"

slay -Q -f supervisor >/dev/null 2>&1 || true
slay -Q -f telemetry >/dev/null 2>&1 || true
slay -Q -f source >/dev/null 2>&1 || true
slay -Q -f relay >/dev/null 2>&1 || true
slay -Q -f sink >/dev/null 2>&1 || true
# qconn ignores SIGTERM and keeps TCP 8000 (F-026). slay's default is SIGTERM.
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
    /tmp/source.out /tmp/telemetry.out /tmp/telemetry-done

/tmp/supervisor -k 3 -u 12 -c 6 -b 6 -g 200 -w 2000 -L >/tmp/supervisor.out 2>&1 &
sup=$!

i=0
while [ "$i" -lt 600 ]; do
    if [ -f /tmp/telemetry-done ]; then
        break
    fi
    if ! kill -0 "$sup" >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.2
done

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
exit "$status"
