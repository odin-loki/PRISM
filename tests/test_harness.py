"""BMC ++/--, uninit-read, and POINTER harness materialization."""

from __future__ import annotations

import unittest
from pathlib import Path

from helix import laws
from helix.bmc import HAS_Z3, bmc_function
from helix.cparse import extract_functions
from helix.harness import materialize, run_harness_bmc
from helix.models import FunctionInfo

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def load(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, str(p)):
            if f.name == name:
                return f, p
    raise AssertionError(name)


class TestMaterialize(unittest.TestCase):
    def test_no_requires_stays_none(self):
        f, _ = load("null_branch")
        self.assertIsNone(materialize(f))

    def test_copy_materializes_scalar(self):
        f, _ = load("copy")
        self.assertEqual(f.kind, "POINTER")
        h = materialize(f)
        self.assertIsNotNone(h)
        self.assertIn(h.kind, {"SCALAR", "VOID"})
        self.assertIn("_h_", h.body)
        self.assertIn("if (!", h.body)

    def test_deref_ok_materializes(self):
        f, _ = load("deref_ok")
        h = materialize(f)
        self.assertIsNotNone(h)
        self.assertIn(h.kind, {"SCALAR", "VOID"})


@unittest.skipUnless(HAS_Z3, "z3-solver not installed")
class TestHarnessBMC(unittest.TestCase):
    def test_plusplus_overflow(self):
        fn = FunctionInfo(
            file="synthetic.c",
            name="inc_ovf",
            kind="SCALAR",
            line=1,
            signature="int inc_ovf(int x)",
            params=[("int", "x")],
            body="x++; return x;",
        )
        r = bmc_function(fn, 8, enums={})
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")

        fn.body = "++x; return x;"
        r = bmc_function(fn, 8, enums={})
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")

    def test_uninit_read_failed(self):
        fn = FunctionInfo(
            file="synthetic.c",
            name="uninit_read",
            kind="VOID",
            line=1,
            signature="int uninit_read(void)",
            params=[],
            body="int x; return x;",
        )
        r = bmc_function(fn, 8, enums={})
        self.assertEqual(r.status, laws.FAILED, r.message)
        self.assertEqual(r.cls, "UNINIT-READ")

    def test_harness_proved_assuming_not_proved(self):
        f, _ = load("copy")
        recs = run_harness_bmc([f], 8)
        self.assertTrue(recs)
        r = recs[0]
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertEqual(r.status, laws.PROVED_ASSUMING, r.message)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertTrue(r.extra.get("assumed"))

    def test_no_requires_harness_empty(self):
        f, _ = load("null_branch")
        self.assertEqual(run_harness_bmc([f], 8), [])


if __name__ == "__main__":
    unittest.main()
