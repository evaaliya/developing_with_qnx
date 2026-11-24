#!/usr/bin/ruby
# encoding: utf-8
# Host check of the S1-S4 table, including a verdict that fails on purpose.

Encoding.default_external = Encoding::UTF_8
Encoding.default_internal = Encoding::UTF_8

require "fileutils"
require "open3"
require_relative "../scripts/scenario_report"

failures = 0

def check(name, ok, failures)
  if ok
    puts "PASS #{name}"
  else
    puts "FAIL #{name}"
    failures += 1
  end
  failures
end

root = File.expand_path("..", __dir__)
dir = File.join(root, "build", "scenario-self-check")
FileUtils.mkdir_p(dir)

signal = "sink: signal %d class %s on time, fresh, hops 10 ns, 20 ns, 30 ns, total %d ns\n"
relay = "relay signal-relay-a: sent signal 1 class URGENT; still waiting: none arrived 100 start 200 end 300 picked 150\n"
relay_b = "relay signal-relay-b: sent signal 1 class URGENT; still waiting: none arrived 110 start 210 end 310 picked 160\n"

# qnx-late-check.sh lines (D-064).
def late_lines(urgent_late = 0, urgent_stall = 0, urgent_out = 0)
  out = "late-check: stall probe from 1000 ms, stalls over 5 ms 3, worst 40000000 ns\n"
  out << "late-check: late URGENT #{urgent_late}: inside guest stalls #{urgent_stall}, inside faults 0, outside #{urgent_out}\n"
  out << "late-check: late COMMAND 0: inside guest stalls 0, inside faults 0, outside 0\n"
  out << "late-check: late URGENT or COMMAND inside guest stalls #{urgent_stall}, inside faults 0, outside #{urgent_out}\n"
  out
end

s1 = ""
s1 << "live: path /dev/signal-source\n"
s1 << "s4: live seconds 4\n"
s1 << "sink: state INIT\n"
s1 << "sink: state NORMAL\n"
100.times do |i|
  s1 << format(signal, i + 1, "URGENT", 800_000)
end
s1 << format(signal, 201, "COMMAND", 1_800_000)
s1 << format(signal, 202, "BACKGROUND", 5_000_000)
s1 << "sink: summary URGENT received 100, on time 100, late 0, lost 0, dropped 0\n"
s1 << "sink: summary COMMAND received 1, on time 1, late 0, lost 0, dropped 0\n"
s1 << "sink: summary BACKGROUND received 1, on time 1, late 0, lost 0, dropped 0\n"
s1 << "sink: unaccounted 0\n"
s1 << relay << relay_b
s1 << late_lines

s2 = ""
s2 << "s2: path /dev/signal-source\n"
s2 << "s2: window ms 2000\n"
s2 << "s2: window start 100.000000000\n"
s2 << "s2: window end 102.050000000\n"
s2 << "source: flood every 500000000 ns, burst 8, cap 48\n"
s2 << "sink: state NORMAL\n"
s2 << "sink: state DEGRADED\n"
32.times do |i|
  at = 1_000_000_000 + (i / 8) * 500_000_000 + (i % 8) * 1000
  s2 << "source: sent signal #{10 + i} class BACKGROUND\n"
  s2 << "source: flood signal #{10 + i} at #{at} ns\n"
end
s2 << "relay signal-relay-a: dropped signal 10 class BACKGROUND at 1000000000 ns\n"
s2 << "relay signal-relay-a: dropped signal 20 class BACKGROUND at 1750000000 ns\n"
s2 << "relay signal-relay-a: dropped signal 30 class BACKGROUND at 2500007000 ns\n"
100.times do |i|
  s2 << format(signal, 300 + i, "URGENT", 900_000)
end
s2 << format(signal, 2, "COMMAND", 1_900_000)
s2 << "sink: summary URGENT received 100, on time 100, late 0, lost 0, dropped 0\n"
s2 << "sink: summary COMMAND received 2, on time 2, late 0, lost 0, dropped 0\n"
s2 << "sink: summary BACKGROUND received 16, on time 12, late 0, lost 0, dropped 4\n"
s2 << "sink: unaccounted 0\n"
s2 << relay << relay_b
s2 << late_lines

s3 = <<LOG
===== s3 kill relay B =====
scenario: clock before-kill 100.000000000
scenario: clock safe 100.400000000
scenario: clock recovered 100.900000000
sink: state SAFE
sink: state NORMAL
sink: unaccounted 0
===== s3 stop relay B =====
sink: state SAFE
sink: state NORMAL
sink: unaccounted 0
===== s3 kill source =====
sink: state SAFE
supervisor: restarted source pid=3
sink: unaccounted 0
===== s3 K crashes =====
supervisor: LOCKED_SAFE
sink: state LOCKED_SAFE
===== s3 URGENT silent =====
sink: state SAFE
scenario: source still alive pid 42
LOG
s3 << late_lines

s4 = ""
s4 << "live: path /dev/signal-source\n"
s4 << "s4: live seconds 4\n"
s4 << "s4: panel killed\n"
s4 << "s4: telemetry killed\n"
s4 << "supervisor: restarted telemetry pid=9\n"
s4 << "sink: state NORMAL\n"
100.times do |i|
  s4 << format(signal, i + 1, "URGENT", 810_000)
end
s4 << "sink: summary URGENT received 100, on time 100, late 0, lost 0, dropped 0\n"
s4 << "sink: summary COMMAND received 1, on time 1, late 0, lost 0, dropped 0\n"
s4 << "sink: summary BACKGROUND received 1, on time 1, late 0, lost 0, dropped 0\n"
s4 << "sink: unaccounted 0\n"
s4 << relay << relay_b
s4 << late_lines

def write_logs(dir, logs, tag = "")
  %w[s1 s2 s3 s4].each_with_index.map do |name, index|
    path = File.join(dir, "#{name}#{tag}.log")
    File.write(path, logs[index])
    path
  end
end

paths = write_logs(dir, [s1, s2, s3, s4])

script = File.join(root, "scripts", "scenario_report.rb")
report_env = { "LATENCY_REPORT" => File.join(root, "build", "latency-report") }
out, status = Open3.capture2e(report_env, "/usr/bin/ruby", script, *paths)
failures = check("a complete table passes",
                 status.success? && out.include?("scenarios failed 0") &&
                   out.include?("порога нет") && out.include?("прошёл") &&
                   !out.include?(FAIL),
                 failures)
failures = check("R1 passes when every URGENT is on time and n is at least 100",
                 out =~ /S1 \| R1 \| URGENT on time 100\/100; threshold 1 and n >= 100 \| #{PASS}/,
                 failures)
failures = check("R6 is one run and has no threshold",
                 out.include?("one run, not a measurement; kill to SAFE 400000000 ns") &&
                   out.include?("S3 | R6 |") && out.include?(NONE),
                 failures)

failures = check("each scenario has a late row that passes",
                 %w[S1 S2 S3 S4].all? { |name| out =~ /#{name} \| late \| URGENT late 0: .* threshold outside 0 \| #{PASS}/ },
                 failures)

# S2 with one URGENT 61.8 ms late, as in B23: sink lines, the late-check
# lines, and the summary all say so.
def s2_with_late(s2, inside)
  text = s2.sub("sink: summary URGENT received 100, on time 100, late 0, lost 0, dropped 0\n",
                "sink: signal 400 class URGENT late, fresh, hops 60700000 ns, 1000000 ns, 100000 ns, total 61800000 ns\n" \
                "sink: late signal 400 class URGENT at 9000 ms, total 61800000 ns\n" \
                "sink: summary URGENT received 101, on time 100, late 1, lost 0, dropped 0\n")
  if inside
    text.sub(late_lines, "late-check: late URGENT 400 inside a guest stall of 62 ms at 8940 ms (on its way 8938 ms to 9000 ms)\n" + late_lines(1, 1, 0))
  else
    text.sub(late_lines, "late-check: FAIL late URGENT 400 NOT inside a guest stall or a fault of this check (on its way 8938 ms to 9000 ms, total 61800000 ns)\n" + late_lines(1, 0, 1))
  end
end

inside_paths = write_logs(dir, [s1, s2_with_late(s2, true), s3, s4], "-inside")
inside, inside_status = Open3.capture2e(report_env, "/usr/bin/ruby", script, *inside_paths)
failures = check("a late URGENT inside a guest stall passes and is named",
                 inside_status.success? && inside.include?("scenarios failed 0") &&
                   inside.include?("S2 late-check: late URGENT 400 inside a guest stall of 62 ms") &&
                   inside =~ /S2 \| R2 share \| URGENT on time flood 100\/101; late inside guest stalls 1 against/ &&
                   inside =~ /S2 \| R2 median \| .*max 61800000 ns; .*above the deadline 1, each inside a guest stall \| #{PASS}/ &&
                   inside =~ /S2 \| late \| URGENT late 1: inside guest stalls 1, inside faults 0, outside 0; .* \| #{PASS}/,
                 failures)

outside_paths = write_logs(dir, [s1, s2_with_late(s2, false), s3, s4], "-outside")
outside, outside_status = Open3.capture2e(report_env, "/usr/bin/ruby", script, *outside_paths)
failures = check("a late URGENT outside a guest stall fails with a clear line",
                 !outside_status.success? &&
                   outside.include?("S2 late-check: FAIL late URGENT 400 NOT inside a guest stall") &&
                   outside =~ /S2 \| R2 share \| .* \| #{FAIL}/ &&
                   outside =~ /S2 \| R2 median \| .* \| #{FAIL}/ &&
                   outside =~ /S2 \| late \| URGENT late 1: inside guest stalls 0, inside faults 0, outside 1; .* \| #{FAIL}/,
                 failures)

missing_paths = write_logs(dir, [s1.sub(late_lines, ""), s2, s3, s4], "-missing")
missing, missing_status = Open3.capture2e(report_env, "/usr/bin/ruby", script, *missing_paths)
failures = check("a log without the late check fails",
                 !missing_status.success? &&
                   missing =~ /S1 \| late \| late check missing: .* \| #{FAIL}/,
                 failures)

ENV["SCENARIO_R2_MAX_NS"] = "1"
tight, tight_status = Open3.capture2e(
  { "SCENARIO_R2_MAX_NS" => "1",
    "LATENCY_REPORT" => File.join(root, "build", "latency-report") },
  "/usr/bin/ruby", script, *paths
)
ENV.delete("SCENARIO_R2_MAX_NS")
failures = check("a deadline below the real latency fails the run",
                 !tight_status.success? && tight.include?(FAIL) &&
                   tight.include?("URGENT median 900000 ns; S1 median 800000 ns; threshold 1 ns") &&
                   tight =~ /scenarios failed [1-9]/,
                 failures)

if failures == 0
  puts "all tests passed"
  exit 0
end
warn "#{failures} test(s) failed"
exit 1
