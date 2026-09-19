"""Tests for mined FuSeBMC / contracts / Strix / differential methods."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from helix import laws
from helix.bmc import HAS_Z3, bmc_function
from helix.contracts import parse_comments, prove_contracts
from helix.cparse import extract_functions
from helix.diff import run_diff
from helix.fuse import branch_goals, run_fuse, seeds_from_bmc
from helix.ltl import F_BOUND, check_safety, extract_fsm, run_ltl

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"
HAS_CC = bool(shutil.which("gcc") or shutil.which("clang") or shutil.which("cl"))


def fn(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, p.name):
            if f.name == name:
                return f, p
    raise AssertionError(name)


def all_fns():
    out = []
    for p in TD.glob("*.c"):
        out.extend(extract_functions(p, str(p)))
    return out


class TestContracts(unittest.TestCase):
    def test_parse_comments(self):
        f, _ = fn("inc")
        spec = parse_comments(f)
        self.assertTrue(any("x < 100" in r.replace(" ", "") or "x < 100" in r for r in spec["requires"]))
        self.assertTrue(any("result" in e and "x+1" in e.replace(" ", "") for e in spec["ensures"]))

    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_proved_assuming_not_proved(self):
        f, _ = fn("inc")
        recs = prove_contracts([f], 8)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertFalse(r.status == laws.PROVED)
        self.assertIn("requires", r.extra)
        self.assertTrue(r.extra.get("assumed"))
        self.assertEqual(r.stage, "contracts")

    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_inc_unconstrained_overflows(self):
        """Without the requires, x+1 is signed overflow — the assume is load-bearing."""
        f, _ = fn("inc")
        raw = bmc_function(f, 8)
        self.assertEqual(raw.status, laws.FAILED)
        self.assertEqual(raw.cls, "INT-SIGNED-OVF")

    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_countdown_decreases_proved_assuming(self):
        f, _ = fn("countdown")
        spec = parse_comments(f)
        self.assertTrue(spec["decreases"])
        recs = prove_contracts([f], 8)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.PROVED_UNBOUNDED)
        self.assertIn("decreases", r.extra)
        self.assertTrue(r.extra.get("decreases_encoded"))

    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_open_countdown_not_proved_unbounded(self):
        """Unconstrained n must not become PROVED-UNBOUNDED from decreases alone."""
        f, _ = fn("countdown_open")
        spec = parse_comments(f)
        self.assertEqual(spec["requires"], [])
        recs = prove_contracts([f], 8)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertNotEqual(r.status, laws.PROVED_UNBOUNDED)
        self.assertNotEqual(r.extra.get("original_status"), laws.PROVED_UNBOUNDED)
        self.assertFalse((r.extra.get("requires") or "").find("n >= 0") >= 0 and "n < 8" in (r.extra.get("requires") or ""))

    def test_acsl_parse_comments(self):
        f, _ = fn("acsl_abs")
        spec = parse_comments(f)
        self.assertTrue(
            any(
                "x>-2147483647" in r.replace(" ", "") or "x > -2147483647" in r
                for r in spec["requires"]
            )
        )
        self.assertTrue(
            any("result>=0" in e.replace(" ", "") or "result >= 0" in e for e in spec["ensures"])
        )
        for e in spec["ensures"]:
            self.assertNotIn("\\result", e)

    def test_acsl_does_not_leak_to_next_function(self):
        f, _ = fn("acsl_plain")
        spec = parse_comments(f)
        self.assertEqual(spec["requires"], [])
        self.assertEqual(spec["ensures"], [])

    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_acsl_proved_assuming_not_proved(self):
        f, _ = fn("acsl_abs")
        recs = prove_contracts([f], 8)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.PROVED_UNBOUNDED)
        self.assertTrue(r.extra.get("assumed"))
        self.assertIn("requires", r.extra)

    def test_invariant_parse_comments(self):
        f, _ = fn("sum_inv")
        spec = parse_comments(f)
        self.assertTrue(spec["invariant"])
        self.assertTrue(any("s >= 0" in inv.replace(" ", "") or "s>=0" in inv.replace(" ", "") for inv in spec["invariant"]))

    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_invariant_proved_assuming(self):
        f, _ = fn("sum_inv")
        recs = prove_contracts([f], 8)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.PROVED_UNBOUNDED)
        self.assertIn("invariant", r.extra)
        self.assertTrue(r.extra.get("invariant_encoded"))
        self.assertIn("invariant", r.message)


class TestLTL(unittest.TestCase):
    def setUp(self):
        f, _ = fn("fsm_step")
        self.fn = f
        self.fsm = extract_fsm(f.body)
        self.assertIsNotNone(self.fsm)

    def test_g_p_bad_fails(self):
        recs = run_ltl([self.fn], [TD / "protocol.ltl"])
        failed = [r for r in recs if r.status == laws.FAILED]
        self.assertTrue(failed)
        self.assertEqual(failed[0].cls, "LTL-SAFETY")

    def test_g_next_holds(self):
        f = check_safety(
            "G (state == ST_IDLE -> X (state == ST_IDLE || state == ST_WORK))",
            self.fsm,
        )
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.PROVED)

    def test_g_next_fails(self):
        f = check_safety(
            "G (state == ST_IDLE -> X (state == ST_WORK))",
            self.fsm,
        )
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.FAILED)

    def test_g_next_violation_stays_failed(self):
        fsm = {
            "states": ["S_REQ", "S_BAD", "S_OK"],
            "cases": ["S_REQ"],
            "assigns": [],
            "transitions": [("S_REQ", "S_BAD")],
        }
        f = check_safety("G (state == S_REQ -> X (state == S_OK))", fsm)
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.FAILED)
        self.assertNotEqual(f.status, laws.HYPOTHESIS)

    def test_g_next_missing_transition_hypothesis(self):
        fsm = {
            "states": ["WAIT", "DONE"],
            "cases": ["WAIT"],
            "assigns": [],
            "transitions": [],
        }
        formula = "G (state == WAIT -> X (state == DONE))"
        f = check_safety(formula, fsm)
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.HYPOTHESIS)
        self.assertEqual(f.strength, laws.STRENGTH_READS)
        self.assertIn("synthesis", f.extra)
        self.assertEqual(f.extra["synthesis"], [("WAIT", "DONE")])
        self.assertIn("HYPOTHESIS", f.message)

    def test_bounded_f_holds_now(self):
        f = check_safety(
            "G (state == ST_IDLE -> F_8 (state == ST_IDLE))",
            self.fsm,
        )
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.PROVED)
        self.assertEqual(F_BOUND, 8)

    def test_bounded_f_fails_bad_sink(self):
        f = check_safety(
            "G (state == ST_WORK -> F_8 (state == ST_IDLE))",
            self.fsm,
        )
        self.assertIsNotNone(f)
        self.assertEqual(f.status, laws.FAILED)

    def test_unbounded_liveness_notrun_without_strix(self):
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "live.ltl"
            spec.write_text("F (state == ST_IDLE)\n", encoding="utf-8")
            recs = run_ltl([self.fn], [spec])
        self.assertTrue(recs)
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertIn("Strix", recs[0].message)

    def test_until_notrun(self):
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "until.ltl"
            spec.write_text("G (state == ST_WORK U state == ST_IDLE)\n", encoding="utf-8")
            recs = run_ltl([self.fn], [spec])
        self.assertEqual(recs[0].status, laws.NOTRUN)


class TestFuse(unittest.TestCase):
    @unittest.skipUnless(HAS_Z3, "z3-solver not installed")
    def test_seeds_from_bmc_cex(self):
        f, _ = fn("add_overflow")
        r = bmc_function(f, 8)
        self.assertEqual(r.status, laws.FAILED)
        seeds = seeds_from_bmc(f, [r])
        self.assertTrue(seeds)
        self.assertEqual(len(seeds[0]), 4)

    def test_branch_goals(self):
        f, _ = fn("saturate")
        goals = branch_goals(f)
        self.assertGreaterEqual(len(goals), 1)

    @unittest.skipUnless(HAS_CC, "no C compiler")
    def test_clean_is_not_proof(self):
        f, p = fn("saturate")
        recs = run_fuse([f], [], p.parent, budget=0.4, iters=8)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(r.status))
        self.assertNotIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING})
        self.assertIn("not a proof", r.message)


@unittest.skipUnless(HAS_CC, "no C compiler")
class TestDiff(unittest.TestCase):
    def test_disagree_failed(self):
        funcs = []
        for p in (TD / "diff_a.c", TD / "diff_b.c"):
            funcs.extend(extract_functions(p, p.name))
        recs = run_diff(funcs, TD)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.FAILED)
        self.assertTrue(r.counterexample)


if __name__ == "__main__":
    unittest.main()
