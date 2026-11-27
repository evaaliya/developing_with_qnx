#!/bin/sh
# A short run of the "QNX tools" network on the guest (D-098), started by
# HAM through /tmp/qnx-tools/launch.
#   qnx-tools-run.sh [calm|kill]
# calm: the network runs about 3 s with the arm going home and 1 s of
#   flood, then stops. It passes when the sink reached NORMAL, source
#   issued URGENT, COMMAND and BACKGROUND, both printed exit status 0.
# kill: the same, and while it runs relay A is killed with SIGKILL three
#   times 2 s apart; launch prints the time from each kill to the start of
#   the relay HAM started again. The source lines send-failed and
#   send-timeout show what source saw.
set -eu

mode=${1:-calm}
case "$mode" in
    calm | kill) ;;
    *) echo "qnx-tools-run: mode must be calm or kill" >&2; exit 2 ;;
esac
bin=/tmp/qnx-tools
for p in source relay sink launch ham/ham ham/hamctrl ham/libham.so.2; do
    [ -e "$bin/$p" ] || { echo "qnx-tools-run: $bin/$p is missing" >&2; exit 2; }
done

show_logs() {
    for f in source relay-a relay-b sink; do
        echo "--- $f ---"
        cat "/tmp/$f.out" 2>/dev/null || true
    done
}

fail() {
    echo "qnx-tools-run: $1" >&2
    "$bin/launch" stop || true
    slay -Q -f source >/dev/null 2>&1 || true
    slay -Q -f relay >/dev/null 2>&1 || true
    slay -Q -f sink >/dev/null 2>&1 || true
    show_logs
    exit 1
}

echo "guest: $(uname -a)"
echo "qnx-tools-run: mode $mode"
"$bin/launch" stop >/dev/null 2>&1 || true
slay -Q -f supervisor >/dev/null 2>&1 || true
slay -Q -f telemetry >/dev/null 2>&1 || true
slay -Q -f source >/dev/null 2>&1 || true
slay -Q -f relay >/dev/null 2>&1 || true
slay -Q -f sink >/dev/null 2>&1 || true
rm -f /tmp/source.out /tmp/sink.out /tmp/relay-a.out /tmp/relay-b.out

export SIGNAL_QUIET=1
"$bin/launch" start "$bin" 2000 || fail "launch did not start the network"

i=0
while [ "$i" -lt 30 ]; do
    grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1 && break
    i=$((i + 1))
    sleep 0.2
done
grep 'sink: state NORMAL' /tmp/sink.out >/dev/null 2>&1 || fail "sink did not reach NORMAL"

echo 'arm home' > /dev/signal-source
sleep 1
echo 'flood on' > /dev/signal-source
echo "qnx-tools-run: flood on"
if [ "$mode" = kill ]; then
    "$bin/launch" kill /tmp/relay-a.out 3 2000 || fail "HAM did not restart relay A each time"
    sleep 1
else
    sleep 1
fi
echo 'flood off' > /dev/signal-source
sleep 1
"$bin/launch" stop
echo 'stop' > /dev/signal-source

i=0
while [ "$i" -lt 50 ]; do
    grep -q '^source: exit status ' /tmp/source.out 2>/dev/null &&
        grep -q '^sink: exit status ' /tmp/sink.out 2>/dev/null && break
    i=$((i + 1))
    sleep 0.2
done
slay -Q -f relay >/dev/null 2>&1 || true
sleep 0.2
show_logs

grep -q '^source: exit status 0$' /tmp/source.out || { echo "qnx-tools-run: source did not exit 0" >&2; slay -Q -f source >/dev/null 2>&1 || true; exit 1; }
grep -q '^sink: exit status 0$' /tmp/sink.out || { echo "qnx-tools-run: sink did not exit 0" >&2; slay -Q -f sink >/dev/null 2>&1 || true; exit 1; }
line=$(grep '^source: issued URGENT ' /tmp/source.out || true)
echo "qnx-tools-run: $line"
set -- $line
# source: issued URGENT u COMMAND c BACKGROUND b
[ "${4:-0}" -gt 0 ] && [ "${6:-0}" -gt 0 ] && [ "${8:-0}" -gt 0 ] ||
    { echo "qnx-tools-run: source did not issue every class" >&2; exit 1; }
echo "qnx-tools-run: PASS"
exit 0
