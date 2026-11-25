# Tests for scripts/arm_compare.rb (D-085, D-086). ours-calm-01 and
# naive-flood-01 are real B28 trial runs; naive-calm-02 is made from a
# real naive run: no URGENT with the flag arrived and the tool touched
# the eye.
require_relative "../scripts/arm_compare"
require "json"
require "stringio"
require "tmpdir"

$failures = 0
def check(name, ok)
  puts "#{ok ? 'PASS' : 'FAIL'} #{name}"
  $failures += 1 unless ok
end

dir = File.join(__dir__, "fixtures", "arm-compare")
logs = %w[ours-calm-01 naive-flood-01 naive-calm-02].map { |n| File.join(dir, "#{n}.txt") }

Dir.mktmpdir do |tmp|
  out = StringIO.new
  code = ArmCompare.report(logs, out, File.join(tmp, "replay"))
  text = out.string
  check("a naive run that reached the eye does not fail the series", code == 0)
  check("one row per run", text.scan(/^arm-compare (ours|naive)-(calm|flood)-0\d /).length == 3)
  check("our calm delay", text.include?("arm-compare ours calm n 1 delay median 1.749 ms p99 1.749 ms worst 1.749 ms (ours-calm-01)"))
  check("naive flood delay from the stop line",
        text.include?("arm-compare naive flood n 1 delay median 33.598 ms"))
  check("the late stop signal is counted late, not lost",
        text.include?("arm-compare naive flood stop signal dropped or lost in 0 of 1 runs (dropped at relay A 0, at relay B 0, lost 0; late 1)"))
  check("no brake: the touch run",
        text.include?("arm-compare naive-calm-02 naive calm none none - - 0.0 yes 90 dropped_relay_A 3"))
  check("touch counted as reaching the eye",
        text.include?("arm-compare naive calm closest to the eye 0.0 mm (naive-calm-02); reached the eye 1 of 1"))
  check("dropped stop signal and flagged URGENT on their own line",
        text.include?("stop signal dropped or lost in 1 of 1 runs (dropped at relay A 1, at relay B 0, lost 0; late 0); flagged URGENT dropped or lost before the brake 3 in 1 runs"))
  check("worst run of each network named",
        text.include?("arm-compare ours worst run ours-calm-01:") &&
        text.include?("arm-compare naive worst run naive-calm-02: rest 0.0 mm from the eye, delay none ms, brake by none, stop signal dropped relay A"))
  check("our network comes first", text.index("arm-compare ours calm n") < text.index("arm-compare naive calm n"))

  files = Dir.children(File.join(tmp, "replay")).sort
  check("one replay file per run", files == %w[naive-calm-02.json naive-flood-01.json ours-calm-01.json])
  j = JSON.parse(File.read(File.join(tmp, "replay", "naive-flood-01.json")))
  check("replay: network and mode", j["network"] == "naive" && j["mode"] == "flood" && j["run"] == 1)
  steps = j["samples"].each_cons(2).map { |a, b| (b[0] - a[0]).round(3) }.uniq
  check("replay: a point every 2 ms", steps == [2.0])
  check("replay: starts at home", (j["samples"][0][1] - 199.5).abs < 0.1)
  check("replay: event and brake in ms from the approach",
        j["event_ms"] == 550.831 && j["brake_ms"] == 584.429 && j["brake_by"] == "person")
  check("replay: stop signal", j["stop_signal"] == { "number" => 2119, "class" => "URGENT", "fate" => "late" })
  check("replay: rest", j["rest_mm"] == 28.68 && j["touch_ms"].nil?)
  t = JSON.parse(File.read(File.join(tmp, "replay", "naive-calm-02.json")))
  check("replay of the touch: no brake, touch time", t["brake_ms"].nil? && t["brake_by"].nil? && t["touch_ms"] == 799.0)
  check("replay of the touch: ends at the eye", t["samples"].last[1] == 0.0 && t["rest_mm"] == 0.0)

  # A naive log named ours, and our network reaching the eye.
  File.write(File.join(tmp, "ours-calm-07.log"), File.read(logs[2]))
  out = StringIO.new
  code = ArmCompare.report([File.join(tmp, "ours-calm-07.log")], out)
  check("a log of the other build fails", code == 1 && out.string.include?("FAIL ours-calm-07: built as naive, named ours"))
  File.write(File.join(tmp, "ours-calm-08.log"), File.read(logs[2]).gsub(/build naive/, "build ours"))
  out = StringIO.new
  code = ArmCompare.report([File.join(tmp, "ours-calm-08.log")], out)
  check("our network reaching the eye fails",
        code == 1 && out.string.include?("FAIL ours-calm-08: the tool reached the eye") &&
        out.string.include?("FAIL ours-calm-08: no stop by the person"))
  File.write(File.join(tmp, "naive-flood-03.log"), File.read(logs[1]).gsub(/^sink: arm-track-um 64 .*\n/, ""))
  out = StringIO.new
  code = ArmCompare.report([File.join(tmp, "naive-flood-03.log")], out)
  check("a track with a missing line fails", code == 1 && out.string.include?("FAIL naive-flood-03: no whole arm-track"))
end
check("old names are our network", ArmCompare.name_parts("calm-03.log") == { name: "calm-03", net: "ours", mode: "calm", run: 3 })
check("a stray name is refused", ArmCompare.name_parts("report.txt").nil?)

if $failures > 0
  puts "#{$failures} test(s) failed"
  exit 1
end
puts "all tests passed"
