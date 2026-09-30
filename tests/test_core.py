"""Unit tests that do not need the model. python -m unittest tests.test_core"""

from __future__ import annotations

import unittest
from pathlib import Path

from prism import laws
from prism.bmc import HAS_Z3, bmc_function
from prism.cparse import extract_functions
from prism.laws import refuse_merge

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def fn(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, p.name):
            if f.name == name:
                return f, p
    raise AssertionError(name)


class TestLaws(unittest.TestCase):
    def test_refuse_merge_proofs(self):
        with self.assertRaises(ValueError):
            refuse_merge(laws.PROVED, laws.BOUNDED)

    def test_refuse_promote_fuzz(self):
        with self.assertRaises(ValueError):
            refuse_merge(laws.CLEAN, laws.PROVED)


class TestClassify(unittest.TestCase):
    def test_scalar(self):
        f, _ = fn("add_overflow")
        self.assertEqual(f.kind, "SCALAR")

    def test_pointer(self):
        f, _ = fn("null_branch")
        self.assertEqual(f.kind, "POINTER")

    def test_void_params(self):
        # shift_ub takes an unused int — SCALAR
        f, _ = fn("shift_ub")
        self.assertEqual(f.kind, "SCALAR")

    def test_gnu_attribute_is_not_pointer(self):
        f, _ = fn("dead_attr")
        self.assertEqual(f.kind, "SCALAR")
        self.assertEqual([p[1] for p in f.params], ["c"])

    def test_char_literal_survives_comment_strip(self):
        f, _ = fn("trunc_ok")
        self.assertIn("'A'", f.body)

    def test_unchecked_alloc_is_void(self):
        f, _ = fn("unchecked_alloc")
        self.assertEqual(f.kind, "VOID")


# TestLints moved to tests/cpp/test_lint_corpus.cpp (the planted-bug corpus table).


class TestBMC(unittest.TestCase):
    def test_overflow_failed(self):
        f, _ = fn("add_overflow")
        r = bmc_function(f, 8)
        self.assertEqual(r.status, laws.FAILED)
        self.assertEqual(r.cls, "INT-SIGNED-OVF")
        self.assertTrue(r.counterexample)

    def test_div0_failed(self):
        f, _ = fn("div_param")
        r = bmc_function(f, 8)
        self.assertEqual(r.status, laws.FAILED)
        self.assertEqual(r.cls, "INT-DIV-ZERO")

    def test_oob_failed(self):
        f, _ = fn("oob_write")
        r = bmc_function(f, 8)
        self.assertEqual(r.status, laws.FAILED)
        self.assertIn(r.cls, {"MEM-OOB-WRITE", "MEM-OOB-READ"})

    def test_abs_proved(self):
        f, _ = fn("abs_ok")
        r = bmc_function(f, 8)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})

    def test_pointer_not_unguarded(self):
        f, _ = fn("null_branch")
        r = bmc_function(f, 8)
        self.assertEqual(r.status, laws.NEEDS_HARNESS)

    def test_saturate_proved(self):
        f, _ = fn("saturate")
        r = bmc_function(f, 8)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})


if __name__ == "__main__":
    unittest.main()
