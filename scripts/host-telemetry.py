#!/usr/bin/env python3
"""Read telemetry snapshots from localhost:8000 while the guest network runs.

The guest keeps issuing signals. This script checks that a snapshot matches
the sink summary, that killing or stopping telemetry does not stop the
network, and that a new TCP client still receives snapshots.
"""

import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SSH = [
    "ssh",
    "-p",
    "2222",
    "-i",
    os.path.expanduser("~/.ssh/id_ed25519"),
    "-o",
    "BatchMode=yes",
    "root@localhost",
]
CLASSES = ("URGENT", "COMMAND", "BACKGROUND")
NODES = ("source", "relay-a", "relay-b", "sink")


def fail(message):
    print("telemetry-check FAIL " + message)
    sys.exit(1)


def ssh(command, timeout=20):
    try:
        result = subprocess.run(
            SSH + [command],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        fail("ssh timed out: " + command)
    return result.returncode, result.stdout, result.stderr


def parse_snapshot(text):
    snap = {
        "seq": None,
        "time_ns": None,
        "state": None,
        "source_mode": None,
        "nodes": {},
        "classes": {},
        "raw": text,
    }
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "seq" and len(parts) == 2:
            snap["seq"] = int(parts[1])
        elif parts[0] == "time_ns" and len(parts) == 2:
            snap["time_ns"] = int(parts[1])
        elif parts[0] == "state" and len(parts) == 2:
            snap["state"] = parts[1]
        elif parts[0] == "source_mode" and len(parts) == 2:
            snap["source_mode"] = parts[1]
        elif parts[0] == "node" and len(parts) == 6 and parts[2] == "alive":
            snap["nodes"][parts[1]] = {
                "alive": parts[3],
                "restarts": int(parts[5]),
            }
        elif parts[0] == "class" and len(parts) == 16:
            snap["classes"][parts[1]] = {
                "issued": int(parts[3]),
                "on_time": int(parts[5]),
                "late": int(parts[7]),
                "lost": int(parts[9]),
                "dropped": int(parts[11]),
                "median_ns": parts[13],
                "max_ns": parts[15],
            }
        elif parts[0] != "end":
            return None
    if snap["seq"] is None or snap["state"] is None or snap["source_mode"] is None:
        return None
    if len(snap["nodes"]) != 4 or len(snap["classes"]) != 3:
        return None
    return snap


class Stream:
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", 8000), 5)
        self.sock.settimeout(0.4)
        self.buf = ""
        self.last = None

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def pump(self):
        """Read one chunk. True means the socket is still open."""
        try:
            data = self.sock.recv(4096)
        except socket.timeout:
            return True
        except OSError:
            return False
        if not data:
            # QEMU accepts hostfwd before the guest listens, then
            # this recv is empty. The caller has to connect again.
            return False
        self.buf += data.decode("utf-8", "replace")
        while "\nend\n" in self.buf:
            head, self.buf = self.buf.split("\nend\n", 1)
            parsed = parse_snapshot(head + "\nend\n")
            if parsed is not None:
                self.last = parsed
        return True

    def wait(self, pred, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.last is not None and pred(self.last):
                return self.last
            if self.pump():
                time.sleep(0.05)
                continue
            self.close()
            self.last = None
            self.buf = ""
            try:
                self.sock = socket.create_connection(("127.0.0.1", 8000), 2)
                self.sock.settimeout(0.4)
            except OSError:
                pass
            time.sleep(0.2)
        if self.last is not None and pred(self.last):
            return self.last
        return None


def connect(timeout):
    deadline = time.time() + timeout
    last_error = "port closed"
    while time.time() < deadline:
        try:
            return Stream()
        except OSError as error:
            last_error = str(error)
            time.sleep(0.2)
    fail("localhost:8000 did not accept a client (" + last_error + ")")


def telemetry_pids():
    code, out, err = ssh(
        "grep -E 'supervisor: (node|restarted) telemetry pid=' /tmp/supervisor.out || true"
    )
    return re.findall(r"telemetry pid=(\d+)", out)


def count_lines(pattern):
    code, out, err = ssh(
        "grep -c " + shell_quote(pattern) + " /tmp/sink.out || true"
    )
    text = out.strip()
    if text == "":
        return 0
    return int(text.splitlines()[-1])


def shell_quote(text):
    return "'" + text.replace("'", "'\\''") + "'"


def ready(snap):
    if snap["state"] != "NORMAL" or snap["source_mode"] != "normal":
        return False
    if snap["classes"]["URGENT"]["on_time"] < 1:
        return False
    for name in NODES:
        if snap["nodes"].get(name, {}).get("alive") != "1":
            return False
    return True


def guest_alive(proc):
    if proc.poll() is not None:
        fail("guest scenario exited early with status %d" % proc.returncode)


def main():
    os.chdir(ROOT)
    log_path = os.path.join(ROOT, "build", "telemetry-guest.log")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    log = open(log_path, "w")
    finished = False
    proc = subprocess.Popen(
        SSH
        + [
            "chmod +x /tmp/qnx-telemetry.sh /tmp/supervisor /tmp/source "
            "/tmp/sink /tmp/relay /tmp/telemetry; /tmp/qnx-telemetry.sh"
        ],
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    try:
        stream = connect(15)
        guest_alive(proc)
        first = stream.wait(ready, 12)
        if first is None:
            fail("no snapshot showed NORMAL, mode normal, and live nodes")
        print(
            "telemetry-check first seq %d state %s mode %s urgent on_time %d"
            % (
                first["seq"],
                first["state"],
                first["source_mode"],
                first["classes"]["URGENT"]["on_time"],
            )
        )
        code, status, err = ssh("cat /dev/signal-source")
        if code != 0 or "mode normal" not in status:
            fail("source status was not mode normal:\n" + status + err)
        print("telemetry-check source status mode normal")

        pids = telemetry_pids()
        if not pids:
            fail("supervisor did not print a telemetry pid")
        old = pids[-1]
        stream.close()
        # One remote wait: a new ssh on every poll would spend the
        # live source's 64-signal budget before SIGSTOP.
        code, out, err = ssh(
            "kill -KILL "
            + old
            + "; i=0; while [ \"$i\" -lt 40 ]; do "
            + "line=$(grep -E 'supervisor: restarted telemetry pid=' "
            + "/tmp/supervisor.out | tail -1); "
            + "pid=${line##*pid=}; "
            + "if [ -n \"$pid\" ] && [ \"$pid\" != "
            + old
            + " ]; then echo \"$pid\"; exit 0; fi; "
            + "i=$((i + 1)); sleep 0.2; done; exit 1",
            timeout=20,
        )
        new = out.strip().splitlines()[-1] if out.strip() else ""
        if code != 0 or not new.isdigit() or new == old:
            fail("supervisor did not restart telemetry after kill: " + err + out)
        code, out, err = ssh(
            "grep -c 'telemetry is not a network node, K unchanged' "
            "/tmp/supervisor.out || true"
        )
        if out.strip() == "0":
            fail("supervisor spent K or did not say K was unchanged")
        stream = connect(8)
        again = stream.wait(ready, 8)
        if again is None:
            fail("snapshots did not return after telemetry was restarted")
        print(
            "telemetry-check killed %s restarted %s seq %d urgent on_time %d"
            % (old, new, again["seq"], again["classes"]["URGENT"]["on_time"])
        )

        before = count_lines("sink: signal ")
        code, out, err = ssh("kill -STOP " + new)
        if code != 0:
            fail("could not stop telemetry: " + err)
        time.sleep(1.5)
        guest_alive(proc)
        after = count_lines("sink: signal ")
        late = count_lines("class URGENT late")
        safe = count_lines("sink: state SAFE")
        code, out, err = ssh("kill -0 " + new)
        if code != 0:
            fail("stopped telemetry did not stay alive")
        if after <= before:
            fail(
                "network stopped issuing while telemetry was stopped (%d then %d)"
                % (before, after)
            )
        if late != 0 or safe != 0:
            fail(
                "telemetry stop moved the network: URGENT late %d, SAFE lines %d"
                % (late, safe)
            )
        print(
            "telemetry-check sigstop signals %d -> %d urgent late %d safe %d"
            % (before, after, late, safe)
        )
        code, out, err = ssh("kill -CONT " + new)
        if code != 0:
            fail("could not continue telemetry: " + err)
        resumed = stream.wait(lambda snap: snap["seq"] > again["seq"], 8)
        if resumed is None:
            fail("snapshots did not resume after SIGCONT")
        print("telemetry-check continued seq %d" % resumed["seq"])

        stream.close()
        fresh = connect(5)
        guest_alive(proc)
        reconnected = fresh.wait(ready, 8)
        if reconnected is None:
            fail("a new client did not receive a snapshot")
        print(
            "telemetry-check reconnected seq %d state %s"
            % (reconnected["seq"], reconnected["state"])
        )
        example = os.path.join(ROOT, "build", "telemetry-snapshot.txt")
        with open(example, "w") as handle:
            handle.write(reconnected["raw"])
        print("telemetry-check example snapshot")
        print(reconnected["raw"], end="" if reconnected["raw"].endswith("\n") else "\n")

        code, out, err = ssh("touch /tmp/telemetry-done")
        if code != 0:
            fail("could not ask the guest to stop: " + err)

        final = None
        deadline = time.time() + 12
        while time.time() < deadline:
            if not fresh.pump():
                fresh.close()
                try:
                    fresh = Stream()
                except OSError:
                    time.sleep(0.2)
                    continue
            code, text, err = ssh("cat /tmp/sink.out || true")
            if fresh.last is not None and "sink: unaccounted 0" in text:
                if summary_matches(fresh.last, text):
                    final = fresh.last
                    break
            time.sleep(0.4)
        if final is None:
            code, text, err = ssh("tail -n 40 /tmp/sink.out || true")
            fail("final snapshot did not match the sink summary\n" + text)
        print("telemetry-check final seq %d state %s mode %s" % (
            final["seq"], final["state"], final["source_mode"]))
        for name in CLASSES:
            klass = final["classes"][name]
            print(
                "telemetry-check %s issued %d on_time %d late %d lost %d dropped %d"
                % (
                    name,
                    klass["issued"],
                    klass["on_time"],
                    klass["late"],
                    klass["lost"],
                    klass["dropped"],
                )
            )
        fresh.close()
        finished = True
    finally:
        if not finished:
            ssh("touch /tmp/telemetry-done >/dev/null 2>&1 || true")
        try:
            proc.wait(timeout=25)
        except subprocess.TimeoutExpired:
            proc.kill()
            fail("guest scenario did not exit")
        log.close()
    if proc.returncode != 0:
        fail("guest scenario exited %d" % proc.returncode)
    code, out, err = ssh(
        "grep -E 'LOCKED_SAFE|restarted relay|restarted source' /tmp/supervisor.out || true"
    )
    if out.strip() != "":
        fail("network was restarted or locked:\n" + out)
    code, out, err = ssh("grep -c 'sink: unaccounted 0' /tmp/sink.out || true")
    if out.strip() == "0":
        fail("sink did not finish with unaccounted 0")
    print("telemetry-check guest exit 0 unaccounted 0")


def summary_matches(snap, sink_text):
    state = None
    rows = {}
    for line in sink_text.splitlines():
        match = re.match(r"sink: state (\S+)", line)
        if match:
            state = match.group(1)
        match = re.match(
            r"sink: summary (\S+) received (\d+), on time (\d+), late (\d+), "
            r"lost (\d+), dropped (\d+)",
            line,
        )
        if match:
            rows[match.group(1)] = tuple(int(part) for part in match.groups()[1:])
    if state is None or snap["state"] != state or snap["source_mode"] != "normal":
        return False
    for name in CLASSES:
        if name not in rows or name not in snap["classes"]:
            return False
        received, on_time, late, lost, dropped = rows[name]
        klass = snap["classes"][name]
        if (klass["on_time"], klass["late"], klass["lost"], klass["dropped"]) != (
            on_time,
            late,
            lost,
            dropped,
        ):
            return False
        if klass["issued"] != on_time + late + lost + dropped:
            return False
        if received != on_time + late:
            return False
    return True


if __name__ == "__main__":
    main()
