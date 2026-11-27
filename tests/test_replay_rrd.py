"""B32: the gap line in the written .rrd, frame by frame, against the journal.

    build/replay/venv/bin/python tests/test_replay_rrd.py build/replay/b29.rrd

Needs Rerun (the replay venv); `make replay-rrd` runs it right after it
writes the file. Reads the recording back with rerun.chunk.RrdReader, so
what is checked is what the viewer shows, not what the script meant to log.
The expected text is built here from the journal samples (D-085), not by
replay_model.gap_labels, so the two are independent.
"""

import math
import os
import sys
import unittest

import rerun.chunk as rc

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import replay_model as rm  # noqa: E402

RUNS = os.path.join(ROOT, "replay", "b29")
POSE = os.path.join(ROOT, "build", "replay", "arm-replay-pose")
RRD = os.path.join(ROOT, "build", "replay", "b29.rrd")
SIDES = (("naive", "naive-flood-75"), ("ours", "ours-flood-11"))


def gap_rows(path):
    """{net: [(t_ns, label, strip)]} for /<net>/scene/gap, sorted by time."""
    rows = {}
    for chunk in rc.RrdReader(path).stream():
        ent = chunk.entity_path
        if not ent.endswith("/scene/gap"):
            continue
        net = ent.strip("/").split("/")[0]
        rb = chunk.to_record_batch()
        times = rb.column("replay").cast("int64").to_pylist()
        labels = rb.column("LineStrips3D:labels").to_pylist()
        strips = rb.column("LineStrips3D:strips").to_pylist()
        rows.setdefault(net, []).extend(
            (t, lab, st) for t, lab, st in zip(times, labels, strips))
    return {k: sorted(v, key=lambda r: r[0]) for k, v in rows.items()}


class GapLineInTheRecording(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = gap_rows(RRD)

    def test_one_label_per_journal_frame(self):
        for net, name in SIDES:
            run = rm.load_run(RUNS, name)
            pose = rm.poses(run, POSE)
            rows = self.rows[net]
            self.assertEqual([r[0] for r in rows], [f["t_ns"] for f in pose["frames"]], name)

    def test_label_is_the_journal_distance_of_that_frame(self):
        for net, name in SIDES:
            run = rm.load_run(RUNS, name)
            last = len(run["samples"]) - 1
            for i, ((_, label, _), (_, eye_mm)) in enumerate(zip(self.rows[net], run["samples"])):
                self.assertEqual(len(label), 1)
                if run["track_end"] == "touch" and i == last:
                    self.assertEqual(eye_mm, 0.0)
                    self.assertEqual(label[0], "contact", (name, i))
                else:
                    self.assertEqual(label[0], "%.2f mm" % eye_mm, (name, i))

    def test_contact_only_where_the_journal_touches(self):
        for net, name in SIDES:
            run = rm.load_run(RUNS, name)
            got = [i for i, r in enumerate(self.rows[net]) if r[1] == ["contact"]]
            want = [len(run["samples"]) - 1] if run["track_end"] == "touch" else []
            self.assertEqual(got, want, name)

    def test_line_is_the_measured_gap(self):
        # The line runs from the tip to the eye point; its length is the
        # model's distance, which is the journal's within 0.01 mm (D-095).
        for net, name in SIDES:
            run = rm.load_run(RUNS, name)
            for (_, _, strips), (_, eye_mm) in zip(self.rows[net], run["samples"]):
                (a, b), = strips
                self.assertLessEqual(abs(math.dist(a, b) - eye_mm), 0.011, name)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        RRD = os.path.abspath(sys.argv.pop(1))
    unittest.main(verbosity=2)
