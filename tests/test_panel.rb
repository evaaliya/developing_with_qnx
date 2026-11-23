#!/usr/bin/ruby
# encoding: utf-8

# Parser and staleness rule for the host panel. No network.

require_relative "../scripts/panel_view"

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

def snapshot(state, mode, issued, median)
  <<SNAP
seq 4
time_ns 1000
state #{state}
source_mode #{mode}
node source alive 1 restarts 0
node relay-a alive 1 restarts 0
node relay-b alive 0 restarts 1
node sink alive 1 restarts 0
class URGENT issued #{issued} on_time 3 late 1 lost 0 dropped 0 median_ns #{median} max_ns 2000
class COMMAND issued 1 on_time 1 late 0 lost 0 dropped 0 median_ns none max_ns none
class BACKGROUND issued 2 on_time 2 late 0 lost 0 dropped 0 median_ns 5 max_ns 9
end
SNAP
end

text = snapshot("NORMAL", "normal", 424242, 1000)
parsed = PanelView.parse(text)
failures = check("parse keeps the urgent issued count",
                 parsed && parsed["classes"]["URGENT"]["issued"] == 424242,
                 failures)
failures = check("parse keeps a missing median as none",
                 parsed && parsed["classes"]["COMMAND"]["median_ns"].nil?,
                 failures)
failures = check("parse keeps relay B down with one restart",
                 parsed && parsed["nodes"]["relay-b"]["alive"] == "0" &&
                   parsed["nodes"]["relay-b"]["restarts"] == 1,
                 failures)
failures = check("parse rejects a snapshot with no end",
                 PanelView.parse(text.sub("end\n", "")).nil?,
                 failures)
failures = check("parse rejects an unknown state",
                 PanelView.parse(text.sub("state NORMAL", "state ASLEEP")).nil?,
                 failures)
failures = check("parse rejects one-sided latency",
                 PanelView.parse(text.sub("median_ns none max_ns none",
                                          "median_ns none max_ns 4")).nil?,
                 failures)

model = PanelView::Model.new
now = 1_000.0
failures = check("no snapshot says the guest is not reachable",
                 begin
                   shown = PanelView.render_text(model.sample(now)[:view])
                   shown.include?("guest or telemetry is not reachable") &&
                     !shown.include?("424242") && !shown.include?("issued")
                 end,
                 failures)

failures = check("a snapshot is accepted", model.push(text, now), failures)

fresh = model.sample(now + PanelView::STALE_AFTER_S - 0.001)[:view]
fresh_text = PanelView.render_text(fresh)
failures = check("just under 2 seconds is still current",
                 fresh[:kind] == "current" && fresh_text.include?("issued 424242"),
                 failures)
fresh_html = PanelView.render_fragment(fresh)
map_line = fresh_text.lines.find { |line| line.start_with?("map:") }
map_html = fresh_html[/class="map">.*?<\/div>/m]
failures = check("a current view names the chain and the mode",
                 fresh_text.include?("source (alive, restarts 0)") &&
                   fresh_text.include?("relay A (alive, restarts 0)") &&
                   fresh_text.include?("relay B (dead, restarts 1)") &&
                   fresh_text.include?("sink (alive, restarts 0)") &&
                   fresh_text.include?("telemetry (serving)") &&
                   fresh_text.include?("source mode: normal") &&
                   fresh_text.include?("median 1000 ns") &&
                   fresh_text.include?("median none"),
                 failures)
failures = check("telemetry is drawn off the signal path",
                 map_line && !map_line.include?("telemetry") &&
                   map_line.include?("→") &&
                   fresh_text.include?("not on the signal path") &&
                   map_html && !map_html.include?("telemetry") &&
                   fresh_html.include?("not on the signal path"),
                 failures)

stale = model.sample(now + PanelView::STALE_AFTER_S)[:view]
stale_text = PanelView.render_text(stale)
stale_html = PanelView.render_fragment(stale)
failures = check("at 2 seconds the panel says the data is stale and gives the age",
                 stale[:kind] == "stale" &&
                   stale_text.include?("data is stale") &&
                   stale_text.include?("age 2 seconds") &&
                   stale_html.include?("data is stale") &&
                   stale_html.include?("age 2 seconds"),
                 failures)
failures = check("stale text does not repeat the old issued count",
                 !stale_text.include?("424242") && !stale_text.include?("issued") &&
                   !stale_html.include?("424242"),
                 failures)

model.note_down("connection refused")
down = model.sample(now + 0.2)[:view]
down_text = PanelView.render_text(down)
failures = check("a short gap still shows the current numbers",
                 down[:kind] == "current" && down_text.include?("issued 424242"),
                 failures)
down_late = model.sample(now + 3.4)[:view]
down_late_text = PanelView.render_text(down_late)
failures = check("a dead connection past the threshold names both faults and hides numbers",
                 down_late[:kind] == "stale" &&
                   down_late_text.include?("data is stale") &&
                   down_late_text.include?("age 3 seconds") &&
                   down_late_text.include?("guest or telemetry is not reachable") &&
                   !down_late_text.include?("424242"),
                 failures)

%w[flood mute].each do |mode|
  other = snapshot("DEGRADED", mode, 9, 1000)
  item = PanelView::Model.new
  item.push(other, now)
  shown = PanelView.render_text(item.sample(now)[:view])
  failures = check("source mode #{mode} is on the panel",
                   shown.include?("source mode: #{mode}"),
                   failures)
end

colors = {}
PanelView::STATES.each do |state|
  item = PanelView::Model.new
  item.push(snapshot(state, "normal", 1, 1), now)
  html = PanelView.render_fragment(item.sample(now)[:view])
  color = PanelView::STATE_COLOR[state]
  colors[state] = color
  failures = check("state #{state} has its own colour on the page",
                   html.include?("class=\"banner state-#{state}\" style=\"background:#{color}\""),
                   failures)
end
failures = check("the six state colours differ",
                 colors.values.uniq.length == 6,
                 failures)

if failures == 0
  puts "all tests passed"
  exit 0
end
exit 1
