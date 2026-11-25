#!/usr/bin/ruby
# Report of an arm stop series (D-082): one row per run, then per mode
# the median, p99 and worst of the delay from "person in" to the brake,
# and of both paths. p99 is the nearest-rank 99th percentile; with 34
# runs it is the worst run.
#   arm_report.rb LOG...        (calm-NN.log and flood-NN.log)
# Exit 1 when a log has no arm-stop line or a stop did not end short of
# the eye.

module ArmReport
  KEYS = %w[event_ms delay_ns issue_to_brake_ns speed_mm_s reaction_mm
            braking_mm eye_event_mm eye_brake_mm eye_rest_mm before_eye
            margin released].freeze

  module_function

  def parse_stop(text)
    line = text[/^sink: arm-stop .*$/]
    return nil if line.nil?
    words = line.split(" ")[2..-1]
    return nil unless words.length == KEYS.length * 2
    row = {}
    KEYS.each_with_index do |key, i|
      return nil unless words[2 * i] == key
      value = words[2 * i + 1]
      row[key] = %w[before_eye margin released].include?(key) ? value : Float(value)
    end
    row
  rescue ArgumentError
    nil
  end

  def model_line(text)
    m = text.match(/^sink: arm model steps (\d+), .* step median (\d+) us, max (\d+) ns, over 1 ms (\d+), URGENT inside a step (\d+),/)
    return nil if m.nil?
    { steps: m[1].to_i, median_us: m[2].to_i, max_ns: m[3].to_i,
      over_ms: m[4].to_i, urgent_in_step: m[5].to_i }
  end

  def late_line(text)
    text[/^late-check: late URGENT or COMMAND inside guest stalls \d+, inside faults \d+, outside \d+$/]
  end

  def percentile(sorted, p)
    return nil if sorted.empty?
    rank = (p * sorted.length).ceil
    rank = 1 if rank < 1
    sorted[rank - 1]
  end

  def median(sorted)
    return nil if sorted.empty?
    n = sorted.length
    n.odd? ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0
  end

  def mode_of(path)
    File.basename(path)[/\A(calm|flood)/, 1] || "run"
  end

  def report(paths, out = $stdout)
    rows = []
    bad = []
    paths.sort.each do |path|
      text = File.read(path)
      stop = parse_stop(text)
      if stop.nil?
        bad << "#{File.basename(path)}: no arm-stop line"
        next
      end
      rows << { name: File.basename(path, File.extname(path)), mode: mode_of(path), stop: stop,
                model: model_line(text), late: late_line(text) }
      bad << "#{File.basename(path)}: the tool reached the eye" if stop["before_eye"] != "yes"
    end
    out.puts "arm-stop run delay_ms issue_to_brake_ms speed_mm_s reaction_mm braking_mm eye_event_mm eye_rest_mm before_eye margin"
    rows.each do |r|
      s = r[:stop]
      out.puts format("arm-stop %s %.3f %.3f %.1f %.3f %.3f %.1f %.1f %s %s",
                      r[:name], s["delay_ns"] / 1e6, s["issue_to_brake_ns"] / 1e6,
                      s["speed_mm_s"], s["reaction_mm"], s["braking_mm"],
                      s["eye_event_mm"], s["eye_rest_mm"], s["before_eye"], s["margin"])
    end
    rows.group_by { |r| r[:mode] }.sort.each do |mode, group|
      delays = group.map { |r| r[:stop]["delay_ns"] }.sort
      reaction = group.map { |r| r[:stop]["reaction_mm"] }.sort
      braking = group.map { |r| r[:stop]["braking_mm"] }.sort
      worst = group.max_by { |r| r[:stop]["delay_ns"] }
      before = group.count { |r| r[:stop]["before_eye"] == "yes" }
      margin = group.count { |r| r[:stop]["margin"] == "yes" }
      out.puts format("arm-stop %s n %d delay median %.3f ms p99 %.3f ms worst %.3f ms (%s)",
                      mode, group.length, median(delays) / 1e6,
                      percentile(delays, 0.99) / 1e6, delays.last / 1e6, worst[:name])
      out.puts format("arm-stop %s reaction path median %.3f mm p99 %.3f mm worst %.3f mm; braking path median %.3f mm worst %.3f mm",
                      mode, median(reaction), percentile(reaction, 0.99), reaction.last,
                      median(braking), braking.last)
      out.puts format("arm-stop %s stopped before the eye %d of %d, in the 10 mm margin %d of %d; worst run rests %.1f mm from the eye",
                      mode, before, group.length, margin, group.length,
                      worst[:stop]["eye_rest_mm"])
      models = group.map { |r| r[:model] }.compact
      unless models.empty?
        out.puts format("arm-stop %s model step median %d-%d us, max %.3f ms, steps over 1 ms %d of %d, URGENT inside a step %d",
                        mode, models.map { |m| m[:median_us] }.min, models.map { |m| m[:median_us] }.max,
                        models.map { |m| m[:max_ns] }.max / 1e6,
                        models.sum { |m| m[:over_ms] }, models.sum { |m| m[:steps] },
                        models.sum { |m| m[:urgent_in_step] })
      end
      lates = group.map { |r| r[:late] }
      outside = lates.compact.sum { |l| l[/outside (\d+)/, 1].to_i }
      out.puts format("arm-stop %s late URGENT or COMMAND outside guest stalls and faults %d; runs without a late-check line %d",
                      mode, outside, lates.count(&:nil?))
    end
    bad.each { |b| out.puts "arm-stop FAIL #{b}" }
    bad.empty? ? 0 : 1
  end
end

if $PROGRAM_NAME == __FILE__
  if ARGV.empty?
    warn "usage: arm_report.rb LOG..."
    exit 2
  end
  exit ArmReport.report(ARGV)
end
