"""Frama-C EVA adapter: 0 alarms is UNKNOWN, not a proof.

`_run_frama_c` is value analysis (EVA). Helix WP (`helix/wp.py`) is a
separate in-tree stage: a closed check is PROVED-ASSUMING. EVA never
emits PROVED or PROVED-UNBOUNDED. A missing frama-c binary is NOTRUN.
python -m unittest tests.test_framac
"""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path
from unittest import mock

from helix import laws
from helix.adapters_extra import _run_frama_c, run_optional_tools
from helix.config import Config

EXE = r"C:\tools\frama-c.exe"
C_FILE = Path("planted.c")

_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}


def _no_adapter(*_a, **_k):
    return None


def _proc(stdout: str = "", stderr: str = "", rc: int = 0):
    return mock.Mock(returncode=rc, stdout=stdout, stderr=stderr)


class TestFramaCEVA(unittest.TestCase):
    def _never_proof(self, findings):
        self.assertTrue(findings)
        for f in findings:
            self.assertEqual(f.stage, "frama-c", f.stage)
            self.assertNotEqual(f.status, laws.PROVED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_ASSUMING, f.message)
            self.assertNotIn(f.status, _PROOF, f.message)
            self.assertNotEqual(f.status, laws.CLEAN, f.message)
            self.assertFalse(laws.is_proof(f.status), f.status)

    def _eva(self, paths, run_side_effect):
        with mock.patch("helix.adapters_extra._run", side_effect=run_side_effect) as run:
            out = _run_frama_c(EXE, paths, Config())
        return out, run

    def test_no_c_files_is_unknown_never_proved(self):
        with mock.patch("helix.adapters_extra._run") as run:
            out = _run_frama_c(EXE, [Path("unit.cpp"), Path("hdr.h")], Config())
        run.assert_not_called()
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.UNKNOWN)
        self.assertIn("no .c files", out[0].message)
        self._never_proof(out)

        empty, _ = self._eva([], lambda *_a, **_k: _proc())
        self.assertEqual(empty[0].status, laws.UNKNOWN)
        self._never_proof(empty)

    def test_zero_alarms_is_unknown_not_a_proof(self):
        for text in ("0 alarm", "0 alarms"):
            with self.subTest(text=text):
                out, run = self._eva(
                    [C_FILE],
                    lambda *_a, **_k: _proc(stdout=f"[eva] {text} emitted\n"),
                )
                run.assert_called_once()
                cmd = run.call_args[0][0]
                self.assertIn("-eva", cmd)
                self.assertEqual(len(out), 1)
                self.assertEqual(out[0].status, laws.UNKNOWN)
                self.assertIn("not a proof", out[0].message.lower())
                self.assertNotEqual(out[0].status, laws.PROVED_UNBOUNDED)
                self._never_proof(out)

    def test_warning_line_is_failed_func_contract(self):
        out, _ = self._eva(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="warning: signed overflow\n", rc=1),
        )
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self.assertTrue(all(f.cls == "FUNC-CONTRACT" for f in out))
        self.assertIn("warning:", out[0].message.lower())
        self._never_proof(out)

    def test_nonzero_alarm_line_is_failed(self):
        out, _ = self._eva(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="[eva] 1 alarm emitted\n"),
        )
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self._never_proof(out)

        uninit, _ = self._eva(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="[eva] alarm: accessing uninitialized left-value\n"),
        )
        self.assertEqual(uninit[0].status, laws.FAILED)
        self.assertEqual(uninit[0].cls, "UNINIT-READ")
        self._never_proof(uninit)

        # "10 alarms" contains the substring "0 alarm" but is not zero.
        ten, _ = self._eva(
            [C_FILE],
            lambda *_a, **_k: _proc(stdout="[eva] 10 alarms emitted\n"),
        )
        self.assertTrue(ten)
        self.assertTrue(all(f.status == laws.FAILED for f in ten), [f.status for f in ten])
        self.assertNotEqual(ten[0].status, laws.UNKNOWN)
        self._never_proof(ten)

    def test_timeout_is_timeout(self):
        def boom(cmd, timeout):
            raise subprocess.TimeoutExpired(cmd, timeout)

        out, run = self._eva([C_FILE], boom)
        run.assert_called_once()
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.TIMEOUT)
        self.assertIn("timeout", out[0].message.lower())
        self._never_proof(out)

    def test_missing_binary_is_notrun_never_proved(self):
        with mock.patch("helix.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("helix.adapters_extra.shutil.which", return_value=None), \
             mock.patch("helix.adapters_extra._run") as run:
            findings = run_optional_tools([C_FILE], Config())
        run.assert_not_called()
        frama = next(f for f in findings if f.stage == "frama-c")
        self.assertEqual(frama.status, laws.NOTRUN)
        self.assertIn("not found", frama.message)
        self.assertNotEqual(frama.status, laws.PROVED)
        self.assertNotEqual(frama.status, laws.PROVED_UNBOUNDED)
        self.assertNotEqual(frama.status, laws.PROVED_ASSUMING)
        self.assertFalse(laws.is_proof(frama.status))
        self.assertFalse(any(f.stage == "wp" for f in findings))

    def test_doctest_binary_is_notrun_never_unknown(self):
        out, run = self._eva(
            [C_FILE],
            lambda *_a, **_k: _proc(
                stdout="",
                stderr="[doctest] doctest version is 2.4.11\nUnknown option: --timeout\n",
                rc=1,
            ),
        )
        run.assert_called_once()
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertIn("not Frama-C", out[0].message)
        self.assertNotEqual(out[0].status, laws.UNKNOWN)
        self.assertNotEqual(out[0].status, laws.ERROR)
        self._never_proof(out)

    def test_cpp_frama_zero_alarm_is_unknown_never_proved(self):
        text = Path(__file__).resolve().parents[1].joinpath(
            "src", "prism", "adapters.cpp"
        ).read_text(encoding="utf-8")
        start = text.find("std::vector<Finding> run_frama_c(")
        if start < 0:
            self.skipTest("adapters.cpp has no run_frama_c")
        stub = text[start:text.find("std::vector<Finding> run_klee(", start)]
        self.assertIn("laws::UNKNOWN", stub)
        self.assertIn("0 alarms (not a proof)", stub)
        self.assertIn("laws::FAILED", stub)
        self.assertNotIn("laws::CLEAN", stub)
        self.assertNotIn("laws::PROVED", stub)
        self.assertIn('re_search("(?i)', stub)
        self.assertNotIn('low.find("0 alarm") == std::string::npos', stub)


if __name__ == "__main__":
    unittest.main()
