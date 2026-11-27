"""Host tests for the B31 replay (scripts/replay_model.py).

Run from the project root after build/replay/arm-replay-pose is built
(make test does both). Standard library only; Rerun is not needed.
Expected numbers are the B29 results written in D-092, not read back from
the files under test.
"""

import copy
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import replay_model as rm  # noqa: E402

RUNS = os.path.join(ROOT, "replay", "b29")
POSE = os.path.join(ROOT, "build", "replay", "arm-replay-pose")


def load(name):
    run = rm.load_run(RUNS, name)
    return run, rm.poses(run, POSE)


class ZeroIsTheEvent(unittest.TestCase):
    def test_zero_of_the_timeline_is_person_in(self):
        for name in ("ours-flood-11", "naive-flood-75"):
            run, pose = load(name)
            log = run["logged"]
            marks = rm.events(run)
            person = [m for m in marks if m["kind"] == "person_in"]
            self.assertEqual(len(person), 1, name)
            self.assertEqual(person[0]["t_ns"], 0, name)
            # Frame times are the journal's grid, shifted by the event only.
            for f in pose["frames"]:
                self.assertEqual(f["t_ns"] + log["event_ns"],
                                 log["approach_start_ns"] + f["index"] * run["step_ns"])
            # The event is the planned instant of D-090: 668.141101 ms.
            self.assertEqual(log["event_ns"] - log["approach_start_ns"], 668141101)
            self.assertEqual(pose["frames"][0]["t_ns"], -668141101)


class ModelMatchesJournal(unittest.TestCase):
    def test_every_frame_within_0_01_mm(self):
        for name in ("ours-flood-11", "naive-flood-75"):
            _, pose = load(name)
            self.assertLessEqual(pose["worst_diff_mm"], rm.POSE_TOLERANCE_MM, name)
            for f in pose["frames"]:
                self.assertLessEqual(abs(f["model_eye_mm"] - f["eye_mm"]), 0.01)

    def test_a_frame_off_by_0_02_mm_fails(self):
        run = rm.load_run(RUNS, "ours-flood-11")
        bad = copy.deepcopy(run)
        bad["samples"][300][1] += 0.02
        with self.assertRaises(rm.ReplayError) as e:
            rm.poses(bad, POSE)
        self.assertIn("frame 300", str(e.exception))

    def test_a_wrong_brake_time_fails(self):
        run = rm.load_run(RUNS, "ours-flood-11")
        bad = copy.deepcopy(run)
        bad["logged"]["track_brake_ns"] += 1000000  # 1 ms late: 0.25 mm more
        with self.assertRaises(rm.ReplayError):
            rm.poses(bad, POSE)

    def test_log_and_json_must_agree(self):
        run = rm.load_run(RUNS, "naive-flood-75")
        self.assertEqual(run["logged"]["track_samples"], len(run["samples"]))


class NumbersOfB29(unittest.TestCase):
    """D-092: ours-flood-11 rests 6.91 mm from the eye, 0.84 mm past the
    plan, delay 3.360 ms; naive-flood-75 touches, delay 35.194 ms, extra
    path 8.80 mm, stop signal late."""

    def test_ours_stops_where_b29_says(self):
        run, pose = load("ours-flood-11")
        s = rm.summary(run)
        self.assertEqual(run["track_end"], "stop")
        self.assertAlmostEqual(s["delay_ms"], 3.360, places=3)
        self.assertEqual(round(s["rest_mm"], 2), 6.91)
        self.assertEqual(round(s["past_plan_mm"], 2), 0.84)
        self.assertEqual(s["stop_fate"], "delivered")
        last = pose["frames"][-1]
        self.assertEqual(round(last["model_eye_mm"], 2), 6.91)
        stop = [m for m in rm.events(run) if m["kind"] == "stop"]
        self.assertEqual(len(stop), 1)
        self.assertEqual(stop[0]["t_ns"], last["t_ns"])

    def test_naive_touches_where_b29_says(self):
        run, pose = load("naive-flood-75")
        s = rm.summary(run)
        self.assertEqual(run["track_end"], "touch")
        self.assertAlmostEqual(s["delay_ms"], 35.194, places=3)
        self.assertEqual(round(s["extra_path_mm"], 2), 8.80)
        self.assertEqual(s["rest_mm"], 0.0)
        self.assertEqual(s["stop_fate"], "late")
        # The touch frame: the first at the eye, the last of the journal.
        at_eye = [f for f in pose["frames"] if f["eye_mm"] == 0.0]
        self.assertEqual(at_eye[0]["index"], len(pose["frames"]) - 1)
        self.assertEqual(at_eye[0]["model_eye_mm"], 0.0)
        touch = [m for m in rm.events(run) if m["kind"] == "touch"]
        self.assertEqual(len(touch), 1)
        # The touch lies inside the last 2 ms step, as the sink computed it.
        self.assertLess(pose["frames"][-2]["t_ns"], touch[0]["t_ns"])
        self.assertLessEqual(touch[0]["t_ns"], pose["frames"][-1]["t_ns"])

    def test_stop_signal_marks(self):
        for name, number, fate in (("ours-flood-11", 2259, "delivered"),
                                   ("naive-flood-75", 2262, "late")):
            run = rm.load_run(RUNS, name)
            marks = rm.events(run)
            arrived = [m for m in marks if m["kind"] == "urgent_arrived"]
            self.assertEqual(len(arrived), 1)
            self.assertIn("#%d" % number, arrived[0]["text"])
            self.assertIn(fate, arrived[0]["text"])
            issued = [m for m in marks if m["kind"] == "urgent_issued"][0]
            self.assertLess(0, issued["t_ns"])
            self.assertLess(issued["t_ns"], arrived[0]["t_ns"])

    def test_series_totals(self):
        t = rm.series_totals(os.path.join(RUNS, "series-report.txt"))
        self.assertEqual(t[("naive", "flood")], (16, 97))
        self.assertEqual(t[("ours", "flood")], (0, 34))
        self.assertEqual(t[("ours", "calm")], (0, 34))


if __name__ == "__main__":
    unittest.main(verbosity=2)
