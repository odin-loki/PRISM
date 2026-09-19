"""Thread stage: shared global writes without mutex."""

from __future__ import annotations

import unittest
from pathlib import Path

from helix import laws
from helix.cparse import extract_functions
from helix.thread import run_thread

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


class TestThread(unittest.TestCase):
    def test_race_global_fires(self):
        path = TD / "race_global.c"
        fns = extract_functions(path, str(path))
        self.assertGreaterEqual(len(fns), 3)
        hits = run_thread(fns)
        bad = [f for f in hits if f.cls == "RACE-SHARED"]
        self.assertTrue(bad)
        self.assertEqual(bad[0].stage, "thread")
        self.assertEqual(bad[0].status, laws.FAILED)
        writers = bad[0].extra.get("writers", [])
        self.assertIn("t1", writers)
        self.assertIn("t2", writers)

    def test_relative_file_path(self):
        path = TD / "race_global.c"
        fns = extract_functions(path, "race_global.c")
        hits = run_thread(fns)
        self.assertTrue([f for f in hits if f.cls == "RACE-SHARED"])
        path = TD / "taint_sink.c"
        fns = extract_functions(path, str(path))
        self.assertEqual(run_thread(fns), [])

    def test_no_proved_or_clean(self):
        path = TD / "race_global.c"
        fns = extract_functions(path, str(path))
        hits = run_thread(fns)
        self.assertFalse(any(f.status == laws.CLEAN for f in hits))
        self.assertFalse(any(f.status == laws.PROVED for f in hits))


if __name__ == "__main__":
    unittest.main()
