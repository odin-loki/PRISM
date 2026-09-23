"""RapidCheck-style property tests and mutation scoring.

python -m unittest tests.test_rapid -v
"""

from __future__ import annotations

import inspect
import unittest
from pathlib import Path
from unittest import mock

from prism import laws, sandbox
from prism.cparse import extract_functions
from prism.muttest import iter_mutations, run_muttest
from prism.rapid import check_function, run_rapid

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def fn(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, p.name):
            if f.name == name:
                return f, p
    raise AssertionError(name)


class TestRapidSignature(unittest.TestCase):
    def test_run_rapid_signature(self):
        sig = inspect.signature(run_rapid)
        self.assertEqual(list(sig.parameters), ["functions", "trials"])
        self.assertEqual(sig.parameters["trials"].default, 64)

    def test_run_muttest_signature(self):
        sig = inspect.signature(run_muttest)
        self.assertEqual(list(sig.parameters), ["functions", "trials"])
        self.assertEqual(sig.parameters["trials"].default, 32)


class TestRapid(unittest.TestCase):
    def test_inc_clean_not_a_proof(self):
        f, _ = fn("inc")
        recs = run_rapid([f], trials=64)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.PROVED_ASSUMING)
        self.assertIn("not a proof", r.message.lower())
        self.assertEqual(r.stage, "rapid")
        self.assertEqual(r.function, "inc")

    def test_skip_no_ensures(self):
        f, _ = fn("add_overflow")
        recs = run_rapid([f], trials=8)
        self.assertEqual(recs, [])

    def test_not_inc_failed(self):
        f, _ = fn("not_inc")
        recs = run_rapid([f], trials=16)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.FAILED)
        self.assertTrue(r.counterexample)
        self.assertIn("ensures", r.message.lower())

    def test_pointer_skipped(self):
        f, _ = fn("null_branch")
        recs = run_rapid([f], trials=4)
        self.assertEqual(recs, [])

    def test_pointer_with_ensures_is_needs_harness_never_clean(self):
        f, _ = fn("wp_ptr_get")
        recs = run_rapid([f], trials=4)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.NEEDS_HARNESS)
        self.assertEqual(r.stage, "rapid")
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertFalse(laws.is_proof(r.status))
        self.assertIn("POINTER", r.message)

    def test_acsl_pointer_with_ensures_is_needs_harness(self):
        f, _ = fn("wp_acsl_ptr")
        recs = run_rapid([f], trials=4)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.NEEDS_HARNESS)
        self.assertEqual(r.stage, "rapid")
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertFalse(laws.is_proof(r.status))
        self.assertIn("POINTER", r.message)

    def test_muttest_pointer_with_ensures_is_needs_harness(self):
        f, _ = fn("wp_ptr_get")
        recs = run_muttest([f], trials=4)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.NEEDS_HARNESS)
        self.assertEqual(r.stage, "muttest")
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertFalse(laws.is_proof(r.status))
        self.assertIn("POINTER", r.message)

    def test_muttest_acsl_pointer_is_needs_harness(self):
        f, _ = fn("wp_acsl_ptr")
        recs = run_muttest([f], trials=4)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS)
        self.assertEqual(recs[0].stage, "muttest")
        self.assertNotEqual(recs[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(recs[0].status))

    def test_missing_compiler_is_notrun_never_clean(self):
        f, _ = fn("inc")
        missing = {
            "ok": False,
            "error": "no gcc/clang on PATH",
            "counterexample": "",
            "engine": "",
        }
        with mock.patch("prism.rapid.run_plan", return_value=missing):
            rec = check_function(f, trials=4)
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.NOTRUN)
        self.assertNotEqual(rec.status, laws.CLEAN)
        self.assertNotEqual(rec.status, laws.PROVED)
        self.assertFalse(laws.is_proof(rec.status))
        self.assertEqual((rec.extra or {}).get("install"), "install gcc or clang")
        self.assertIn("gcc", rec.message.lower())

    def test_loose_add_clean(self):
        f, _ = fn("loose_add")
        recs = run_rapid([f], trials=32)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.CLEAN)

    def test_shrinks_to_boundary(self):
        f, _ = fn("shrink_ge")
        recs = run_rapid([f], trials=64)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.FAILED)
        self.assertNotEqual(r.status, laws.PROVED)
        compact = r.counterexample.replace(" ", "")
        self.assertIn("x=10", compact)
        self.assertGreaterEqual(int((r.extra or {}).get("shrinks") or 0), 1)


class TestMuttest(unittest.TestCase):
    def setUp(self):
        # Law 9: these tests drive the execute path on purpose (--allow-exec).
        self.addCleanup(sandbox.set_allowed, sandbox.set_allowed(True))

    def test_inc_plus_to_minus_killed(self):
        f, _ = fn("inc")
        sites = iter_mutations(f.body)
        self.assertTrue(any(src == "+" and dst == "-" for _, _, src, dst in sites), sites)
        recs = run_muttest([f], trials=32)
        self.assertTrue(recs)
        plus = [r for r in recs if (r.extra or {}).get("from") == "+"]
        self.assertTrue(plus, recs[0].message)
        r = plus[0]
        self.assertEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.PROVED_ASSUMING)
        self.assertFalse(laws.is_proof(r.status))
        self.assertIn("mutant killed", r.message.lower())
        self.assertIn("not a proof", r.message.lower())
        self.assertEqual(r.stage, "muttest")

    def test_loose_add_mutant_survived(self):
        f, _ = fn("loose_add")
        recs = run_muttest([f], trials=32)
        survived = [r for r in recs if "mutant survived" in r.message.lower()]
        self.assertTrue(survived)
        self.assertEqual(survived[0].status, laws.FAILED)
        self.assertNotEqual(survived[0].status, laws.PROVED)
        self.assertFalse(laws.is_proof(survived[0].status))
        self.assertIn("not a proof", survived[0].message.lower())

    def test_missing_compiler_is_notrun_not_killed(self):
        f, _ = fn("inc")
        missing = {
            "ok": False,
            "error": "no gcc/clang on PATH",
            "counterexample": "",
            "engine": "",
        }
        with mock.patch("prism.muttest.run_plan", return_value=missing):
            recs = run_muttest([f], trials=4)
        self.assertTrue(recs)
        r = recs[0]
        self.assertEqual(r.status, laws.NOTRUN)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.FAILED)
        self.assertIn("gcc", r.message.lower())
        self.assertTrue((r.extra or {}).get("install"))

    def test_killed_mutant_is_clean_not_a_proof(self):
        f, _ = fn("inc")
        ok = {"ok": True, "error": None, "counterexample": "", "engine": "concrete", "n": 4}
        dead = {
            "ok": False,
            "error": None,
            "counterexample": "x=0 -> result=0",
            "engine": "concrete",
        }

        def fake_plan(fn_info, _plan):
            if "-" in fn_info.body and "+" not in fn_info.body.replace("++", ""):
                return dead
            return ok

        with mock.patch("prism.muttest.run_plan", side_effect=fake_plan):
            recs = run_muttest([f], trials=4)
        plus = [r for r in recs if (r.extra or {}).get("from") == "+"]
        self.assertTrue(plus, recs)
        r = plus[0]
        self.assertEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.BOUNDED)
        self.assertNotEqual(r.status, laws.FAILED)
        self.assertFalse(laws.is_proof(r.status))
        self.assertIn("mutant killed", r.message.lower())
        self.assertIn("not a proof", r.message.lower())

    def test_missing_compiler_which_is_notrun(self):
        f, _ = fn("inc")
        missing = {
            "ok": False,
            "error": "cannot evaluate inc: compile failed",
            "counterexample": "",
            "engine": "",
        }
        with mock.patch("prism.muttest.run_plan", return_value=missing), mock.patch(
            "prism.muttest.shutil.which", return_value=None
        ):
            recs = run_muttest([f], trials=4)
        self.assertTrue(recs)
        r = recs[0]
        self.assertEqual(r.status, laws.NOTRUN)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.FAILED)
        self.assertIn("cannot score", r.message.lower())
        self.assertEqual((r.extra or {}).get("install"), "install gcc or clang")

    def test_eval_error_is_not_a_killed_mutant(self):
        f, _ = fn("inc")
        ok = {"ok": True, "error": None, "counterexample": "", "engine": "concrete", "n": 4}
        bad = {"ok": False, "error": "cannot evaluate inc: boom", "counterexample": "", "engine": ""}

        def fake_plan(fn_info, _plan):
            if "-" in fn_info.body and "+" not in fn_info.body.replace("++", ""):
                return bad
            return ok

        with mock.patch("prism.muttest.run_plan", side_effect=fake_plan):
            recs = run_muttest([f], trials=4)
        plus = [r for r in recs if (r.extra or {}).get("from") == "+"]
        self.assertTrue(plus, recs)
        r = plus[0]
        self.assertIn(r.status, {laws.ERROR, laws.NOTRUN})
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotIn("mutant killed", r.message.lower())

    def test_skip_no_ensures(self):
        f, _ = fn("add_overflow")
        recs = run_muttest([f], trials=4)
        self.assertEqual(recs, [])

    def test_silence_is_not_a_proof(self):
        f, _ = fn("inc")
        recs = run_muttest([f], trials=8)
        self.assertTrue(recs)
        blob = " ".join(r.message.lower() for r in recs)
        self.assertTrue("not a proof" in blob or "silence is not a proof" in blob)


class TestCppRapidMissingCompilerSourceContract(unittest.TestCase):
    def test_finding_from_plan_missing_compiler_is_notrun(self):
        src = (Path(__file__).resolve().parents[1] / "src" / "prism" / "stages_rest.cpp").read_text(
            encoding="utf-8"
        )
        i = src.find("Finding finding_from_plan(")
        self.assertNotEqual(i, -1)
        body = src[i : i + 2500]
        self.assertIn("NOTRUN", body)
        self.assertIn("install gcc or clang", body)
        self.assertIn("no gcc", body)
        self.assertNotRegex(body[:800], r"make_find\(stage, laws::ERROR, fn, \"\", plan\.error")


if __name__ == "__main__":
    unittest.main()
