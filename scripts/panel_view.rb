# encoding: utf-8
# frozen_string_literal: false

# Host-panel view of one telemetry snapshot (D-056).
# No clock and no socket: the caller passes the time and the text.

module PanelView
  STALE_AFTER_S = 2.0
  NODES = %w[source relay-a relay-b sink].freeze
  NODE_LABEL = {
    "source" => "source",
    "relay-a" => "relay A",
    "relay-b" => "relay B",
    "sink" => "sink"
  }.freeze
  CLASSES = %w[URGENT COMMAND BACKGROUND].freeze
  STATES = %w[INIT NORMAL DEGRADED SAFE RECOVERING LOCKED_SAFE].freeze
  MODES = %w[unknown normal flood mute].freeze
  # Each network state has its own colour. The page uses these and no others.
  STATE_COLOR = {
    "INIT" => "#4b5563",
    "NORMAL" => "#166534",
    "DEGRADED" => "#b45309",
    "SAFE" => "#b91c1c",
    "RECOVERING" => "#1d4ed8",
    "LOCKED_SAFE" => "#6d28d9"
  }.freeze

  module_function

  def integer(text)
    return nil unless text.is_a?(String) && text =~ /\A-?\d+\z/
    text.to_i
  end

  # A full TCP snapshot, or nil when the text is not one.
  def parse(text)
    return nil unless text.is_a?(String)
    snap = {
      "seq" => nil,
      "time_ns" => nil,
      "state" => nil,
      "source_mode" => nil,
      "nodes" => {},
      "classes" => {}
    }
    ended = false
    text.each_line do |raw|
      line = raw.sub(/\r?\n\z/, "")
      next if line.empty?
      parts = line.split(" ")
      return nil if ended
      case parts[0]
      when "seq"
        return nil unless parts.length == 2 && snap["seq"].nil?
        snap["seq"] = integer(parts[1])
        return nil if snap["seq"].nil? || snap["seq"] < 0
      when "time_ns"
        return nil unless parts.length == 2 && snap["time_ns"].nil?
        snap["time_ns"] = integer(parts[1])
        return nil if snap["time_ns"].nil?
      when "state"
        return nil unless parts.length == 2 && snap["state"].nil?
        return nil unless STATES.include?(parts[1])
        snap["state"] = parts[1]
      when "source_mode"
        return nil unless parts.length == 2 && snap["source_mode"].nil?
        return nil unless MODES.include?(parts[1])
        snap["source_mode"] = parts[1]
      when "node"
        return nil unless parts.length == 6 && parts[2] == "alive" && parts[4] == "restarts"
        return nil unless NODES.include?(parts[1]) && !snap["nodes"].key?(parts[1])
        alive = parts[3]
        return nil unless %w[0 1 unknown].include?(alive)
        restarts = integer(parts[5])
        return nil if restarts.nil? || restarts < 0
        snap["nodes"][parts[1]] = { "alive" => alive, "restarts" => restarts }
      when "class"
        return nil unless parts.length == 16
        return nil unless CLASSES.include?(parts[1]) && !snap["classes"].key?(parts[1])
        return nil unless parts[2] == "issued" && parts[4] == "on_time"
        return nil unless parts[6] == "late" && parts[8] == "lost"
        return nil unless parts[10] == "dropped"
        return nil unless parts[12] == "median_ns" && parts[14] == "max_ns"
        issued = integer(parts[3])
        on_time = integer(parts[5])
        late = integer(parts[7])
        lost = integer(parts[9])
        dropped = integer(parts[11])
        return nil if [issued, on_time, late, lost, dropped].any? { |n| n.nil? || n < 0 }
        median, maximum = latency_pair(parts[13], parts[15])
        return nil if median == :bad
        snap["classes"][parts[1]] = {
          "issued" => issued,
          "on_time" => on_time,
          "late" => late,
          "lost" => lost,
          "dropped" => dropped,
          "median_ns" => median,
          "max_ns" => maximum
        }
      when "end"
        return nil unless parts.length == 1
        return nil if ended
        ended = true
      else
        return nil
      end
    end
    return nil unless ended
    return nil if snap["seq"].nil? || snap["time_ns"].nil?
    return nil if snap["state"].nil? || snap["source_mode"].nil?
    return nil unless NODES.all? { |name| snap["nodes"].key?(name) }
    return nil unless CLASSES.all? { |name| snap["classes"].key?(name) }
    snap
  end

  def latency_pair(median_text, max_text)
    if median_text == "none" && max_text == "none"
      return [nil, nil]
    end
    if median_text == "none" || max_text == "none"
      return [:bad, :bad]
    end
    median = integer(median_text)
    maximum = integer(max_text)
    return [:bad, :bad] if median.nil? || maximum.nil?
    [median, maximum]
  end

  class Model
    def initialize(stale_after = PanelView::STALE_AFTER_S)
      @stale_after = stale_after.to_f
      @mu = Mutex.new
      @link_down = true
      @link_error = "localhost:8000 did not accept a connection"
      @last = nil
      @last_at = nil
      @last_raw = nil
      @safe_html = nil
    end

    def note_down(message)
      @mu.synchronize do
        @link_down = true
        @link_error = message.to_s
      end
    end

    def note_up
      @mu.synchronize { @link_down = false }
    end

    # True when the text was a snapshot. The time is seconds from any clock.
    def push(raw, now)
      parsed = PanelView.parse(raw)
      return false if parsed.nil?
      @mu.synchronize do
        @last = parsed
        @last_raw = raw
        @last_at = now.to_f
        @link_down = false
        view = view_unlocked(now.to_f)
        if view[:kind] == "current" && view[:state] == "SAFE"
          @safe_html = PanelView.render_fragment(view)
        end
      end
      true
    end

    def sample(now)
      @mu.synchronize do
        view = view_unlocked(now.to_f)
        { :view => view, :raw => @last_raw, :safe_html => @safe_html }
      end
    end

    def view_unlocked(now)
      if @last.nil?
        return {
          :kind => "unreachable",
          :message => "guest or telemetry is not reachable",
          :detail => @link_error,
          :age_seconds => nil,
          :link_down => true
        }
      end
      age = now.to_f - @last_at
      age = 0.0 if age < 0.0
      shown = age.floor
      if age >= @stale_after
        message = "data is stale"
        detail = @link_down ? "guest or telemetry is not reachable" : nil
        return {
          :kind => "stale",
          :message => message,
          :detail => detail,
          :age_seconds => shown,
          :link_down => @link_down
        }
      end
      nodes = {}
      @last["nodes"].each do |name, node|
        nodes[name] = {
          "alive" => node["alive"],
          "restarts" => node["restarts"]
        }
      end
      {
        :kind => "current",
        :message => nil,
        :detail => nil,
        :age_seconds => shown,
        :link_down => @link_down,
        :state => @last["state"],
        :mode => @last["source_mode"],
        :seq => @last["seq"],
        :nodes => nodes,
        :classes => @last["classes"],
        :telemetry => "serving"
      }
    end
  end

  def watch_line(view)
    case view[:kind]
    when "current"
      restarts = view[:nodes]["relay-b"]["restarts"]
      "panel-watch current state #{view[:state]} mode #{view[:mode]} " \
        "relay-b-restarts #{restarts} age #{view[:age_seconds]}"
    when "stale"
      line = "panel-watch stale age #{view[:age_seconds]}"
      line += " unreachable" if view[:detail]
      line
    else
      "panel-watch unreachable"
    end
  end

  def alive_word(value)
    return "alive" if value == "1"
    return "dead" if value == "0"
    "unknown"
  end

  def latency_word(value)
    value.nil? ? "none" : "#{value} ns"
  end

  def render_text(view)
    lines = []
    case view[:kind]
    when "unreachable"
      lines << "guest or telemetry is not reachable"
      lines << view[:detail].to_s unless view[:detail].nil? || view[:detail].empty?
    when "stale"
      lines << "data is stale"
      lines << "age #{view[:age_seconds]} seconds"
      lines << "These numbers are not current."
      if view[:detail]
        lines << "guest or telemetry is not reachable"
      end
    else
      chain = NODES.map do |name|
        node = view[:nodes][name]
        "#{NODE_LABEL[name]} (#{alive_word(node['alive'])}, restarts #{node['restarts']})"
      end
      lines << "map: #{chain.join(' → ')}"
      lines << "telemetry (serving), not on the signal path"
      lines << "state: #{view[:state]}"
      lines << "source mode: #{view[:mode]}"
      lines << "snapshot age: #{view[:age_seconds]} seconds"
      CLASSES.each do |name|
        row = view[:classes][name]
        lines << "class #{name} issued #{row['issued']} on time #{row['on_time']} " \
          "late #{row['late']} lost #{row['lost']} dropped #{row['dropped']} " \
          "median #{latency_word(row['median_ns'])} max #{latency_word(row['max_ns'])}"
      end
    end
    lines.join("\n") + "\n"
  end

  def h(text)
    text.to_s.gsub("&", "&amp;").gsub("<", "&lt;").gsub(">", "&gt;").gsub('"', "&quot;")
  end

  def legend(active)
    boxes = STATES.map do |name|
      mark = active == name ? " active" : ""
      "<span class=\"swatch state-#{name}#{mark}\" style=\"background:#{STATE_COLOR[name]}\">#{h(name)}</span>"
    end
    "<p class=\"legend\">#{boxes.join}</p>"
  end

  def render_fragment(view)
    parts = []
    parts << legend(view[:kind] == "current" ? view[:state] : nil)
    case view[:kind]
    when "unreachable"
      parts << "<p class=\"banner bad\">guest or telemetry is not reachable</p>"
      unless view[:detail].nil? || view[:detail].empty?
        parts << "<p class=\"detail\">#{h(view[:detail])}</p>"
      end
    when "stale"
      parts << "<p class=\"banner bad\">data is stale</p>"
      parts << "<p class=\"age\">age #{view[:age_seconds]} seconds</p>"
      parts << "<p class=\"detail\">These numbers are not current.</p>"
      if view[:detail]
        parts << "<p class=\"banner bad\">guest or telemetry is not reachable</p>"
      end
    else
      color = STATE_COLOR[view[:state]]
      parts << "<p class=\"banner state-#{view[:state]}\" style=\"background:#{color}\">state #{h(view[:state])}</p>"
      parts << "<p class=\"mode\">source mode: #{h(view[:mode])}</p>"
      parts << "<p class=\"age\">snapshot age: #{view[:age_seconds]} seconds</p>"
      cards = NODES.map do |name|
        node = view[:nodes][name]
        "<div class=\"node alive-#{h(node['alive'])}\"><strong>#{h(NODE_LABEL[name])}</strong>" \
          "<span>#{h(alive_word(node['alive']))}</span>" \
          "<span>restarts #{node['restarts']}</span></div>"
      end
      parts << "<div class=\"map\">#{cards.join('<span class="arrow">→</span>')}</div>"
      parts << "<p class=\"telemetry\">telemetry (serving), not on the signal path</p>"
      rows = CLASSES.map do |name|
        row = view[:classes][name]
        "<tr><th>#{h(name)}</th><td>#{row['issued']}</td><td>#{row['on_time']}</td>" \
          "<td>#{row['late']}</td><td>#{row['lost']}</td><td>#{row['dropped']}</td>" \
          "<td>#{h(latency_word(row['median_ns']))}</td><td>#{h(latency_word(row['max_ns']))}</td></tr>"
      end
      parts << "<table><thead><tr><th>class</th><th>issued</th><th>on time</th>" \
        "<th>late</th><th>lost</th><th>dropped</th><th>median</th><th>max</th></tr></thead>" \
        "<tbody>#{rows.join}</tbody></table>"
    end
    parts.join("\n")
  end

  def page(fragment, title)
    <<HTML
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>#{h(title)}</title>
<style>
  body { font: 16px/1.4 sans-serif; margin: 24px; background: #f8fafc; color: #0f172a; }
  h1 { font-size: 22px; margin: 0 0 12px; }
  .banner { display: inline-block; color: #fff; font-size: 28px; font-weight: 700;
            padding: 12px 18px; border-radius: 8px; margin: 8px 0; }
  .bad { background: #7f1d1d; }
  .age, .mode, .detail { margin: 6px 0; }
  .controls { margin: 16px 0; }
  .controls button { font: inherit; padding: 6px 12px; margin: 0 6px 6px 0; border-radius: 6px;
                     border: 1px solid #475569; background: #fff; cursor: pointer; }
  .command-failed { color: #b91c1c; font-weight: 700; }
  .legend { margin: 8px 0 16px; }
  .swatch { display: inline-block; color: #fff; margin: 0 6px 6px 0; padding: 4px 8px;
            border-radius: 4px; font-size: 12px; }
  .swatch.active { outline: 3px solid #0f172a; }
  .map { display: flex; align-items: center; flex-wrap: wrap; gap: 8px; margin: 12px 0; }
  .node { background: #fff; border: 2px solid #94a3b8; border-radius: 8px; padding: 8px 10px;
          min-width: 110px; }
  .node.alive-1 { border-color: #166534; }
  .node.alive-0 { border-color: #b91c1c; }
  .node span, .node strong { display: block; }
  .arrow { font-size: 22px; }
  .telemetry { margin: 4px 0 12px; color: #475569; }
  table { border-collapse: collapse; background: #fff; }
  th, td { border: 1px solid #cbd5e1; padding: 6px 10px; text-align: right; }
  th { text-align: center; }
</style>
</head>
<body>
<h1>#{h(title)}</h1>
<div id="panel">
#{fragment}
</div>
</body>
</html>
HTML
  end
end
