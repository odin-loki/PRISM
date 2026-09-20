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
        self.assertEqual(bad[0].strength, laws.STRENGTH_FINDS)
        self.assertFalse(laws.is_proof(bad[0].status))
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

    def test_iso_thrd_race(self):
        path = TD / "iso_thread_race.c"
        fns = extract_functions(path, "iso_thread_race.c")
        hits = run_thread(fns)
        bad = [f for f in hits if f.cls == "RACE-SHARED"]
        self.assertTrue(bad)
        self.assertEqual(bad[0].strength, laws.STRENGTH_FINDS)
        self.assertNotEqual(bad[0].strength, laws.STRENGTH_PROVES)
        self.assertFalse(laws.is_proof(bad[0].status))
        writers = bad[0].extra.get("writers", [])
        self.assertIn("iso_thrd_t1", writers)
        self.assertIn("iso_thrd_t2", writers)
        self.assertGreaterEqual(len(writers), 2)

    def test_single_unsync_writer_is_not_a_race(self):
        path = TD / "iso_thread_one_writer.c"
        fns = extract_functions(path, "iso_thread_one_writer.c")
        hits = run_thread(fns)
        self.assertFalse([f for f in hits if f.cls == "RACE-SHARED"])
        path = TD / "iso_thread_race.c"
        one = [fn for fn in extract_functions(path, "iso_thread_race.c")
               if fn.name != "iso_thrd_t2"]
        self.assertTrue(any(fn.name == "iso_thrd_t1" for fn in one))
        self.assertFalse([f for f in run_thread(one) if f.cls == "RACE-SHARED"])

    def test_iso_mtx_lock_writers_are_not_a_race(self):
        path = TD / "iso_thread_one_writer.c"
        fns = extract_functions(path, "iso_thread_one_writer.c")
        names = {fn.name for fn in fns}
        self.assertIn("iso_thrd_locked_a", names)
        self.assertIn("iso_thrd_locked_b", names)
        self.assertFalse([f for f in run_thread(fns) if f.cls == "RACE-SHARED"])

    def test_iso_mtx_timedlock_writers_are_not_a_race(self):
        path = TD / "iso_thread_timedlock.c"
        fns = extract_functions(path, "iso_thread_timedlock.c")
        names = {fn.name for fn in fns}
        self.assertIn("iso_timed_t1", names)
        self.assertIn("iso_timed_t2", names)
        self.assertTrue(any("thrd_create" in fn.body for fn in fns))
        hits = [f for f in run_thread(fns) if f.cls == "RACE-SHARED"]
        self.assertFalse(hits)

    def test_jthread_unsync_writers_are_race_shared(self):
        path = TD / "jthread_race.cpp"
        fns = extract_functions(path, "jthread_race.cpp")
        hits = [f for f in run_thread(fns) if f.cls == "RACE-SHARED"]
        self.assertTrue(hits)
        self.assertEqual(hits[0].strength, laws.STRENGTH_FINDS)
        self.assertFalse(laws.is_proof(hits[0].status))
        writers = hits[0].extra.get("writers", [])
        self.assertIn("jrace_t1", writers)
        self.assertIn("jrace_t2", writers)
        lint = TD / "jthread_lint.cpp"
        silent = run_thread(extract_functions(lint, "jthread_lint.cpp"))
        self.assertFalse([f for f in silent if f.cls == "RACE-SHARED"])

    def test_freebsd_thr_is_not_iso_thrd_race(self):
        for stem in ("thr_api.c", "thr_unenc.c", "thrd_unenc.c", "thrd_join_api.c"):
            path = TD / stem
            hits = run_thread(extract_functions(path, stem))
            self.assertFalse([f for f in hits if f.cls == "RACE-SHARED"], msg=stem)

    def test_iso_thrd_one_line_not_missing_return(self):
        from helix.checkers import run_lints
        path = TD / "iso_thread_race.c"
        hits = [f for f in run_lints([path], TD) if f.cls == "CTRL-MISSING-RETURN"]
        names = {f.function for f in hits}
        self.assertNotIn("iso_thrd_t1", names)
        self.assertNotIn("iso_thrd_t2", names)


if __name__ == "__main__":
    unittest.main()
