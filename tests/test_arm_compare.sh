#!/bin/bash
# scripts/arm-compare.sh: the order of the alternating blocks, the binary
# directory of each network, failed runs counted, a dead guest stops it.
set -u

here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0

expect() {
    if [ "$2" = "$3" ]; then
        echo "PASS $1"
    else
        echo "FAIL $1 (got '$2', want '$3')"
        failures=$((failures + 1))
    fi
}

# A run that prints its arguments; run 3 of naive flood fails.
cat > "$work/fake-run.sh" <<'EOF'
#!/bin/bash
echo "run $1 $2 $3"
[ "$1 $2 $3" = "flood 3 /tmp/naive" ] && exit 1
exit 0
EOF
chmod +x "$work/fake-run.sh"

out=$(ARM_COMPARE_RUN="$work/fake-run.sh" bash "$here/scripts/arm-compare.sh" 5 2 30 "$work/logs" 2>&1)
status=$?
expect "one failed run gives exit 1" "$status" 1
order=$(echo "$out" | sed -n 's/^arm-compare: \([a-z]*\) \([a-z]*\) \([0-9]*\) exit.*/\1-\2-\3/p' | tr '\n' ' ')
expect "blocks alternate, odd blocks start with ours" "$order" \
    "ours-calm-1 ours-calm-2 naive-calm-1 naive-calm-2 ours-flood-1 ours-flood-2 naive-flood-1 naive-flood-2 naive-calm-3 naive-calm-4 ours-calm-3 ours-calm-4 naive-flood-3 naive-flood-4 ours-flood-3 ours-flood-4 ours-calm-5 naive-calm-5 ours-flood-5 naive-flood-5 "
expect "20 runs, one failed" "$(echo "$out" | tail -1)" "arm-compare: runs 20, failed 1"
expect "a log per run" "$(ls "$work/logs" | grep -c '\.log$')" 20
expect "our network runs from /tmp" "$(cat "$work/logs/ours-calm-05.log")" "run calm 5 /tmp"
expect "the naive one from /tmp/naive" "$(cat "$work/logs/naive-flood-02.log")" "run flood 2 /tmp/naive"

# A guest that stops answering ends the series with exit 3.
cat > "$work/dead-run.sh" <<'EOF'
#!/bin/bash
exit 3
EOF
chmod +x "$work/dead-run.sh"
ARM_COMPARE_RUN="$work/dead-run.sh" bash "$here/scripts/arm-compare.sh" 4 2 30 "$work/dead" > "$work/dead.out" 2>&1
expect "a dead guest stops the series with exit 3" "$?" 3
expect "after the first run" "$(ls "$work/dead" | grep -c '\.log$')" 1

bash "$here/scripts/arm-compare.sh" 0 2 30 "$work/bad" > /dev/null 2>&1
expect "N 0 is refused" "$?" 2

if [ "$failures" -ne 0 ]; then
    echo "$failures test(s) failed"
    exit 1
fi
echo "all tests passed"
