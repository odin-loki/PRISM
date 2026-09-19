"""RapidCheck-style property tests and mutation scoring.

python -m unittest tests.test_rapid -v
"""

from __future__ import annotations

import inspect
import unittest
from pathlib import Path

from helix import laws
from helix.cparse import extract_functions
from helix.muttest import iter_mutations, run_muttest
from helix.rapid import run_rapid

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

    def test_loose_add_clean(self):
        f, _ = fn("loose_add")
        recs = run_rapid([f], trials=32)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.CLEAN)


class TestMuttest(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
