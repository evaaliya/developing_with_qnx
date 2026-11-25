#!/bin/bash
# Run one make target N times in a row and keep every run's output.
#   scripts/series.sh N TARGET [MAKE_ARGS...]
# Logs: build/series/<TARGET>-<time>/NN.log, with the files a check
# writes to fixed paths under build/ copied next to it, and summary.txt
# with the exit status of each run. Nothing from an earlier run is erased.
#
# Watchdog (B26): a run that takes longer than SERIES_LIMIT_S seconds
# (default 300, 900 for run-long-live and run-live-restart) is killed and
# counted failed (exit 124 in summary.txt). Then the guest is asked over
# ssh. If it answers, the network nodes on it are stopped and the series
# goes on; if not, the QEMU serial log is copied next to the run and the
# series stops with exit 3 (a guest kernel stop, F-032, hung a series for
# 33 minutes). The limit is scripts/watchdog.sh. SERIES_MAKE replaces
# make; SERIES_GUEST_CHECK, SERIES_GUEST_CLEAN and SERIES_SERIAL are
# passed to it as WATCHDOG_* (tests/test_series_watchdog.sh).
set -u

if [ "$#" -lt 2 ]; then
    echo "usage: scripts/series.sh N TARGET [MAKE_ARGS...]" >&2
    exit 2
fi
n=$1
target=$2
shift 2
case "$n" in
    '' | *[!0-9]* | 0)
        echo "series: N must be a positive integer" >&2
        exit 2
        ;;
esac
case "$target" in
    '' | *[!A-Za-z0-9_-]*)
        echo "series: TARGET must be a make target name" >&2
        exit 2
        ;;
esac

case "$target" in
    run-long-live | run-live-restart) limit_default=900 ;;
    *) limit_default=300 ;;
esac
limit=${SERIES_LIMIT_S:-$limit_default}
case "$limit" in
    '' | *[!0-9]* | 0)
        echo "series: SERIES_LIMIT_S must be a positive integer" >&2
        exit 2
        ;;
esac
make_cmd=${SERIES_MAKE:-make}
[ -n "${SERIES_GUEST_CHECK:-}" ] && export WATCHDOG_GUEST_CHECK="$SERIES_GUEST_CHECK"
[ -n "${SERIES_GUEST_CLEAN:-}" ] && export WATCHDOG_GUEST_CLEAN="$SERIES_GUEST_CLEAN"
[ -n "${SERIES_SERIAL:-}" ] && export WATCHDOG_SERIAL="$SERIES_SERIAL"

root=$(cd "$(dirname "$0")/.." && pwd)
dir="$root/build/series/$target-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$dir"
summary="$dir/summary.txt"
echo "series: $n x make $target $*" | tee "$summary"
echo "series: logs in ${dir#"$root"/}" | tee -a "$summary"
echo "series: each run is stopped after $limit s" | tee -a "$summary"

# Files that checks write to fixed paths, copied after each run.
keep_fixed() {
    run_dir=$1
    if [ -d "$root/build/live-restart" ]; then
        mkdir -p "$run_dir/live-restart"
        cp -p "$root/build/live-restart/"* "$run_dir/live-restart/" 2>/dev/null || true
    fi
    for f in panel-check.log panel-check-guest.log panel-check-panel.log; do
        if [ -f "$root/build/$f" ]; then
            cp -p "$root/build/$f" "$run_dir/" 2>/dev/null || true
        fi
    done
}

failed=0
i=1
while [ "$i" -le "$n" ]; do
    tag=$(printf '%02d' "$i")
    log="$dir/$tag.log"
    start=$(date '+%H:%M:%S')
    (cd "$root" && bash "$root/scripts/watchdog.sh" "$limit" "$tag" "$dir" \
        $make_cmd "$target" "$@") > "$log" 2>&1
    status=$?
    end=$(date '+%H:%M:%S')
    if [ "$status" -eq 124 ]; then
        echo "series: run $tag TIMEOUT after $limit s; killed; the guest answers, the nodes on it are stopped, the series goes on" | tee -a "$summary"
    elif [ "$status" -eq 3 ] && grep -q '^watchdog: the guest does not answer' "$log"; then
        echo "series: run $tag TIMEOUT after $limit s; killed" | tee -a "$summary"
        echo "series: run $tag $start-$end exit $status" | tee -a "$summary"
        echo "series: the guest does not answer; serial log kept in ${dir#"$root"/}/$tag-serial.log; the series stops" | tee -a "$summary"
        echo "series: $target failed $((failed + 1)) of $i tried, $n planned" | tee -a "$summary"
        exit 3
    fi
    case "$target" in
        run-live-restart | panel-check)
            keep_fixed "$dir/$tag-files"
            ;;
    esac
    if [ "$status" -ne 0 ]; then
        failed=$((failed + 1))
    fi
    echo "series: run $tag $start-$end exit $status" | tee -a "$summary"
    i=$((i + 1))
done
echo "series: $target failed $failed of $n" | tee -a "$summary"
[ "$failed" -eq 0 ]
