"""Replay of two real B29 runs as a Rerun recording (B31).

    build/replay/venv/bin/python scripts/replay_rrd.py \
        --runs replay/b29 --pose build/replay/arm-replay-pose \
        --out build/replay/b29.rrd

Left: naive-flood-75 (same QNX, naive network). Right: ours-flood-11.
Both under flood, chosen in D-092.
B32: the default layout is a close-up from the side of the eye (tab
"close-up"); the B31 layout stays as the tab "overview". One timeline "replay", zero at the
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
# The naive run's last frame: the 2 ms frame where the journal has the
# touch (touch_ns +194.304 ms lies inside it). Checked in main().
NAIVE_TOUCH_FRAME_NS = 195_858_899

EYE_RADIUS_MM = 12.0
# Eye drawing (B32): angles from the axis of the eye that faces the arm.
PUPIL_RADIUS_MM = 2.2
IRIS_RADIUS_MM = 5.5
SCLERA_COLOR = [236, 233, 226]
LIMBUS_COLOR = [38, 62, 104]
IRIS_INNER_COLOR = [96, 150, 212]
IRIS_OUTER_COLOR = [52, 96, 164]
PUPIL_COLOR = [10, 10, 14]
# The tool: the last link drawn as a thin rod that narrows to the tip.
TOOL_COLOR = [214, 218, 226]
TOOL_SHAFT_RADIUS_MM = 0.9
TOOL_TIP_RADIUS_MM = 0.05
TOOL_TAPER_MM = 10.0
GAP_COLOR = [250, 250, 250]
# The close-up camera (B32, chosen by look, D-096): Rerun's 3D eye has a
# fixed vertical field of view of 55 degrees (DEFAULT_FOV_Y, re_view_spatial
# eye.rs, checked with a 10 mm ruler); the camera distance gives
# CLOSEUP_WIDTH_MM across a scene view of CLOSEUP_ASPECT (width / height),
# at the depth of the eye point. The aspect is the close-up view in the
# window that scripts/replay_view.py opens (1410x589 px, window 2848x1622
# px on a 1440x900 pt Retina screen); another window shape keeps the
# height (12.55 mm) and changes the width.
FOV_Y_DEG = 55.0
CLOSEUP_WIDTH_MM = 30.0
CLOSEUP_ASPECT = 2.39
CLOSEUP_TILT_DEG = 30.0
CLOSEUP_TOOL_SCREEN_DEG = 45.0
CLOSEUP_TARGET_MM = (5.0, 2.5)
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


def unit(v):
    v = np.asarray(v, dtype=float)
    return v / np.linalg.norm(v)


def eye_mesh(eye_mm, toward, rings=96, segments=192):
    """A stylized eye as one shaded sphere (B32): sclera, a dark limbus,
    iris and pupil coloured by the angle from the axis that faces the arm.
    The front of the pupil is the eye point itself (where touch is counted).
    No blood, no texture (PLAN, scene rules)."""
    u = unit(toward)  # from the arm into the eye
    front = -u
    a = unit(np.cross(front, [0.0, 0.0, 1.0]))
    b = np.cross(front, a)
    centre = np.array(eye_mm) + u * EYE_RADIUS_MM
    pupil = math.asin(PUPIL_RADIUS_MM / EYE_RADIUS_MM)
    iris = math.asin(IRIS_RADIUS_MM / EYE_RADIUS_MM)
    limbus = iris + math.radians(3.0)
    pos, nrm, col = [], [], []
    for i in range(rings + 1):
        th = math.pi * i / rings
        for j in range(segments):
            ph = 2.0 * math.pi * j / segments
            n = math.cos(th) * front + math.sin(th) * (math.cos(ph) * a + math.sin(ph) * b)
            pos.append(centre + EYE_RADIUS_MM * n)
            nrm.append(n)
            if th <= pupil:
                c = PUPIL_COLOR
            elif th <= iris:
                k = (th - pupil) / (iris - pupil)
                stripe = 0.88 + 0.12 * math.cos(28.0 * ph)
                c = [(IRIS_INNER_COLOR[m] * (1 - k) + IRIS_OUTER_COLOR[m] * k) * stripe
                     for m in range(3)]
            elif th <= limbus:
                k = (th - iris) / (limbus - iris)
                c = [LIMBUS_COLOR[m] * (1 - k) + SCLERA_COLOR[m] * k for m in range(3)]
            else:
                c = SCLERA_COLOR
            col.append([int(round(x)) for x in c])
    tri = []
    for i in range(rings):
        for j in range(segments):
            p0 = i * segments + j
            p1 = i * segments + (j + 1) % segments
            q0, q1 = p0 + segments, p1 + segments
            tri += [[p0, q0, q1], [p0, q1, p1]]
    return rr.Mesh3D(vertex_positions=pos, vertex_normals=nrm, vertex_colors=col,
                     triangle_indices=tri)


def tool_mesh(length_mm, segments=40):
    """The last link as a tool in its own frame: the tip at the origin,
    the rod along +z to the joint, narrowing over TOOL_TAPER_MM to the tip."""
    slope = (TOOL_SHAFT_RADIUS_MM - TOOL_TIP_RADIUS_MM) / TOOL_TAPER_MM
    # Two rings at the end of the taper: a crisp edge between cone and rod.
    profile = [(0.0, TOOL_TIP_RADIUS_MM, slope),
               (TOOL_TAPER_MM, TOOL_SHAFT_RADIUS_MM, slope),
               (TOOL_TAPER_MM, TOOL_SHAFT_RADIUS_MM, 0.0),
               (length_mm, TOOL_SHAFT_RADIUS_MM, 0.0)]
    pos, nrm, tri = [], [], []
    for z, r, dr in profile:
        for j in range(segments):
            ph = 2.0 * math.pi * j / segments
            pos.append([r * math.cos(ph), r * math.sin(ph), z])
            nrm.append(unit([math.cos(ph), math.sin(ph), -dr]).tolist())
    for k in (0, 2):
        for j in range(segments):
            p0 = k * segments + j
            p1 = k * segments + (j + 1) % segments
            tri += [[p0, p1, p1 + segments], [p0, p1 + segments, p0 + segments]]
    return rr.Mesh3D(vertex_positions=pos, vertex_normals=nrm,
                     vertex_colors=[TOOL_COLOR] * len(pos), triangle_indices=tri)


def tool_transform(joint, tip):
    """Puts the tool's +z on the link from the tip to the joint."""
    d = unit(np.array(joint) - np.array(tip))
    angle = math.degrees(math.acos(max(-1.0, min(1.0, d[2]))))
    return rr.Transform3D(translation=tip, rotation=rr.RotationAxisAngle(
        axis=[-d[1], d[0], 0.0], degrees=angle))


def log_scene(net, run, pose, marks, s):
    root = "%s/scene" % net
    rr.log(root, rr.ViewCoordinates.RIGHT_HAND_Y_UP, static=True)
    eye = mm(pose["eye_point"])
    # Direction of the last approach, from the tip at the event to the eye.
    at_event = min(pose["frames"], key=lambda f: abs(f["t_ns"]))
    tip0 = mm(at_event["joints"][2])
    toward = [eye[0] - tip0[0], eye[1] - tip0[1], 0.0]
    rr.log(root + "/eye", eye_mesh(eye, toward), static=True)
    ring = [[eye[0] + s["plan_point_mm"] * math.cos(a),
             eye[1] + s["plan_point_mm"] * math.sin(a), 0.0]
            for a in np.linspace(0, 2 * math.pi, 129)]
    # The planned stop (D-089): a ring at plan_point_mm around the eye
    # point; explained in the caption, no label over the place of contact.
    rr.log(root + "/plan", rr.LineStrips3D(
        [ring], radii=[0.12], colors=[PLAN_COLOR]), static=True)
    rr.log(root + "/base", rr.Boxes3D(
        centers=[[0.0, -20.0, 0.0]], half_sizes=[[45.0, 20.0, 45.0]],
        colors=[JOINT_COLOR], fill_mode="solid"), static=True)

    j2, tip_home = mm(pose["frames"][0]["joints"][1]), mm(pose["frames"][0]["joints"][2])
    rr.log(root + "/arm/tool", tool_mesh(float(np.linalg.norm(
        np.array(tip_home) - np.array(j2)))), static=True)
    gaps = rm.gap_labels(run, pose)
    trail = []
    for f, gap in zip(pose["frames"], gaps):
        at(f["t_ns"])
        pts = [[0.0, 0.0, 0.0]] + [mm(j) for j in f["joints"]]
        trail.append(pts[-1])
        rr.log(root + "/arm/links", rr.LineStrips3D(
            [pts[0:2], pts[1:3]], radii=[9.0, 7.0], colors=[ARM_COLOR] * 2))
        rr.log(root + "/arm/joints", rr.Points3D(
            pts[0:3], radii=[12.0, 10.0, 8.0], colors=[JOINT_COLOR] * 3))
        rr.log(root + "/arm/tool", tool_transform(pts[2], pts[3]))
        rr.log(root + "/arm/tip", rr.Points3D(
            [pts[3]], radii=[0.15], colors=[NET_COLOR[net]]))
        rr.log(root + "/arm/trail", rr.LineStrips3D(
            [trail], radii=[0.08], colors=[NET_COLOR[net]]))
        # The gap: a thin line from the tip to the eye point (what the
        # journal measures), with the journal's own number for this frame.
        rr.log(root + "/gap", rr.LineStrips3D(
            [[pts[3], eye]], radii=[0.04], colors=[GAP_COLOR],
            labels=[gap["text"]], show_labels=True))

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
    """The B31 camera (tab "overview"): looking at the last part of the
    approach (eye and forearm), slightly from above and from the side."""
    e = np.array(eye_mm)
    h = np.array(home_tip_mm)
    target = e + (h - e) / np.linalg.norm(h - e) * 16.0
    position = target + np.array([-6.0, 4.0, 72.0])
    return rrb.EyeControls3D(
        kind=rrb.Eye3DKind.Orbital, position=position.tolist(),
        look_target=target.tolist(), eye_up=[0.0, 1.0, 0.0])


def closeup_distance_mm():
    """Distance from the camera to the target that puts CLOSEUP_WIDTH_MM
    across the view at the target (fixed 55 degree vertical field)."""
    half_h = CLOSEUP_WIDTH_MM / CLOSEUP_ASPECT / 2.0
    return half_h / math.tan(math.radians(FOV_Y_DEG) / 2.0)


def closeup_camera(eye_mm, toward, tool_dir):
    """One camera for both close-ups (B32): from the side of the eye, so
    the gap between the tip and the eye is seen against the background,
    tilted CLOSEUP_TILT_DEG towards the arm so the iris shows; the tool
    enters from the upper right at CLOSEUP_TOOL_SCREEN_DEG."""
    u = unit(toward)
    z = np.array([0.0, 0.0, 1.0])
    t = math.radians(CLOSEUP_TILT_DEG)
    back = math.cos(t) * z - math.sin(t) * u  # from the target to the camera
    # Screen axes: up and right, with the tool's direction from the tip
    # (towards its joint) pointing up-right on screen.
    d = unit(tool_dir)
    d = unit(d - np.dot(d, back) * back)
    right_of_d = unit(np.cross(d, back))   # 90 degrees clockwise on screen
    a = math.radians(CLOSEUP_TOOL_SCREEN_DEG)
    up = math.sin(a) * d + math.cos(a) * -right_of_d
    right = np.cross(up, back)
    e = np.array(eye_mm)
    target = e + CLOSEUP_TARGET_MM[0] * right + CLOSEUP_TARGET_MM[1] * up
    position = target + back * closeup_distance_mm()
    return rrb.EyeControls3D(
        kind=rrb.Eye3DKind.Orbital, position=position.tolist(),
        look_target=target.tolist(), eye_up=up.tolist())


def blueprint(eye_mm, home_tip_mm, toward, tool_dir):
    eye = camera(eye_mm, home_tip_mm)
    close = closeup_camera(eye_mm, toward, tool_dir)

    def side(net, title, cam, shares, caption_title=True):
        return rrb.Vertical(
            rrb.Spatial3DView(name=title, origin="/%s/scene" % net,
                              eye_controls=cam, background=[14, 15, 19],
                              line_grid=False),
            rrb.TextDocumentView(name="%s: caption" % net,
                                 origin="/%s/caption" % net,
                                 titlebar=caption_title),
            row_shares=shares)

    # The close-up keeps the captions and the clock, without title bars so
    # they fit at a larger UI zoom; the event log stays in the overview.
    closeup = rrb.Vertical(
        rrb.Horizontal(
            side("naive", "naive network (same QNX)", close, [3, 2], False),
            side("ours", "our network (same QNX)", close, [3, 2], False)),
        rrb.TextDocumentView(name="clock", origin="/clock", titlebar=False),
        row_shares=[9, 2], name="close-up")

    overview = rrb.Vertical(
        rrb.Horizontal(
            side("naive", "naive network (same QNX)", eye, [3, 2]),
            side("ours", "our network (same QNX)", eye, [3, 2])),
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
        row_shares=[3, 1], name="overview")

    return rrb.Blueprint(
        rrb.Tabs(closeup, overview, active_tab=0),
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
    naive_last = [d for d in data if d[0] == "naive"][0][2]["frames"][-1]["t_ns"]
    if naive_last != NAIVE_TOUCH_FRAME_NS:
        raise rm.ReplayError("naive last frame at %d ns, NAIVE_TOUCH_FRAME_NS is %d"
                             % (naive_last, NAIVE_TOUCH_FRAME_NS))
    eye = mm(data[0][2]["eye_point"])
    home_tip = mm(data[0][2]["frames"][0]["joints"][2])
    # Both runs share the eye and the approach up to the event: one camera.
    at_event = min(data[0][2]["frames"], key=lambda f: abs(f["t_ns"]))
    tip0, j0 = mm(at_event["joints"][2]), mm(at_event["joints"][1])
    toward = [eye[0] - tip0[0], eye[1] - tip0[1], 0.0]
    tool_dir = [j0[0] - tip0[0], j0[1] - tip0[1], 0.0]
    rr.init(APP_ID, spawn=False)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    bp = blueprint(eye, home_tip, toward, tool_dir)
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
