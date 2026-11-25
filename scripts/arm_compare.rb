#!/usr/bin/ruby
# Report of the arm stop on both networks (D-084 to D-086), and the track
# of every run for the replay page (D-085).
#   arm_compare.rb [--replay DIR] LOG...
# LOG names are <ours|naive>-<calm|flood>-NN.log; a name without the
# network (calm-NN.log) is our network. One row per run, then per network
# and mode: the delay from "person in" to the brake (median, nearest-rank
# p99, worst), the reaction path, the closest the tool came to the eye,
# how many runs reached the eye, and how many stop signals were dropped or
# lost. Then the worst run of each network by name: the run where the tool
# came closest to the eye; between runs that reached it, the later brake
# (no brake at all counts as the latest).
# Exit 1 when a log cannot be read as a run, when its build is not the
# network its name says, or when a run of our network reached the eye or
# had no stop by the person. The naive network reaching the eye is a
# result, not a failure.
require "json"
require_relative "arm_report"

module ArmCompare
  TRACK_RE = /^sink: arm-track start_ns (\d+) step_ns (\d+) samples (\d+) end (\w+) brake_ns (\d+) brake_by (\w+) stop_number (\d+) touch_ns (\d+)$/

  module_function

  def name_parts(path)
    base = File.basename(path, File.extname(path))
    m = base.match(/\A(?:(ours|naive)-)?(calm|flood)-(\d+)\z/)
    return nil if m.nil?
    { name: base, net: m[1] || "ours", mode: m[2], run: m[3].to_i }
  end

  def parse_track(text)
    m = text.match(TRACK_RE)
    return nil if m.nil?
    n = m[3].to_i
    um = Array.new(n)
    text.scan(/^sink: arm-track-um (\d+)((?: -?\d+)+)$/) do |first, values|
      values.split(" ").each_with_index do |v, k|
        i = first.to_i + k
        um[i] = v.to_i if i < n
      end
    end
    return nil if um.any?(&:nil?)
    { start_ns: m[1].to_i, step_ns: m[2].to_i, end: m[4], brake_ns: m[5].to_i,
      brake_by: m[6], stop_number: m[7].to_i, touch_ns: m[8].to_i, um: um }
  end

  def builds(text)
    found = []
    found << Regexp.last_match(1) if text =~ /^source: SCHED_FIFO ceiling .* build (\w+)$/
    text.scan(/^relay signal-relay-[ab]: build (\w+), queue \d+$/) { |b| found << b[0] }
    found << Regexp.last_match(1) if text =~ /^sink: arm model every .* build (\w+)$/
    found
  end

  # The fate of each URGENT number the log names.
  def urgent_fates(text)
    fate = {}
    text.scan(/^sink: late signal (\d+) class URGENT at/) { |n| fate[n[0].to_i] = "late" }
    text.scan(/^(?:sink|relay signal-relay-[ab]): lost signal (\d+) class URGENT$/) do |n|
      fate[n[0].to_i] = "lost"
    end
    text.scan(/^relay signal-relay-([ab]): dropped signal (\d+) class URGENT at/) do |hop, n|
      fate[n.to_i] = "dropped relay #{hop.upcase}"
    end
    fate
  end

  def run_of(path)
    parts = name_parts(path)
    return [nil, "#{File.basename(path)}: the name is not <net>-<mode>-NN"] if parts.nil?
    text = File.read(path)
    b = builds(text)
    if b.length != 4 || b.uniq != [parts[:net]]
      return [nil, "#{parts[:name]}: built as #{b.uniq.join('+')}, named #{parts[:net]} (4 build lines expected, #{b.length} found)"]
    end
    track = parse_track(text)
    return [nil, "#{parts[:name]}: no whole arm-track"] if track.nil?
    ev = text.match(/^source: person-in event_ns (\d+) URGENT (\d+)$/)
    return [nil, "#{parts[:name]}: no person-in event line from source"] if ev.nil?
    event_ns = ev[1].to_i
    stop_number = ev[2].to_i
    flagged = (text[/^source: person-in URGENT((?: \d+)+)$/, 1] || "").split(" ").map(&:to_i)
    fates = urgent_fates(text)
    stop = ArmReport.parse_stop(text)
    brake_ns = nil
    brake_by = nil
    if track[:brake_by] != "none"
      brake_ns = track[:brake_ns]
      brake_by = track[:brake_by]
    elsif stop
      brake_ns = event_ns + stop["delay_ns"].to_i
      brake_by = "person"
    end
    brake_number = track[:stop_number] > 0 ? track[:stop_number] : nil
    touched = track[:end] == "touch" || (stop && stop["before_eye"] != "yes")
    rest_mm = if touched
                0.0
              elsif stop
                stop["eye_rest_mm"]
              else
                track[:um].last / 1000.0
              end
    lost_flagged = flagged.select { |n| fates[n] && fates[n] != "late" }
    lost_flagged = lost_flagged.select { |n| n < brake_number } if brake_number
    delay_ns = if stop
                 stop["delay_ns"]
               elsif brake_ns
                 brake_ns - event_ns
               end
    row = parts.merge(
      text: text, track: track, stop: stop, event_ns: event_ns,
      stop_signal: stop_number, stop_fate: fates[stop_number] || "delivered",
      brake_ns: brake_ns, brake_by: brake_by, brake_number: brake_number,
      touched: touched, rest_mm: rest_mm, delay_ns: delay_ns,
      lost_flagged: lost_flagged.length,
      model: ArmReport.model_line(text), late: ArmReport.late_line(text)
    )
    [row, nil]
  end

  def ms(ns)
    ns.nil? ? nil : (ns / 1e6).round(3)
  end

  def replay(r)
    t = r[:track]
    start = t[:start_ns]
    {
      "network" => r[:net], "mode" => r[:mode], "run" => r[:run], "log" => "#{r[:name]}.log",
      "approach_start_ns" => start, "step_ns" => t[:step_ns],
      "samples" => t[:um].each_with_index.map { |v, i| [(i * t[:step_ns] / 1e6).round(3), v / 1000.0] },
      "event_ms" => ms(r[:event_ns] - start),
      "brake_ms" => r[:brake_ns] ? ms(r[:brake_ns] - start) : nil,
      "brake_by" => r[:brake_by],
      "touch_ms" => t[:end] == "touch" ? ms(t[:touch_ns] - start) : nil,
      "rest_mm" => r[:rest_mm].round(3),
      "track_end" => t[:end],
      "stop_signal" => { "number" => r[:stop_signal], "class" => "URGENT", "fate" => r[:stop_fate] },
      "brake_signal" => r[:brake_number],
      "flagged_lost_before_brake" => r[:lost_flagged]
    }
  end

  # Smaller is worse: closest to the eye, then the later brake.
  def worst_key(r)
    delay = r[:brake_by] == "person" && r[:delay_ns] ? r[:delay_ns] : Float::INFINITY
    [r[:rest_mm], -delay]
  end

  def fmt_ms(ns)
    ns.nil? ? "none" : format("%.3f", ns / 1e6)
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
    out.puts "arm-compare run net mode delay_ms brake_by reaction_mm eye_event_mm rest_mm reached_eye stop_signal fate flagged_lost"
    rows.each do |r|
      s = r[:stop]
      out.puts format("arm-compare %s %s %s %s %s %s %s %.1f %s %d %s %d",
                      r[:name], r[:net], r[:mode], fmt_ms(r[:delay_ns]), r[:brake_by] || "none",
                      s ? format("%.3f", s["reaction_mm"]) : "-",
                      s ? format("%.1f", s["eye_event_mm"]) : "-",
                      r[:rest_mm], r[:touched] ? "yes" : "no", r[:stop_signal],
                      r[:stop_fate].tr(" ", "_"), r[:lost_flagged])
    end
    rows.group_by { |r| [r[:net] == "ours" ? 0 : 1, r[:mode]] }.sort.each do |(_, mode), group|
      net = group.first[:net]
      tag = "arm-compare #{net} #{mode}"
      person = group.select { |r| r[:brake_by] == "person" && r[:delay_ns] }
      delays = person.map { |r| r[:delay_ns] }.sort
      if delays.empty?
        out.puts "#{tag} n #{group.length} delay: no run braked by the person"
      else
        worst_delay = person.max_by { |r| r[:delay_ns] }
        out.puts format("%s n %d delay median %.3f ms p99 %.3f ms worst %.3f ms (%s); braked by the person %d, by SAFE %d, no brake %d",
                        tag, group.length, ArmReport.median(delays) / 1e6,
                        ArmReport.percentile(delays, 0.99) / 1e6, delays.last / 1e6, worst_delay[:name],
                        person.length, group.count { |r| r[:brake_by] == "state" },
                        group.count { |r| r[:brake_by].nil? })
      end
      reaction = group.map { |r| r[:stop] && r[:stop]["reaction_mm"] }.compact.sort
      unless reaction.empty?
        out.puts format("%s reaction path median %.3f mm p99 %.3f mm worst %.3f mm (%d runs with a stop line)",
                        tag, ArmReport.median(reaction), ArmReport.percentile(reaction, 0.99),
                        reaction.last, reaction.length)
      end
      closest = group.min_by { |r| worst_key(r) }
      out.puts format("%s closest to the eye %.1f mm (%s); reached the eye %d of %d",
                      tag, closest[:rest_mm], closest[:name], group.count { |r| r[:touched] }, group.length)
      fates = group.map { |r| r[:stop_fate] }
      lost = fates.count { |f| f.start_with?("dropped", "lost") }
      out.puts format("%s stop signal dropped or lost in %d of %d runs (dropped at relay A %d, at relay B %d, lost %d; late %d); flagged URGENT dropped or lost before the brake %d in %d runs",
                      tag, lost, group.length, fates.count("dropped relay A"), fates.count("dropped relay B"),
                      fates.count("lost"), fates.count("late"), group.sum { |r| r[:lost_flagged] },
                      group.count { |r| r[:lost_flagged] > 0 })
      models = group.map { |r| r[:model] }.compact
      unless models.empty?
        out.puts format("%s model step max %.3f ms, steps over 1 ms %d of %d, URGENT inside a step %d",
                        tag, models.map { |m| m[:max_ns] }.max / 1e6, models.sum { |m| m[:over_ms] },
                        models.sum { |m| m[:steps] }, models.sum { |m| m[:urgent_in_step] })
      end
      lates = group.map { |r| r[:late] }
      out.puts format("%s late URGENT or COMMAND outside guest stalls and faults %d; runs without a late-check line %d",
                      tag, lates.compact.sum { |l| l[/outside (\d+)/, 1].to_i }, lates.count(&:nil?))
    end
    rows.group_by { |r| r[:net] }.sort_by { |net, _| net == "ours" ? 0 : 1 }.each do |net, group|
      w = group.min_by { |r| worst_key(r) }
      out.puts format("arm-compare %s worst run %s: rest %.1f mm from the eye, delay %s ms, brake by %s, stop signal %s",
                      net, w[:name], w[:rest_mm], fmt_ms(w[:delay_ns]), w[:brake_by] || "none", w[:stop_fate])
    end
    bad.each { |b| out.puts "arm-compare FAIL #{b}" }
    bad.empty? ? 0 : 1
  end
end

if $PROGRAM_NAME == __FILE__
  args = ARGV.dup
  replay_dir = nil
  if args.first == "--replay"
    args.shift
    replay_dir = args.shift
  end
  if args.empty? || replay_dir == ""
    warn "usage: arm_compare.rb [--replay DIR] LOG..."
    exit 2
  end
  exit ArmCompare.report(args, $stdout, replay_dir)
end
