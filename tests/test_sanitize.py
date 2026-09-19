"""Sanitizer adapter: missing compiler/sanitizer = NOTRUN, never CLEAN from a probe.

python -m unittest tests.test_sanitize
"""

from __future__ import annotations

import shutil
import unittest
from pathlib import Path
from unittest import mock

from helix import laws
from helix.config import Config
from helix.models import Finding
from helix.sanitize import run_sanitize

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"
HAS_CC = bool(shutil.which("gcc") or shutil.which("clang"))


class TestSanitize(unittest.TestCase):
    def test_signature_returns_list_of_findings(self):
        with mock.patch("helix.sanitize.shutil.which", return_value=None):
            out = run_sanitize([], Config())
        self.assertIsInstance(out, list)
        self.assertTrue(out)
        self.assertTrue(all(isinstance(f, Finding) for f in out))

    def test_missing_compiler_is_notrun(self):
        with mock.patch("helix.sanitize.shutil.which", return_value=None):
            out = run_sanitize([TD / "abs_ok.c"], Config())
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].stage, "sanitize")
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertIn("not on PATH", out[0].message)
        self.assertTrue((out[0].extra or {}).get("install"))

    def test_unsupported_ubsan_is_notrun(self):
        with mock.patch("helix.sanitize._find_cc", return_value="/bin/gcc"), \
             mock.patch("helix.sanitize._probe_sanitizer", return_value=False):
            out = run_sanitize([TD / "abs_ok.c"], Config())
        ubsan = [f for f in out if (f.extra or {}).get("sanitizer") == "ubsan"]
        tsan = [f for f in out if (f.extra or {}).get("sanitizer") == "tsan"]
        self.assertEqual(len(ubsan), 1)
        self.assertEqual(ubsan[0].status, laws.NOTRUN)
        self.assertIn("no UBSan", ubsan[0].message)
        self.assertEqual(len(tsan), 1)
        self.assertEqual(tsan[0].status, laws.NOTRUN)
        self.assertNotIn(laws.CLEAN, {f.status for f in out})

    def test_supported_ubsan_clean_is_not_a_proof(self):
        with mock.patch("helix.sanitize._find_cc", return_value="/bin/gcc"), \
             mock.patch("helix.sanitize._probe_sanitizer", side_effect=lambda _cc, flags: flags[0] == "-fsanitize=undefined"), \
             mock.patch("helix.sanitize._compile_and_run", return_value=(laws.CLEAN, "ran under sanitizer with exit 0 (not a proof of absence)", "")):
            out = run_sanitize([TD / "abs_ok.c"], Config())
        ubsan = [f for f in out if (f.extra or {}).get("sanitizer") == "ubsan" and f.file]
        self.assertTrue(ubsan)
        self.assertEqual(ubsan[0].status, laws.CLEAN)
        self.assertIn("not a proof", ubsan[0].message)

    def test_sanitizer_abort_is_failed(self):
        with mock.patch("helix.sanitize._find_cc", return_value="/bin/gcc"), \
             mock.patch("helix.sanitize._probe_sanitizer", side_effect=lambda _cc, flags: flags[0] == "-fsanitize=undefined"), \
             mock.patch(
                 "helix.sanitize._compile_and_run",
                 return_value=(laws.FAILED, "UndefinedBehaviorSanitizer: shift exponent", "ubsan trace"),
             ):
            out = run_sanitize([TD / "shift_ub.c"], Config())
        ubsan = [f for f in out if (f.extra or {}).get("sanitizer") == "ubsan" and f.file]
        self.assertEqual(ubsan[0].status, laws.FAILED)

    @unittest.skipUnless(HAS_CC, "no C compiler")
    def test_live_probe_reports_notrun_or_results(self):
        out = run_sanitize([TD / "abs_ok.c"], Config())
        self.assertTrue(out)
        self.assertTrue(all(f.stage == "sanitize" for f in out))
        self.assertNotIn(laws.CLEAN, {f.status for f in out if f.status == laws.NOTRUN})
        for f in out:
            self.assertIn((f.extra or {}).get("sanitizer"), {"ubsan", "tsan"})


if __name__ == "__main__":
    unittest.main()
