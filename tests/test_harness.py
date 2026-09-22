"""BMC ++/--, uninit-read, and POINTER harness materialization."""

from __future__ import annotations

import unittest
from pathlib import Path

from prism import laws
from prism.bmc import HAS_Z3, bmc_function
from prism.cparse import extract_functions
from prism.harness import materialize, run_harness_bmc
from prism.models import FunctionInfo
from prism.pipeline import STAGE_ORDER

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def load(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, str(p)):
            if f.name == name:
                return f, p
    raise AssertionError(name)


def _scalar(name: str = "inc") -> FunctionInfo:
    return FunctionInfo(
        file="synthetic.c",
        name=name,
        kind="SCALAR",
        line=1,
        signature=f"int {name}(int x)",
        params=[("int", "x")],
        body="return x + 1;",
    )


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

    def test_scalar_does_not_materialize(self):
        self.assertIsNone(materialize(_scalar()))


class TestHarnessHonesty(unittest.TestCase):
    """Honesty that must not depend on Z3: no unguarded BMC, SCALAR skipped."""

    def test_stage_after_bmc(self):
        self.assertIn("harness", STAGE_ORDER)
        self.assertEqual(STAGE_ORDER[STAGE_ORDER.index("bmc") + 1], "harness")

    def test_pointer_without_requires_needs_harness_not_error(self):
        f, _ = load("null_branch")
        recs = run_harness_bmc([f], 8)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.stage, "harness")
        self.assertEqual(r.status, laws.NEEDS_HARNESS, r.message)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CRASH)
        self.assertNotEqual(r.status, laws.FAILED)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.NOTRUN)
        self.assertEqual(r.cls, "")
        self.assertFalse(r.extra.get("assumed"))
        self.assertFalse(r.extra.get("harness"))

    def test_scalar_skipped_empty(self):
        self.assertEqual(run_harness_bmc([_scalar()], 8), [])

    def test_scalar_mixed_with_pointer_without_requires(self):
        f, _ = load("null_branch")
        recs = run_harness_bmc([_scalar(), f], 8)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].function, f.name)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS)


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
        self.assertEqual(r.stage, "harness")
        self.assertNotEqual(r.status, laws.ERROR, r.message)
        self.assertEqual(r.status, laws.PROVED_ASSUMING, r.message)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.PROVED_UNBOUNDED)
        self.assertTrue(r.extra.get("assumed"))
        self.assertTrue(r.extra.get("harness"))
        self.assertIn("never", r.message.lower())

    def test_deref_ok_proved_assuming_not_proved(self):
        f, _ = load("deref_ok")
        recs = run_harness_bmc([f], 8)
        self.assertTrue(recs)
        r = recs[0]
        self.assertEqual(r.stage, "harness")
        self.assertEqual(r.status, laws.PROVED_ASSUMING, r.message)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.PROVED_UNBOUNDED)
        self.assertTrue(r.extra.get("assumed"))


class TestCppHarnessKinductionSource(unittest.TestCase):
    def test_harness_pointer_without_requires_emits_needs_harness(self):
        src = (ROOT / "src" / "prism" / "stages_rest.cpp").read_text(encoding="utf-8")
        start = src.find("std::vector<Finding> run_harness_bmc(")
        self.assertGreaterEqual(start, 0)
        body = src[start : start + 2500]
        self.assertIn("if (!harnessed)", body)
        self.assertIn("laws::NEEDS_HARNESS", body)
        self.assertIn("no honest requires", body)
        self.assertNotIn("if (!harnessed) continue;", body)
        self.assertIn("run_bmc({*harnessed}, unwind, true)", body)

    def test_k_induction_bounded_step_is_not_closed_proof(self):
        src = (ROOT / "src" / "prism" / "bmc.cpp").read_text(encoding="utf-8")
        start = src.find("Finding k_induction(")
        self.assertGreaterEqual(start, 0)
        body = src[start : start + 3500]
        self.assertIn("step.status != laws::PROVED && step.status != laws::PROVED_UNBOUNDED", body)
        self.assertNotIn("step.status != laws::BOUNDED && step.status != laws::PROVED", body)


if __name__ == "__main__":
    unittest.main()
