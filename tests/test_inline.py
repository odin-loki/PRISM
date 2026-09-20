"""Deterministic static scalar inliner."""

from __future__ import annotations

import unittest
from pathlib import Path

from helix import laws
from helix.bmc import HAS_Z3, bmc_function, run_bmc
from helix.cparse import extract_functions
from helix.inline import inline_static
from helix.models import FunctionInfo

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


class TestInlineStatic(unittest.TestCase):
    def test_inline_add_caller(self):
        path = TD / "inline_add.c"
        fns = extract_functions(path, str(path))
        out = inline_static(fns)
        caller = next(f for f in out if f.name == "caller")
        self.assertNotIn("bump(", caller.body)
        self.assertRegex(caller.body.replace(" ", ""), r"x\+1|_i0\+1|_ret")

    def test_pointer_functions_unchanged(self):
        path = TD / "ptr_copy.c"
        fns = extract_functions(path, str(path))
        orig = {f.name: f.body for f in fns}
        out = inline_static(fns)
        for fn in out:
            if fn.kind == "POINTER":
                self.assertEqual(fn.body, orig[fn.name])

    def test_does_not_mutate_input(self):
        path = TD / "inline_add.c"
        fns = extract_functions(path, str(path))
        before = [(f.name, f.body) for f in fns]
        inline_static(fns)
        after = [(f.name, f.body) for f in fns]
        self.assertEqual(before, after)

    def test_skips_non_static_callee(self):
        fns = [
            FunctionInfo(
                file="a.c",
                name="pub",
                kind="SCALAR",
                line=1,
                signature="int pub(int x)",
                params=[("int", "x")],
                body="return x + 1;",
                static=False,
            ),
            FunctionInfo(
                file="a.c",
                name="caller",
                kind="SCALAR",
                line=2,
                signature="int caller(int x)",
                params=[("int", "x")],
                body="return pub(x);",
                static=False,
            ),
        ]
        out = inline_static(fns)
        caller = next(f for f in out if f.name == "caller")
        self.assertIn("pub(", caller.body)

    def test_one_level_no_transitive_inline(self):
        fns = [
            FunctionInfo(
                file="a.c",
                name="inner",
                kind="SCALAR",
                line=1,
                signature="static int inner(int x)",
                params=[("int", "x")],
                body="return x + 1;",
                static=True,
            ),
            FunctionInfo(
                file="a.c",
                name="mid",
                kind="SCALAR",
                line=2,
                signature="static int mid(int x)",
                params=[("int", "x")],
                body="return inner(x);",
                static=True,
            ),
            FunctionInfo(
                file="a.c",
                name="caller",
                kind="SCALAR",
                line=3,
                signature="int caller(int x)",
                params=[("int", "x")],
                body="return mid(x);",
                static=False,
            ),
        ]
        out = inline_static(fns)
        caller = next(f for f in out if f.name == "caller")
        self.assertIn("mid(", caller.body)
        mid = next(f for f in out if f.name == "mid")
        self.assertNotIn("inner(", mid.body)
        self.assertRegex(mid.body.replace(" ", ""), r"x\+1|_i0\+1|_ret")

    def test_assignment_form(self):
        fns = [
            FunctionInfo(
                file="a.c", name="bump", kind="SCALAR", line=1,
                signature="static int bump(int x)", params=[("int", "x")],
                body="return x + 1;", static=True,
            ),
            FunctionInfo(
                file="a.c", name="caller", kind="SCALAR", line=2,
                signature="int caller(int x)", params=[("int", "x")],
                body="int y; y = bump(x); return y;", static=False,
            ),
        ]
        caller = next(f for f in inline_static(fns) if f.name == "caller")
        self.assertNotIn("bump(", caller.body)

    def test_decl_init_form(self):
        fns = [
            FunctionInfo(
                file="a.c", name="bump", kind="SCALAR", line=1,
                signature="static int bump(int x)", params=[("int", "x")],
                body="return x + 1;", static=True,
            ),
            FunctionInfo(
                file="a.c", name="caller", kind="SCALAR", line=2,
                signature="int caller(int x)", params=[("int", "x")],
                body="int y = bump(x); return y;", static=False,
            ),
        ]
        caller = next(f for f in inline_static(fns) if f.name == "caller")
        self.assertNotIn("bump(", caller.body)

    def test_void_helper_form(self):
        fns = [
            FunctionInfo(
                file="a.c", name="bump", kind="VOID", line=1,
                signature="static void bump(int x)", params=[("int", "x")],
                return_type="void", body="(void)x;", static=True,
            ),
            FunctionInfo(
                file="a.c", name="caller", kind="SCALAR", line=2,
                signature="int caller(int x)", params=[("int", "x")],
                body="bump(x); return x;", static=False,
            ),
        ]
        caller = next(f for f in inline_static(fns) if f.name == "caller")
        self.assertNotIn("bump(", caller.body)


@unittest.skipUnless(HAS_Z3, "z3-solver not installed")
class TestInlineBeforeBMC(unittest.TestCase):
    def test_inlined_caller_is_not_an_unconstrained_proof(self):
        path = TD / "inline_add.c"
        fns = extract_functions(path, str(path))
        recs = {f.function: f for f in run_bmc(fns, 8)}
        self.assertEqual(recs["caller"].status, laws.FAILED)
        self.assertEqual(recs["caller"].cls, "INT-SIGNED-OVF")
        raw = next(f for f in fns if f.name == "caller")
        r = bmc_function(raw, 8)
        self.assertIn(r.status, {laws.PROVED, laws.PROVED_UNBOUNDED})


if __name__ == "__main__":
    unittest.main()
