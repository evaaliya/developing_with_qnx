#!/bin/bash
# scripts/series.sh ends a run that hangs (B26): the run is killed after
# SERIES_LIMIT_S, the guest is checked, and the series goes on when the
# guest answers or stops with exit 3 when it does not. No guest is used:
# SERIES_MAKE, SERIES_GUEST_CHECK and SERIES_GUEST_CLEAN stand in for
# make and ssh, SERIES_SERIAL for the QEMU serial log.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"; rm -rf "$root"/build/series/watchdog-test-*' EXIT
failures=0

check() {
    if [ "$2" = "$3" ]; then
        echo "PASS $1"
    else
        echo "FAIL $1 (got '$2', want '$3')"
        failures=$((failures + 1))
    fi
}

# A fake make: target "watchdog-test-hang" sleeps, anything else passes.
cat > "$work/make" <<'EOF'
#!/bin/bash
case "$1" in
    watchdog-test-hang) sleep 60 ;;
    *) echo "fake make $1"; exit 0 ;;
esac
EOF
chmod +x "$work/make"
echo "serial log line" > "$work/serial.log"

# 1. A run that finishes in time: no watchdog line, exit 0.
out=$(SERIES_MAKE="$work/make" SERIES_LIMIT_S=5 \
    bash "$root/scripts/series.sh" 2 watchdog-test-ok 2>&1)
check "runs in time pass" "$?" 0
check "no timeout line" "$(echo "$out" | grep -c TIMEOUT)" 0

# 2. Hanging runs, the guest answers: each run is killed and counted
#    failed, the guest is cleaned, the series goes on to the end.
start=$(date +%s)
out=$(SERIES_MAKE="$work/make" SERIES_LIMIT_S=2 \
    SERIES_GUEST_CHECK=true SERIES_GUEST_CLEAN="echo cleaned" \
    bash "$root/scripts/series.sh" 2 watchdog-test-hang 2>&1)
rc=$?
took=$(( $(date +%s) - start ))
check "hanging runs fail the series" "$rc" 1
check "two timeout lines" "$(echo "$out" | grep -c 'TIMEOUT after 2 s')" 2
check "guest answered, series went on" "$(echo "$out" | grep -c 'failed 2 of 2')" 1
check "killed well before the fake make ends" "$([ "$took" -lt 30 ] && echo yes)" yes
check "no sleep left behind" "$(pgrep -f 'sleep 60' >/dev/null && echo left || echo none)" none

# 3. A hanging run and a guest that does not answer: the serial log is
#    kept and the series stops at once with exit 3.
out=$(SERIES_MAKE="$work/make" SERIES_LIMIT_S=2 SERIES_GUEST_CHECK=false \
    SERIES_SERIAL="$work/serial.log" \
    bash "$root/scripts/series.sh" 3 watchdog-test-hang 2>&1)
rc=$?
check "dead guest stops the series with 3" "$rc" 3
check "only the first run was tried" "$(echo "$out" | grep -c 'TIMEOUT')" 1
check "says the guest does not answer" "$(echo "$out" | grep -c 'guest does not answer')" 1
dir=$(echo "$out" | awk '/series: logs in/ { print $NF }')
check "serial log kept next to the run" "$(cat "$root/$dir/01-serial.log" 2>/dev/null)" "serial log line"

if [ "$failures" -ne 0 ]; then
    echo "$failures test(s) failed"
    exit 1
fi
echo "series watchdog: all tests passed"
