#!/usr/bin/ruby
# encoding: utf-8

# Host panel. One command, and only the Ruby that ships with macOS (D-056):
#   /usr/bin/ruby scripts/host-panel.rb
# It reads telemetry text from localhost:8000 and serves the page on
# 127.0.0.1:8765. Pass --no-browser to leave the browser closed.

require "json"
require "socket"
require "webrick"
require_relative "panel_view"

HOST = "127.0.0.1"
TELEMETRY_PORT = 8000
PAGE_PORT = 8765
TICK_S = 0.05

browser = true
log_path = nil
snap_path = nil
ARGV.each do |arg|
  case arg
  when "--no-browser"
    browser = false
  when /\A--log=(.+)\z/
    log_path = $1
  when /\A--snapshot=(.+)\z/
    snap_path = $1
  else
    warn "usage: /usr/bin/ruby scripts/host-panel.rb [--no-browser] [--log=PATH] [--snapshot=PATH]"
    exit 2
  end
end

model = PanelView::Model.new
stop = false
last_line = nil
last_raw = nil

def remember(log_path, snap_path, sample, last_line, last_raw)
  view = sample[:view]
  line = PanelView.watch_line(view)
  if line != last_line
    stamp = Time.now.strftime("%H:%M:%S")
    puts "#{stamp} #{line}"
    $stdout.flush
    if log_path
      File.open(log_path, "a") { |file| file.puts "#{stamp} #{line}" }
    end
    last_line = line
  end
  raw = sample[:raw]
  if snap_path && raw && raw != last_raw
    File.open(snap_path, "w") { |file| file.write(raw) }
    last_raw = raw
  end
  [last_line, last_raw]
end

reader = Thread.new do
  until stop
    socket = nil
    begin
      socket = TCPSocket.new(HOST, TELEMETRY_PORT)
      model.note_up
      buffer = String.new(encoding: Encoding::ASCII_8BIT)
      raw_mode = nil
      opened_at = Time.now
      saw_snapshot = false
      until stop
        # QEMU accepts the forwarded port before telemetry listens and
        # then sends nothing. Leave that socket instead of sitting on it.
        if !saw_snapshot && Time.now - opened_at > 0.5
          model.note_down("guest or telemetry is not reachable")
          break
        end
        ready = IO.select([socket], nil, nil, 0.2)
        next if ready.nil?
        begin
          chunk = socket.recv(4096)
        rescue IOError, SystemCallError
          model.note_down("guest or telemetry is not reachable")
          break
        end
        if chunk.nil? || chunk.empty?
          model.note_down("guest or telemetry closed the connection")
          break
        end
        buffer << chunk.force_encoding(Encoding::ASCII_8BIT)
        if buffer.bytesize > 65_536 && buffer.index("\nend\n").nil?
          buffer = +(buffer.byteslice(-4096, 4096) || "")
        end
        while (mark = buffer.index("\nend\n"))
          piece = buffer.byteslice(0, mark + 5)
          rest = buffer.byteslice(mark + 5, buffer.bytesize)
          buffer = rest.nil? ? +"" : +rest
          mode = piece[/source_mode (\S+)/, 1]
          if mode && mode != raw_mode
            raw_mode = mode
            puts "panel: snapshot source_mode #{mode}"
            $stdout.flush
          end
          saw_snapshot = true if model.push(piece, Time.now.to_f)
        end
      end
    rescue StandardError => error
      warn "panel: reader #{error.class}: #{error.message}"
      model.note_down("guest or telemetry is not reachable")
      sleep 0.2
    ensure
      socket.close if socket && !socket.closed?
    end
  end
end

ticker = Thread.new do
  until stop
    sample = model.sample(Time.now.to_f)
    last_line, last_raw = remember(log_path, snap_path, sample, last_line, last_raw)
    sleep TICK_S
  end
end

begin
  server = WEBrick::HTTPServer.new(
    :BindAddress => HOST,
    :Port => PAGE_PORT,
    :AccessLog => [],
    :Logger => WEBrick::Log.new(File.open(File::NULL, "w"), WEBrick::Log::ERROR)
  )
rescue Errno::EADDRINUSE
  warn "panel: #{HOST}:#{PAGE_PORT} is already in use"
  exit 1
end

server.mount_proc "/" do |_req, res|
  sample = model.sample(Time.now.to_f)
  body = PanelView.page(PanelView.render_fragment(sample[:view]), "Signal network")
  body.sub!("</body>", <<'JS')
<script>
async function tick() {
  try {
    const res = await fetch("/fragment", {cache: "no-store"});
    if (!res.ok) throw new Error("page request failed");
    document.getElementById("panel").innerHTML = await res.text();
  } catch (error) {
    document.getElementById("panel").innerHTML =
      "<p class=\"banner bad\">guest or telemetry is not reachable</p>";
  }
}
setInterval(tick, 300);
</script>
</body>
JS
  res["Content-Type"] = "text/html; charset=utf-8"
  res["Cache-Control"] = "no-store"
  res.body = body
end

server.mount_proc "/fragment" do |_req, res|
  sample = model.sample(Time.now.to_f)
  res["Content-Type"] = "text/html; charset=utf-8"
  res["Cache-Control"] = "no-store"
  res.body = PanelView.render_fragment(sample[:view])
end

server.mount_proc "/safe" do |_req, res|
  sample = model.sample(Time.now.to_f)
  fragment = sample[:safe_html] ||
    "<p class=\"banner bad\">SAFE has not been shown yet</p>"
  res["Content-Type"] = "text/html; charset=utf-8"
  res["Cache-Control"] = "no-store"
  res.body = PanelView.page(fragment, "Signal network, captured in SAFE")
end

server.mount_proc "/text" do |_req, res|
  sample = model.sample(Time.now.to_f)
  res["Content-Type"] = "text/plain; charset=utf-8"
  res["Cache-Control"] = "no-store"
  res.body = PanelView.render_text(sample[:view])
end

trap("INT") { stop = true; server.shutdown }
trap("TERM") { stop = true; server.shutdown }

puts "panel: open http://#{HOST}:#{PAGE_PORT}/"
puts "panel: reading #{HOST}:#{TELEMETRY_PORT}"
$stdout.flush
if browser
  started = Process.spawn("/usr/bin/open", "http://#{HOST}:#{PAGE_PORT}/")
  Process.detach(started)
end

server.start
stop = true
reader.join(1)
ticker.join(1)
