"""Replay data for the B31 video, without Rerun (standard library only).

Reads one kept run (replay/b29/<run>.json and <run>.txt, D-085), runs the
sink's arm model again through build/replay/arm-replay-pose to get the
joint angles, checks the model against the journal in every frame, and
builds the event marks. scripts/replay_rrd.py turns this into a .rrd.

Time: one timeline, zero at the "person in" event, in integer ns taken
from the journal (event_ns - approach start). Nothing is rescaled.
"""

import json
import os
import re
import subprocess

# The model must give the journal's eye distance within this, every frame.
POSE_TOLERANCE_MM = 0.01

FATE_TEXT = {
    "delivered": "delivered in time",
    "late": "late (past the 31 ms URGENT deadline)",
    "dropped": "dropped",
    "lost": "lost",
}


class ReplayError(Exception):
    """The journal and the replay disagree; the replay must not be drawn."""


def _ints(line, names):
    out = {}
    for name in names:
        m = re.search(r"\b%s (-?\d+)\b" % re.escape(name), line)
        if m is None:
            raise ReplayError("no %s in: %s" % (name, line.strip()))
        out[name] = int(m.group(1))
    return out


def _floats(line, names):
    out = {}
    for name in names:
        m = re.search(r"\b%s (-?[0-9.]+)\b" % re.escape(name), line)
        if m is None:
            raise ReplayError("no %s in: %s" % (name, line.strip()))
        out[name] = float(m.group(1))
    return out


def _one(lines, prefix, path):
    found = [l for l in lines if l.startswith(prefix)]
    if len(found) != 1:
        raise ReplayError("%s: %d lines start with %r, need 1"
                          % (path, len(found), prefix))
    return found[0]


def parse_log(path):
    """The lines of a run log the replay needs."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    plan = _one(lines, "source: person-plan ", path)
    stop = _one(lines, "sink: arm-stop ", path)
    track = _one(lines, "sink: arm-track ", path)
    log = {}
    log.update(_ints(plan, ["approach_start_ns", "offset_ns", "event_ns",
                            "woke_late_ns", "margin_um", "event_eye_um"]))
    log.update(_ints(stop, ["delay_ns", "issue_to_brake_ns"]))
    log.update(_floats(stop, ["reaction_mm", "eye_brake_mm", "eye_rest_mm"]))
    t = _ints(track, ["start_ns", "step_ns", "samples", "brake_ns",
                      "stop_number", "touch_ns"])
    log.update({"track_" + k: v for k, v in t.items()})
    m = re.search(r"\bend (\w+)\b", track)
    log["track_end"] = m.group(1) if m else None
    return log


def load_run(runs_dir, name):
    """JSON and log of one run, checked against each other."""
    with open(os.path.join(runs_dir, name + ".json"), encoding="utf-8") as f:
        run = json.load(f)
    log = parse_log(os.path.join(runs_dir, name + ".txt"))
    checks = [
        ("approach start", run["approach_start_ns"], log["approach_start_ns"]),
        ("approach start (track)", run["approach_start_ns"], log["track_start_ns"]),
        ("event offset", log["event_ns"] - log["approach_start_ns"], log["offset_ns"]),
        ("samples", len(run["samples"]), log["track_samples"]),
        ("step", run["step_ns"], log["track_step_ns"]),
        ("track end", run["track_end"], log["track_end"]),
        ("stop signal", run["stop_signal"]["number"], log["track_stop_number"]),
    ]
    for what, a, b in checks:
        if a != b:
            raise ReplayError("%s: %s differs: json %r, log %r" % (name, what, a, b))
    run["name"] = name
    run["logged"] = log
    return run


def poses(run, pose_bin):
    """Frames from the model, checked against the journal (0.01 mm)."""
    log = run["logged"]
    brake = log["track_brake_ns"] - log["track_start_ns"] if log["track_brake_ns"] else -1
    out = subprocess.run(
        [pose_bin, str(run["step_ns"]), str(len(run["samples"])), str(brake)],
        check=True, stdout=subprocess.PIPE, universal_newlines=True).stdout
    frames = []
    eye = None
    for line in out.splitlines():
        p = line.split()
        if p[0] == "P":
            v = [float(x) for x in p[4:]]
            frames.append({
                "index": int(p[1]),
                "t_ns": int(p[2]) - log["offset_ns"],
                "model_eye_mm": int(p[3]) / 1000.0,
                "q": v[0:3],
                "joints": [(v[3], v[4]), (v[5], v[6]), (v[7], v[8])],
            })
        elif p[0] == "E":
            eye = (float(p[1]), float(p[2]))
    if len(frames) != len(run["samples"]) or eye is None:
        raise ReplayError("%s: model gave %d frames, journal has %d"
                          % (run["name"], len(frames), len(run["samples"])))
    worst = 0.0
    for f, (t_ms, eye_mm) in zip(frames, run["samples"]):
        if abs(f["index"] * run["step_ns"] / 1e6 - t_ms) > 1e-9:
            raise ReplayError("%s: frame %d at %r ms, journal %r ms"
                              % (run["name"], f["index"], f["index"] * 2, t_ms))
        diff = abs(f["model_eye_mm"] - eye_mm)
        if diff > POSE_TOLERANCE_MM:
            raise ReplayError(
                "%s: frame %d (%.3f ms after the event): model %.3f mm, "
                "journal %.3f mm from the eye, differ by %.3f mm > %.2f mm"
                % (run["name"], f["index"], f["t_ns"] / 1e6, f["model_eye_mm"],
                   eye_mm, diff, POSE_TOLERANCE_MM))
        f["eye_mm"] = eye_mm
        worst = max(worst, diff)
    return {"frames": frames, "eye_point": eye, "worst_diff_mm": worst}


def events(run):
    """Marks on the timeline, ns from the event, with what the journal says."""
    log = run["logged"]
    rel = lambda ns: ns - log["event_ns"]
    brake = rel(log["track_brake_ns"]) if log["track_brake_ns"] else None
    marks = [{"t_ns": 0, "kind": "person_in", "eye_mm": run["event_eye_mm"],
              "text": "person in (the sensor sees the tip at %.2f mm)"
                      % run["event_eye_mm"]}]
    stop = run["stop_signal"]
    for u in run["urgent_until_brake"]:
        if u["number"] != stop["number"]:
            # The journal times only the stop signal; others cannot be placed.
            raise ReplayError("%s: URGENT %d has no time in the journal"
                              % (run["name"], u["number"]))
        fate = FATE_TEXT.get(u["fate"].split()[0], u["fate"])
        issued = brake - log["issue_to_brake_ns"]
        marks.append({"t_ns": issued, "kind": "urgent_issued", "eye_mm": None,
                      "text": "URGENT #%d (stop signal) issued by source"
                              % u["number"]})
        marks.append({"t_ns": brake, "kind": "urgent_arrived",
                      "eye_mm": log["eye_brake_mm"],
                      "text": "URGENT #%d reaches the arm: %s" % (u["number"], fate)})
    if brake is not None:
        marks.append({"t_ns": brake, "kind": "brake", "eye_mm": log["eye_brake_mm"],
                      "text": "brake engaged, %.3f ms after person in, tip %.2f mm "
                              "from the eye" % (brake / 1e6, log["eye_brake_mm"])})
    last_t = rel(log["approach_start_ns"]) + (len(run["samples"]) - 1) * run["step_ns"]
    if run["track_end"] == "stop":
        marks.append({"t_ns": last_t, "kind": "stop", "eye_mm": run["rest_mm"],
                      "text": "arm at rest %.2f mm from the eye (first 2 ms frame "
                              "at rest)" % run["rest_mm"]})
    elif run["track_end"] == "touch":
        t = rel(log["track_touch_ns"])
        marks.append({"t_ns": t, "kind": "touch", "eye_mm": 0.0,
                      "text": "the tip touches the eye, %.1f ms after person in"
                              % (t / 1e6)})
    else:
        raise ReplayError("%s: track ended %r" % (run["name"], run["track_end"]))
    return sorted(marks, key=lambda m: m["t_ns"])


def series_totals(report_path):
    """B29 series lines: touched the eye X of N, per network and mode."""
    out = {}
    with open(report_path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"arm-precise (\w+) (\w+) closest to the eye .*touched "
                         r"the eye (\d+) of (\d+)", line)
            if m:
                out[(m.group(1), m.group(2))] = (int(m.group(3)), int(m.group(4)))
    if len(out) != 4:
        raise ReplayError("series report: %d of 4 touch lines" % len(out))
    return out


def summary(run):
    """Numbers shown on screen, all from the journal."""
    log = run["logged"]
    return {
        "delay_ms": (log["track_brake_ns"] - log["event_ns"]) / 1e6,
        "delay_ns_sink": log["delay_ns"],
        "extra_path_mm": run["extra_path_mm"],
        "rest_mm": run["rest_mm"],
        "past_plan_mm": run["past_plan_mm"],
        "plan_point_mm": run["plan_point_mm"],
        "touch_ms": (log["track_touch_ns"] - log["event_ns"]) / 1e6
                    if run["track_end"] == "touch" else None,
        "stop_number": run["stop_signal"]["number"],
        "stop_fate": run["stop_signal"]["fate"],
    }


def gap_labels(run, pose):
    """B32: the text on the gap line in every frame, from the journal.

    The number is the journal's tip-to-eye distance of that 2 ms frame
    (D-085), printed to 0.01 mm, not measured from the drawing. "contact"
    only in the frame where the journal ends with a touch (distance 0).
    """
    frames = pose["frames"]
    if len(frames) != len(run["samples"]):
        raise ReplayError("%s: %d frames, %d journal samples"
                          % (run["name"], len(frames), len(run["samples"])))
    out = []
    for i, (f, (_, eye_mm)) in enumerate(zip(frames, run["samples"])):
        if eye_mm == 0.0:
            if run["track_end"] != "touch" or i != len(frames) - 1:
                raise ReplayError("%s: distance 0 in frame %d, but the journal "
                                  "ends with %r" % (run["name"], i, run["track_end"]))
            text = "contact"
        else:
            text = "%.2f mm" % eye_mm
        out.append({"t_ns": f["t_ns"], "eye_mm": eye_mm, "text": text})
    return out
