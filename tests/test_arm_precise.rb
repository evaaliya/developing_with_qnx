# Tests for scripts/arm_precise.rb (D-089, D-090). ours-calm-01 and
# naive-flood-01 are real B29 trial runs of make run-arm-precise; the
# other cases are made from them.
require_relative "../scripts/arm_precise"
require "json"
require "stringio"
require "tmpdir"

$failures = 0
def check(name, ok)
  puts "#{ok ? 'PASS' : 'FAIL'} #{name}"
  $failures += 1 unless ok
end

dir = File.join(__dir__, "fixtures", "arm-precise")
ours = File.join(dir, "ours-calm-01.txt")
naive = File.join(dir, "naive-flood-01.txt")

Dir.mktmpdir do |tmp|
  out = StringIO.new
  code = ArmPrecise.report([ours, naive], out, File.join(tmp, "replay"))
  text = out.string
  check("two whole runs pass", code == 0)
  check("the plan is printed", text.include?("margin 7.75 mm (31 ms x 250 mm/s), event at 32.75 mm from the eye"))
  check("our row: the tool at 32.750 mm at the event, rest and past the plan to 0.01 mm",
        text.include?("arm-precise ours-calm-01 ours calm 32.750 2.798 person 0.70 7.05 0.70 no 98 delivered 98:delivered 1821.9"))
  check("naive row: 23.77 ms is 5.94 mm past the plan",
        text.include?("arm-precise naive-flood-01 naive flood 32.750 23.770 person 5.94 1.81 5.94 no 2260 delivered 2260:delivered 1722.9"))
  check("summary with median, p99 and worst",
        text.include?("arm-precise naive flood extra path median 5.94 mm p99 5.94 mm worst 5.94 mm"))
  check("touches counted", text.include?("arm-precise ours calm closest to the eye 7.05 mm (ours-calm-01); touched the eye 0 of 1"))
  check("the event check is reported", text.include?("arm-precise ours calm tool at the event: off 32.75 mm by at most 0.000 mm"))
  check("no touch: the naive run with the largest extra path for the video",
        text.include?("arm-precise video naive naive-flood-01: no touch in the naive runs, largest extra path"))

  j = JSON.parse(File.read(File.join(tmp, "replay", "naive-flood-01.json")))
  check("replay keeps the D-085 fields", j["event_ms"] == 668.141 && j["brake_ms"] == 691.911 && j["brake_by"] == "person")
  check("replay has the planned point", j["plan_point_mm"] == 7.75 && j["event_eye_mm"] == 32.75)
  check("replay: rest and past the plan", j["rest_mm"] == 1.811 && j["past_plan_mm"] == 5.939 && j["extra_path_mm"] == 5.942)
  check("replay: URGENT up to the brake", j["urgent_until_brake"] == [{ "number" => 2260, "fate" => "delivered" }])

  # The event 1 ms later than the plan: the tool is 0.25 mm closer.
  src = File.read(ours)
  ev = src[/^source: person-plan .* event_ns (\d+) /, 1]
  shifted = src.gsub(ev, (ev.to_i + 1_000_000).to_s)
  File.write(File.join(tmp, "ours-calm-02.log"), shifted)
  out = StringIO.new
  code = ArmPrecise.report([File.join(tmp, "ours-calm-02.log")], out)
  check("an event 0.25 mm off the plan fails the run",
        code == 1 && out.string.include?("arm-precise FAIL ours-calm-02: the tool was 32.500 mm from the eye at the event, not 32.75 +- 0.05"))
  out = StringIO.new
  ArmPrecise.report([ours, File.join(tmp, "ours-calm-02.log")], out)
  check("a run off the plan is named and left out of the numbers",
        out.string.include?("arm-precise ours calm runs 2, failed the event check 1 (ours-calm-02 32.500 mm); the numbers below are over 1 runs") &&
        out.string.include?("arm-precise ours calm closest to the eye 7.05 mm (ours-calm-01); touched the eye 0 of 1"))

  # No person-plan line: the zone sensor did not issue the event.
  File.write(File.join(tmp, "naive-calm-03.log"), File.read(naive).gsub(/^source: person-plan .*\n/, "").gsub("build/arm", "x"))
  out = StringIO.new
  code = ArmPrecise.report([File.join(tmp, "naive-calm-03.log")], out)
  check("a run without the planned event fails",
        code == 1 && out.string.include?("FAIL naive-calm-03: no person-plan line from source"))
  File.write(File.join(tmp, "naive-calm-04.log"),
             File.read(naive).sub(/^source: person-plan .*$/, "source: person-plan failed: no approach began within 3 s; approach_start_ns 0"))
  out = StringIO.new
  ArmPrecise.report([File.join(tmp, "naive-calm-04.log")], out)
  check("the sensor's own failure is named", out.string.include?("FAIL naive-calm-04: source: person-plan failed: no approach began within 3 s"))
end

# The naive run for the video: among touches, the latest brake; no brake is latest.
rows = [
  { name: "a", touched: true, brake_by: "person", delay_ns: 40_000_000, extra_mm: 10.0 },
  { name: "b", touched: true, brake_by: nil, delay_ns: nil, extra_mm: nil },
  { name: "c", touched: false, brake_by: "person", delay_ns: 90_000_000, extra_mm: 22.5 }
]
check("video: a touch without a brake beats a later non-touch", ArmPrecise.naive_video(rows)[:name] == "b")
check("video: without touches, the largest extra path", ArmPrecise.naive_video(rows.reject { |r| r[:touched] })[:name] == "c")

# Lost URGENT in a row, by the issued list.
log = <<~LOG
  source: urgent-issued 0 10 11 12 13 14 15 16 17 18 19
  source: urgent-issued count 10 not listed 0
  relay signal-relay-a: dropped signal 12 class URGENT at 5 ns
  relay signal-relay-a: dropped signal 13 class URGENT at 6 ns
  sink: lost signal 14 class URGENT
  relay signal-relay-b: dropped signal 17 class URGENT at 9 ns
  sink: late signal 19 class URGENT at 3 ms, total 40000000 ns
LOG
runs, n, how = ArmPrecise.streaks_of(log)
check("streaks from the list: 3 then 1, late is not lost", runs == [3, 1] && n == 10 && how == "list")
old = "sink: summary URGENT received 140, on time 130, late 10, lost 1, dropped 0\nsink: lost signal 7 class URGENT\n"
runs, n, how = ArmPrecise.streaks_of(old)
check("a log without the list and one loss: a streak of 1", runs == [1] && n == 141 && how == "summary")
runs, _, how = ArmPrecise.streaks_of(old + "sink: lost signal 9 class URGENT\n")
check("without the list two losses cannot be ordered", runs.nil? && how.include?("2 lost"))
out = StringIO.new
Dir.mktmpdir do |tmp|
  File.write(File.join(tmp, "naive-flood-01.log"), log)
  File.write(File.join(tmp, "naive-flood-02.log"), old)
  code = ArmPrecise.streaks(Dir[File.join(tmp, "*.log")], out)
  check("streaks summed over logs", code == 0 && out.string.include?("URGENT 151, dropped or lost 5") &&
        out.string.include?("in a row: 1 x 2, 2 x 0, 3 or more x 1"))
  check("each streak length gives its extra path", out.string.include?("a streak of 2 adds 2 URGENT periods (40 ms) before the next flagged URGENT: 10.0 mm more"))
end

if $failures.zero?
  puts "all tests passed"
else
  puts "#{$failures} test(s) failed"
  exit 1
end
