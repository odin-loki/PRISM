"""Dafny adapter: missing is NOTRUN; verify rc=0 is the binary's PROVED.

Python engine `run_dafny` is law. A missing dafny binary is NOTRUN (never CLEAN,
never PROVED). No `.dfy` in scope is also NOTRUN with that message.

A present Dafny that exits 0 is recorded as PROVED — allowed because the
adapter actually ran — and must never be treated as in-tree BMC
PROVED-UNBOUNDED. Nonzero is FAILED.

Timeout is asserted only if the Python engine maps TimeoutExpired (C++ does; Python engine
currently lets it propagate). C++ `run_dafny` should match the Python engine;
source-contract is optional.

python -m unittest tests.test_dafny
"""

from __future__ import annotations

import inspect
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.adapters import run_dafny
from prism.config import Config, adapter_install
from prism.laws import refuse_merge
from prism.models import Finding

ROOT = Path(__file__).resolve().parents[1]
_CPP = ROOT / "src" / "prism" / "adapters.cpp"
EXE = r"C:\tools\dafny.exe"
_PROOF_UNBOUNDED = laws.PROVED_UNBOUNDED


def _no_adapter(*_a, **_k):
    return None


def _proc(stdout: str = "", stderr: str = "", rc: int = 0):
    return mock.Mock(returncode=rc, stdout=stdout, stderr=stderr)


def _cpp_between(start_token: str, end_token: str) -> str:
    text = _CPP.read_text(encoding="utf-8")
    start = text.find(start_token)
    if start < 0:
        return ""
    rest = text[start:]
    end = rest.find(end_token)
    return rest if end < 0 else rest[:end]


class TestDafnyAdapter(unittest.TestCase):
    def _never_clean_or_proved(self, findings: list[Finding]) -> None:
        self.assertTrue(findings)
        self.assertTrue(all(isinstance(f, Finding) for f in findings))
        for f in findings:
            self.assertEqual(f.stage, "dafny", f.stage)
            self.assertNotEqual(f.status, laws.CLEAN, f.message)
            self.assertNotEqual(f.status, laws.PROVED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_ASSUMING, f.message)
            self.assertFalse(laws.is_proof(f.status), f.status)

    def _dafny(self, paths, run_side_effect, exe=EXE):
        with mock.patch("prism.adapters.resolve_adapter", return_value=exe), \
             mock.patch("prism.adapters.subprocess.run", side_effect=run_side_effect) as run:
            out = run_dafny(paths, Config())
        return out, run

    def test_missing_dafny_is_notrun_never_clean_never_proved(self):
        with mock.patch("prism.adapters.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters.subprocess.run") as run:
            findings = run_dafny([Path("spec.dfy")], Config())
        run.assert_not_called()
        self.assertEqual(len(findings), 1)
        f = findings[0]
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertEqual(f.stage, "dafny")
        self.assertIn("not found", f.message)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED)
        self.assertFalse(laws.is_proof(f.status))
        install = (f.extra or {}).get("install", "")
        self.assertEqual(install, adapter_install("dafny"))
        self._never_clean_or_proved(findings)

    def test_present_no_dfy_files_is_notrun_message(self):
        # Python engine: present binary, zero .dfy in scope → NOTRUN, that message.
        with mock.patch("prism.adapters.resolve_adapter", return_value=EXE), \
             mock.patch("prism.adapters.subprocess.run") as run:
            out = run_dafny([Path("unit.cpp"), Path("hdr.h")], Config())
        run.assert_not_called()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertEqual(f.stage, "dafny")
        self.assertEqual(f.strength, laws.STRENGTH_PROVES)
        self.assertIn("no .dfy files in scope", f.message)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.PROVED)
        self._never_clean_or_proved(out)

        empty, run2 = self._dafny([], lambda *_a, **_k: _proc())
        run2.assert_not_called()
        self.assertEqual(empty[0].status, laws.NOTRUN)
        self.assertIn("no .dfy files in scope", empty[0].message)
        self._never_clean_or_proved(empty)

    def test_verify_rc0_is_proved_not_proved_unbounded(self):
        # Python engine records the Dafny binary's claim: rc=0 → PROVED. Allowed
        # because Dafny ran. Must not be treated as BMC PROVED-UNBOUNDED.
        with tempfile.TemporaryDirectory() as td:
            dfy = Path(td) / "spec.dfy"
            dfy.write_text("method M() ensures true { }\n", encoding="utf-8")
            out, run = self._dafny(
                [dfy],
                lambda *_a, **_k: _proc(stdout="Dafny program verifier finished with 0 errors\n"),
            )
        run.assert_called_once()
        cmd = run.call_args[0][0]
        self.assertEqual(cmd[0], EXE)
        self.assertEqual(cmd[1], "verify")
        self.assertEqual(cmd[2], str(dfy))
        kwargs = run.call_args.kwargs
        self.assertEqual(kwargs.get("timeout"), 60)

        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "dafny")
        self.assertEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED)
        self.assertEqual(f.cls, "FUNC-CONTRACT")
        self.assertEqual(f.strength, laws.STRENGTH_PROVES)
        self.assertEqual(f.file, str(dfy))
        self.assertTrue(laws.is_proof(f.status))
        with self.assertRaises(ValueError) as ctx:
            refuse_merge(f.status, _PROOF_UNBOUNDED)
        self.assertIn("refusing to merge", str(ctx.exception))

    def test_verify_nonzero_rc_is_failed(self):
        with tempfile.TemporaryDirectory() as td:
            dfy = Path(td) / "bad.dfy"
            dfy.write_text("method M() ensures false { }\n", encoding="utf-8")
            out, run = self._dafny(
                [dfy],
                lambda *_a, **_k: _proc(stdout="verification failed\n", rc=1),
            )
        run.assert_called_once()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.FAILED)
        self.assertEqual(f.stage, "dafny")
        self.assertEqual(f.cls, "FUNC-CONTRACT")
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(f.status))

    def test_timeout_if_prism_maps_it(self):
        src = inspect.getsource(run_dafny)
        if "TimeoutExpired" not in src:
            self.skipTest("Python engine run_dafny does not map TimeoutExpired")

        def boom(*_a, **_k):
            raise subprocess.TimeoutExpired(EXE, 60)

        with tempfile.TemporaryDirectory() as td:
            dfy = Path(td) / "slow.dfy"
            dfy.write_text("method M() { }\n", encoding="utf-8")
            out, run = self._dafny([dfy], boom)
        run.assert_called_once()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.TIMEOUT)
        self.assertEqual(f.stage, "dafny")
        self.assertIn("timeout", f.message.lower())
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(f.status))

    def test_doctest_binary_is_notrun_never_proved(self):
        with tempfile.TemporaryDirectory() as td:
            dfy = Path(td) / "spec.dfy"
            dfy.write_text("method M() ensures true { }\n", encoding="utf-8")
            out, run = self._dafny(
                [dfy],
                lambda *_a, **_k: _proc(
                    stdout="[doctest] doctest version is \"2.4.11\"\nCatch2 v3.0\n",
                    rc=0,
                ),
            )
        run.assert_called_once()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertIn("not Dafny", f.message)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(f.status))
        self.assertEqual((f.extra or {}).get("install"), adapter_install("dafny"))
        self._never_clean_or_proved(out)

    def test_cpp_run_dafny_matches_prism_mappings(self):
        stub = _cpp_between(
            "std::vector<Finding> run_dafny(",
            "std::vector<Finding> run_sanitize(",
        )
        if not stub:
            self.skipTest("adapters.cpp has no run_dafny")
        self.assertIn('notrun("dafny"', stub)
        self.assertIn("no .dfy files in scope", stub)
        self.assertIn("laws::PROVED", stub)
        self.assertIn("laws::FAILED", stub)
        self.assertIn("laws::NOTRUN", stub)
        self.assertIn("FUNC-CONTRACT", stub)
        self.assertIn("verify", stub)
        self.assertNotIn("laws::CLEAN", stub)
        self.assertNotIn("PROVED-UNBOUNDED", stub)
        self.assertNotIn("PROVED_UNBOUNDED", stub)


if __name__ == "__main__":
    unittest.main()
