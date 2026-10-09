"""Python cparse classification on testdata. python -m unittest tests.test_core"""

from __future__ import annotations

import unittest
from pathlib import Path

from prism.cparse import extract_functions

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def fn(name: str):
    for p in TD.glob("*.c"):
        for f in extract_functions(p, p.name):
            if f.name == name:
                return f, p
    raise AssertionError(name)


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


if __name__ == "__main__":
    unittest.main()
