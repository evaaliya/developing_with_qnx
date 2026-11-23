#!/bin/sh
# Counted calm chain under the supervisor, so the host panel can read it.
# The counts are the same 3 / 3 / 3 as make run. make run itself does not
# publish telemetry (D-054, D-056).
set -eu

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

/tmp/supervisor -k 3 -u 3 -c 3 -b 3 -w 2000 >/tmp/supervisor.out 2>&1 &
sup=$!

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
