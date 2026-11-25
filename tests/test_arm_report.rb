# Tests for scripts/arm_report.rb (D-082).
require_relative "../scripts/arm_report"
require "stringio"

$failures = 0
def check(name, ok)
  puts "#{ok ? 'PASS' : 'FAIL'} #{name}"
  $failures += 1 unless ok
end

dir = File.join(__dir__, "fixtures", "arm")
out = StringIO.new
code = ArmReport.report(%w[calm-01 flood-01 flood-02].map { |n| File.join(dir, "#{n}.txt") }, out)
text = out.string
check("good series exits 0", code == 0)
check("one row per run", text.scan(/^arm-stop (calm|flood)-0\d /).length == 3)
check("calm delay", text.include?("arm-stop calm n 1 delay median 2.472 ms p99 2.472 ms worst 2.472 ms (calm-01)"))
check("flood median of two is the mean", text.include?("arm-stop flood n 2 delay median 3.550 ms p99 4.100 ms worst 4.100 ms (flood-01)"))
check("flood reaction path", text.include?("reaction path median 0.888 mm p99 1.025 mm worst 1.025 mm"))
check("before the eye counted", text.include?("arm-stop flood stopped before the eye 2 of 2, in the 10 mm margin 2 of 2"))
check("model line", text.include?("steps over 1 ms 14 of 1712, URGENT inside a step 2"))
check("late line", text.include?("arm-stop flood late URGENT or COMMAND outside guest stalls and faults 0; runs without a late-check line 0"))

out = StringIO.new
code = ArmReport.report([File.join(dir, "hit-01.txt")], out)
check("a run that reached the eye fails", code == 1 && out.string.include?("FAIL hit-01.txt: the tool reached the eye"))
out = StringIO.new
code = ArmReport.report([File.join(dir, "none-01.txt")], out)
check("a run without a stop line fails", code == 1 && out.string.include?("FAIL none-01.txt: no arm-stop line"))
check("bad stop line is not parsed", ArmReport.parse_stop("sink: arm-stop event_ms x").nil?)
check("p99 nearest rank of 34 is the worst", ArmReport.percentile((1..34).to_a, 0.99) == 34)
check("p99 nearest rank of 200", ArmReport.percentile((1..200).to_a, 0.99) == 198)

if $failures > 0
  puts "#{$failures} test(s) failed"
  exit 1
end
puts "all tests passed"
