#!/usr/bin/ruby
# Report of the planned stop at the eye on both networks (D-089, D-090).
#   arm_precise.rb [--replay DIR] LOG...
#   arm_precise.rb --streaks LOG...
# LOG names are <ours|naive>-<calm|flood>-NN.log, as for arm_compare.rb.
#
# The plan: the stop margin is the URGENT deadline times the approach
# speed, 31 ms x 250 mm/s = 7.75 mm; "person in" comes when the tool is
# 7.75 + 25.0 mm from the eye, so with no delay the arm rests at the
# planned point and every millisecond of delay is 0.25 mm past it.
#
# One row per run: delay from the event to the brake, the extra path
# (speed x delay), where the tool rests from the eye and past the planned
# point, a touch of the eye, and the fate of the stop signal and of every
# flagged URGENT up to the one that braked. Then per network and mode:
# median, nearest-rank p99 and worst, and touches out of runs. Then the
# runs for the video: the worst of our network under flood (closest to
# the eye, then the later brake, D-086) and of the naive one the touch
# whose brake came latest (no brake is latest), or, with no touch, the
# largest extra path.
#
# A run fails when its log is not a whole run (arm_compare.rb), when the
# zone sensor did not report the planned event, or when the tool at the
# event was more than 0.05 mm off 32.75 mm by the track of the model
# (for example when the network held the arm in SAFE during the approach,
# so it lagged behind the plan). Such a run is named on a line of its
# own and left out of the medians, p99, worst, touches and the video.
# Our network touching the eye fails; the naive one touching it is a
# result. Exit 1 when any run failed.
#
# --streaks: URGENT dropped or lost in a row, in issue order, from the
# list "source: urgent-issued"; a log without the list counts only when
# it has at most one lost URGENT (a streak of 1 or none).
require "json"
require_relative "arm_compare"
require_relative "arm_report"

module ArmPrecise
  SPEED_MM_MS = 0.25
  MARGIN_MM = 7.75
  BRAKING_MM = 25.0
  EVENT_MM = MARGIN_MM + BRAKING_MM
  EVENT_TOL_MM = 0.05
  PLAN_RE = /^source: person-plan approach_start_ns (\d+) start_eye_um (-?\d+) offset_ns (\d+) event_ns (\d+) woke_late_ns (-?\d+) /

  module_function

  # Eye distance at t from the track, linear between grid points, mm.
  def eye_at(track, t_ns)
    off = t_ns - track[:start_ns]
    return nil if off.negative?
    i = off / track[:step_ns]
    um = track[:um]
    return nil if i >= um.length
    return um[i] / 1000.0 if i + 1 >= um.length
    f = (off % track[:step_ns]).to_f / track[:step_ns]
    (um[i] + (um[i + 1] - um[i]) * f) / 1000.0
  end

  def flagged_until_brake(r)
    flagged = (r[:text][/^source: person-in URGENT((?: \d+)+)$/, 1] || "").split(" ").map(&:to_i)
    flagged = flagged.select { |n| n <= r[:brake_number] } if r[:brake_number]
    fates = ArmCompare.urgent_fates(r[:text])
    flagged.map { |n| [n, fates[n] || "delivered"] }
  end

  def run_of(path)
    row, err = ArmCompare.run_of(path)
    return [nil, err] if row.nil?
    plan = row[:text].match(PLAN_RE)
    if plan.nil?
      why = row[:text][/^source: person-plan failed: .*$/] || "no person-plan line from source"
      return [nil, "#{row[:name]}: #{why}"]
    end
    if plan[4].to_i != row[:event_ns]
      return [nil, "#{row[:name]}: the event (#{row[:event_ns]}) is not the planned one (#{plan[4]})"]
    end
    if plan[1].to_i != row[:track][:start_ns]
      return [nil, "#{row[:name]}: the zone sensor saw approach #{plan[1]}, the track began at #{row[:track][:start_ns]}"]
    end
    event_mm = eye_at(row[:track], row[:event_ns])
    return [nil, "#{row[:name]}: the event is outside the track"] if event_mm.nil?
    rest = if row[:touched]
             0.0
           elsif row[:track][:end] == "stop"
             row[:track][:um].last / 1000.0
           else
             row[:rest_mm]
           end
    delay = row[:delay_ns]
    row.merge!(
      plan_start_eye_mm: plan[2].to_i / 1000.0, offset_ns: plan[3].to_i,
      woke_late_ns: plan[5].to_i, event_mm: event_mm,
      event_off_mm: event_mm - EVENT_MM, rest_mm: rest,
      past_plan_mm: MARGIN_MM - rest,
      extra_mm: delay ? SPEED_MM_MS * delay / 1e6 : nil,
      until_brake: flagged_until_brake(row)
    )
    [row, nil]
  end

  def replay(r)
    j = ArmCompare.replay(r)
    j["rest_mm"] = r[:rest_mm].round(3)
    j["plan_point_mm"] = MARGIN_MM
    j["event_eye_mm"] = r[:event_mm].round(3)
    j["extra_path_mm"] = r[:extra_mm]&.round(3)
    j["past_plan_mm"] = r[:past_plan_mm].round(3)
    j["urgent_until_brake"] = r[:until_brake].map { |n, f| { "number" => n, "fate" => f } }
    j
  end

  def worst_key(r)
    ArmCompare.worst_key(r)
  end

  # The naive run for the video: a touch with the latest brake (no brake
  # is latest), else the largest extra path.
  def naive_video(group)
    touched = group.select { |r| r[:touched] }
    unless touched.empty?
      return touched.max_by { |r| r[:brake_by] == "person" && r[:delay_ns] ? r[:delay_ns] : Float::INFINITY }
    end
    group.select { |r| r[:extra_mm] }.max_by { |r| r[:extra_mm] }
  end

  def stat(values, unit, fmt = "%.3f")
    v = values.compact.sort
    return "none" if v.empty?
    format("median #{fmt} #{unit} p99 #{fmt} #{unit} worst #{fmt} #{unit}",
           ArmReport.median(v), ArmReport.percentile(v, 0.99), v.last)
  end

  def report(paths, out = $stdout, replay_dir = nil)
    rows = []
    bad = []
    paths.sort.each do |path|
      row, err = run_of(path)
      if row.nil?
        bad << err
        next
      end
      rows << row
      if row[:event_off_mm].abs > EVENT_TOL_MM
        row[:off_plan] = true
        bad << format("%s: the tool was %.3f mm from the eye at the event, not %.2f +- %.2f",
                      row[:name], row[:event_mm], EVENT_MM, EVENT_TOL_MM)
      end
      next unless row[:net] == "ours"
      bad << "#{row[:name]}: the tool reached the eye" if row[:touched]
      bad << "#{row[:name]}: no stop by the person" if row[:stop].nil?
    end
    if replay_dir
      require "fileutils"
      FileUtils.mkdir_p(replay_dir)
      rows.each do |r|
        File.write(File.join(replay_dir, "#{r[:net]}-#{r[:mode]}-#{format('%02d', r[:run])}.json"),
                   JSON.generate(replay(r)) + "\n")
      end
    end
    out.puts "arm-precise plan: margin #{MARGIN_MM} mm (31 ms x 250 mm/s), event at #{EVENT_MM} mm from the eye, extra path 0.25 mm per ms of delay"
    out.puts "arm-precise run net mode event_mm delay_ms brake_by extra_mm rest_mm past_plan_mm touch stop_signal fate urgent_until_brake woke_late_us"
    rows.each do |r|
      list = r[:until_brake].map { |n, f| "#{n}:#{f.tr(' ', '_')}" }.join(",")
      out.puts format("arm-precise %s %s %s %.3f %s %s %s %.2f %.2f %s %d %s %s %.1f",
                      r[:name], r[:net], r[:mode], r[:event_mm], ArmCompare.fmt_ms(r[:delay_ns]),
                      r[:brake_by] || "none", r[:extra_mm] ? format("%.2f", r[:extra_mm]) : "-",
                      r[:rest_mm], r[:past_plan_mm], r[:touched] ? "yes" : "no", r[:stop_signal],
                      r[:stop_fate].tr(" ", "_"), list.empty? ? "-" : list, r[:woke_late_ns] / 1000.0)
    end
    rows.group_by { |r| [r[:net] == "ours" ? 0 : 1, r[:mode]] }.sort.each do |(_, mode), all|
      tag = "arm-precise #{all.first[:net]} #{mode}"
      off = all.select { |r| r[:off_plan] }
      group = all.reject { |r| r[:off_plan] }
      out.puts format("%s runs %d, failed the event check %d%s; the numbers below are over %d runs",
                      tag, all.length, off.length,
                      off.empty? ? "" : " (#{off.map { |r| format('%s %.3f mm', r[:name], r[:event_mm]) }.join(', ')})",
                      group.length)
      next if group.empty?
      person = group.select { |r| r[:brake_by] == "person" && r[:delay_ns] }
      worst = person.max_by { |r| r[:delay_ns] }
      out.puts format("%s n %d delay %s%s; braked by the person %d, by SAFE %d, no brake %d",
                      tag, group.length, stat(person.map { |r| r[:delay_ns] / 1e6 }, "ms"),
                      worst ? " (#{worst[:name]})" : "", person.length,
                      group.count { |r| r[:brake_by] == "state" }, group.count { |r| r[:brake_by].nil? })
      out.puts "#{tag} extra path #{stat(group.map { |r| r[:extra_mm] }, 'mm', '%.2f')}"
      out.puts "#{tag} past the planned point #{stat(group.map { |r| r[:past_plan_mm] }, 'mm', '%.2f')}"
      closest = group.min_by { |r| worst_key(r) }
      out.puts format("%s closest to the eye %.2f mm (%s); touched the eye %d of %d",
                      tag, closest[:rest_mm], closest[:name], group.count { |r| r[:touched] }, group.length)
      offs = group.map { |r| r[:event_off_mm].abs }.sort
      out.puts format("%s tool at the event: off 32.75 mm by at most %.3f mm; zone sensor woke late %s",
                      tag, offs.last, stat(group.map { |r| r[:woke_late_ns] / 1000.0 }, "us", "%.1f"))
      fates = group.map { |r| r[:stop_fate] }
      after = group.sum { |r| r[:until_brake].count { |_, f| f != "delivered" } }
      out.puts format("%s stop signal: delivered %d, late %d, dropped at relay A %d, at relay B %d, lost %d; flagged URGENT up to the brake not delivered in time %d",
                      tag, fates.count("delivered"), fates.count("late"), fates.count("dropped relay A"),
                      fates.count("dropped relay B"), fates.count("lost"), after)
      lates = group.map { |r| r[:late] }
      out.puts format("%s late URGENT or COMMAND outside guest stalls and faults %d; runs without a late-check line %d",
                      tag, lates.compact.sum { |l| l[/outside (\d+)/, 1].to_i }, lates.count(&:nil?))
    end
    ours = rows.select { |r| r[:net] == "ours" && r[:mode] == "flood" && !r[:off_plan] }
    unless ours.empty?
      w = ours.min_by { |r| worst_key(r) }
      out.puts format("arm-precise video ours flood %s: rest %.2f mm from the eye, %.2f mm past the plan, delay %s ms, stop signal %s",
                      w[:name], w[:rest_mm], w[:past_plan_mm], ArmCompare.fmt_ms(w[:delay_ns]), w[:stop_fate])
    end
    naive = rows.select { |r| r[:net] == "naive" && !r[:off_plan] }
    v = naive.empty? ? nil : naive_video(naive)
    if v
      out.puts format("arm-precise video naive %s: %s, rest %.2f mm from the eye, extra path %s mm, delay %s ms, stop signal %s",
                      v[:name], v[:touched] ? "touched the eye" : "no touch in the naive runs, largest extra path",
                      v[:rest_mm], v[:extra_mm] ? format("%.2f", v[:extra_mm]) : "none",
                      ArmCompare.fmt_ms(v[:delay_ns]), v[:stop_fate])
    end
    bad.each { |b| out.puts "arm-precise FAIL #{b}" }
    bad.empty? ? 0 : 1
  end

  # Lost (dropped or lost) URGENT in a row, per run.
  def streaks_of(text)
    fates = ArmCompare.urgent_fates(text)
    lost = fates.select { |_, f| f != "late" }.keys.sort
    issued = []
    text.scan(/^source: urgent-issued (\d+)((?: \d+)+)$/) do |_, values|
      issued.concat(values.split(" ").map(&:to_i))
    end
    count = text[/^source: urgent-issued count (\d+) not listed (\d+)$/]
    if count && Regexp.last_match(2).to_i.zero? && issued.length == Regexp.last_match(1).to_i
      runs = []
      cur = 0
      issued.each do |n|
        if lost.include?(n)
          cur += 1
        elsif cur.positive?
          runs << cur
          cur = 0
        end
      end
      runs << cur if cur.positive?
      return [runs, issued.length, "list"]
    end
    m = text.match(/^sink: summary URGENT received (\d+), on time \d+, late \d+, lost (\d+),/)
    return [nil, 0, "no list and no sink summary"] if m.nil?
    total = m[1].to_i + m[2].to_i
    return [nil, total, "no list and #{lost.length} lost"] if lost.length > 1
    [lost.length == 1 ? [1] : [], total, "summary"]
  end

  def streaks(paths, out = $stdout)
    hist = Hash.new(0)
    total = 0
    unknown = []
    by = Hash.new(0)
    paths.sort.each do |path|
      runs, n, how = streaks_of(File.read(path))
      if runs.nil?
        unknown << "#{File.basename(path)} (#{how})"
        next
      end
      by[how] += 1
      total += n
      runs.each { |len| hist[len >= 3 ? 3 : len] += 1 }
    end
    lost = hist.sum { |len, c| len * c }
    out.puts format("arm-streaks logs %d (with the issued list %d, from the sink summary %d), URGENT %d, dropped or lost %d",
                    paths.length - unknown.length, by["list"], by["summary"], total, lost)
    out.puts format("arm-streaks in a row: 1 x %d, 2 x %d, 3 or more x %d", hist[1], hist[2], hist[3])
    [1, 2, 3].each do |len|
      out.puts format("arm-streaks a streak of %d%s adds %d URGENT periods (%d ms) before the next flagged URGENT: %.1f mm more at 250 mm/s",
                      len, len == 3 ? " (or more, each one more adds 5 mm)" : "", len, len * 20, len * 20 * SPEED_MM_MS)
    end
    unknown.each { |u| out.puts "arm-streaks cannot count #{u}" }
    unknown.empty? ? 0 : 1
  end
end

if $PROGRAM_NAME == __FILE__
  args = ARGV.dup
  if args.first == "--streaks"
    args.shift
    if args.empty?
      warn "usage: arm_precise.rb --streaks LOG..."
      exit 2
    end
    exit ArmPrecise.streaks(args)
  end
  replay_dir = nil
  if args.first == "--replay"
    args.shift
    replay_dir = args.shift
  end
  if args.empty? || replay_dir == ""
    warn "usage: arm_precise.rb [--replay DIR] LOG... | --streaks LOG..."
    exit 2
  end
  exit ArmPrecise.report(args, $stdout, replay_dir)
end
