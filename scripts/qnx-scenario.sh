#!/bin/sh
# Runs on the QNX guest. kill: one death of relay B, then recovery.
# lock: K deaths in a row, then LOCKED_SAFE. K is chosen here.
set -eu

mode=${1:-}
case "$mode" in
kill) k=3 ;;
lock) k=2 ;;
*)
    echo "usage: qnx-scenario.sh kill|lock" >&2
    exit 2
    ;;
esac

echo "guest: $(uname -a)"

slay -Q -f supervisor >/dev/null 2>&1 || true
slay -Q -f source >/dev/null 2>&1 || true
slay -Q -f relay >/dev/null 2>&1 || true
slay -Q -f sink >/dev/null 2>&1 || true

rm -f /tmp/supervisor.out /tmp/sink.out /tmp/relay-a.out /tmp/relay-b.out /tmp/source.out

/tmp/supervisor -k "$k" -u 12 -c 6 -b 6 -g 200 -w 2000 >/tmp/supervisor.out 2>&1 &
sup=$!

i=0
pid=
while [ "$i" -lt 20 ]; do
    if grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1; then
        pid=$(grep 'supervisor: node relay-b pid=' /tmp/supervisor.out | tail -1 | sed 's/.*pid=//')
        break
    fi
    i=$((i + 1))
    sleep 1
done

if [ -z "$pid" ]; then
    echo "scenario: relay-b was not ready"
    kill -KILL "$sup" >/dev/null 2>&1 || true
    wait "$sup" >/dev/null 2>&1 || true
    echo "--- supervisor ---"
    cat /tmp/supervisor.out 2>/dev/null || true
    echo "--- sink ---"
    cat /tmp/sink.out 2>/dev/null || true
    exit 1
fi

echo "scenario: killing relay-b pid $pid"
kill -KILL "$pid"

if [ "$mode" = "lock" ]; then
    j=0
    pid2=
    while [ "$j" -lt 15 ]; do
        if grep 'supervisor: restarted relay-b pid=' /tmp/supervisor.out >/dev/null 2>&1; then
            pid2=$(grep 'supervisor: restarted relay-b pid=' /tmp/supervisor.out | tail -1 | sed 's/.*pid=//')
            break
        fi
        j=$((j + 1))
        sleep 1
    done
    if [ -z "$pid2" ]; then
        echo "scenario: relay-b was not restarted"
        kill -KILL "$sup" >/dev/null 2>&1 || true
        wait "$sup" >/dev/null 2>&1 || true
        echo "--- supervisor ---"
        cat /tmp/supervisor.out 2>/dev/null || true
        exit 1
    fi
    echo "scenario: killing restarted relay-b pid $pid2"
    kill -KILL "$pid2"
fi

set +e
wait "$sup"
status=$?
set -e

echo "--- supervisor (exit $status) ---"
cat /tmp/supervisor.out
echo "--- source ---"
cat /tmp/source.out
echo "--- relay A ---"
cat /tmp/relay-a.out
echo "--- relay B ---"
cat /tmp/relay-b.out
echo "--- sink ---"
cat /tmp/sink.out

if [ "$mode" = "kill" ]; then
    awk '
        /sink: state SAFE/ { safe = 1 }
        safe && /sink: state NORMAL/ { back = 1 }
        END { exit back ? 0 : 1 }
    ' /tmp/sink.out
    grep 'supervisor: restarted relay-b' /tmp/supervisor.out >/dev/null
    grep 'lost signal' /tmp/sink.out /tmp/relay-a.out >/dev/null
fi
if [ "$mode" = "lock" ]; then
    grep 'LOCKED_SAFE' /tmp/supervisor.out /tmp/sink.out >/dev/null
fi

exit "$status"
