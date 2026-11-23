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

paths = %w[s1 s2 s3 s4].each_with_index.map do |name, index|
  path = File.join(dir, "#{name}.log")
  File.write(path, [s1, s2, s3, s4][index])
  path
end

script = File.join(root, "scripts", "scenario_report.rb")
out, status = Open3.capture2e({ "LATENCY_REPORT" => File.join(root, "build", "latency-report") },
                              "/usr/bin/ruby", script, *paths)
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
