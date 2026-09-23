"""Differential testing honesty: missing gcc/clang is NOTRUN; agreement is not a proof.

python -m unittest tests.test_diff -v
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from prism import laws, sandbox
from prism.cparse import extract_functions
from prism.diff import run_diff
from prism.models import FunctionInfo

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"


def _pair():
    funcs = []
    for p in (TD / "diff_a.c", TD / "diff_b.c"):
        funcs.extend(extract_functions(p, p.name))
    return funcs


def _scalar(name: str, *, file: str = "x.c", body: str = "    return x;",
            kind: str = "SCALAR", line: int = 2) -> FunctionInfo:
    return FunctionInfo(
        file=file, name=name, kind=kind, line=line,
        signature=f"int {name}(int x)", params=[("int", "x")],
        return_type="int", body=body, span=(line, line + 3),
    )


class TestDiffHonesty(unittest.TestCase):
    def setUp(self):
        # Law 9: these tests drive the execute path on purpose (--allow-exec).
        self.addCleanup(sandbox.set_allowed, sandbox.set_allowed(True))

    def test_missing_compiler_is_notrun_not_clean(self):
        with mock.patch("prism.diff.shutil.which", return_value=None):
            recs = run_diff(_pair(), TD)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.NOTRUN)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(r.status))
        self.assertIn("gcc", r.message.lower())
        self.assertTrue((r.extra or {}).get("install"))

    def test_cl_alone_is_still_notrun(self):
        """MSVC cl is not gcc/clang; disagreement must not look CLEAN."""
        def fake_which(name):
            return r"C:\cl.exe" if name == "cl" else None

        with mock.patch("prism.diff.shutil.which", side_effect=fake_which):
            recs = run_diff(_pair(), TD)
        self.assertEqual(recs[0].status, laws.NOTRUN)
        self.assertNotEqual(recs[0].status, laws.CLEAN)

    def test_agreement_is_clean_not_a_proof(self):
        with mock.patch("prism.diff.shutil.which", return_value="/usr/bin/gcc"), \
             mock.patch("prism.diff._compile", return_value=(True, "")), \
             mock.patch("prism.diff._run", return_value=("ok", "")):
            recs = run_diff(_pair(), TD)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertNotEqual(r.status, laws.PROVED_ASSUMING)
        self.assertFalse(laws.is_proof(r.status))
        self.assertIn("not a proof", r.message.lower())
        self.assertEqual(r.stage, "diff")

    def test_disagree_is_failed(self):
        with mock.patch("prism.diff.shutil.which", return_value="/usr/bin/gcc"), \
             mock.patch("prism.diff._compile", return_value=(True, "")), \
             mock.patch("prism.diff._run", return_value=("disagree", "DIFF 0 1\n")):
            recs = run_diff(_pair(), TD)
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.FAILED)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertNotEqual(r.status, laws.PROVED)
        self.assertIn("disagree", r.message.lower())

    def test_timeout_is_not_agreement(self):
        with mock.patch("prism.diff.shutil.which", return_value="/usr/bin/gcc"), \
             mock.patch("prism.diff._compile", return_value=(True, "")), \
             mock.patch("prism.diff._run", return_value=("timeout", "")):
            recs = run_diff(_pair(), TD)
        self.assertEqual(recs[0].status, laws.TIMEOUT)
        self.assertNotEqual(recs[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(recs[0].status))

    def test_name_a_b_pairing_disagree_is_failed(self):
        funcs = [_scalar("foo_a"), _scalar("foo_b", body="    return x ^ 1;")]
        with mock.patch("prism.diff.shutil.which", return_value="/usr/bin/gcc"), \
             mock.patch("prism.diff._compile", return_value=(True, "")), \
             mock.patch("prism.diff._run", return_value=("disagree", "DIFF 0 1\n")):
            recs = run_diff(funcs, TD)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.FAILED)
        self.assertNotEqual(recs[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(recs[0].status))
        self.assertIn("foo_a", recs[0].function)
        self.assertIn("foo_b", recs[0].function)

    def test_comment_diff_pairing_disagree_is_failed(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / "pair.c"
            src.write_text(
                "// diff: impl_b\n"
                "int impl_a(int x) { return x; }\n"
                "int impl_b(int x) { return x ^ 1; }\n",
                encoding="utf-8",
            )
            funcs = extract_functions(src, str(src))
            self.assertEqual({f.name for f in funcs}, {"impl_a", "impl_b"})
            with mock.patch("prism.diff.shutil.which", return_value="/usr/bin/gcc"), \
                 mock.patch("prism.diff._compile", return_value=(True, "")), \
                 mock.patch("prism.diff._run", return_value=("disagree", "DIFF 0 1\n")):
                recs = run_diff(funcs, root)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0].status, laws.FAILED)
        self.assertNotEqual(recs[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(recs[0].status))
        self.assertIn("disagree", recs[0].message.lower())

    def test_compile_fail_is_error_not_clean(self):
        with mock.patch("prism.diff.shutil.which", return_value="/usr/bin/gcc"), \
             mock.patch("prism.diff._compile", return_value=(False, "error: boom")):
            recs = run_diff(_pair(), TD)
        self.assertEqual(recs[0].status, laws.ERROR)
        self.assertNotEqual(recs[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(recs[0].status))

    def test_crash_is_not_agreement(self):
        with mock.patch("prism.diff.shutil.which", return_value="/usr/bin/gcc"), \
             mock.patch("prism.diff._compile", return_value=(True, "")), \
             mock.patch("prism.diff._run", return_value=("crash", "signal 11")):
            recs = run_diff(_pair(), TD)
        self.assertEqual(recs[0].status, laws.CRASH)
        self.assertNotEqual(recs[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(recs[0].status))

    def test_pointer_pair_is_needs_harness_never_error(self):
        recs = run_diff(
            [_scalar("foo_a", kind="POINTER"), _scalar("foo_b", kind="POINTER")],
            TD,
        )
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r.status, laws.NEEDS_HARNESS)
        self.assertNotEqual(r.status, laws.ERROR)
        self.assertNotEqual(r.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(r.status))
        self.assertIn("POINTER", r.message)

    def test_other_pair_is_needs_harness_never_error(self):
        recs = run_diff(
            [_scalar("foo_a", kind="OTHER"), _scalar("foo_b", kind="OTHER")],
            TD,
        )
        self.assertEqual(recs[0].status, laws.NEEDS_HARNESS)
        self.assertNotEqual(recs[0].status, laws.ERROR)
        self.assertFalse(laws.is_proof(recs[0].status))
        self.assertIn("OTHER", recs[0].message)


class TestCppDiffSource(unittest.TestCase):
    """C++ diff_pair must match the Python engine: gcc/clang only, timeout is not CLEAN."""

    def test_diff_pair_gcc_clang_only_and_timeout_before_clean(self):
        src = (ROOT / "src" / "prism" / "stages" / "diff.cpp").read_text(encoding="utf-8")
        start = src.find("Finding diff_pair(")
        self.assertGreaterEqual(start, 0)
        body = src[start : start + 5500]
        self.assertIn('which({"gcc", "clang"})', body)
        self.assertIn("no gcc/clang on PATH", body)
        self.assertNotIn('which({"gcc", "clang", "cl"})', body)
        self.assertIn("rr.timeout", body)
        self.assertIn("laws::TIMEOUT", body)
        self.assertIn("not agreement, not a proof", body)
        timeout_at = body.find("laws::TIMEOUT")
        clean_at = body.rfind("laws::CLEAN")
        self.assertGreater(timeout_at, 0)
        self.assertGreater(clean_at, timeout_at)


if __name__ == "__main__":
    unittest.main()
