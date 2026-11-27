"""Open the B31 replay in a Rerun window, with the cursor where it belongs.

    build/replay/venv/bin/python scripts/replay_view.py --rrd build/replay/b29.rrd
        opens the window paused at -100 ms (start of the loop, D-095);
    ... --shot evidence/replay-b29-stop.png
        opens the window at the last frame (both arms at their end: ours
        at rest, naive at the eye), saves a screenshot, closes the window.

B32: the window opens on the "close-up" tab; the UI zoom is set to
100 % + UI_ZOOM_STEPS x 10 % so the text is readable on a 1080p video.

The blueprint (layout, camera, playback 0.02) is inside the .rrd; the
time cursor is not part of a blueprint, so it is set here through the
viewer's gRPC port. The viewer listens on 127.0.0.1 only.
"""

import argparse
import subprocess
import sys
import time

import numpy as np
from rerun.experimental import ViewerClient

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import replay_rrd  # noqa: E402

PORT = 9876
# B32: the whole interface (captions, clock, labels) larger for a 1080p
# video: the viewer's own UI zoom, from 100 % in steps of 10 %.
UI_ZOOM_STEPS = 3


def connect(proc, limit_s=30.0):
    url = "rerun+http://127.0.0.1:%d/proxy" % PORT
    t0 = time.monotonic()
    while True:
        if proc.poll() is not None:
            sys.exit("replay_view: the viewer exited with %d" % proc.returncode)
        try:
            client = ViewerClient.connect(url)
            client.get_state()
            return client
        except Exception as e:  # the viewer is still starting
            if time.monotonic() - t0 > limit_s:
                proc.terminate()
                sys.exit("replay_view: no viewer at %s after %.0f s: %s" % (url, limit_s, e))
            time.sleep(0.5)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--rrd", required=True)
    ap.add_argument("--rerun", default=os.path.join(os.path.dirname(sys.executable), "rerun"))
    ap.add_argument("--shot", help="save a screenshot at the last frame and close")
    ap.add_argument("--at-ms", type=float, default=None,
                    help="cursor, ms after person in (default: -100, or the last frame with --shot)")
    a = ap.parse_args(argv)

    rbl = os.path.splitext(a.rrd)[0] + ".rbl"
    if not os.path.exists(rbl):
        sys.exit("replay_view: no %s; run make replay-rrd" % rbl)
    args = [a.rerun, "--bind", "127.0.0.1", "--port", str(PORT), a.rrd, rbl]
    if a.shot:
        args += ["--window-size", "1920x1080"]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=True)
    client = connect(proc)
    # The recording loads asynchronously; give it a moment before seeking.
    time.sleep(2.0)
    # The zoom is a viewer setting that outlives the window: start from
    # 100 % every time, so every run looks the same.
    client.run_command("ui.reset_ui_zoom")
    for _ in range(UI_ZOOM_STEPS):
        client.run_command("ui.zoom_in_ui")
    if a.at_ms is not None:
        at_ns = int(round(a.at_ms * 1e6))
    elif a.shot:
        at_ns = replay_rrd.LAST_FRAME_NS
    else:
        at_ns = replay_rrd.LOOP_FROM_NS
    client.set_time(replay_rrd.TIMELINE, duration=np.timedelta64(at_ns, "ns"),
                    play_state="paused")
    print("replay_view: viewer pid %d, cursor at %+.3f ms, paused" % (proc.pid, at_ns / 1e6))
    if a.shot:
        time.sleep(2.0)
        os.makedirs(os.path.dirname(os.path.abspath(a.shot)), exist_ok=True)
        client.save_screenshot(a.shot)
        print("replay_view: screenshot %s" % a.shot)
        proc.terminate()
        proc.wait(timeout=10)
    else:
        print("replay_view: press Space (or the play button) to play; "
              "the loop runs from %+.0f to %+.0f ms at %.2fx"
              % (replay_rrd.LOOP_FROM_NS / 1e6, replay_rrd.LOOP_TO_NS / 1e6,
                 replay_rrd.PLAYBACK_SPEED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
