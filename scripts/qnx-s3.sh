#!/bin/sh
# Four R8 faults, then URGENT going silent. Each fault is the existing
# guest scenario. Clocks for R6 and R7 are printed by the kill scenario.
set -eu

failed=0

section() {
    name=$1
    shift
    echo "===== s3 $name ====="
    set +e
    "$@"
    status=$?
    set -e
    echo "s3: section $name exit $status"
    if [ "$status" -ne 0 ]; then
        failed=1
    fi
}

urgent_silent() {
    echo "guest: $(uname -a)"
    slay -Q -f supervisor >/dev/null 2>&1 || true
    slay -Q -f telemetry >/dev/null 2>&1 || true
    slay -Q -f source >/dev/null 2>&1 || true
    slay -Q -f relay >/dev/null 2>&1 || true
    slay -Q -f sink >/dev/null 2>&1 || true
    rm -f /tmp/source.out /tmp/sink.out /tmp/relay-a.out /tmp/relay-b.out

    /tmp/qnx-late-check.sh start 120 || return 1
    /tmp/sink 64 2000 > /tmp/sink.out 2>&1 &
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
    if ! grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1; then
        echo "s3: urgent silent did not reach NORMAL" >&2
        slay -Q -f source >/dev/null 2>&1 || true
        slay -Q -f relay >/dev/null 2>&1 || true
        slay -Q -f sink >/dev/null 2>&1 || true
        echo "--- sink ---"
        cat /tmp/sink.out 2>/dev/null || true
        return 1
    fi

    echo 'urgent off' > /dev/signal-source
    i=0
    while [ "$i" -lt 30 ]; do
        if grep 'sink: state SAFE' /tmp/sink.out >/dev/null 2>&1; then
            break
        fi
        i=$((i + 1))
        sleep 0.2
    done
    if ! grep 'sink: state SAFE' /tmp/sink.out >/dev/null 2>&1; then
        echo "s3: urgent silent did not reach SAFE" >&2
        echo 'stop' > /dev/signal-source || true
        return 1
    fi
    if ! kill -0 "$source_pid" >/dev/null 2>&1; then
        echo "s3: source died when urgent stopped" >&2
        return 1
    fi
    echo "scenario: source still alive pid $source_pid"
    echo 'stop' > /dev/signal-source || true
    i=0
    while [ "$i" -lt 30 ]; do
        if ! kill -0 "$source_pid" >/dev/null 2>&1; then
            break
        fi
        i=$((i + 1))
        sleep 0.2
    done

    set +e
    wait "$source_pid"
    wait "$sink_pid"
    slay -Q -f relay >/dev/null 2>&1 || true
    wait "$relay_a" >/dev/null 2>&1 || true
    wait "$relay_b" >/dev/null 2>&1 || true
    set -e

    echo "--- source ---"
    cat /tmp/source.out 2>/dev/null || true
    echo "--- sink ---"
    cat /tmp/sink.out 2>/dev/null || true
    /tmp/qnx-late-check.sh check late-check || return 1
    return 0
}

section "kill relay B" /tmp/qnx-scenario.sh kill
section "stop relay B" /tmp/qnx-scenario.sh stop
section "kill source" /tmp/qnx-scenario.sh source
section "K crashes" /tmp/qnx-scenario.sh lock
section "URGENT silent" urgent_silent

exit "$failed"
