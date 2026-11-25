#!/usr/bin/env python3
"""Telemetry gives the arm to a client that asks for it (D-080).

On the guest: telemetry and a live network with the arm cycling between
home and the eye. From the host: one plain client must see no arm line
(the B21 panel format), one client that sends "arm" must see the arm
move. Run make upload first. Exit 0 on PASS.
"""
import re
import socket
import subprocess
import sys
import time

SSH = ["ssh", "-p", "2222", "-i", "~/.ssh/id_ed25519", "-o", "BatchMode=yes",
       "root@localhost"]
SSH[3] = subprocess.os.path.expanduser(SSH[3])

GUEST_START = r"""
slay -Q -f supervisor telemetry source relay sink >/dev/null 2>&1
slay -Q -f qconn >/dev/null 2>&1
rm -f /tmp/telemetry.out /tmp/source.out /tmp/sink.out /tmp/relay-a.out /tmp/relay-b.out
export SIGNAL_QUIET=1 SIGNAL_TELEMETRY=1
nohup /tmp/telemetry > /tmp/telemetry.out 2>&1 &
sleep 0.5
nohup /tmp/sink 64 2000 > /tmp/sink.out 2>&1 &
nohup /tmp/relay signal-relay-b signal-sink > /tmp/relay-b.out 2>&1 &
nohup /tmp/relay signal-relay-a signal-relay-b > /tmp/relay-a.out 2>&1 &
nohup /tmp/source live signal-relay-a > /tmp/source.out 2>&1 &
i=0; while [ $i -lt 30 ] && [ ! -e /dev/signal-source ]; do i=$((i+1)); sleep 0.2; done
sleep 1
echo 'arm cycle' > /dev/signal-source
echo started
"""
GUEST_STOP = r"""
echo 'stop' > /dev/signal-source 2>/dev/null
sleep 2
slay -Q -f telemetry relay >/dev/null 2>&1
grep -E '^sink: (arm|unaccounted)' /tmp/sink.out
grep -E 'arm' /tmp/telemetry.out
"""

ARM_RE = re.compile(
    r"^arm brake (on|off) person (in|out) q1_mrad (-?\d+) q2_mrad (-?\d+) "
    r"q3_mrad (-?\d+) eye_um (\d+) speed_um_s (\d+) person_stops (\d+)$",
    re.M)


def read_snapshots(sock, count, timeout):
    buf = ""
    snaps = []
    end = time.time() + timeout
    sock.settimeout(0.5)
    while len(snaps) < count and time.time() < end:
        try:
            data = sock.recv(4096)
        except socket.timeout:
            continue
        if not data:
            break
        buf += data.decode("ascii", "replace")
        while "\nend\n" in buf:
            head, buf = buf.split("\nend\n", 1)
            snaps.append(head + "\nend\n")
    return snaps


def main():
    out = subprocess.run(SSH + [GUEST_START], capture_output=True, text=True,
                         timeout=60)
    if "started" not in out.stdout:
        print("arm-telemetry-check: FAIL guest did not start:", out.stderr)
        return 1
    ok = True
    try:
        plain = socket.create_connection(("127.0.0.1", 8000), timeout=5)
        snaps = read_snapshots(plain, 3, 5)
        plain.close()
        if not snaps:
            print("arm-telemetry-check: FAIL no plain snapshot")
            ok = False
        elif any("\narm" in s for s in snaps):
            print("arm-telemetry-check: FAIL a plain client got an arm line")
            ok = False
        else:
            print("arm-telemetry-check: plain client, %d snapshots, no arm line"
                  % len(snaps))
        armc = socket.create_connection(("127.0.0.1", 8000), timeout=5)
        armc.sendall(b"arm\n")
        snaps = read_snapshots(armc, 60, 8)
        armc.close()
        rows = [m.groups() for s in snaps for m in [ARM_RE.search(s)] if m]
        eyes = [int(r[5]) for r in rows]
        print("arm-telemetry-check: arm client, %d snapshots, %d with the arm"
              % (len(snaps), len(rows)))
        if rows:
            print("arm-telemetry-check: first %s" % " ".join(rows[0]))
            print("arm-telemetry-check: eye distance seen %d..%d um"
                  % (min(eyes), max(eyes)))
        if len(rows) < 10 or max(eyes) - min(eyes) < 100000:
            print("arm-telemetry-check: FAIL the arm did not show moving "
                  "(need 10 snapshots and 100 mm of eye distance)")
            ok = False
    finally:
        out = subprocess.run(SSH + [GUEST_STOP], capture_output=True,
                             text=True, timeout=60)
        print(out.stdout, end="")
    print("arm-telemetry-check: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
