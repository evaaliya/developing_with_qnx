#!/bin/sh
# Runs on the QNX guest.
# calm: the ordinary chain. flood: the same chain with a short queue on
# relay A. Counts come from the host so make run and make run-flood keep
# their numbers. kill: one death of relay B, then recovery.
# lock: K deaths in a row, then LOCKED_SAFE. K is chosen here.
# stop: SIGSTOP relay B, then SIGCONT. The process stays alive.
# source: one death of source, then the supervisor restarts it.
set -eu

# Prints the same sections make run used to print from the ssh command.
# set -e is off around source and the waits: a non-zero source still has
# to show its log before the script exits with that status.
run_chain() {
    mode=$1
    urgent=$2
    command=$3
    background=$4
    queue=${5:-}
    seed=${6:-}
    pin=${7:-}
    total=$((urgent + command + background))
    sink_pid=
    relay_a=
    relay_b=
    source_status=0
    sink_status=0

    if [ "$pin" = "pin" ]; then
        /tmp/sink -c 0 "$total" > /tmp/sink.out 2>&1 &
    else
        /tmp/sink "$total" > /tmp/sink.out 2>&1 &
    fi
    sink_pid=$!
    /tmp/relay signal-relay-b signal-sink > /tmp/relay-b.out 2>&1 &
    relay_b=$!
    if [ "$mode" = "flood" ]; then
        /tmp/relay signal-relay-a signal-relay-b -q "$queue" > /tmp/relay-a.out 2>&1 &
    else
        /tmp/relay signal-relay-a signal-relay-b > /tmp/relay-a.out 2>&1 &
    fi
    relay_a=$!

    set +e
    if [ -n "$seed" ]; then
        /tmp/source -s "$seed" "$urgent" "$command" "$background" signal-relay-a > /tmp/source.out 2>&1
    else
        /tmp/source "$urgent" "$command" "$background" signal-relay-a > /tmp/source.out 2>&1
    fi
    source_status=$?
    sink_status=0
    wait "$sink_pid" || sink_status=$?
    slay -Q -f relay >/dev/null 2>&1 || true
    slay -Q -f source >/dev/null 2>&1 || true
    slay -Q -f sink >/dev/null 2>&1 || true
    slay -Q -f telemetry >/dev/null 2>&1 || true
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
    exit "$sink_status"
}

mode=${1:-}
case "$mode" in
calm)
    seed=
    pin=
    if [ "$#" -eq 4 ]; then
        :
    elif [ "$#" -eq 5 ] && [ "$5" = "pin" ]; then
        pin=pin
    elif [ "$#" -eq 5 ]; then
        seed=$5
    else
        echo "usage: qnx-scenario.sh calm URGENT COMMAND BACKGROUND [SEED|pin]" >&2
        exit 2
    fi
    run_chain calm "$2" "$3" "$4" "" "$seed" "$pin"
    ;;
flood)
    seed=
    if [ "$#" -eq 5 ]; then
        :
    elif [ "$#" -eq 6 ]; then
        seed=$6
    else
        echo "usage: qnx-scenario.sh flood URGENT COMMAND BACKGROUND QUEUE [SEED]" >&2
        exit 2
    fi
    run_chain flood "$2" "$3" "$4" "$5" "$seed" ""
    ;;
kill) k=3 ;;
lock) k=2 ;;
stop)
    k=3
    urgent=20
    command=10
    background=10
    ;;
source) k=3 ;;
*)
    echo "usage: qnx-scenario.sh calm|flood|kill|lock|stop|source" >&2
    exit 2
    ;;
esac

urgent=${urgent:-12}
command=${command:-6}
background=${background:-6}

echo "guest: $(uname -a)"

slay -Q -f supervisor >/dev/null 2>&1 || true
slay -Q -f telemetry >/dev/null 2>&1 || true
slay -Q -f source >/dev/null 2>&1 || true
slay -Q -f relay >/dev/null 2>&1 || true
slay -Q -f sink >/dev/null 2>&1 || true
# qconn ignores SIGTERM and keeps TCP 8000 (F-026). slay's default is SIGTERM.
slay -Q -f -s SIGKILL qconn >/dev/null 2>&1 || true

rm -f /tmp/supervisor.out /tmp/sink.out /tmp/relay-a.out /tmp/relay-b.out /tmp/source.out

/tmp/supervisor -k "$k" -u "$urgent" -c "$command" -b "$background" -g 200 -w 2000 >/tmp/supervisor.out 2>&1 &
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

if [ "$mode" = "stop" ]; then
    echo "scenario: stopping relay-b pid $pid"
    kill -STOP "$pid"
    s=0
    while [ "$s" -lt 8 ]; do
        if grep 'sink: state SAFE' /tmp/sink.out >/dev/null 2>&1; then
            break
        fi
        s=$((s + 1))
        sleep 1
    done
    if ! grep 'sink: state SAFE' /tmp/sink.out >/dev/null 2>&1; then
        echo "scenario: sink did not reach SAFE while relay-b was stopped"
        kill -CONT "$pid" >/dev/null 2>&1 || true
        kill -KILL "$sup" >/dev/null 2>&1 || true
        wait "$sup" >/dev/null 2>&1 || true
        echo "--- supervisor ---"
        cat /tmp/supervisor.out 2>/dev/null || true
        echo "--- sink ---"
        cat /tmp/sink.out 2>/dev/null || true
        exit 1
    fi
    sleep 1
    if ! kill -0 "$pid" >/dev/null 2>&1; then
        echo "scenario: relay-b died while stopped"
        exit 1
    fi
    echo "scenario: relay-b still alive pid $pid"
    pidin -p "$pid" || true
    if grep 'supervisor: restarted relay-b' /tmp/supervisor.out >/dev/null 2>&1; then
        echo "scenario: relay-b was restarted while stopped"
        kill -CONT "$pid" >/dev/null 2>&1 || true
        exit 1
    fi
    echo "scenario: continuing relay-b pid $pid"
    kill -CONT "$pid"
elif [ "$mode" = "source" ]; then
    source_pid=$(grep 'supervisor: node source pid=' /tmp/supervisor.out | tail -1 | sed 's/.*pid=//')
    if [ -z "$source_pid" ]; then
        echo "scenario: source was not ready"
        kill -KILL "$sup" >/dev/null 2>&1 || true
        wait "$sup" >/dev/null 2>&1 || true
        echo "--- supervisor ---"
        cat /tmp/supervisor.out 2>/dev/null || true
        exit 1
    fi
    echo "scenario: killing source pid $source_pid"
    if [ ! -e /dev/signal-source ]; then
        echo "scenario: /dev/signal-source is missing before the kill"
        kill -KILL "$sup" >/dev/null 2>&1 || true
        wait "$sup" >/dev/null 2>&1 || true
        echo "--- supervisor ---"
        cat /tmp/supervisor.out 2>/dev/null || true
        echo "--- source ---"
        cat /tmp/source.out 2>/dev/null || true
        exit 1
    fi
    echo "scenario: path before kill"
    ls -l /dev/signal-source
    kill -KILL "$source_pid"
    i=0
    while [ "$i" -lt 10 ]; do
        if [ ! -e /dev/signal-source ]; then
            break
        fi
        i=$((i + 1))
        sleep 1
    done
    if [ -e /dev/signal-source ]; then
        echo "scenario: /dev/signal-source stayed after the kill"
        kill -KILL "$sup" >/dev/null 2>&1 || true
        wait "$sup" >/dev/null 2>&1 || true
        echo "--- supervisor ---"
        cat /tmp/supervisor.out 2>/dev/null || true
        exit 1
    fi
    echo "scenario: path gone after the kill"
    i=0
    while [ "$i" -lt 20 ]; do
        if [ -e /dev/signal-source ]; then
            echo "scenario: path after restart"
            ls -l /dev/signal-source
            break
        fi
        i=$((i + 1))
        sleep 1
    done
    if [ ! -e /dev/signal-source ]; then
        echo "scenario: /dev/signal-source did not return after restart"
        kill -KILL "$sup" >/dev/null 2>&1 || true
        wait "$sup" >/dev/null 2>&1 || true
        echo "--- supervisor ---"
        cat /tmp/supervisor.out 2>/dev/null || true
        echo "--- source ---"
        cat /tmp/source.out 2>/dev/null || true
        exit 1
    fi
else
    echo "scenario: killing relay-b pid $pid"
    if [ "$mode" = "kill" ]; then
        echo "scenario: clock before-kill $(date +%s.%N)"
    fi
    kill -KILL "$pid"
    if [ "$mode" = "kill" ]; then
        s=0
        while [ "$s" -lt 50 ]; do
            if grep 'sink: state SAFE' /tmp/sink.out >/dev/null 2>&1; then
                echo "scenario: clock safe $(date +%s.%N)"
                break
            fi
            s=$((s + 1))
            sleep 0.2
        done
        s=0
        while [ "$s" -lt 50 ]; do
            if awk '
                /sink: state SAFE/ { safe = 1 }
                safe && /sink: signal / && /URGENT/ && /on time/ && /fresh/ { found = 1 }
                END { exit found ? 0 : 1 }
            ' /tmp/sink.out; then
                echo "scenario: clock recovered $(date +%s.%N)"
                break
            fi
            s=$((s + 1))
            sleep 0.2
        done
    fi
fi

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
if [ "$mode" = "stop" ]; then
    if grep 'supervisor: restarted relay-b' /tmp/supervisor.out >/dev/null; then
        echo "scenario: relay-b was restarted" >&2
        exit 1
    fi
    awk '
        /sink: state SAFE/ { safe = 1 }
        safe && /sink: state NORMAL/ { back = 1 }
        /sink: unaccounted 0/ { books = 1 }
        /sink: signal / && / late/ && /, fresh/ { bad_fresh++ }
        /sink: signal / && / on time/ && $(NF) == "ns" && $(NF-2) == "total" && ($(NF-1) + 0) > 1000000000 { bad_ontime++ }
        /sink: signal / && / late/ { late_n++ }
        END { exit (safe && back && books && late_n > 0 && bad_fresh == 0 && bad_ontime == 0) ? 0 : 1 }
    ' /tmp/sink.out
fi
if [ "$mode" = "source" ]; then
    awk '
        /sink: state SAFE/ { safe = 1 }
        safe && /sink: state NORMAL/ { back = 1 }
        END { exit back ? 0 : 1 }
    ' /tmp/sink.out
    grep 'supervisor: restarted source' /tmp/supervisor.out >/dev/null
    grep 'died before its issued list arrived' /tmp/sink.out >/dev/null
    grep 'sink: unaccounted 0' /tmp/sink.out >/dev/null
fi

exit "$status"
