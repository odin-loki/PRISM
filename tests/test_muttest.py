"""Mutation scoring honesty: killed is CLEAN (not a proof); survived is FAILED.

python -m unittest tests.test_muttest -q
"""

from __future__ import annotations

import inspect
import unittest
from pathlib import Path
from typing import get_type_hints
from unittest import mock

from helix import laws
from helix.cparse import extract_functions
from helix.models import Finding, FunctionInfo
from helix.muttest import run_muttest
from helix.rapid import plan_trials

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"

_OK = {"ok": True, "error": None, "counterexample": "", "engine": "mock"}
_DEAD = {
    "ok": False,
    "error": None,
    "counterexample": "x=0 -> result=-1",
    "engine": "mock",
}
_COMPILE_ERR = {
    "ok": False,
    "error": "cannot evaluate inc: compile failed",
    "counterexample": "",
    "engine": "",
}


def load(plant: str, name: str) -> FunctionInfo:
    p = TD / plant
    for f in extract_functions(p, p.name):
        if f.name == name:
            return f
    raise AssertionError(f"{plant}:{name}")


def _no_proof(recs: list[Finding]) -> None:
    for r in recs:
        if r.status == laws.PROVED:
            raise AssertionError(f"muttest must never emit PROVED: {r}")
        if laws.is_proof(r.status):
            raise AssertionError(f"muttest must never be a proof: {r.status} {r.message}")


class TestMuttestSignature(unittest.TestCase):
    def test_run_muttest_returns_list_of_finding(self):
        sig = inspect.signature(run_muttest)
        self.assertEqual(list(sig.parameters), ["functions", "trials"])
        self.assertEqual(sig.parameters["trials"].default, 32)
        hints = get_type_hints(run_muttest)
        self.assertEqual(hints["return"], list[Finding])
        out = run_muttest([])
        self.assertIsInstance(out, list)
        self.assertEqual(out, [])
        self.assertTrue(all(isinstance(f, Finding) for f in out))


class TestMuttestHonesty(unittest.TestCase):
    def test_killed_mutant_is_clean_not_a_proof(self):
        f = load("contract_add.c", "inc")
        self.assertIsNotNone(plan_trials(f, 4))

        def fake_plan(fn_info, _plan):
            if "+" in fn_info.body:
                return _OK
            return _DEAD

        with mock.patch("helix.muttest.run_plan", side_effect=fake_plan):
            recs = run_muttest([f], trials=4)
        self.assertTrue(recs)
        self.assertTrue(all(isinstance(r, Finding) for r in recs))
        plus = [r for r in recs if (r.extra or {}).get("from") == "+"]
        self.assertTrue(plus, recs)
        r = plus[0]
        self.assertEqual(r.stage, "muttest")
        self.assertEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.FAILED)
        self.assertFalse(laws.is_proof(r.status))
        self.assertIn("mutant killed", r.message.lower())
        self.assertIn("not a proof", r.message.lower())
        _no_proof(recs)

    def test_survived_mutant_is_failed_not_proved(self):
        f = load("contract_add.c", "loose_add")
        self.assertIsNotNone(plan_trials(f, 8))
        with mock.patch("helix.muttest.run_plan", return_value=_OK):
            recs = run_muttest([f], trials=8)
        self.assertTrue(recs)
        r = recs[0]
        self.assertEqual(r.stage, "muttest")
        self.assertEqual(r.status, laws.FAILED)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(r.status))
        self.assertIn("mutant survived", r.message.lower())
        self.assertIn("not a proof", r.message.lower())
        _no_proof(recs)

    def test_missing_compiler_which_is_notrun_never_clean(self):
        f = load("contract_add.c", "inc")
        with mock.patch("helix.muttest.run_plan", return_value=_COMPILE_ERR), mock.patch(
            "helix.muttest.shutil.which", return_value=None
        ):
            recs = run_muttest([f], trials=4)
        self.assertTrue(recs)
        r = recs[0]
        self.assertEqual(r.status, laws.NOTRUN)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.FAILED)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertFalse(laws.is_proof(r.status))
        self.assertIn("cannot score", r.message.lower())
        self.assertEqual((r.extra or {}).get("install"), "install gcc or clang")
        _no_proof(recs)

    def test_compiler_missing_helper_is_notrun(self):
        f = load("contract_add.c", "inc")
        with mock.patch("helix.muttest.run_plan", return_value=_COMPILE_ERR), mock.patch(
            "helix.muttest._compiler_missing", return_value=True
        ):
            recs = run_muttest([f], trials=4)
        self.assertTrue(recs)
        r = recs[0]
        self.assertEqual(r.status, laws.NOTRUN)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(r.status))
        self.assertTrue((r.extra or {}).get("install"))
        _no_proof(recs)

    def test_no_plan_trials_returns_empty_not_fake_clean(self):
        f = load("contract_add.c", "inc")
        with mock.patch("helix.muttest.plan_trials", return_value=None):
            recs = run_muttest([f], trials=8)
        self.assertEqual(recs, [])
        self.assertIsInstance(recs, list)

    def test_plants_without_ensures_return_empty(self):
        plain = load("acsl_abs.c", "acsl_plain")
        self.assertIsNone(plan_trials(plain, 8))
        recs = run_muttest([plain], trials=8)
        self.assertEqual(recs, [])

        abs_fn = load("acsl_abs.c", "acsl_abs")
        if plan_trials(abs_fn, 8) is None:
            recs_abs = run_muttest([abs_fn], trials=8)
            self.assertEqual(recs_abs, [])
            self.assertNotIn(laws.CLEAN, {r.status for r in recs_abs})


if __name__ == "__main__":
    unittest.main()
