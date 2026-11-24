#!/usr/bin/ruby
# encoding: utf-8
# Table for S1-S4. The three verdict words are the ones batch B19 prints.
# A pass is allowed only for a threshold already written in DECISIONS.md.

PASS = "прошёл".freeze
FAIL = "не прошёл".freeze
NONE = "порога нет".freeze

# D-051 (r): R5 violations across the 34+34 measure. Applied as written.
R5_MAX = 31

# D-058 deadlines and the R1, R2, R10 thresholds fixed before the B20 run.
URGENT_DEADLINE_NS = 31_000_000
R1_MIN_N = 100
R10_MEDIAN_DELTA_NS = 5_000_000

class ScenarioReport
  def self.run(paths)
    new(paths).run
  end

  def initialize(paths)
    @paths = paths
    @rows = []
    # Unset means this live window has no absolute median ceiling.
    # D-051's 1.19 ms stays on make measure. Setting the variable is the
    # broken-condition check: a ceiling below the latency of this run.
    @r2_max = read_override("SCENARIO_R2_MAX_NS")
  end

  def run
    if @paths.length != 4 || @r2_max == :bad
      warn "usage: scenario_report.rb S1.log S2.log S3.log S4.log"
      return 2
    end
    logs = @paths.map { |path| File.exist?(path) ? File.read(path) : nil }
    if logs.any?(&:nil?)
      warn "scenario report: a log is missing"
      return 2
    end
    s1, s2, s3, s4 = logs
    @late = {
      "S1" => late_check(s1), "S2" => late_check(s2),
      "S3" => late_check(s3), "S4" => late_check(s4)
    }
    print_late_lines([["S1", s1], ["S2", s2], ["S3", s3], ["S4", s4]])
    lat1 = latency(s1_path)
    lat2 = latency(s2_path)
    lat4 = latency(s4_path)
    fill_s1(s1, lat1)
    fill_s2(s2, lat2, s1, lat1)
    fill_s3(s3)
    fill_s4(s4, lat4, s1, lat1)
    print_table
    failed = @rows.count { |row| row[3] == FAIL }
    puts "scenarios failed #{failed}"
    failed == 0 ? 0 : 1
  end

  private

  def s1_path
    @paths[0]
  end

  def s2_path
    @paths[1]
  end

  def s4_path
    @paths[3]
  end

  # nil when the variable is unset. :bad when it is not a whole number.
  def read_override(name)
    raw = ENV[name]
    return nil if raw.nil? || raw.empty?
    return :bad unless raw =~ /\A[0-9]+\z/
    raw.to_i
  end

  def latency(path)
    bin = ENV["LATENCY_REPORT"] || "build/latency-report"
    return "" unless File.executable?(bin)
    IO.popen([bin, path], "r") { |io| io.read }
  rescue StandardError
    ""
  end

  def add(scenario, requirement, numbers, verdict)
    @rows << [scenario, requirement, numbers, verdict]
  end

  def guest_exit(text)
    match = text[/^scenario-host: guest exit (\d+)/, 1]
    match && match.to_i
  end

  def summaries(text)
    found = {}
    text.scan(/^sink: summary (\S+) received (\d+), on time (\d+), late (\d+), lost (\d+), dropped (\d+)/) do
      |name, received, on_time, late, lost, dropped|
      found[name] = {
        :received => received.to_i,
        :on_time => on_time.to_i,
        :late => late.to_i,
        :lost => lost.to_i,
        :dropped => dropped.to_i
      }
    end
    found
  end

  def accounted(row)
    return 0 if row.nil?
    row[:on_time] + row[:late] + row[:lost] + row[:dropped]
  end

  def unaccounted(text)
    values = text.scan(/^sink: unaccounted (\d+)/).flatten
    return nil if values.empty?
    values.last.to_i
  end

  def states(text)
    text.scan(/^sink: state (\S+)/).flatten
  end

  def series(text, klass)
    return nil if text.nil? || text.empty?
    match = text.match(
      /^latency class #{klass} total n (\d+) median (-?\d+) ns p99 (n\/a|-?\d+ ns) max (-?\d+) ns$/
    )
    return nil if match.nil?
    {
      :n => match[1].to_i,
      :median => match[2].to_i,
      :p99 => match[3],
      :max => match[4].to_i
    }
  end

  def r5_sum(text)
    return nil if text.nil?
    lines = text.scan(/^relay \S+ R5 violations (\d+)$/).flatten
    return nil if lines.empty?
    lines.map(&:to_i).inject(0, :+)
  end

  def clock_ns(token)
    return nil if token.nil?
    match = token.match(/\A(\d+)\.(\d+)\z/)
    return nil if match.nil?
    match[1].to_i * 1_000_000_000 + match[2].ljust(9, "0")[0, 9].to_i
  end

  def fraction_text(row)
    return "missing" if row.nil?
    den = accounted(row)
    return "0/0" if den == 0
    "#{row[:on_time]}/#{den}"
  end

  def all_on_time?(row)
    !row.nil? && accounted(row) >= R1_MIN_N && row[:on_time] == accounted(row)
  end

  # qnx-late-check.sh lines (D-064), summed over every block in the log
  # (S3 has one per section). nil when the log has none: then late
  # signals were not held against the stall probe.
  def late_check(text)
    return nil if text.nil?
    rows = text.scan(
      /^late-check: late (URGENT|COMMAND) (\d+): inside guest stalls (\d+), inside faults (\d+), outside (\d+)$/
    )
    return nil if rows.empty?
    found = {}
    %w[URGENT COMMAND].each do |name|
      found[name] = { :late => 0, :stall => 0, :fault => 0, :outside => 0 }
    end
    rows.each do |name, late, stall, fault, outside|
      found[name][:late] += late.to_i
      found[name][:stall] += stall.to_i
      found[name][:fault] += fault.to_i
      found[name][:outside] += outside.to_i
    end
    probe = text.scan(/^late-check: stall probe from -?\d+ ms, stalls over 5 ms (\d+), worst (\d+) ns$/)
    found[:stalls] = probe.map { |n, _| n.to_i }.inject(0, :+)
    found[:worst] = probe.map { |_, w| w.to_i }.max || 0
    found[:missing] = text.scan(/^late-check: FAIL no stall probe output/).length
    found
  end

  def late_outside(check)
    return nil if check.nil?
    check["URGENT"][:outside] + check["COMMAND"][:outside]
  end

  # URGENT that were late while the whole guest stood still. They are
  # set aside from R1, R2, and R10; the thresholds do not change.
  def excused(scenario)
    check = @late[scenario]
    check.nil? ? 0 : check["URGENT"][:stall]
  end

  # R1 with the D-064 rule: every URGENT is on time, or late inside a
  # guest stall the probe saw, and none is late outside one.
  def r1_ok?(row, scenario)
    return false if row.nil? || accounted(row) < R1_MIN_N
    outside = late_outside(@late[scenario])
    return false if !outside.nil? && outside > 0
    row[:on_time] + excused(scenario) == accounted(row)
  end

  def excused_text(scenario)
    n = excused(scenario)
    n > 0 ? "; late inside guest stalls #{n}" : ""
  end

  def print_late_lines(logs)
    logs.each do |scenario, text|
      next if text.nil?
      text.scan(/^late-check: (?:FAIL )?late (?:URGENT|COMMAND) \d+ .*$/).each do |line|
        puts "#{scenario} #{line}"
      end
    end
  end

  def add_late_row(scenario)
    check = @late[scenario]
    if check.nil? || check[:missing] > 0
      add(scenario, "late",
          "late check missing: no stall probe output, late URGENT and COMMAND cannot be told apart",
          FAIL)
      return
    end
    parts = %w[URGENT COMMAND].map do |name|
      c = check[name]
      "#{name} late #{c[:late]}: inside guest stalls #{c[:stall]}, " \
        "inside faults #{c[:fault]}, outside #{c[:outside]}"
    end
    outside = late_outside(check)
    add(scenario, "late",
        "#{parts.join('; ')}; guest stalls over 5 ms #{check[:stalls]}, " \
        "worst #{check[:worst]} ns; threshold outside 0",
        outside == 0 ? PASS : FAIL)
  end

  def p99_ns(item)
    return nil if item.nil?
    text = item[:p99].to_s
    return nil if text == "n/a"
    match = text.match(/\A(-?\d+) ns\z/)
    return nil if match.nil?
    match[1].to_i
  end

  def drop_times(text)
    text.scan(/dropped signal \d+ class BACKGROUND at (\d+) ns/).flatten.map(&:to_i)
  end

  # Counts of BACKGROUND drops in the first, middle, and last third of
  # the span from the earliest drop to the latest. nil when the span
  # does not cover three separated thirds.
  def drop_thirds(times)
    return nil if times.nil? || times.length < 3
    ordered = times.sort
    span = ordered.last - ordered.first
    return nil if span <= 0
    first_end = ordered.first + span / 3
    second_end = ordered.first + (2 * span) / 3
    counts = [0, 0, 0]
    ordered.each do |stamp|
      if stamp < first_end
        counts[0] += 1
      elsif stamp < second_end
        counts[1] += 1
      else
        counts[2] += 1
      end
    end
    counts
  end

  # INIT before the first NORMAL is the machine starting. Anything
  # after that first NORMAL, other than NORMAL, left the calm state.
  def stayed_normal?(text)
    seen = states(text)
    calm = false
    return false if seen.empty?
    seen.each do |name|
      if calm
        return false unless name == "NORMAL"
      elsif name == "NORMAL"
        calm = true
      elsif name != "INIT"
        return false
      end
    end
    calm
  end

  def fill_s1(text, latency_text)
    sums = summaries(text)
    urgent = sums["URGENT"]
    den = accounted(urgent)
    share = den == 0 ? "missing" : "#{urgent[:on_time]}/#{den}"
    verdict = r1_ok?(urgent, "S1") ? PASS : FAIL
    add("S1", "R1",
        "URGENT on time #{share}#{excused_text('S1')}; threshold 1 and n >= #{R1_MIN_N}",
        verdict)

    detail = %w[URGENT COMMAND BACKGROUND].map do |name|
      item = series(latency_text, name)
      if item.nil?
        "#{name} missing"
      else
        "#{name} n #{item[:n]} median #{item[:median]} ns p99 #{item[:p99]} max #{item[:max]} ns"
      end
    end.join("; ")
    add("S1", "R3", detail, NONE)

    books = unaccounted(text)
    if books.nil?
      add("S1", "R4", "unaccounted missing", FAIL)
    elsif books == 0
      add("S1", "R4", "unaccounted 0; threshold 0", PASS)
    else
      add("S1", "R4", "unaccounted #{books}; threshold 0", FAIL)
    end

    counts = %w[URGENT COMMAND BACKGROUND].map { |name|
      "#{name} #{accounted(sums[name])}"
    }.join(" ")
    path = text.include?("live: path /dev/signal-source")
    problems = []
    problems << "path missing" unless path
    problems << "a class has no signals" if %w[URGENT COMMAND BACKGROUND].any? { |name|
      accounted(sums[name]) == 0
    }
    problems << "state left NORMAL" unless stayed_normal?(text)
    exit_code = guest_exit(text)
    problems << "guest exit #{exit_code}" if exit_code && exit_code != 0
    problems << "latency missing" if series(latency_text, "URGENT").nil?
    verdict = problems.empty? ? PASS : FAIL
    numbers = "path #{path ? 'yes' : 'no'}; #{counts}; states #{states(text).uniq.join(',')}"
    numbers += "; #{problems.join('; ')}" unless problems.empty?
    add("S1", "checks", numbers, verdict)
    add_late_row("S1")
  end

  def fill_s2(text, latency_text, calm, calm_latency)
    calm_urgent = series(calm_latency, "URGENT")
    flood_urgent = series(latency_text, "URGENT")
    calm_share = fraction_text(summaries(calm)["URGENT"])
    flood_share = fraction_text(summaries(text)["URGENT"])
    share_ok = r1_ok?(summaries(text)["URGENT"], "S2") &&
               r1_ok?(summaries(calm)["URGENT"], "S1")
    add("S2", "R2 share",
        "URGENT on time flood #{flood_share}#{excused_text('S2')} against S1 " \
        "#{calm_share}#{excused_text('S1')}; threshold 1 and n >= #{R1_MIN_N}",
        share_ok ? PASS : FAIL)

    if flood_urgent.nil? || calm_urgent.nil?
      add("S2", "R2 median", "median missing", FAIL)
    else
      limit_2x = calm_urgent[:median] * 2
      p99 = p99_ns(flood_urgent)
      # An URGENT above the deadline is late. When every such URGENT
      # was on its way during a guest stall, the samples left are all
      # within the deadline, so p99 and max of those meet it (D-064).
      flood_row = summaries(text)["URGENT"]
      above = flood_row ? flood_row[:late] : 0
      set_aside = above > 0 && excused("S2") == above &&
                  late_outside(@late["S2"]) == 0
      within = !p99.nil? &&
               ((p99 <= URGENT_DEADLINE_NS && flood_urgent[:max] <= URGENT_DEADLINE_NS) ||
                set_aside)
      ok = flood_urgent[:median] <= limit_2x &&
           flood_urgent[:n] >= R1_MIN_N &&
           within
      if !@r2_max.nil? && flood_urgent[:median] > @r2_max
        ok = false
      end
      extra = @r2_max.nil? ? "" : "; threshold #{@r2_max} ns"
      add("S2", "R2 median",
          "URGENT median #{flood_urgent[:median]} ns; S1 median #{calm_urgent[:median]} ns" \
          "#{extra}; threshold 2x S1 #{limit_2x} ns; " \
          "p99 #{flood_urgent[:p99]}; max #{flood_urgent[:max]} ns; " \
          "deadline #{URGENT_DEADLINE_NS} ns; n #{flood_urgent[:n]}" \
          "#{set_aside ? "; above the deadline #{above}, each inside a guest stall" : ''}",
          ok ? PASS : FAIL)
    end

    sums = summaries(text)
    books = unaccounted(text)
    urgent_drop = sums["URGENT"] && sums["URGENT"][:dropped]
    command_drop = sums["COMMAND"] && sums["COMMAND"][:dropped]
    background_drop = sums["BACKGROUND"] && sums["BACKGROUND"][:dropped]
    if books.nil? || urgent_drop.nil? || command_drop.nil?
      add("S2", "R4", "counts missing", FAIL)
    else
      ok = books == 0 && urgent_drop == 0 && command_drop == 0
      add("S2", "R4",
          "unaccounted #{books}; dropped URGENT #{urgent_drop} " \
          "COMMAND #{command_drop} BACKGROUND #{background_drop}; " \
          "threshold unaccounted 0 and URGENT 0 and COMMAND 0",
          ok ? PASS : FAIL)
    end

    violations = r5_sum(latency_text)
    if violations.nil?
      add("S2", "R5", "violations missing", FAIL)
    elsif violations <= R5_MAX
      add("S2", "R5", "violations #{violations}; threshold #{R5_MAX}", PASS)
    else
      add("S2", "R5", "violations #{violations}; threshold #{R5_MAX}", FAIL)
    end

    times = text.scan(/^source: flood signal \d+ at (\d+) ns/).flatten.map(&:to_i)
    window_ms = text[/^s2: window ms (\d+)/, 1]
    period = text[/^source: flood every (\d+) ns/, 1]
    burst = text[/burst (\d+)/, 1]
    start_ns = clock_ns(text[/^s2: window start (\S+)/, 1])
    end_ns = clock_ns(text[/^s2: window end (\S+)/, 1])
    duration = (start_ns && end_ns) ? end_ns - start_ns : nil
    issued = text.scan(/^source: sent signal \d+ class BACKGROUND$/).length
    span = times.length >= 2 ? times.last - times.first : nil
    window_ns = window_ms ? window_ms.to_i * 1_000_000 : nil
    period_ns = period ? period.to_i : nil
    burst_n = burst ? burst.to_i : nil
    held = span && period_ns && window_ns && burst_n &&
           times.length > burst_n &&
           span >= period_ns &&
           span + period_ns >= window_ns - 80_000_000
    duration_ok = duration && window_ns &&
                  duration >= window_ns - 200_000_000 &&
                  duration <= window_ns + 1_500_000_000
    exit_code = guest_exit(text)
    problems = []
    problems << "flood did not cover the window" unless held
    problems << "window duration missing or outside the sleep" unless duration_ok
    problems << "no BACKGROUND issued" if issued == 0
    problems << "guest exit #{exit_code}" if exit_code && exit_code != 0
    thirds = drop_thirds(drop_times(text))
    if thirds.nil? || thirds.any? { |count| count <= 0 }
      problems << "BACKGROUND drops do not cover the three thirds"
    end
    problems << "sink was not DEGRADED during the flood" unless text.include?("sink: state DEGRADED")
    numbers = "BACKGROUND issued #{issued}; flood signals #{times.length}; " \
              "window ms #{window_ms || 'missing'}; " \
              "duration ns #{duration || 'missing'}; span ns #{span || 'missing'}; " \
              "drop thirds #{thirds ? thirds.join('/') : 'missing'}"
    numbers += "; #{problems.join('; ')}" unless problems.empty?
    add("S2", "checks", numbers, problems.empty? ? PASS : FAIL)
    add_late_row("S2")
  end

  def s3_sections(text)
    found = {}
    text.split(/^===== s3 /).drop(1).each do |chunk|
      name, body = chunk.split(/=====\n/, 2)
      next if name.nil? || body.nil?
      found[name.strip] = body
    end
    found
  end

  def saw_safe?(text)
    !text.nil? && text.include?("sink: state SAFE")
  end

  def fill_s3(text)
    sections = s3_sections(text)
    kill = sections["kill relay B"]
    stop = sections["stop relay B"]
    source = sections["kill source"]
    lock = sections["K crashes"]
    silent = sections["URGENT silent"]

    [
      ["R8 kill relay B", kill, saw_safe?(kill) ? "SAFE yes" : "SAFE no"],
      ["R8 stop relay B", stop, saw_safe?(stop) ? "SAFE yes" : "SAFE no"],
      ["R8 kill source", source, saw_safe?(source) ? "SAFE yes" : "SAFE no"],
      ["R8 K crashes", lock,
       (lock && lock.include?("LOCKED_SAFE")) ? "LOCKED_SAFE yes" : "LOCKED_SAFE no"]
    ].each do |requirement, body, result|
      books = body ? unaccounted(body) : nil
      extra = books.nil? ? "" : "; unaccounted #{books}"
      add("S3", requirement, "#{result}#{extra}", NONE)
    end

    alive = silent && silent.include?("source still alive")
    add("S3", "URGENT silent",
        "SAFE #{saw_safe?(silent) ? 'yes' : 'no'}; source alive #{alive ? 'yes' : 'no'}",
        NONE)

    before = kill && clock_ns(kill[/^scenario: clock before-kill (\S+)/, 1])
    safe_at = kill && clock_ns(kill[/^scenario: clock safe (\S+)/, 1])
    back_at = kill && clock_ns(kill[/^scenario: clock recovered (\S+)/, 1])
    if before && safe_at
      add("S3", "R6",
          "one run, not a measurement; kill to SAFE #{safe_at - before} ns",
          NONE)
    else
      add("S3", "R6", "one run, clock missing", NONE)
    end
    if before && back_at
      add("S3", "R7",
          "one run, not a measurement; kill to fresh URGENT #{back_at - before} ns",
          NONE)
    else
      add("S3", "R7", "one run, clock missing", NONE)
    end

    locked = lock && lock.include?("LOCKED_SAFE")
    add("S3", "R9", "LOCKED_SAFE #{locked ? 'yes' : 'no'}", NONE)

    problems = []
    problems << "kill relay B did not reach SAFE" unless saw_safe?(kill)
    problems << "stop relay B did not reach SAFE" unless saw_safe?(stop)
    problems << "stop restarted relay B" if stop && stop.include?("supervisor: restarted relay-b")
    problems << "kill source did not reach SAFE" unless saw_safe?(source)
    problems << "source was not restarted" unless source && source.include?("restarted source")
    problems << "K crashes did not lock" unless locked
    problems << "URGENT silent did not reach SAFE" unless saw_safe?(silent)
    problems << "source died when URGENT stopped" unless alive
    problems << "R6 clock missing" unless before && safe_at
    problems << "R7 clock missing" unless before && back_at
    exit_code = guest_exit(text)
    problems << "guest exit #{exit_code}" if exit_code && exit_code != 0
    add("S3", "checks",
        problems.empty? ? "four faults and URGENT silent observed" : problems.join("; "),
        problems.empty? ? PASS : FAIL)
    add_late_row("S3")
  end

  def fill_s4(text, latency_text, calm, calm_latency)
    calm_row = summaries(calm)["URGENT"]
    fault_row = summaries(text)["URGENT"]
    calm_lat = series(calm_latency, "URGENT")
    fault_lat = series(latency_text, "URGENT")
    if calm_lat.nil? || fault_lat.nil? || calm_row.nil? || fault_row.nil?
      add("S4", "R10", "median missing", FAIL)
    else
      median_delta = fault_lat[:median] - calm_lat[:median]
      max_delta = fault_lat[:max] - calm_lat[:max]
      # With late URGENT inside guest stalls set aside, each max is at
      # most the deadline, so the max delta is too (D-064).
      set_aside = excused("S1") + excused("S4") > 0
      ok = r1_ok?(calm_row, "S1") && r1_ok?(fault_row, "S4") &&
           median_delta.abs <= R10_MEDIAN_DELTA_NS &&
           (max_delta.abs <= URGENT_DEADLINE_NS || set_aside)
      add("S4", "R10",
          "R1 S1 #{fraction_text(calm_row)}#{excused_text('S1')} " \
          "S4 #{fraction_text(fault_row)}#{excused_text('S4')}; " \
          "R3 URGENT median delta #{median_delta} ns; " \
          "max delta #{max_delta} ns; " \
          "S4 median #{fault_lat[:median]} ns max #{fault_lat[:max]} ns; " \
          "threshold fraction equal, |median delta| <= #{R10_MEDIAN_DELTA_NS} ns, " \
          "|max delta| <= #{URGENT_DEADLINE_NS} ns" \
          "#{set_aside ? ' without the URGENT late inside guest stalls' : ''}",
          ok ? PASS : FAIL)
    end

    problems = []
    problems << "panel was not killed" unless text.include?("s4: panel killed")
    problems << "telemetry was not killed" unless text.include?("s4: telemetry killed")
    problems << "telemetry was not restarted" unless text.include?("supervisor: restarted telemetry")
    problems << "path missing" unless text.include?("live: path /dev/signal-source")
    s1_seconds = calm[/^s4: live seconds (\d+)/, 1]
    s4_seconds = text[/^s4: live seconds (\d+)/, 1]
    problems << "window differs from S1" if s1_seconds.nil? || s1_seconds != s4_seconds
    problems << "URGENT summary missing" if fault_row.nil?
    problems << "median missing" if fault_lat.nil?
    exit_code = guest_exit(text)
    problems << "guest exit #{exit_code}" if exit_code && exit_code != 0
    add("S4", "checks",
        problems.empty? ? "panel and telemetry killed during a #{s4_seconds} s window" : problems.join("; "),
        problems.empty? ? PASS : FAIL)
    add_late_row("S4")
  end

  def print_table
    puts "scenario | requirement | numbers | verdict"
    @rows.each do |row|
      puts row.join(" | ")
    end
  end
end

if __FILE__ == $0
  exit ScenarioReport.run(ARGV)
end
