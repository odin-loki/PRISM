"""clang-tidy: empty diagnostics are UNKNOWN, never CLEAN or PROVED.

python -m unittest tests.test_clang_tidy
"""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.adapters_extra import _run_clang_tidy, run_optional_tools
from prism.config import Config, adapter_install
from prism.models import Finding

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"
ABS_OK = TD / "abs_ok.c"
EXE = r"C:\tools\clang-tidy.exe"


def _no_adapter(*_a, **_k):
    return None


def _assert_no_proof(test: unittest.TestCase, findings: list[Finding]) -> None:
    test.assertTrue(findings)
    test.assertTrue(all(isinstance(f, Finding) for f in findings))
    for f in findings:
        test.assertFalse(laws.is_proof(f.status), msg=f"{f.stage} status={f.status}")
        test.assertNotEqual(f.status, laws.CLEAN)
        test.assertNotEqual(f.status, laws.PROVED)
        test.assertNotEqual(f.status, laws.PROVED_UNBOUNDED)
        test.assertNotEqual(f.status, laws.PROVED_ASSUMING)


class TestClangTidy(unittest.TestCase):
    def test_present_exe_no_tus_is_unknown(self):
        out = _run_clang_tidy(EXE, [], Config())
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "clang-tidy")
        self.assertEqual(f.status, laws.UNKNOWN)
        self.assertIn("no C/C++ translation units", f.message)
        self.assertEqual(f.strength, laws.STRENGTH_FINDS)
        _assert_no_proof(self, out)

        header_only = _run_clang_tidy(EXE, [Path("n.h")], Config())
        self.assertEqual(header_only[0].status, laws.UNKNOWN)
        self.assertIn("no C/C++ translation units", header_only[0].message)
        _assert_no_proof(self, header_only)

    def test_empty_diagnostics_is_unknown_not_clean(self):
        self.assertTrue(ABS_OK.is_file())
        proc = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch("prism.adapters_extra._run", return_value=proc):
            out = _run_clang_tidy(EXE, [ABS_OK], Config())
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "clang-tidy")
        self.assertEqual(f.status, laws.UNKNOWN)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertIn("no diagnostics (not a proof)", f.message)
        self.assertEqual(f.strength, laws.STRENGTH_FINDS)
        _assert_no_proof(self, out)

    def test_warning_line_is_failed(self):
        self.assertTrue(ABS_OK.is_file())
        proc = mock.Mock(
            returncode=0,
            stdout=f"{ABS_OK}:3:5: warning: use after free [clang-analyzer-unix.Malloc]",
            stderr="",
        )
        with mock.patch("prism.adapters_extra._run", return_value=proc):
            out = _run_clang_tidy(EXE, [ABS_OK], Config())
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self.assertTrue(all(f.stage == "clang-tidy" for f in out))
        self.assertIn(": warning:", out[0].message)
        self.assertEqual(out[0].cls, "clang-tidy")
        _assert_no_proof(self, out)

    def test_error_line_is_failed(self):
        self.assertTrue(ABS_OK.is_file())
        proc = mock.Mock(
            returncode=1,
            stdout=f"{ABS_OK}:1:1: error: no matching function [clang-diagnostic-error]",
            stderr="",
        )
        with mock.patch("prism.adapters_extra._run", return_value=proc):
            out = _run_clang_tidy(EXE, [ABS_OK], Config())
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self.assertIn(": error:", out[0].message)
        _assert_no_proof(self, out)

    def test_rc2_without_warning_or_error_is_error(self):
        self.assertTrue(ABS_OK.is_file())
        proc = mock.Mock(
            returncode=2,
            stdout="",
            stderr="clang-tidy: could not find compile_commands.json",
        )
        with mock.patch("prism.adapters_extra._run", return_value=proc):
            out = _run_clang_tidy(EXE, [ABS_OK], Config())
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "clang-tidy")
        self.assertEqual(f.status, laws.ERROR)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.UNKNOWN)
        _assert_no_proof(self, out)

    def test_timeout(self):
        self.assertTrue(ABS_OK.is_file())

        def boom(cmd, timeout):
            raise subprocess.TimeoutExpired(cmd, timeout)

        with mock.patch("prism.adapters_extra._run", side_effect=boom):
            out = _run_clang_tidy(EXE, [ABS_OK], Config())
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "clang-tidy")
        self.assertEqual(f.status, laws.TIMEOUT)
        self.assertEqual(f.file, str(ABS_OK))
        self.assertIn("timeout", f.message.lower())
        _assert_no_proof(self, out)

    def test_cpp_units_use_cxx_std_not_c11(self):
        captured: list[list[str]] = []

        def fake_run(cmd, timeout):
            captured.append(list(cmd))
            return mock.Mock(returncode=0, stdout="", stderr="")

        with mock.patch("prism.adapters_extra._run", side_effect=fake_run):
            _run_clang_tidy(EXE, [Path("unit.cpp"), Path("unit.cc"), Path("unit.cxx")], Config())
        self.assertEqual(len(captured), 3)
        for cmd in captured:
            self.assertIn("-std=c++11", cmd)
            self.assertNotIn("-std=c11", cmd)
            self.assertNotIn("-w", cmd)
            for flag in cmd:
                self.assertFalse(str(flag).startswith("-Wno-"), msg=flag)

        captured.clear()
        with mock.patch("prism.adapters_extra._run", side_effect=fake_run):
            _run_clang_tidy(EXE, [Path("unit.c")], Config())
        self.assertTrue(captured)
        self.assertIn("-std=c11", captured[0])
        self.assertNotIn("-std=c++11", captured[0])

    def test_missing_binary_is_notrun(self):
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([ABS_OK], Config())
        tidy = next(f for f in findings if f.stage == "clang-tidy")
        self.assertEqual(tidy.status, laws.NOTRUN)
        self.assertNotEqual(tidy.status, laws.CLEAN)
        self.assertNotEqual(tidy.status, laws.PROVED)
        self.assertFalse(laws.is_proof(tidy.status))
        self.assertIn("not found", tidy.message)
        install = (tidy.extra or {}).get("install", "")
        self.assertEqual(install, adapter_install("clang-tidy"))
        self.assertIn("system tool", install)
        self.assertIn("third_party/MANIFEST.toml", install)
        _assert_no_proof(self, [tidy])

    def test_doctest_binary_is_notrun_even_on_rc0(self):
        self.assertTrue(ABS_OK.is_file())
        proc = mock.Mock(
            returncode=0,
            stdout="[doctest] doctest version is \"2.4.11\"\nUnknown option: --timeout\n",
            stderr="",
        )
        with mock.patch("prism.adapters_extra._run", return_value=proc):
            out = _run_clang_tidy(EXE, [ABS_OK], Config())
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertIn("not clang-tidy", out[0].message.lower())
        self.assertNotEqual(out[0].status, laws.UNKNOWN)
        self.assertEqual((out[0].extra or {}).get("install"), adapter_install("clang-tidy"))
        _assert_no_proof(self, out)

    def test_cpp_clang_tidy_cxx_units_use_cxx_std(self):
        text = ROOT.joinpath("src", "prism", "adapters.cpp").read_text(encoding="utf-8")
        start = text.find("std::vector<Finding> run_clang_tidy(")
        if start < 0:
            self.skipTest("adapters.cpp has no run_clang_tidy")
        stub = text[start:text.find("std::vector<Finding> run_cbmc(", start)]
        self.assertIn("-std=c++11", stub)
        self.assertIn(".cxx", stub)
        self.assertIn(".cc", stub)
        self.assertIn(".cpp", stub)
        self.assertIn("-std=c11", stub)
        self.assertIn("laws::UNKNOWN", stub)
        self.assertIn("no diagnostics (not a proof)", stub)
        self.assertNotIn("laws::CLEAN", stub)
        self.assertNotIn("laws::PROVED", stub)


if __name__ == "__main__":
    unittest.main()
