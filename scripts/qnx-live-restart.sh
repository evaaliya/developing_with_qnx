#!/bin/sh
# Runs on the QNX guest. The live network under supervisor -L, quiet
# logs, a flood, and node kills (D-063).
#   qnx-live-restart.sh RUN_S all       RUN_S >= 300: kill relay A, relay B,
#                                        sink, and source once each, all
#                                        while the flood is on
#   qnx-live-restart.sh RUN_S <node>    RUN_S >= 60: kill one node
#                                        (source, relay-a, relay-b, sink)
# Big logs are read only after the network has stopped.
set -eu

RUN_S=${1:-300}
PLAN=${2:-all}
K=3
WINDOW_MS=1500
SIGNAL_QUIET=1
export SIGNAL_QUIET

case "$PLAN" in
    all) [ "$RUN_S" -ge 300 ] || { echo "live-restart: plan all needs at least 300 s" >&2; exit 2; } ;;
    source|relay-a|relay-b|sink)
        [ "$RUN_S" -ge 60 ] || { echo "live-restart: one kill needs at least 60 s" >&2; exit 2; } ;;
    *) echo "usage: qnx-live-restart.sh RUN_S all|source|relay-a|relay-b|sink" >&2; exit 2 ;;
esac

# Second of the run, then what to do.
events() {
    if [ "$PLAN" = all ]; then
        echo "20 cmd flood on"
        echo "40 kill relay-a"
        echo "70 kill relay-b"
        echo "100 kill sink"
        echo "130 kill source"
        echo "160 cmd flood off"
        echo "180 cmd flood on"
        echo "210 cmd flood off"
        echo "230 cmd mute"
        echo "240 cmd restore"
    else
        echo "10 cmd flood on"
        echo "25 kill $PLAN"
        echo "45 cmd flood off"
    fi
}

stop_all() {
    slay -Q -f supervisor >/dev/null 2>&1 || true
    slay -Q -f telemetry >/dev/null 2>&1 || true
    slay -Q -f source >/dev/null 2>&1 || true
    slay -Q -f relay >/dev/null 2>&1 || true
    slay -Q -f sink >/dev/null 2>&1 || true
}

node_pid() {
    grep -E "^supervisor: (node|restarted) $1 pid=" /tmp/supervisor.out |
        tail -1 | sed 's/.*pid=//'
}

echo "guest: $(uname -a)"
stop_all
rm -f /tmp/supervisor.out /tmp/source.out /tmp/sink.out /tmp/relay-a.out \
    /tmp/relay-b.out /tmp/telemetry.out /tmp/live-restart-events.txt \
    /tmp/stall-probe.out

if ! /tmp/qnx-late-check.sh start $((RUN_S + 120)); then
    echo "live-restart: FAIL the stall probe did not start" >&2
    exit 1
fi
/tmp/supervisor -k "$K" -w "$WINDOW_MS" -L > /tmp/supervisor.out 2>&1 &
sup=$!

i=0
while [ "$i" -lt 50 ]; do
    grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1 && break
    i=$((i + 1))
    sleep 0.2
done
if ! grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1; then
    echo "live-restart: FAIL sink did not reach NORMAL" >&2
    stop_all
    cat /tmp/supervisor.out 2>/dev/null || true
    exit 1
fi

start=$(date +%s)
echo "live-restart: plan $PLAN, start $start, running ${RUN_S} s, K $K"
events > /tmp/live-restart-plan.txt
t=0
while [ "$t" -lt "$RUN_S" ]; do
    sleep 1
    t=$(( $(date +%s) - start ))
    kill -0 "$sup" >/dev/null 2>&1 || break
    while read -r at what arg1 arg2; do
        [ "$t" -ge "$at" ] || continue
        grep "^$at " /tmp/live-restart-events.txt >/dev/null 2>&1 && continue
        if [ "$what" = kill ]; then
            pid=$(node_pid "$arg1")
            if [ -n "$pid" ] && kill -9 "$pid" >/dev/null 2>&1; then
                echo "$at kill $arg1 pid $pid at $t" >> /tmp/live-restart-events.txt
            else
                echo "$at kill $arg1 FAILED pid ${pid:-none} at $t" >> /tmp/live-restart-events.txt
            fi
            continue
        fi
        cmd="$arg1${arg2:+ $arg2}"
        # The path is gone while source is being restarted: try next second.
        if [ -e /dev/signal-source ] && echo "$cmd" > /dev/signal-source 2>/dev/null; then
            mode=$(grep '^mode ' /dev/signal-source 2>/dev/null | cut -d' ' -f2)
            echo "$at $cmd mode $mode at $t" >> /tmp/live-restart-events.txt
        fi
    done < /tmp/live-restart-plan.txt
done
end=$(date +%s)

if [ -e /dev/signal-source ]; then
    mode_end=$(grep '^mode ' /dev/signal-source 2>/dev/null | cut -d' ' -f2)
    echo stop > /dev/signal-source 2>/dev/null || true
else
    mode_end=none
fi
i=0
while [ "$i" -lt 300 ]; do
    kill -0 "$sup" >/dev/null 2>&1 || break
    i=$((i + 1))
    sleep 0.2
done
if kill -0 "$sup" >/dev/null 2>&1; then
    echo "live-restart: supervisor still running 60 s after stop; killing the network"
    stop_all
fi
set +e
wait "$sup"
sup_status=$?
set -e

# The network has stopped: from here on the logs may be read whole.
set +e
echo "live-restart: ran $((end - start)) s, supervisor exit $sup_status, mode at the end $mode_end"
sed 's/^/live-restart: event /' /tmp/live-restart-events.txt 2>/dev/null || true
echo "--- supervisor ---"
grep -v ' progress, streak cleared' /tmp/supervisor.out || true
echo "--- source (state lines) ---"
grep -E '^source: (resum|reopen|relay|link|issued|sent [0-9]|sent manifest|the |flood stopped|closes are)|failed|not ' /tmp/source.out | head -60 || true
for r in a b; do
    echo "--- relay $r (state lines) ---"
    grep -E 'resum|link down|reopened|not back|failed|full|kept|boosts|repicks' /tmp/relay-$r.out | head -40 || true
    echo "relay $r: lost lines $(grep -c ' lost signal ' /tmp/relay-$r.out || true)"
done
echo "--- sink (state lines) ---"
grep -E '^sink: (resum|books|summary|unaccounted|issued list|manifest|the network)|state (INIT|SAFE|RECOVERING|LOCKED)|already|repeats|marks|stopped|failed|could not' /tmp/sink.out | head -80 || true
for st in INIT NORMAL DEGRADED SAFE RECOVERING; do
    echo "sink: state $st entries $(grep -c "^sink: state $st" /tmp/sink.out)"
done
echo "sink: lost lines $(grep -c '^sink: lost signal' /tmp/sink.out || true)"
echo "sink: closes $(grep -c '^sink: closed ' /tmp/sink.out || true)"
df -k /tmp | tail -1 | sed 's/^/live-restart: guest \/tmp /'
ls -l /tmp/source.out /tmp/relay-a.out /tmp/relay-b.out /tmp/sink.out /tmp/supervisor.out |
    awk '{ print "live-restart: log " $NF " " $5 " bytes" }'

failed=0
check() {
    if [ "$1" -eq 0 ]; then
        echo "live-restart: ok   $2"
    else
        echo "live-restart: FAIL $2"
        failed=1
    fi
}

[ "$((end - start))" -ge "$RUN_S" ]; check $? "the run lasted ${RUN_S} s"
[ "$sup_status" -eq 0 ]; check $? "supervisor exit 0"
! grep 'LOCKED_SAFE' /tmp/supervisor.out >/dev/null; check $? "restart limit K=$K not reached (no LOCKED_SAFE)"
over=$(awk -v k="$K" '/ streak=/ { split($3, s, "="); if (s[2] + 0 > k) n++ } END { print n + 0 }' /tmp/supervisor.out)
[ "$over" -eq 0 ]; check $? "no streak above K"

if [ "$PLAN" = all ]; then nodes="relay-a relay-b sink source"; else nodes=$PLAN; fi
for n in $nodes; do
    grep "kill $n pid" /tmp/live-restart-events.txt >/dev/null; check $? "$n was killed"
    c=$(grep -c "^supervisor: restarted $n pid=" /tmp/supervisor.out || true)
    [ "$c" -eq 1 ]; check $? "$n restarted once (restarts $c)"
done
c=$(grep -c '^supervisor: restarted ' /tmp/supervisor.out || true)
k=$(echo $nodes | wc -w | tr -d ' ')
[ "$c" -eq "$k" ]; check $? "no restart besides the kills ($c restarts, $k kills)"

grep '^sink: unaccounted 0$' /tmp/sink.out >/dev/null; check $? "sink: unaccounted 0"
c=$(grep -c -E 'already accounted|repeats signal|issued list did not include' /tmp/sink.out || true)
[ "$c" -eq 0 ]; check $? "no number arrived or was closed twice ($c lines)"
c=$(grep -c -E 'books (were )?reset|not kept' /tmp/sink.out /tmp/source.out /tmp/relay-a.out /tmp/relay-b.out | awk -F: '{ s += $NF } END { print s + 0 }')
[ "$c" -eq 0 ]; check $? "every restarted node kept its books ($c reset lines)"
for c in URGENT COMMAND BACKGROUND; do
    issued=$(awk -v c="$c" '$1 == "source:" && $2 == "issued" {
        for (i = 3; i < NF; i += 2) if ($i == c) print $(i + 1) }' /tmp/source.out | tail -1)
    booked=$(awk -v c="$c" '$1 == "sink:" && $2 == "summary" && $3 == c {
        gsub(",", ""); print $5 + $12 + $14 }' /tmp/sink.out | tail -1)
    echo "live-restart: $c issued ${issued:-none}, on the sink totals ${booked:-none}"
    [ -n "$issued" ] && [ "$issued" = "${booked:-x}" ]; check $? "$c issued equals the sink totals"
done

# URGENT is due every 20 ms but the 10 ms tick makes it about 38 a
# second (run-long-live: 22839 in 600 s). If the totals cover only the
# time after a restart, both sides lost the start of the run and could
# still agree, so the count must cover most of the run.
urgent=$(awk '$1 == "source:" && $2 == "issued" { print $4 }' /tmp/source.out | tail -1)
[ "${urgent:-0}" -ge $((RUN_S * 30)) ]; check $? "URGENT issued ${urgent:-none} covers the run (at least $((RUN_S * 30)))"

# Late URGENT or COMMAND against the stall probe (qnx-late-check.sh,
# D-064). The kills are faults of this check, but a kill loses signals,
# it does not delay them, so no fault window is marked: a late signal
# must be inside a guest stall.
/tmp/qnx-late-check.sh check live-restart
check $? "every late URGENT or COMMAND is inside a guest stall"

if [ "$failed" -eq 0 ]; then
    echo "live-restart: PASS"
    exit 0
fi
echo "live-restart: FAIL"
exit 1
