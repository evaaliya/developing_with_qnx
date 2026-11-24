#!/usr/bin/ruby
# encoding: utf-8

# Parser and staleness rule for the host panel. No network.

require_relative "../scripts/panel_control"
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

# Panel commands: only four fixed texts reach the guest.
expected = { "flood-on" => "flood on", "flood-off" => "flood off",
             "mute" => "mute", "restore" => "restore" }
expected.each do |name, text|
  failures = check("panel command #{name} writes #{text}",
                   PanelControl.command_for(name) == text, failures)
end
["stop", "flood on", "", "flood-on; reboot", nil].each do |name|
  failures = check("panel command #{name.inspect} is refused",
                   PanelControl.command_for(name).nil?, failures)
end
argv = PanelControl.ssh_argv("flood on")
failures = check("panel ssh uses port 2222 in batch mode",
                 argv[0] == "ssh" && argv[1, 2] == ["-p", "2222"] &&
                 argv.include?("BatchMode=yes") && argv.include?("root@localhost"),
                 failures)
failures = check("panel ssh writes the device",
                 argv.last.start_with?("echo 'flood on' > /dev/signal-source && "), failures)
failures = check("panel ssh reads mode and error back",
                 argv.last.end_with?("grep -E '^(mode|error) ' /dev/signal-source"), failures)
stopped = lambda { |_a| [0, "mode normal\nerror flood stopped: 10240 signals wait to be closed\n", ""] }
result = PanelControl.send_command("flood-on", stopped)
failures = check("a flood that stopped itself shows its reason",
                 PanelControl.result_text(result).include?("error flood stopped: 10240 signals wait to be closed"),
                 failures)

seen = []
ok_runner = lambda { |a| seen << a; [0, "", ""] }
result = PanelControl.send_command("mute", ok_runner)
failures = check("a sent command reports success", result[:ok] && result[:command] == "mute", failures)
failures = check("a sent command runs ssh once", seen.length == 1, failures)
seen.clear
result = PanelControl.send_command("stop", ok_runner)
failures = check("stop never runs ssh", !result[:ok] && seen.empty?, failures)
down = lambda { |_a| [255, "", "ssh: connect to host localhost port 2222: Connection refused\n"] }
result = PanelControl.send_command("flood-on", down)
failures = check("an unreachable guest is a failure", !result[:ok], failures)
failures = check("the ssh error is shown",
                 PanelControl.result_text(result).include?("Connection refused"), failures)
hung = lambda { |_a| [nil, "", ""] }
result = PanelControl.send_command("flood-off", hung)
failures = check("a hung ssh is a failure with a reason",
                 !result[:ok] && result[:error].include?("did not finish"), failures)
rejected = lambda { |_a| [1, "", "/dev/signal-source: Invalid argument\n"] }
result = PanelControl.send_command("flood-on", rejected)
failures = check("a refused device write is a failure",
                 !result[:ok] && PanelControl.result_text(result).include?("Invalid argument"),
                 failures)

host = "127.0.0.1:8765"
failures = check("the page may send a command",
                 PanelControl.request_allowed?("POST", host, "http://#{host}", "1", host), failures)
failures = check("a command without Origin is allowed from the page",
                 PanelControl.request_allowed?("POST", host, nil, "1", host), failures)
failures = check("GET cannot send a command",
                 !PanelControl.request_allowed?("GET", host, nil, "1", host), failures)
failures = check("a command without the header is refused",
                 !PanelControl.request_allowed?("POST", host, nil, nil, host), failures)
failures = check("another origin is refused",
                 !PanelControl.request_allowed?("POST", host, "http://example.com", "1", host), failures)
failures = check("another Host is refused",
                 !PanelControl.request_allowed?("POST", "evil.test:8765", nil, "1", host), failures)

if failures == 0
  puts "all tests passed"
  exit 0
end
exit 1
