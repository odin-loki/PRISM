"""Infer adapter honesty: missing is NOTRUN; silence is UNKNOWN.

Helix `_run_infer` is STRENGTH_FINDS, never a proof. Present + no `.c`
is UNKNOWN. `error:` lines are FAILED. `No issues found` / empty output
is UNKNOWN, never CLEAN or PROVED.

python -m unittest tests.test_infer
"""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path
from unittest import mock

from helix import laws
from helix.adapters_extra import _run_infer, run_optional_tools
from helix.config import Config

EXE = r"C:\tools\infer"
GCC = r"C:\tools\gcc.exe"
C_FILE = Path("planted.c")
_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}


def _no_adapter(*_a, **_k):
    return None


def _proc(stdout: str = "", stderr: str = "", rc: int = 0):
    return mock.Mock(returncode=rc, stdout=stdout, stderr=stderr)


class TestInferHonesty(unittest.TestCase):
    def _never_proof(self, findings):
        self.assertTrue(findings)
        for f in findings:
            self.assertEqual(f.stage, "infer", f.stage)
            self.assertEqual(f.strength, laws.STRENGTH_FINDS, f.strength)
            self.assertNotEqual(f.status, laws.PROVED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_ASSUMING, f.message)
            self.assertNotIn(f.status, _PROOF, f.message)
            self.assertNotEqual(f.status, laws.CLEAN, f.message)
            self.assertFalse(laws.is_proof(f.status), f.status)

    def _infer(self, paths, run_side_effect, which=GCC):
        with mock.patch("helix.adapters_extra.shutil.which", return_value=which), \
             mock.patch("helix.adapters_extra._run", side_effect=run_side_effect) as run:
            out = _run_infer(EXE, paths, Config())
        return out, run

    def test_missing_infer_via_run_optional_tools_is_notrun(self):
        with mock.patch("helix.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("helix.adapters_extra.shutil.which", return_value=None), \
             mock.patch("helix.adapters_extra._run") as run:
            findings = run_optional_tools([C_FILE], Config())
        run.assert_not_called()
        infer = next(f for f in findings if f.stage == "infer")
        self.assertEqual(infer.status, laws.NOTRUN)
        self.assertIn("not found", infer.message)
        self.assertNotEqual(infer.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(infer.status))
        self._never_proof([infer])

    def test_present_no_c_files_is_unknown(self):
        with mock.patch("helix.adapters_extra._run") as run:
            out = _run_infer(EXE, [Path("unit.cpp"), Path("hdr.h")], Config())
        run.assert_not_called()
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.UNKNOWN)
        self.assertIn("no .c files", out[0].message)
        self._never_proof(out)

    def test_no_compiler_is_notrun_never_clean(self):
        with mock.patch("helix.adapters_extra.shutil.which", return_value=None), \
             mock.patch("helix.adapters_extra._run") as run:
            out = _run_infer(EXE, [C_FILE], Config())
        run.assert_not_called()
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.ERROR)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertIn("gcc/clang not on PATH", out[0].message)
        self.assertEqual((out[0].extra or {}).get("install"), "install gcc or clang")
        self._never_proof(out)

    def test_no_issues_found_is_unknown_not_clean(self):
        out, run = self._infer(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="No issues found\n"),
        )
        run.assert_called_once()
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.UNKNOWN)
        self.assertIn("no issues", out[0].message.lower())
        self.assertIn("not a proof", out[0].message.lower())
        self._never_proof(out)

        empty, _ = self._infer([C_FILE], lambda *_a, **_k: _proc(stdout="", stderr=""))
        self.assertEqual(empty[0].status, laws.UNKNOWN)
        self._never_proof(empty)

    def test_error_line_is_failed(self):
        out, _ = self._infer(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="planted.c:3: error: null dereference\n", rc=1),
        )
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self.assertEqual(out[0].cls, "infer")
        self._never_proof(out)

    def test_doctest_binary_is_notrun(self):
        out, run = self._infer(
            [C_FILE],
            lambda *_a, **_k: _proc(
                stdout="[doctest] doctest version is 2.4.11\nCatch2 v3.0\n",
                rc=0,
            ),
        )
        run.assert_called_once()
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertIn("not infer", out[0].message.lower())
        self.assertNotEqual(out[0].status, laws.UNKNOWN)
        self._never_proof(out)

    def test_timeout_is_timeout(self):
        def boom(cmd, timeout):
            raise subprocess.TimeoutExpired(cmd, timeout)

        out, run = self._infer([C_FILE], boom)
        run.assert_called_once()
        self.assertEqual(out[0].status, laws.TIMEOUT)
        self.assertIn("timeout", out[0].message.lower())
        self._never_proof(out)


if __name__ == "__main__":
    unittest.main()
