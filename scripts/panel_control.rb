# encoding: utf-8

# Commands the host panel may send to /dev/signal-source on the guest.
# Only the four names below are accepted; the text written on the guest
# is fixed here, never taken from the request. stop is not offered.

require "open3"
require "timeout"

module PanelControl
  COMMANDS = {
    "flood-on" => "flood on",
    "flood-off" => "flood off",
    "mute" => "mute",
    "restore" => "restore"
  }.freeze

  LABELS = {
    "flood-on" => "Flood on",
    "flood-off" => "Flood off",
    "mute" => "Mute URGENT",
    "restore" => "Restore URGENT"
  }.freeze

  SSH_TIMEOUT_S = 15
  DEVICE = "/dev/signal-source"
  HEADER = "X-Panel-Command"

  module_function

  def command_for(name)
    COMMANDS[name.to_s]
  end

  # The same ssh as the Makefile: port 2222, root, the user's key, batch mode.
  def ssh_argv(command)
    key = File.join(Dir.home, ".ssh", "id_ed25519")
    [
      "ssh", "-p", "2222", "-i", key,
      "-o", "BatchMode=yes", "-o", "ConnectTimeout=5",
      "root@localhost",
      "echo '#{command}' > #{DEVICE}"
    ]
  end

  # A command may only come from the panel page itself. A page on another
  # site cannot add the custom header without a CORS preflight, which this
  # server never answers; the Host check stops DNS rebinding.
  def request_allowed?(method, host, origin, header, page_host)
    return false unless method == "POST"
    return false unless header == "1"
    return false unless host == page_host
    return true if origin.nil? || origin.empty?
    origin == "http://#{page_host}"
  end

  # Runs one command. Returns a hash the page shows as it is.
  def send_command(name, runner = method(:run_ssh))
    command = command_for(name)
    if command.nil?
      return { ok: false, command: nil, exit: nil, output: "",
               error: "unknown command #{name.to_s[0, 40]}" }
    end
    status, output, error = runner.call(ssh_argv(command))
    ok = status == 0
    if !ok && error.strip.empty?
      error = status.nil? ? "ssh did not finish in #{SSH_TIMEOUT_S} s" : "ssh exit #{status}"
    end
    { ok: ok, command: command, exit: status, output: output, error: error }
  end

  def run_ssh(argv)
    out = +""
    err = +""
    status = nil
    Open3.popen3(*argv) do |stdin, stdout, stderr, wait|
      stdin.close
      begin
        Timeout.timeout(SSH_TIMEOUT_S) do
          out = stdout.read
          err = stderr.read
          status = wait.value.exitstatus
        end
      rescue Timeout::Error
        begin
          Process.kill("KILL", wait.pid)
        rescue SystemCallError
          nil
        end
        status = nil
      end
    end
    [status, out.to_s, err.to_s]
  rescue SystemCallError => error
    [127, "", "ssh could not start: #{error.message}"]
  end

  def result_text(result)
    if result[:ok]
      "sent \"#{result[:command]}\"; the mode above comes from telemetry"
    elsif result[:command].nil?
      "refused: #{result[:error].strip}"
    else
      "\"#{result[:command]}\" failed: #{result[:error].strip}"
    end
  end
end
