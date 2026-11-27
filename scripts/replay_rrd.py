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
B33: one take on the close-up tab. A second timeline "video" holds
screen time: the approach at 1/APPROACH_SLOWDOWN, from SLOW_FROM_NS on
at 1/EVENT_SLOWDOWN (0.02, D-095); it plays at speed 1 and starts by
itself. Every frame is logged on both timelines, so a "video" instant
always shows a journal frame with its own real time. A Pinhole camera
on "video" goes from a view of the whole arm to the B32 close-up; the
close-up views track it. The arm is drawn as a manipulator (base, thick
links, joint housings); its pose is the same model output as before.
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
# The first journal frame of both runs: the start of the approach.
FIRST_FRAME_NS = -668_141_101

# B33, the "video" timeline (screen time). Integer slowdowns, so every
# video time is an exact number of ns. The event part keeps D-095.
VIDEO_TIMELINE = "video"
APPROACH_SLOWDOWN = 10
EVENT_SLOWDOWN = 50
assert abs(PLAYBACK_SPEED * EVENT_SLOWDOWN - 1.0) < 1e-12
SLOW_FROM_NS = -20_000_000
# After the last frame the picture stays this long before the loop restarts.
VIDEO_TAIL_NS = 1_500_000_000
# The camera: the whole arm until CAM_DOLLY_FROM_S, the dolly to the B32
# close-up until CAM_DOLLY_TO_S, then the close-up (screen seconds).
CAM_DOLLY_FROM_S = 2.0
CAM_DOLLY_TO_S = 6.0
CAM_STEP_S = 1.0 / 60.0
# Room around the whole arm in the first shot.
OVERVIEW_MARGIN = 1.15

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
# B33: the manipulator's look (drawing only; lengths and poses are D-079).
LINK_RADII_MM = (26.0, 20.0)
# Joint housings: drums across the arm plane, on the joint axes.
HOUSING_RADII_MM = (42.0, 34.0, 24.0)
HOUSING_LENGTHS_MM = (96.0, 80.0, 58.0)
# The instrument holder: a sleeve on the last link from its joint; the
# rest of the last link is the B32 tool, tip unchanged.
HOLDER_RADIUS_MM = 7.0
HOLDER_LENGTH_MM = 40.0
HOUSING_COLOR = [70, 78, 92]
LINK_COLOR = [214, 219, 226]
NET_COLOR = {"naive": [230, 90, 70], "ours": [70, 190, 120]}
PLAN_COLOR = [240, 200, 60]

SIDES = (
    ("naive", "naive-flood-75"),
    ("ours", "ours-flood-11"),
)


def mm(xy):
    return [xy[0] * 1000.0, xy[1] * 1000.0, 0.0]


def video_ns(t_ns):
    """Screen time of real time t_ns (ns after person in), ns from the
    start of the take: the approach at 1/APPROACH_SLOWDOWN, from
    SLOW_FROM_NS on at 1/EVENT_SLOWDOWN. Exact integers."""
    t = int(t_ns)
    if t < FIRST_FRAME_NS:
        raise ValueError("%d ns is before the first frame" % t)
    if t <= SLOW_FROM_NS:
        return (t - FIRST_FRAME_NS) * APPROACH_SLOWDOWN
    return ((SLOW_FROM_NS - FIRST_FRAME_NS) * APPROACH_SLOWDOWN
            + (t - SLOW_FROM_NS) * EVENT_SLOWDOWN)


def video_end_ns():
    return video_ns(LAST_FRAME_NS) + VIDEO_TAIL_NS


def at(ns):
    """Both timelines: real time from the journal and its screen time."""
    rr.set_time(TIMELINE, duration=np.timedelta64(int(ns), "ns"))
    rr.set_time(VIDEO_TIMELINE, duration=np.timedelta64(video_ns(ns), "ns"))


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


def z_to(v):
    """Rotation that turns +z onto the direction v (in the arm plane)."""
    d = unit(v)
    angle = math.degrees(math.acos(max(-1.0, min(1.0, d[2]))))
    return rr.RotationAxisAngle(axis=[-d[1], d[0], 0.0], degrees=angle)


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
    # The base: a pedestal under the first joint (drawing only, B33).
    rr.log(root + "/base", rr.Boxes3D(
        centers=[[0.0, -HOUSING_RADII_MM[0] - 40.0, 0.0]],
        half_sizes=[[70.0, 40.0, 70.0]],
        colors=[HOUSING_COLOR], fill_mode="solid"), static=True)

    j2, tip_home = mm(pose["frames"][0]["joints"][1]), mm(pose["frames"][0]["joints"][2])
    rr.log(root + "/arm/tool", tool_mesh(float(np.linalg.norm(
        np.array(tip_home) - np.array(j2)))), static=True)
    gaps = rm.gap_labels(run, pose)
    trail = []
    for f, gap in zip(pose["frames"], gaps):
        at(f["t_ns"])
        pts = [[0.0, 0.0, 0.0]] + [mm(j) for j in f["joints"]]
        trail.append(pts[-1])
        # B33: the same joint points as before, drawn as a manipulator:
        # thick links between the joints, a housing on each joint axis
        # (across the arm plane), a holder on the start of the last link.
        rr.log(root + "/arm/links", rr.Capsules3D(
            translations=pts[0:2],
            lengths=[float(np.linalg.norm(np.subtract(pts[k + 1], pts[k])))
                     for k in range(2)],
            radii=list(LINK_RADII_MM),
            rotation_axis_angles=[z_to(np.subtract(pts[k + 1], pts[k])) for k in range(2)],
            colors=[LINK_COLOR] * 2, fill_mode="solid"))
        rr.log(root + "/arm/joints", rr.Cylinders3D(
            centers=pts[0:3], radii=list(HOUSING_RADII_MM),
            lengths=list(HOUSING_LENGTHS_MM),
            colors=[HOUSING_COLOR] * 3, fill_mode="solid"))
        d3 = unit(np.subtract(pts[3], pts[2]))
        rr.log(root + "/arm/holder", rr.Cylinders3D(
            centers=[(np.array(pts[2]) + d3 * HOLDER_LENGTH_MM / 2.0).tolist()],
            radii=[HOLDER_RADIUS_MM], lengths=[HOLDER_LENGTH_MM],
            rotation_axis_angles=[z_to(d3)],
            colors=[LINK_COLOR], fill_mode="solid"))
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
        rr.log("clock", rr.TextDocument(clock_text(f["t_ns"]),
                                        media_type=rr.MediaType.MARKDOWN))


def clock_text(t_ns):
    """The counter: real time from the journal; the screen speed after it."""
    if t_ns > SLOW_FROM_NS:
        speed = "**slow motion ×%d** (playback %.2f×)" % (EVENT_SLOWDOWN, 1.0 / EVENT_SLOWDOWN)
    else:
        speed = "approach ×%d slower" % APPROACH_SLOWDOWN
    return "# %+.3f ms\nsince **person in** · real time from the journal · 2 ms frames · %s" % (
        t_ns / 1e6, speed)


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


def closeup_frame(eye_mm, toward, tool_dir):
    """One camera for both close-ups (B32): from the side of the eye, so
    the gap between the tip and the eye is seen against the background,
    tilted CLOSEUP_TILT_DEG towards the arm so the iris shows; the tool
    enters from the upper right at CLOSEUP_TOOL_SCREEN_DEG. Returns the
    screen axes (right, up, back), the target and the position."""
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
    d = closeup_distance_mm()
    return {"right": right, "up": up, "back": back, "target": target,
            "distance": d, "position": target + back * d}


def overview_frame(close, points_mm):
    """B33, the first shot: the close-up's own direction, pulled back so
    every point (base, joints of all frames, eye) fits with a margin."""
    p = np.array(points_mm, dtype=float)
    r = p @ close["right"]
    u = p @ close["up"]
    centre_r, centre_u = (r.min() + r.max()) / 2.0, (u.min() + u.max()) / 2.0
    # The target on the plane through the eye target, across the screen.
    depth = np.dot(close["target"], close["back"])
    target = (centre_r * close["right"] + centre_u * close["up"]
              + depth * close["back"])
    half_h = max((u.max() - u.min()) / 2.0,
                 (r.max() - r.min()) / 2.0 / CLOSEUP_ASPECT) * OVERVIEW_MARGIN
    d = half_h / math.tan(math.radians(FOV_Y_DEG) / 2.0)
    # Far enough that nothing (nearest point) is behind the camera.
    d += max(0.0, float((p @ close["back"]).max() - depth))
    return {"target": target, "distance": d}


def camera_at(s_video, over, close):
    """Camera target and distance at screen time s_video (s): the whole
    arm, then a dolly (smooth start and stop) to the close-up. Distance
    falls geometrically, so the zoom speed looks even; the target moves
    in step with how much of the distance is covered."""
    if s_video <= CAM_DOLLY_FROM_S:
        k = 0.0
    elif s_video >= CAM_DOLLY_TO_S:
        k = 1.0
    else:
        x = (s_video - CAM_DOLLY_FROM_S) / (CAM_DOLLY_TO_S - CAM_DOLLY_FROM_S)
        k = x * x * (3.0 - 2.0 * x)
    d0, d1 = over["distance"], close["distance"]
    d = d0 * (d1 / d0) ** k
    w = (d0 - d) / (d0 - d1)
    target = over["target"] + (close["target"] - over["target"]) * w
    return target, d


def log_camera(over, close):
    """A Pinhole camera on the "video" timeline only (it is staging, not
    journal data), for both scenes; the close-up views track it. The
    field of view is Rerun's 55 degrees, so the end is the B32 frame."""
    rot = np.column_stack([close["right"], close["up"], close["back"]])
    end_s = video_end_ns() / 1e9
    steps = int(math.ceil(CAM_DOLLY_TO_S / CAM_STEP_S))
    times = [i * CAM_STEP_S for i in range(steps + 1)]
    for net, _ in SIDES:
        path = "%s/scene/camera" % net
        rr.log(path, rr.Pinhole(
            fov_y=math.radians(FOV_Y_DEG), aspect_ratio=CLOSEUP_ASPECT,
            camera_xyz=rr.ViewCoordinates.RUB, image_plane_distance=0.001,
            line_width=0.0), static=True)
        for s in times:
            rr.reset_time()
            rr.set_time(VIDEO_TIMELINE, duration=np.timedelta64(int(round(s * 1e9)), "ns"))
            target, d = camera_at(s, over, close)
            rr.log(path, rr.Transform3D(
                translation=(target + close["back"] * d).tolist(), mat3x3=rot))
    rr.reset_time()
    return end_s


def blueprint(eye_mm, home_tip_mm, toward, tool_dir):
    eye = camera(eye_mm, home_tip_mm)

    def close(net):
        # B33: the view takes the pose and the 55 degree field of view of
        # the scene's Pinhole camera, frame by frame on "video".
        return rrb.EyeControls3D(tracking_entity="/%s/scene/camera" % net)

    def side(net, title, cam, shares, caption_title=True):
        return rrb.Vertical(
            rrb.Spatial3DView(name=title, origin="/%s/scene" % net,
                              eye_controls=cam(net) if callable(cam) else cam,
                              background=[14, 15, 19],
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
            # B33: screen time at speed 1 (the slowdowns are inside the
            # timeline), playing from the start, the whole take in a loop.
            timeline=VIDEO_TIMELINE, playback_speed=1.0,
            play_state="playing", loop_mode="selection",
            time_selection=rrb.components.AbsoluteTimeRange(
                min=0, max=video_end_ns())),
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
    for net, _, pose, _, _ in data:
        if pose["frames"][0]["t_ns"] != FIRST_FRAME_NS:
            raise rm.ReplayError("%s first frame at %d ns, FIRST_FRAME_NS is %d"
                                 % (net, pose["frames"][0]["t_ns"], FIRST_FRAME_NS))
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
    # B33: the first shot holds the whole arm in every frame, the base
    # and the housings around the joints included.
    close = closeup_frame(eye, toward, tool_dir)
    pts = [eye]
    for _, _, pose, _, _ in data:
        for f in pose["frames"]:
            for k, j in enumerate([(0.0, 0.0)] + list(f["joints"])):
                c = mm(j)
                r = HOUSING_RADII_MM[min(k, 2)]
                pts += [[c[0] + dx, c[1] + dy, c[2] + dz]
                        for dx in (-r, r) for dy in (-r, r) for dz in (-r, r)]
    base_y = -HOUSING_RADII_MM[0] - 80.0
    pts += [[x, base_y, z] for x in (-70.0, 70.0) for z in (-70.0, 70.0)]
    over = overview_frame(close, pts)
    log_camera(over, close)
    print("camera: whole arm at %.0f mm, close-up at %.2f mm; take %.3f s on screen "
          "(approach x%d slower, from %+.0f ms x%d slower)"
          % (over["distance"], close["distance"], video_end_ns() / 1e9,
             APPROACH_SLOWDOWN, SLOW_FROM_NS / 1e6, EVENT_SLOWDOWN))
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
