"""Taint stage: conservative intra-function source→sink."""

from __future__ import annotations

import unittest
from pathlib import Path

from helix import laws
from helix.cparse import extract_functions
from helix.taint import run_taint

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


class TestTaint(unittest.TestCase):
    def test_getenv_system_fires(self):
        path = TD / "taint_sink.c"
        fns = extract_functions(path, path.name)
        hits = run_taint(fns)
        bad = [f for f in hits if f.cls == "TAINT-SINK"]
        self.assertTrue(bad)
        self.assertTrue(any(f.function == "run" for f in bad))
        self.assertEqual(bad[0].stage, "taint")
        self.assertEqual(bad[0].status, laws.FAILED)
        self.assertEqual(bad[0].strength, laws.STRENGTH_FINDS)

    def test_literal_system_is_clean(self):
        path = TD / "taint_sink.c"
        fns = extract_functions(path, path.name)
        hits = [f for f in run_taint(fns) if f.function == "ok"]
        self.assertEqual(hits, [])

    def test_no_clean_as_proof(self):
        path = TD / "taint_sink.c"
        fns = extract_functions(path, path.name)
        hits = run_taint(fns)
        self.assertFalse(any(f.status == laws.CLEAN for f in hits))
        self.assertFalse(any(f.status == laws.PROVED for f in hits))

    def test_fread_system_fires(self):
        path = TD / "taint_fread.c"
        fns = extract_functions(path, path.name)
        hits = [f for f in run_taint(fns) if f.function == "run_fread"]
        self.assertTrue(hits)
        self.assertEqual(hits[0].status, laws.FAILED)
        self.assertEqual(hits[0].cls, "TAINT-SINK")

    def test_strcat_getenv_fires(self):
        path = TD / "taint_fread.c"
        fns = extract_functions(path, path.name)
        hits = [f for f in run_taint(fns) if f.function == "run_strcat"]
        self.assertTrue(hits)
        self.assertIn("strcat", hits[0].message)


if __name__ == "__main__":
    unittest.main()
