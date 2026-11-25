#!/bin/bash
# The arm stop on both networks in one guest boot (D-086): our network
# (/tmp) and the naive one (/tmp/naive, D-084), in alternating blocks so
# that neither network has all its runs in a row. Block k runs, for calm
# and then flood, BLOCK runs of each network; odd blocks start with ours,
# even blocks with naive. Every run has the D-077 time limit.
#   scripts/arm-compare.sh N BLOCK LIMIT_S LOG_DIR
# Logs: LOG_DIR/<ours|naive>-<calm|flood>-NN.log.
# Exit 0 when every run passed, 1 when some failed (the series goes on),
# 3 when the guest stopped answering (the series stops), 2 on bad use.
#
# ARM_COMPARE_RUN replaces the command of one run (for
# tests/test_arm_compare.sh); it gets MODE RUN BINDIR as arguments.
set -u

if [ "$#" -ne 4 ]; then
    echo "usage: scripts/arm-compare.sh N BLOCK LIMIT_S LOG_DIR" >&2
    exit 2
fi
n=$1
block=$2
limit=$3
dir=$4
for v in "$n" "$block" "$limit"; do
    case "$v" in
        '' | *[!0-9]* | 0)
            echo "arm-compare: N, BLOCK and LIMIT_S must be positive integers" >&2
            exit 2
            ;;
    esac
done
mkdir -p "$dir"
here=$(cd "$(dirname "$0")" && pwd)
ssh_guest="ssh -p 2222 -i $HOME/.ssh/id_ed25519 -o BatchMode=yes root@localhost"

bindir_of() {
    if [ "$1" = naive ]; then
        echo /tmp/naive
    else
        echo /tmp
    fi
}

one_run() {
    local net=$1 mode=$2 i=$3 log status
    log=$(printf '%s/%s-%s-%02d.log' "$dir" "$net" "$mode" "$i")
    if [ -n "${ARM_COMPARE_RUN:-}" ]; then
        bash "$here/watchdog.sh" "$limit" "$net-$mode-$i" "$dir" \
            $ARM_COMPARE_RUN "$mode" "$i" "$(bindir_of "$net")" > "$log" 2>&1
    else
        bash "$here/watchdog.sh" "$limit" "$net-$mode-$i" "$dir" \
            $ssh_guest "/tmp/qnx-arm-stop.sh $mode $i $(bindir_of "$net")" > "$log" 2>&1
    fi
    status=$?
    echo "arm-compare: $net $mode $i exit $status"
    return "$status"
}

failed=0
runs=0
blocks=$(( (n + block - 1) / block ))
k=1
while [ "$k" -le "$blocks" ]; do
    if [ $((k % 2)) -eq 1 ]; then
        order="ours naive"
    else
        order="naive ours"
    fi
    for mode in calm flood; do
        for net in $order; do
            j=1
            while [ "$j" -le "$block" ]; do
                i=$(( (k - 1) * block + j ))
                if [ "$i" -gt "$n" ]; then
                    break
                fi
                one_run "$net" "$mode" "$i"
                status=$?
                runs=$((runs + 1))
                if [ "$status" -eq 3 ]; then
                    echo "arm-compare: the guest does not answer; stopped after $runs runs" >&2
                    exit 3
                fi
                if [ "$status" -ne 0 ]; then
                    failed=$((failed + 1))
                fi
                j=$((j + 1))
            done
        done
    done
    k=$((k + 1))
done
echo "arm-compare: runs $runs, failed $failed"
[ "$failed" -eq 0 ] || exit 1
exit 0
