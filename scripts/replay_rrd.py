"""Replay of two real B29 runs as a Rerun recording (B31).

    build/replay/venv/bin/python scripts/replay_rrd.py \
        --runs replay/b29 --pose build/replay/arm-replay-pose \
        --out build/replay/b29.rrd

Left: naive-flood-75 (same QNX, naive network). Right: ours-flood-11.
Both under flood, chosen in D-092. One timeline "replay", zero at the
"person in" event, times in integer ns from the journals (no rescaling).
The arm pose comes from the sink's own model (scripts/replay_model.py);
the replay fails if the model differs from the journal by more than
0.01 mm in any frame. Playback speed 0.02 (D-095) is set in the
blueprint stored in the file.
"""

import argparse
import math
import os
import sys

import numpy as np
import rerun as rr
import rerun.blueprint as rrb

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import replay_model as rm  # noqa: E402

APP_ID = "nerve_replay_b29"
TIMELINE = "replay"
# D-095: 50x slower; the 200 ms from the event to the stop take ~10 s.
PLAYBACK_SPEED = 0.02
LOOP_FROM_NS = -100_000_000
LOOP_TO_NS = 250_000_000
# The last journal frame of the two runs (ours at rest; naive touched at
# +194.304 ms). main() checks it against the journals.
LAST_FRAME_NS = 203_858_899

EYE_RADIUS_MM = 12.0
ARM_COLOR = [176, 184, 196]
JOINT_COLOR = [90, 98, 112]
NET_COLOR = {"naive": [230, 90, 70], "ours": [70, 190, 120]}
PLAN_COLOR = [240, 200, 60]

SIDES = (
    ("naive", "naive-flood-75"),
    ("ours", "ours-flood-11"),
)


def mm(xy):
    return [xy[0] * 1000.0, xy[1] * 1000.0, 0.0]


def at(ns):
    rr.set_time(TIMELINE, duration=np.timedelta64(int(ns), "ns"))


def caption(net, run, s, totals):
    touched, n = totals[(net, "flood")]
    head = "**Replay of a real run** · `%s` · B29, 2026-11-25, under flood" % run["name"]
    if net == "naive":
        who = ("**Same QNX, naive network: one priority, first-come queue.** "
               "Touch when the delay exceeds the URGENT deadline (31 ms).")
        end = "**touched the eye** at +%.1f ms" % s["touch_ms"]
    else:
        who = ("**Same QNX, our network: URGENT first, on its own priority "
               "and its own relay thread.**")
        end = "**rests %.2f mm from the eye** (%.2f mm past the plan)" % (
            s["rest_mm"], s["past_plan_mm"])
    run_line = ("person in → brake **%.3f ms** · stop signal URGENT #%d **%s** · "
                "extra path %.2f mm · %s"
                % (s["delay_ms"], s["stop_number"], s["stop_fate"],
                   s["extra_path_mm"], end))
    series = ("Series under flood: touched the eye in %d of %d runs. "
              "Yellow ring: planned stop, %.2f mm from the eye (31 ms × 250 mm/s)."
              % (touched, n, s["plan_point_mm"]))
    return "\n\n".join([head, who, run_line, series])


def eye_parts(eye_mm, toward):
    """A stylized eye: white ball, iris disc and pupil facing the arm.
    The front of the iris is the eye point itself (where touch is counted)."""
    u = np.array(toward) / np.linalg.norm(toward)  # from the arm into the eye
    e = np.array(eye_mm)
    centre = e + u * EYE_RADIUS_MM
    # Rotate the disc's thin z axis onto u (u lies in the arm plane).
    axis = [-u[1], u[0], 0.0]
    rot = rr.RotationAxisAngle(axis=axis, degrees=90.0)
    return centre, e + u * 1.0, rot


def log_scene(net, run, pose, marks, s):
    root = "%s/scene" % net
    rr.log(root, rr.ViewCoordinates.RIGHT_HAND_Y_UP, static=True)
    eye = mm(pose["eye_point"])
    # Direction of the last approach, from the tip at the event to the eye.
    at_event = min(pose["frames"], key=lambda f: abs(f["t_ns"]))
    tip0 = mm(at_event["joints"][2])
    toward = [eye[0] - tip0[0], eye[1] - tip0[1], 0.0]
    centre, iris, rot = eye_parts(eye, toward)
    rr.log(root + "/eye/ball", rr.Ellipsoids3D(
        centers=[centre], half_sizes=[[EYE_RADIUS_MM] * 3],
        colors=[[235, 235, 240]], fill_mode="solid"), static=True)
    rr.log(root + "/eye/iris", rr.Ellipsoids3D(
        centers=[iris], half_sizes=[[5.0, 5.0, 0.98]], rotation_axis_angles=[rot],
        colors=[[70, 120, 200]], fill_mode="solid"), static=True)
    rr.log(root + "/eye/pupil", rr.Ellipsoids3D(
        centers=[iris], half_sizes=[[2.2, 2.2, 1.0]], rotation_axis_angles=[rot],
        colors=[[15, 15, 20]], fill_mode="solid"), static=True)
    ring = [[eye[0] + s["plan_point_mm"] * math.cos(a),
             eye[1] + s["plan_point_mm"] * math.sin(a), 0.0]
            for a in np.linspace(0, 2 * math.pi, 129)]
    # The planned stop (D-089): a ring at plan_point_mm around the eye
    # point; explained in the caption, no label over the place of contact.
    rr.log(root + "/plan", rr.LineStrips3D(
        [ring], radii=[0.25], colors=[PLAN_COLOR]), static=True)
    rr.log(root + "/base", rr.Boxes3D(
        centers=[[0.0, -20.0, 0.0]], half_sizes=[[45.0, 20.0, 45.0]],
        colors=[JOINT_COLOR], fill_mode="solid"), static=True)

    trail = []
    for f in pose["frames"]:
        at(f["t_ns"])
        pts = [[0.0, 0.0, 0.0]] + [mm(j) for j in f["joints"]]
        trail.append(pts[-1])
        rr.log(root + "/arm/links", rr.LineStrips3D(
            [pts[0:2], pts[1:3], pts[2:4]], radii=[9.0, 7.0, 3.0],
            colors=[ARM_COLOR] * 3))
        rr.log(root + "/arm/joints", rr.Points3D(
            pts[0:3], radii=[12.0, 10.0, 8.0], colors=[JOINT_COLOR] * 3))
        rr.log(root + "/arm/tip", rr.Points3D(
            [pts[3]], radii=[1.6], colors=[NET_COLOR[net]]))
        rr.log(root + "/arm/trail", rr.LineStrips3D(
            [trail], radii=[0.25], colors=[NET_COLOR[net]]))

    for m in marks:
        at(m["t_ns"])
        rr.log("events/%s" % net, rr.TextLog(
            m["text"],
            level="WARN" if m["kind"] in ("touch",) or "late" in m["text"] else "INFO"))


def log_chart(net, pose, marks, s):
    rr.log("chart/%s" % net, rr.SeriesLines(
        colors=[NET_COLOR[net]],
        names=["%s network, tip to eye (mm)" % net], widths=[2.0]), static=True)
    rr.log("chart/marks/%s" % net, rr.SeriesPoints(
        colors=[NET_COLOR[net]], names=["%s events" % net], markers=["circle"],
        marker_sizes=[5.0]), static=True)
    for f in pose["frames"]:
        at(f["t_ns"])
        rr.log("chart/%s" % net, rr.Scalars(f["eye_mm"]))
    for m in marks:
        if m["eye_mm"] is None:
            continue
        at(m["t_ns"])
        rr.log("chart/marks/%s" % net, rr.Scalars(m["eye_mm"]))


def log_clock_and_plan(frames, plan_mm):
    rr.log("chart/plan", rr.SeriesLines(
        colors=[PLAN_COLOR], names=["planned stop %.2f mm" % plan_mm],
        widths=[1.0]), static=True)
    for f in frames:
        at(f["t_ns"])
        rr.log("chart/plan", rr.Scalars(plan_mm))
        rr.log("clock", rr.TextDocument(
            "# %+.3f ms\nsince **person in** · 2 ms frames · "
            "playback %.2f× (%d× slower)"
            % (f["t_ns"] / 1e6, PLAYBACK_SPEED, round(1 / PLAYBACK_SPEED)),
            media_type=rr.MediaType.MARKDOWN))


def camera(eye_mm, home_tip_mm):
    """One fixed camera for both views: looking at the last part of the
    approach (eye and forearm), slightly from above and from the side."""
    e = np.array(eye_mm)
    h = np.array(home_tip_mm)
    # Square to the arm plane, close to the eye: the gap between the tool
    # tip and the eye (0 or 6.91 mm) must be visible.
    target = e + (h - e) / np.linalg.norm(h - e) * 16.0
    position = target + np.array([-6.0, 4.0, 72.0])
    return rrb.EyeControls3D(
        kind=rrb.Eye3DKind.Orbital, position=position.tolist(),
        look_target=target.tolist(), eye_up=[0.0, 1.0, 0.0])


def blueprint(eye_mm, home_tip_mm):
    eye = camera(eye_mm, home_tip_mm)

    def side(net, title):
        return rrb.Vertical(
            rrb.Spatial3DView(name=title, origin="/%s/scene" % net,
                              eye_controls=eye, background=[24, 26, 32],
                              line_grid=False),
            rrb.TextDocumentView(name="%s: caption" % net,
                                 origin="/%s/caption" % net),
            row_shares=[3, 2])

    return rrb.Blueprint(
        rrb.Vertical(
            rrb.Horizontal(
                side("naive", "naive network (same QNX)"),
                side("ours", "our network (same QNX)")),
            rrb.Horizontal(
                rrb.TextDocumentView(name="clock", origin="/clock"),
                rrb.TextLogView(name="events", origin="/events"),
                rrb.TimeSeriesView(name="tip to eye, mm", origin="/chart",
                                   axis_y=rrb.ScalarAxis(range=(0.0, 40.0)),
                                   plot_legend=rrb.PlotLegend(rrb.Corner2D.RightTop),
                                   time_ranges=rrb.VisibleTimeRange(
                                       TIMELINE,
                                       start=rrb.TimeRangeBoundary.absolute(nanos=-50_000_000),
                                       end=rrb.TimeRangeBoundary.absolute(nanos=LOOP_TO_NS))),
                column_shares=[1, 2, 2]),
            row_shares=[3, 1]),
        rrb.TimePanel(
            state="collapsed",
            timeline=TIMELINE, playback_speed=PLAYBACK_SPEED,
            play_state="paused", loop_mode="selection",
            time_selection=rrb.components.AbsoluteTimeRange(
                min=LOOP_FROM_NS, max=LOOP_TO_NS)),
        rrb.BlueprintPanel(state="collapsed"),
        rrb.SelectionPanel(state="collapsed"),
        collapse_panels=False,
    )


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--runs", required=True)
    ap.add_argument("--pose", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args(argv)

    totals = rm.series_totals(os.path.join(a.runs, "series-report.txt"))
    data = []
    for net, name in SIDES:
        run = rm.load_run(a.runs, name)
        if run["network"] != net or run["mode"] != "flood":
            raise rm.ReplayError("%s is %s/%s" % (name, run["network"], run["mode"]))
        pose = rm.poses(run, a.pose)
        data.append((net, run, pose, rm.events(run), rm.summary(run)))
        print("%s: %d frames, model vs journal worst %.3f mm (limit %.2f)"
              % (name, len(pose["frames"]), pose["worst_diff_mm"], rm.POSE_TOLERANCE_MM))

    last = max(d[2]["frames"][-1]["t_ns"] for d in data)
    if last != LAST_FRAME_NS:
        raise rm.ReplayError("last frame at %d ns, LAST_FRAME_NS is %d" % (last, LAST_FRAME_NS))
    eye = mm(data[0][2]["eye_point"])
    home_tip = mm(data[0][2]["frames"][0]["joints"][2])
    rr.init(APP_ID, spawn=False)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    bp = blueprint(eye, home_tip)
    rr.save(a.out, default_blueprint=bp)
    # The viewer remembers a layout per application; the .rbl next to the
    # .rrd, opened with it, puts this layout back (scripts/replay_view.py).
    bp.save(APP_ID, os.path.splitext(a.out)[0] + ".rbl")
    longest = max(data, key=lambda d: len(d[2]["frames"]))
    log_clock_and_plan(longest[2]["frames"], longest[4]["plan_point_mm"])
    for net, run, pose, marks, s in data:
        rr.log("%s/caption" % net, rr.TextDocument(
            caption(net, run, s, totals), media_type=rr.MediaType.MARKDOWN),
            static=True)
        log_scene(net, run, pose, marks, s)
        log_chart(net, pose, marks, s)
        for m in marks:
            print("  %-5s %+9.3f ms  %s" % (net, m["t_ns"] / 1e6, m["text"]))
    rr.disconnect()
    print("wrote %s" % a.out)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except rm.ReplayError as e:
        print("replay_rrd: FAIL: %s" % e, file=sys.stderr)
        sys.exit(1)
