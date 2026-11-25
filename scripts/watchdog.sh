#!/bin/bash
# Run one command with a time limit and check the guest when it hangs.
#   scripts/watchdog.sh LIMIT_S LABEL LOG_DIR COMMAND [ARGS...]
# The command's output goes where the caller sends this script's output.
# Exit: the command's own status; 124 when it ran past LIMIT_S and was
# killed with everything it started, and the guest still answers (the
# network nodes on the guest are stopped); 3 when it ran past LIMIT_S and
# the guest does not answer (the QEMU serial log is copied to
# LOG_DIR/LABEL-serial.log). Lines from the watchdog go to stderr.
# A guest kernel stop (F-032) hung a series for 33 minutes in B26 (D-077).
#
# WATCHDOG_GUEST_CHECK, WATCHDOG_GUEST_CLEAN and WATCHDOG_SERIAL replace
# the ssh check, the ssh clean-up and the serial log path
# (tests/test_series_watchdog.sh).
set -u

if [ "$#" -lt 4 ]; then
    echo "usage: scripts/watchdog.sh LIMIT_S LABEL LOG_DIR COMMAND [ARGS...]" >&2
    exit 2
fi
limit=$1
label=$2
log_dir=$3
shift 3
case "$limit" in
    '' | *[!0-9]* | 0)
        echo "watchdog: LIMIT_S must be a positive integer" >&2
        exit 2
        ;;
esac

ssh_guest="ssh -p 2222 -i $HOME/.ssh/id_ed25519 -o BatchMode=yes -o ConnectTimeout=10 root@localhost"
guest_check=${WATCHDOG_GUEST_CHECK:-"$ssh_guest uname -r"}
guest_clean=${WATCHDOG_GUEST_CLEAN:-"$ssh_guest 'for p in supervisor telemetry source relay sink stall-probe hold-next; do slay -Q -f \$p >/dev/null 2>&1; done; true'"}
serial=${WATCHDOG_SERIAL:-$HOME/qnxprojects/targets/qemu-qnx800-aarch64le/output/serial.log}

# Kill a process and everything it started.
kill_tree() {
    local child
    for child in $(pgrep -P "$1" 2>/dev/null); do
        kill_tree "$child"
    done
    kill -9 "$1" 2>/dev/null || true
}

"$@" &
pid=$!
began=$(date +%s)
while kill -0 "$pid" 2>/dev/null; do
    if [ $(( $(date +%s) - began )) -ge "$limit" ]; then
        kill_tree "$pid"
        wait "$pid" 2>/dev/null
        echo "watchdog: $label TIMEOUT after $limit s; killed" >&2
        if eval "$guest_check" > /dev/null 2>&1; then
            echo "watchdog: the guest answers; the nodes on it are stopped" >&2
            eval "$guest_clean" > /dev/null 2>&1 || true
            exit 124
        fi
        mkdir -p "$log_dir"
        if [ -f "$serial" ]; then
            cp "$serial" "$log_dir/$label-serial.log"
        fi
        echo "watchdog: the guest does not answer; serial log kept in $log_dir/$label-serial.log" >&2
        exit 3
    fi
    sleep 1
done
wait "$pid"
