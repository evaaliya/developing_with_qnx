#!/bin/sh
# Runs on the QNX guest. Relay A stops (SIGSTOP) while the flood is on.
# The flood send has a 5 s limit: the flood thread must report it and
# stop instead of waiting forever, and /dev/signal-source must still
# answer while that happens (D-061). The ledger is not checked here:
# with relay A stopped the chain is broken on purpose.
set -eu

VARIANT=${1:-urgent}
case "$VARIANT" in
    urgent|mute) ;;
    *) echo "usage: qnx-flood-hang.sh [urgent|mute]" >&2; exit 2 ;;
esac

fail() {
    echo "flood-hang: FAIL $1" >&2
    # Thread states first, before any signal or kill changes them (F-033).
    echo "--- pidin at the failure, before any kill ---"
    echo "pids: source ${source_pid:-?} relay A ${relay_a:-?} relay B ${relay_b:-?} sink ${sink_pid:-?}"
    pidin -p source 2>&1 || true
    pidin -p relay 2>&1 | grep -v '^ *pid' || true
    pidin -p sink 2>&1 | grep -v '^ *pid' || true
    echo "--- source (lines that are not single signals) ---"
    grep -Ev 'signal [0-9]* class|flood signal|flood batch' /tmp/source.out 2>/dev/null | tail -30 || true
    echo "--- relay A (tail) ---"
    tail -5 /tmp/relay-a.out 2>/dev/null | cut -c1-300 || true
    slay -Q -f -s SIGCONT relay >/dev/null 2>&1 || true
    slay -Q -f -s SIGKILL source >/dev/null 2>&1 || true
    slay -Q -f relay >/dev/null 2>&1 || true
    slay -Q -f sink >/dev/null 2>&1 || true
    exit 1
}

echo "guest: $(uname -a)"

slay -Q -f supervisor >/dev/null 2>&1 || true
slay -Q -f telemetry >/dev/null 2>&1 || true
slay -Q -f source >/dev/null 2>&1 || true
slay -Q -f relay >/dev/null 2>&1 || true
slay -Q -f sink >/dev/null 2>&1 || true
rm -f /tmp/source.out /tmp/sink.out /tmp/relay-a.out /tmp/relay-b.out

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

echo "flood-hang: variant $VARIANT"
if [ "$VARIANT" = mute ]; then
    echo 'mute' > /dev/signal-source
fi
echo 'flood on' > /dev/signal-source
sleep 1
kill -STOP "$relay_a"
stopped=$(date +%s)
echo "flood-hang: relay A stopped at $stopped"

# The device answers while the sends are blocked.
sleep 1
if cat /dev/signal-source > /tmp/dev-status.txt; then
    echo "flood-hang: /dev/signal-source answered while relay A was stopped"
    grep -E '^(mode|flood) ' /tmp/dev-status.txt
else
    fail "/dev/signal-source did not answer"
fi

# source ends its run on the failure: URGENT or the batch times out in
# 5 s, the flood thread is given up after 6 s more, and the manifest has
# 20 s. If relay A took an URGENT and stopped before its reply, the watch
# thread of source sees the timer thread stuck after 6 s and ends the
# run the same way (F-033). A thread waiting for a reply from a
# name_attach channel cannot be torn down until the relay replies or
# dies, so the process finishes exiting only when relay A is killed, as
# the supervisor would (D-061).
i=0
while [ "$i" -lt 225 ]; do
    kill -0 "$source_pid" >/dev/null 2>&1 || break
    if grep 'sending the manifest failed' /tmp/source.out >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 0.2
done
reported=$(date +%s)
grep -Ev 'signal [0-9]* class|flood signal|flood batch' /tmp/source.out | tail -6
if kill -0 "$source_pid" >/dev/null 2>&1 &&
    ! grep 'sending the manifest failed' /tmp/source.out >/dev/null 2>&1; then
    fail "source neither exited nor gave up on the manifest within 45 s"
fi
echo "flood-hang: source gave up $((reported - stopped)) s after the stop"
grep -E 'timed out' /tmp/source.out >/dev/null 2>&1 ||
    fail "no send reported its time limit"
if grep -E 'did not take a BACKGROUND batch|flood thread is still waiting' /tmp/source.out >/dev/null 2>&1; then
    echo "flood-hang: the flood thread was the one stuck"
else
    echo "flood-hang: the flood thread was not stuck in a send"
fi
# relay A took a timer-thread signal and stopped before its reply: the
# watch thread of source ends the run without the timer thread (F-033).
if grep 'the timer thread is stuck' /tmp/source.out >/dev/null 2>&1; then
    echo "flood-hang: the timer thread was stuck waiting for a reply; the watch thread ended the run"
fi

kill -KILL "$relay_a" >/dev/null 2>&1 || true
killed=$(date +%s)
i=0
while [ "$i" -lt 50 ]; do
    kill -0 "$source_pid" >/dev/null 2>&1 || break
    i=$((i + 1))
    sleep 0.1
done
if kill -0 "$source_pid" >/dev/null 2>&1; then
    fail "source did not finish exiting within 5 s of relay A dying"
fi
set +e
wait "$source_pid"
source_status=$?
set -e
echo "flood-hang: source exit $source_status, $(( $(date +%s) - killed )) s after relay A died"

kill -CONT "$relay_a" >/dev/null 2>&1 || true
slay -Q -f relay >/dev/null 2>&1 || true
slay -Q -f sink >/dev/null 2>&1 || true
wait "$relay_a" >/dev/null 2>&1 || true
wait "$relay_b" >/dev/null 2>&1 || true
wait "$sink_pid" >/dev/null 2>&1 || true
[ "$source_status" -ne 0 ] || fail "source exited 0 after a failed send"
echo "flood-hang: PASS $VARIANT"
exit 0
