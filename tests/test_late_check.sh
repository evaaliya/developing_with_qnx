#!/bin/sh
# Host test for scripts/qnx-late-check.sh check: each late URGENT or
# COMMAND is inside a guest stall, inside a fault of the check, or a
# failure (D-064). Uses fixtures, no guest.
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
script="$root/scripts/qnx-late-check.sh"
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
fails=0

pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; fails=$((fails + 1)); }

run_check() {
    LATE_CHECK_DIR="$dir" sh "$script" check t "$@" > "$dir/out" 2>&1
    echo $? > "$dir/status"
}

expect_status() {
    if [ "$(cat "$dir/status")" = "$1" ]; then pass "$2"; else
        fail "$2 (status $(cat "$dir/status"))"; cat "$dir/out"; fi
}

expect_line() {
    if grep -F -- "$1" "$dir/out" >/dev/null; then pass "$2"; else
        fail "$2"; cat "$dir/out"; fi
}

# A probe that started at 1000 ms and saw one stall of 40 ms at 5000 ms.
cat > "$dir/stall-probe.out" <<'EOF'
stall-probe: start 1000 ms, period 10 ms, threshold 5 ms
stall-probe: woke 40000000 ns late at 5000 ms
EOF
: > "$dir/late-faults.txt"

# 1. No late signal: exit 0 and the zero lines.
cat > "$dir/sink.out" <<'EOF'
sink: signal 1 class URGENT on time, fresh, hops 1 ns, 1 ns, 1 ns, total 3 ns
EOF
run_check
expect_status 0 "no late signal passes"
expect_line "t: late URGENT or COMMAND inside guest stalls 0, inside faults 0, outside 0" "no late signal prints zeros"
expect_line "t: stall probe from 1000 ms, stalls over 5 ms 1, worst 40000000 ns" "probe summary line"

# 2. Late URGENT whose way (4990..5030 ms) meets the stall (5000..5040).
cat > "$dir/sink.out" <<'EOF'
sink: late signal 7 class URGENT at 5030 ms, total 40000000 ns
EOF
run_check
expect_status 0 "late inside a stall passes"
expect_line "t: late URGENT 7 inside a guest stall of 40 ms at 5000 ms" "stall line names the signal"
expect_line "t: late URGENT 1: inside guest stalls 1, inside faults 0, outside 0" "per-class count"
expect_line "9.0 ms over the deadline, 30.0 ms of stalls on its way, at most 40.0 ms)" "stall time on the way"
expect_line "t: late inside guest stalls whose stall time seen covers the lateness 1 of 1" "covered count"

# 2b. A long way that only grazes the stall: the stall cannot explain
# 49 ms of lateness even if it began a whole probe period before the
# probe saw it (4990..5001, 11 ms), so the check fails (D-070).
printf 'sink: late signal 8 class URGENT at 5001 ms, total 80000000 ns\n' > "$dir/sink.out"
run_check
expect_status 1 "a grazed stall that does not cover the lateness fails (D-070)"
expect_line "t: FAIL late URGENT 8 meets a guest stall of 40 ms at 5000 ms, but the stalls on its way (1.0 ms seen, at most 11.0 ms) do not cover how late it was (49.0 ms over the deadline; on its way 4921 ms to 5001 ms" "graze line"
expect_line "t: late URGENT 1: inside guest stalls 0, inside faults 0, outside 1" "graze counted outside"

# 2d. Covered only when the stall may have begun up to one probe period
# before the late wake-up: the probe slept 10 ms, so a 10 ms late wake
# can be a 20 ms stop. Way 4964..5010, 15 ms over, 10 ms seen, at most 20.
printf 'stall-probe: start 1000 ms, period 10 ms, threshold 5 ms\nstall-probe: woke 10000000 ns late at 5000 ms\n' > "$dir/stall-probe.out"
printf 'sink: late signal 12 class URGENT at 5010 ms, total 46000000 ns\n' > "$dir/sink.out"
run_check
expect_status 0 "covered with the probe period passes"
expect_line "15.0 ms over the deadline, 10.0 ms of stalls on its way, at most 20.0 ms)" "seen and at most"

# 2e. Two stalls on one way: the period before the second one overlaps
# the first, and that time is counted once. Stalls 5000..5008 and
# 5012..5020; at most 4990..5020 = 30 ms, not 38.
printf 'stall-probe: start 1000 ms, period 10 ms, threshold 5 ms\nstall-probe: woke 8000000 ns late at 5000 ms\nstall-probe: woke 8000000 ns late at 5012 ms\n' > "$dir/stall-probe.out"
printf 'sink: late signal 13 class URGENT at 5020 ms, total 80000000 ns\n' > "$dir/sink.out"
run_check
expect_status 1 "two stalls counted once each still short of 49 ms"
expect_line "(16.0 ms seen, at most 30.0 ms)" "overlapping periods counted once"
printf 'stall-probe: start 1000 ms, period 10 ms, threshold 5 ms\nstall-probe: woke 40000000 ns late at 5000 ms\n' > "$dir/stall-probe.out"

# 2c. A fault of the check wins over a stall on the same way.
printf 'begin stop-relay-b 4000\nend stop-relay-b 5010\n' > "$dir/late-faults.txt"
printf 'sink: late signal 7 class URGENT at 5030 ms, total 40000000 ns\n' > "$dir/sink.out"
run_check
expect_line "t: late URGENT 7 inside the fault stop-relay-b of this check" "fault before stall"
: > "$dir/late-faults.txt"

# 3. Late COMMAND far from the stall: failure with a clear line.
cat > "$dir/sink.out" <<'EOF'
sink: late signal 9 class COMMAND at 9000 ms, total 70000000 ns
EOF
run_check
expect_status 1 "late outside a stall fails"
expect_line "t: FAIL late COMMAND 9 NOT inside a guest stall or a fault of this check (on its way 8930 ms to 9000 ms, total 70000000 ns)" "outside line"

# 4. The same signal inside a fault the check caused.
printf 'begin stop-relay-b 8900\nend stop-relay-b 8950\n' > "$dir/late-faults.txt"
run_check
expect_status 0 "late inside a fault of the check passes"
expect_line "t: late COMMAND 9 inside the fault stop-relay-b of this check" "fault line"

# 5. A fault that never ended reaches the end of the run.
printf 'begin lock 8990\n' > "$dir/late-faults.txt"
run_check
expect_status 0 "an open fault covers the rest of the run"

# 6. A fault that ended before the signal left does not cover it.
printf 'begin kill 100\nend kill 200\n' > "$dir/late-faults.txt"
run_check
expect_status 1 "a fault before the signal does not excuse it"

# 7. BACKGROUND late is not this rule's business.
: > "$dir/late-faults.txt"
cat > "$dir/sink.out" <<'EOF'
sink: late signal 11 class BACKGROUND at 9000 ms, total 170000000 ns
EOF
run_check
expect_status 0 "late BACKGROUND is not checked"

# 8. A signal that left before the probe started cannot be excused.
cat > "$dir/sink.out" <<'EOF'
sink: late signal 3 class URGENT at 1020 ms, total 40000000 ns
EOF
run_check
expect_status 1 "late before the probe started fails"
expect_line "it left before the probe started" "before-probe reason"

# 9. Without probe output nothing can be told apart: exit 2.
rm -f "$dir/stall-probe.out"
run_check
expect_status 2 "missing probe output"
expect_line "t: FAIL no stall probe output" "missing probe line"

# 10. Several logs: a restarted sink appends to another file.
cat > "$dir/stall-probe.out" <<'EOF'
stall-probe: start 1000 ms, period 10 ms, threshold 5 ms
EOF
printf 'sink: late signal 4 class URGENT at 3000 ms, total 35000000 ns\n' > "$dir/a.out"
printf 'sink: late signal 5 class URGENT at 4000 ms, total 35000000 ns\n' > "$dir/b.out"
run_check "$dir/a.out" "$dir/b.out"
expect_status 1 "every log is read"
expect_line "t: late URGENT 2: inside guest stalls 0, inside faults 0, outside 2" "two logs counted"

if [ "$fails" -eq 0 ]; then
    echo "late-check: all tests passed"
    exit 0
fi
echo "late-check: $fails failed"
exit 1
