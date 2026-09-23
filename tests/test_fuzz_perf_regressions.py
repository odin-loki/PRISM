"""Regressions for the concrete-oracle / fuzzer speedups and the bugs they found.

python -m unittest tests.test_fuzz_perf_regressions

- 64-bit `/` and `%` in the concrete oracle went through a double
  (`int(a / b)`), which is wrong once the quotient exceeds 2**53. The C++
  engine divides exactly (src/prism/stages/interp.cpp), so this was also a
  parity bug. Same for rapid's contract evaluator.
- The memoized `unencoded_syntax_reason` shared by concolic / FuSeBMC /
  fuzzer must return exactly what prism.bmc returns, for every engine name.
- The memoized interpreter helpers must not leak state between runs
  (sizeof reads the live array table; unsigned/64-bit fast paths).
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from prism import bmc
from prism.concrete import execute
from prism.cparse import extract_functions
from prism.fuzz import unencoded_syntax_reason
from prism.models import FunctionInfo
from prism.rapid import _eval_c_expr

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"

_LL3 = [("long long", "a"), ("long long", "b"), ("long long", "c")]


def _fn(body: str, params=None, name: str = "f", file: str = "") -> FunctionInfo:
    params = params if params is not None else [("int", "x")]
    sig = f"int {name}(" + ", ".join(f"{t} {n}" for t, n in params) + ")"
    return FunctionInfo(file=file, name=name, kind="SCALAR", line=1,
                        signature=sig, params=params, body=body)


class TestExact64BitDivision(unittest.TestCase):
    def test_quotient_above_2_pow_53_is_exact(self):
        f = _fn("if (a / b == c) return 1; return 0;", _LL3)
        big = 9007199254740993  # 2**53 + 1: not representable as a double
        self.assertEqual(execute(f, {"a": big, "b": 1, "c": big}).value, 1)
        self.assertEqual(execute(f, {"a": -big, "b": 1, "c": -big}).value, 1)

    def test_remainder_of_int64_max_is_exact(self):
        f = _fn("if (a % b == c) return 1; return 0;", _LL3)
        imax = (1 << 63) - 1
        self.assertEqual(execute(f, {"a": imax, "b": 10, "c": 7}).value, 1)
        self.assertEqual(execute(f, {"a": -imax, "b": 10, "c": -7}).value, 1)

    def test_32bit_division_still_truncates_toward_zero(self):
        f = _fn("return x / 2;")
        self.assertEqual(execute(f, {"x": -7}).value, -3)
        g = _fn("return x % 2;")
        self.assertEqual(execute(g, {"x": -7}).value, -1)
        h = _fn("return x / 0;")
        self.assertEqual(execute(h, {"x": 1}).ub, "INT-DIV-ZERO")

    def test_rapid_contract_division_is_exact(self):
        big = 9007199254740993
        self.assertEqual(_eval_c_expr("a / b", {"a": big, "b": 1}), big)
        self.assertEqual(_eval_c_expr("a / b", {"a": -7, "b": 2}), -3)


class TestSharedSyntaxReason(unittest.TestCase):
    ENGINES = ("concolic engine", "FuSeBMC", "fuzzer", "bitvector BMC")

    def _assert_same(self, f: FunctionInfo) -> None:
        for eng in self.ENGINES:
            # twice: the second call is served from the memo
            for _ in range(2):
                self.assertEqual(
                    unencoded_syntax_reason(f, eng),
                    bmc.unencoded_syntax_reason(f, eng),
                    msg=f"{f.name} / {eng}",
                )

    def test_matches_bmc_on_corpus(self):
        seen = 0
        for p in sorted(TD.glob("*.c"))[::60]:
            for f in extract_functions(p, str(p)):
                self._assert_same(f)
                seen += 1
        self.assertGreater(seen, 10)

    def test_needs_harness_message_names_each_engine(self):
        f = _fn("int x; goto *p;", name="cg")
        self._assert_same(f)
        self.assertIn("FuSeBMC", unencoded_syntax_reason(f, "FuSeBMC") or "")
        self.assertIn("fuzzer", unencoded_syntax_reason(f, "fuzzer") or "")

    def test_memo_sees_file_changes(self):
        # `#pragma pack` is read from the file at fn.file, not the body.
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "pk.c"
            path.write_text("int f(int x) { return x; }\n", encoding="utf-8")
            f = _fn("return x;", file=str(path))
            self.assertIsNone(unencoded_syntax_reason(f, "fuzzer"))
            path.write_text("#pragma pack(1)\nint f(int x) { return x; }\n", encoding="utf-8")
            self.assertEqual(
                unencoded_syntax_reason(f, "fuzzer"),
                bmc.unencoded_syntax_reason(f, "fuzzer"),
            )
            self.assertIsNotNone(unencoded_syntax_reason(f, "fuzzer"))


class TestInterpreterMemoIsStateless(unittest.TestCase):
    def test_sizeof_array_is_not_cached_across_runs(self):
        a = _fn("int a[4]; return sizeof(a);")
        b = _fn("int a[9]; return sizeof(a);")
        self.assertEqual(execute(a, {"x": 0}).value, 16)
        self.assertEqual(execute(b, {"x": 0}).value, 36)
        self.assertEqual(execute(a, {"x": 0}).value, 16)

    def test_same_text_different_signedness(self):
        body = "return x / 2;"
        s = _fn(body)
        u = _fn(body, [("unsigned", "x")])
        self.assertEqual(execute(s, {"x": -4}).value, -2)
        self.assertEqual(execute(u, {"x": -4}).value, 0x7FFFFFFE)
        self.assertEqual(execute(s, {"x": -4}).value, -2)

    def test_same_text_different_width(self):
        body = "return x + x;"
        narrow = _fn(body)
        wide = _fn(body, [("long long", "x")])
        self.assertEqual(execute(narrow, {"x": 0x7FFFFFFF}).ub, "INT-SIGNED-OVF")
        self.assertIsNone(execute(wide, {"x": 0x7FFFFFFF}).ub)
        self.assertEqual(execute(narrow, {"x": 0x7FFFFFFF}).ub, "INT-SIGNED-OVF")

    def test_parse_error_is_raised_every_time(self):
        f = _fn("return (x;")
        first = execute(f, {"x": 1})
        second = execute(f, {"x": 1})
        self.assertIsNotNone(first.error)
        self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main()
