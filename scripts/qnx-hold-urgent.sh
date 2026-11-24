#!/bin/sh
# Runs on the QNX guest (F-033). The next node of source takes one URGENT
# and never answers or receives again (hold-next), as relay A stopped
# between its receive and its reply. source must still end its run:
# report that the timer thread is stuck, give up on the manifest within
# 45 s, and finish exiting once the next node dies (D-071). pidin is
# taken while it hangs, before anything is killed.
set -u

fail() {
    echo "hold-urgent: FAIL $1"
    echo "--- pidin, before any kill ---"
    pidin -p source 2>&1 || true
    slay -Q -f -s SIGKILL hold-next >/dev/null 2>&1 || true
    slay -Q -f -s SIGKILL source >/dev/null 2>&1 || true
    echo "--- source (lines that are not single signals) ---"
    grep -Ev 'signal [0-9]* class|flood signal|flood batch' /tmp/source.out 2>/dev/null | tail -20
    exit 1
}

echo "guest: $(uname -a)"
for p in supervisor telemetry source relay sink hold-next; do
    slay -Q -f "$p" >/dev/null 2>&1 || true
done
rm -f /tmp/source.out /tmp/hold-next.out

/tmp/hold-next signal-relay-a > /tmp/hold-next.out 2>&1 &
hold=$!
sleep 0.5
/tmp/source live signal-relay-a > /tmp/source.out 2>&1 &
source_pid=$!

i=0
while [ ! -e /dev/signal-source ] && [ "$i" -lt 30 ]; do
    i=$((i + 1)); sleep 0.2
done
[ -e /dev/signal-source ] || fail "/dev/signal-source did not appear"
echo 'flood on' > /dev/signal-source

i=0
while ! grep 'holding URGENT' /tmp/hold-next.out >/dev/null 2>&1 && [ "$i" -lt 50 ]; do
    i=$((i + 1)); sleep 0.2
done
grep 'holding URGENT' /tmp/hold-next.out || fail "hold-next did not take an URGENT"
held=$(date +%s)

sleep 10
echo "--- pidin 10 s after the hold ---"
pidin -p source 2>&1

i=0
while [ "$i" -lt 175 ]; do
    kill -0 "$source_pid" >/dev/null 2>&1 || break
    grep 'sending the manifest failed' /tmp/source.out >/dev/null 2>&1 && break
    i=$((i + 1)); sleep 0.2
done
grep -Ev 'signal [0-9]* class|flood signal|flood batch' /tmp/source.out | tail -8
if kill -0 "$source_pid" >/dev/null 2>&1 &&
    ! grep 'sending the manifest failed' /tmp/source.out >/dev/null 2>&1; then
    fail "source neither exited nor gave up on the manifest within 45 s of the hold"
fi
echo "hold-urgent: source gave up $(( $(date +%s) - held )) s after the hold"
grep 'the timer thread is stuck' /tmp/source.out >/dev/null 2>&1 ||
    fail "source did not say that the timer thread is stuck"

kill -KILL "$hold" >/dev/null 2>&1 || true
i=0
while kill -0 "$source_pid" >/dev/null 2>&1 && [ "$i" -lt 50 ]; do
    i=$((i + 1)); sleep 0.1
done
kill -0 "$source_pid" >/dev/null 2>&1 &&
    fail "source did not finish exiting within 5 s of the next node dying"
set +e
wait "$source_pid"
status=$?
set -e
[ "$status" -ne 0 ] || fail "source exited 0 after a stuck send"
echo "hold-urgent: source exit $status after the next node died"
echo "hold-urgent: PASS"
exit 0
