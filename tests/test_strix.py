"""Strix optional adapter: missing is NOTRUN; a run is never a proof.

Helix `_run_strix` is law. No `.ltl`/`.tlsf` in the source roots is a
help/version probe (UNKNOWN, not a code verdict). A spec that prints
counterexample/violation/falsified is FAILED. Any other outcome is
UNKNOWN, never CLEAN or PROVED.

python -m unittest tests.test_strix
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from helix import laws
from helix.adapters_extra import _help_ok_finding, _run_strix, run_optional_tools
from helix.config import Config

EXE = r"C:\tools\strix.exe"
_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}


def _no_adapter(*_a, **_k):
    return None


def _proc(stdout: str = "", stderr: str = "", rc: int = 0):
    return mock.Mock(returncode=rc, stdout=stdout, stderr=stderr)


class TestStrixAdapter(unittest.TestCase):
    def _never_proof(self, findings):
        self.assertTrue(findings)
        for f in findings:
            self.assertEqual(f.stage, "strix", f.stage)
            self.assertNotEqual(f.status, laws.PROVED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_ASSUMING, f.message)
            self.assertNotIn(f.status, _PROOF, f.message)
            self.assertNotEqual(f.status, laws.CLEAN, f.message)
            self.assertFalse(laws.is_proof(f.status), f.status)

    def test_missing_strix_via_run_optional_tools_is_notrun(self):
        with mock.patch("helix.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("helix.adapters_extra.shutil.which", return_value=None), \
             mock.patch("helix.adapters_extra._run") as run:
            findings = run_optional_tools([], Config())
        run.assert_not_called()
        strix = next(f for f in findings if f.stage == "strix")
        self.assertEqual(strix.status, laws.NOTRUN)
        self.assertIn("not found", strix.message)
        self.assertNotEqual(strix.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(strix.status))
        self._never_proof([strix])

    def test_present_no_specs_is_help_probe_unknown(self):
        probed = _proc(stdout="strix --help")
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "unit.c"
            src.write_text("int main(void) { return 0; }\n", encoding="utf-8")
            out = _run_strix(EXE, [src], Config(), probed)
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.UNKNOWN)
        self.assertIn("not a code verdict", f.message.lower())
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.PROVED)
        self._never_proof(out)

        help_f = _help_ok_finding("strix", EXE, probed)
        self.assertEqual(help_f.status, laws.UNKNOWN)
        self.assertNotEqual(help_f.status, laws.CLEAN)

    def test_counterexample_is_failed_never_proved(self):
        probed = _proc(stdout="strix --help")
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "bad.ltl"
            spec.write_text("G p\n", encoding="utf-8")
            with mock.patch(
                "helix.adapters_extra._run",
                return_value=_proc(stdout="counterexample found\n", rc=1),
            ) as run:
                out = _run_strix(EXE, [spec], Config(), probed)
        run.assert_called_once()
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self.assertEqual(out[0].cls, "strix")
        self._never_proof(out)

    def test_realizable_run_is_unknown_not_a_proof(self):
        probed = _proc(stdout="strix --help")
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "ok.tlsf"
            spec.write_text("INFO {\n  TITLE: \"x\"\n}\n", encoding="utf-8")
            with mock.patch(
                "helix.adapters_extra._run",
                return_value=_proc(stdout="REALIZABLE\n"),
            ):
                out = _run_strix(EXE, [spec], Config(), probed)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.UNKNOWN)
        self.assertIn("not a proof", out[0].message.lower())
        self._never_proof(out)

    def test_doctest_binary_is_notrun(self):
        probed = _proc(stdout="strix --help")
        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "bad.ltl"
            spec.write_text("G p\n", encoding="utf-8")
            with mock.patch(
                "helix.adapters_extra._run",
                return_value=_proc(stdout="[doctest] doctest version is 2.4.11\n", rc=0),
            ):
                out = _run_strix(EXE, [spec], Config(), probed)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertIn("not strix", out[0].message.lower())
        self.assertNotEqual(out[0].status, laws.UNKNOWN)
        self._never_proof(out)

    def test_timeout_is_timeout(self):
        probed = _proc(stdout="strix --help")

        def boom(cmd, timeout):
            raise subprocess.TimeoutExpired(cmd, timeout)

        with tempfile.TemporaryDirectory() as td:
            spec = Path(td) / "slow.ltl"
            spec.write_text("G p\n", encoding="utf-8")
            with mock.patch("helix.adapters_extra._run", side_effect=boom) as run:
                out = _run_strix(EXE, [spec], Config(), probed)
        run.assert_called_once()
        self.assertEqual(out[0].status, laws.TIMEOUT)
        self.assertIn("timeout", out[0].message.lower())
        self._never_proof(out)


if __name__ == "__main__":
    unittest.main()
