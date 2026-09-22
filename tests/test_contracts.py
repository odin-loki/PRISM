"""Dafny-style contracts: decreases well-formedness honesty.

python -m unittest tests.test_contracts -v
"""

from __future__ import annotations

import unittest
from pathlib import Path

from unittest import mock

from prism import laws
from prism.contracts import parse_comments, prove_contracts
from prism.cparse import extract_functions
from prism.models import FunctionInfo

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


_CONTRACT_PLANTS = (
    TD / "decreases_loop.c",
    TD / "decreases_complex.c",
    TD / "wp_ptr.c",
)


def fn(name: str):
    for p in _CONTRACT_PLANTS:
        for f in extract_functions(p, p.name):
            if f.name == name:
                return f, p
    raise AssertionError(name)


class TestDecreasesHonesty(unittest.TestCase):
    def test_simple_identifier_parses(self):
        f, _ = fn("countdown")
        spec = parse_comments(f)
        self.assertEqual(spec["decreases"], ["i"])

    def test_compound_decreases_is_error_not_proved_assuming(self):
        f, _ = fn("countdown_complex")
        spec = parse_comments(f)
        self.assertTrue(spec["decreases"])
        self.assertIn("-", spec["decreases"][0])
        recs = prove_contracts([f], 8)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.PROVED_ASSUMING)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertTrue(r.extra.get("decreases_unencoded"))
        self.assertIn("n - i", r.message)

    def test_star_decreases_is_error(self):
        f, _ = fn("countdown_star")
        spec = parse_comments(f)
        self.assertEqual(spec["decreases"], ["*"])
        recs = prove_contracts([f], 8)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.ERROR)
        self.assertNotEqual(recs[0].status, laws.PROVED_ASSUMING)
        self.assertTrue(recs[0].extra.get("decreases_unencoded"))

    def test_call_decreases_is_error(self):
        f, _ = fn("countdown_abs")
        recs = prove_contracts([f], 8)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.ERROR)
        self.assertNotEqual(recs[0].status, laws.PROVED_ASSUMING)
        self.assertTrue(recs[0].extra.get("decreases_unencoded"))

    def test_identifier_decreases_is_not_unencoded(self):
        f, _ = fn("countdown")
        spec = parse_comments(f)
        recs = prove_contracts([f], 8)
        self.assertEqual(len(recs), 1)
        self.assertFalse(recs[0].extra.get("decreases_unencoded"))
        self.assertNotEqual(recs[0].status, laws.PROVED)
        self.assertNotEqual(recs[0].status, laws.PROVED_UNBOUNDED)

    def test_open_countdown_not_proved_unbounded(self):
        f, _ = fn("countdown_open")
        spec = parse_comments(f)
        self.assertEqual(spec["requires"], [])
        recs = prove_contracts([f], 8)
        self.assertEqual(len(recs), 1)
        self.assertNotEqual(recs[0].status, laws.PROVED_UNBOUNDED)
        self.assertNotEqual(recs[0].status, laws.PROVED)
        self.assertNotEqual(recs[0].extra.get("original_status"), laws.PROVED_UNBOUNDED)

    def test_pointer_contract_is_needs_harness(self):
        f, _ = fn("wp_ptr_get")
        recs = prove_contracts([f], 8)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS)
        self.assertNotEqual(recs[0].status, laws.PROVED)
        self.assertNotEqual(recs[0].status, laws.PROVED_UNBOUNDED)
        self.assertNotEqual(recs[0].status, laws.PROVED_ASSUMING)
        self.assertNotEqual(recs[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(recs[0].status))
        self.assertIn("POINTER", recs[0].message)

    def test_acsl_pointer_contract_is_needs_harness(self):
        f, _ = fn("wp_acsl_ptr")
        recs = prove_contracts([f], 8)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS)
        self.assertNotEqual(recs[0].status, laws.PROVED)
        self.assertFalse(laws.is_proof(recs[0].status))

    def test_void_other_contracts_are_needs_harness(self):
        spec = {
            "requires": [], "ensures": ["result == 0"],
            "invariant": [], "decreases": [], "diff": None,
        }
        void_fn = FunctionInfo(
            file="x.c", name="v", kind="VOID", line=1,
            signature="void v(void)", params=[], body="return;", return_type="void",
        )
        other_fn = FunctionInfo(
            file="x.c", name="o", kind="OTHER", line=1,
            signature="int o(S s)", params=[("S", "s")], body="return 0;",
        )
        with mock.patch("prism.contracts.parse_comments", return_value=spec):
            void_recs = prove_contracts([void_fn], 8)
            other_recs = prove_contracts([other_fn], 8)
        self.assertEqual(void_recs[0].status, laws.NEEDS_HARNESS)
        self.assertEqual(other_recs[0].status, laws.NEEDS_HARNESS)
        self.assertFalse(laws.is_proof(void_recs[0].status))
        self.assertFalse(laws.is_proof(other_recs[0].status))
        self.assertIn("VOID", void_recs[0].message)
        self.assertIn("OTHER", other_recs[0].message)


if __name__ == "__main__":
    unittest.main()
