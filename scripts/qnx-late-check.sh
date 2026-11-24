#!/bin/sh
# Runs on the QNX guest. One place for the rule every guest check uses on
# late URGENT and COMMAND signals (D-062, D-064):
#
#   qnx-late-check.sh start [SECONDS]   start stall-probe beside the check
#   qnx-late-check.sh fault-begin NAME  the check is about to cause a fault
#   qnx-late-check.sh fault-end NAME    the fault the check caused is over
#   qnx-late-check.sh check LABEL [SINK_LOG...]
#                                       stop the probe and hold each late
#                                       URGENT or COMMAND against it
#
# A late signal was on its way from (arrival - total) to arrival. It is
# inside a fault when that window meets a fault the check caused on
# purpose (SIGSTOP of a relay, a kill), and otherwise inside a guest
# stall when it meets a stall the probe saw ([due, due + late]). Either
# way it gets its own line; a stall line also says how much stall time
# fell on its way and whether that covers how late it was. A late signal
# inside neither is a failure of the check: exit 1. No probe output is
# exit 2: then nothing can be told apart.
#
# The probe, the sink, and the fault marks all print CLOCK_MONOTONIC ms.
set -u

# LATE_CHECK_DIR lets the host tests run the same rule on fixtures.
DIR=${LATE_CHECK_DIR:-/tmp}
PROBE=${STALL_PROBE:-/tmp/stall-probe}
PROBE_OUT=$DIR/stall-probe.out
PROBE_PID=$DIR/stall-probe.pid
FAULTS=$DIR/late-faults.txt
# Wake-ups later than this are printed by the probe (B22: 5 ms).
THRESHOLD_MS=5

stop_probe() {
    if [ -s "$PROBE_PID" ]; then
        pid=$(cat "$PROBE_PID")
        kill "$pid" >/dev/null 2>&1 || true
        i=0
        while [ "$i" -lt 20 ] && kill -0 "$pid" >/dev/null 2>&1; do
            i=$((i + 1))
            sleep 0.1
        done
    fi
    rm -f "$PROBE_PID"
}

cmd=${1:-}
case "$cmd" in
start)
    seconds=${2:-900}
    # A probe left over from a check that failed early would run on.
    stop_probe
    slay -Q -f stall-probe >/dev/null 2>&1 || true
    rm -f "$PROBE_OUT" "$FAULTS"
    : > "$FAULTS"
    if [ ! -x "$PROBE" ]; then
        echo "late-check: no stall probe at $PROBE; late signals cannot be checked" >&2
        exit 1
    fi
    "$PROBE" "$seconds" "$THRESHOLD_MS" > "$PROBE_OUT" 2>&1 &
    echo "$!" > "$PROBE_PID"
    i=0
    while [ "$i" -lt 30 ]; do
        grep '^stall-probe: start ' "$PROBE_OUT" >/dev/null 2>&1 && exit 0
        i=$((i + 1))
        sleep 0.1
    done
    echo "late-check: the stall probe did not start" >&2
    cat "$PROBE_OUT" >&2 2>/dev/null || true
    exit 1
    ;;
fault-begin|fault-end)
    name=${2:-}
    [ -n "$name" ] || { echo "usage: qnx-late-check.sh $cmd NAME" >&2; exit 2; }
    now=$("$PROBE" now) || exit 1
    echo "${cmd#fault-} $name $now" >> "$FAULTS"
    exit 0
    ;;
check)
    label=${2:-}
    [ -n "$label" ] || { echo "usage: qnx-late-check.sh check LABEL [SINK_LOG...]" >&2; exit 2; }
    shift 2
    [ "$#" -gt 0 ] || set -- "$DIR/sink.out"
    stop_probe
    if ! grep '^stall-probe: start ' "$PROBE_OUT" >/dev/null 2>&1; then
        echo "$label: FAIL no stall probe output; late signals cannot be checked"
        exit 2
    fi
    [ -f "$FAULTS" ] || : > "$FAULTS"
    awk -v label="$label" -v probe="$PROBE_OUT" -v faults="$FAULTS" '
        FILENAME == probe && $2 == "start" { p0 = $3; next }
        FILENAME == probe && $2 == "woke" {
            n++; s0[n] = $7; s1[n] = $7 + $3 / 1000000
            if ($3 > worst) worst = $3
            next
        }
        FILENAME == probe { next }
        FILENAME == faults && $1 == "begin" { nf++; fname[nf] = $2; f0[nf] = $3; f1[nf] = -1; next }
        FILENAME == faults && $1 == "end" {
            for (i = nf; i >= 1; i--) if (fname[i] == $2 && f1[i] < 0) { f1[i] = $3; break }
            next
        }
        FILENAME == faults { next }
        # sink: late signal N class C at T ms, total X ns
        $1 == "sink:" && $2 == "late" && $3 == "signal" && ($6 == "URGENT" || $6 == "COMMAND") {
            num = $4; c = $6; t = $8; total = $11
            a = t - total / 1000000
            late[c]++
            hit = 0
            for (i = 1; i <= nf; i++) {
                e = f1[i] < 0 ? t + 1 : f1[i]
                if (f0[i] <= t && e >= a) { hit = i; break }
            }
            if (hit) {
                fault[c]++
                printf "%s: late %s %s inside the fault %s of this check (on its way %d ms to %d ms)\n", label, c, num, fname[hit], a, t
                next
            }
            stalled = 0
            if (a >= p0) {
                for (i = 1; i <= n; i++) if (s0[i] <= t && s1[i] >= a) {
                    if (!hit) hit = i
                    lo = s0[i] > a ? s0[i] : a; hi = s1[i] < t ? s1[i] : t
                    stalled += hi - lo
                }
            }
            if (hit) {
                stall[c]++
                # D-058 deadlines: URGENT 31 ms, COMMAND 62 ms.
                over = total / 1000000 - (c == "URGENT" ? 31 : 62)
                if (stalled >= over) covered++
                printf "%s: late %s %s inside a guest stall of %.0f ms at %d ms (on its way %d ms to %d ms; %.1f ms over the deadline, %.1f ms of stalls on its way%s)\n", label, c, num, s1[hit] - s0[hit], s0[hit], a, t, over, stalled, (stalled >= over ? "" : ", less than it was late")
                next
            }
            out[c]++
            why = a < p0 ? "; it left before the probe started" : ""
            printf "%s: FAIL late %s %s NOT inside a guest stall or a fault of this check (on its way %d ms to %d ms, total %d ns%s)\n", label, c, num, a, t, total, why
        }
        END {
            printf "%s: stall probe from %d ms, stalls over 5 ms %d, worst %d ns\n", label, p0, n, worst
            for (k = 1; k <= 2; k++) {
                c = k == 1 ? "URGENT" : "COMMAND"
                printf "%s: late %s %d: inside guest stalls %d, inside faults %d, outside %d\n", label, c, late[c], stall[c], fault[c], out[c]
            }
            si = stall["URGENT"] + stall["COMMAND"]; fi = fault["URGENT"] + fault["COMMAND"]
            printf "%s: late inside guest stalls whose stall time covers the lateness %d of %d\n", label, covered, si
            oi = out["URGENT"] + out["COMMAND"]
            printf "%s: late URGENT or COMMAND inside guest stalls %d, inside faults %d, outside %d\n", label, si, fi, oi
            exit oi > 0 ? 1 : 0
        }
    ' "$PROBE_OUT" "$FAULTS" "$@"
    exit $?
    ;;
*)
    echo "usage: qnx-late-check.sh start [SECONDS] | fault-begin NAME | fault-end NAME | check LABEL [SINK_LOG...]" >&2
    exit 2
    ;;
esac
