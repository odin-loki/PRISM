"""warnings/compiler adapter: missing is NOTRUN; empty scope is UNKNOWN.

helix.adapters.run_compiler is law. Missing gcc/clang is NOTRUN, never
CLEAN/PROVED. Present with no .c/.cc/.cpp in scope is UNKNOWN (not
silence). gcc and clang both run when both exist; same diagnostic is
not doubled. Silence of -Wall is not CLEAN. Unmatched compiler exit is
FAILED, not dropped. Never pass -w / -Wno-*.

python -m unittest tests.test_compiler
"""

from __future__ import annotations

import shutil
import subprocess
import unittest
from pathlib import Path
from unittest import mock

from helix import laws
from helix.adapters import run_compiler
from helix.config import Config
from helix.models import Finding

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"
HAS_CC = bool(shutil.which("gcc") or shutil.which("clang"))
GCC = r"C:\tools\gcc.exe"
CLANG = r"C:\tools\clang.exe"
C_FILE = Path("planted.c")
_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}


def _which_map(mapping: dict[str, str | None]):
    def fake(name: str):
        return mapping.get(name)
    return fake


def _proc(stdout: str = "", stderr: str = "", rc: int = 0):
    return mock.Mock(returncode=rc, stdout=stdout, stderr=stderr)


class TestCompilerAdapter(unittest.TestCase):
    def _never_proof(self, findings: list[Finding]) -> None:
        self.assertTrue(findings)
        self.assertTrue(all(isinstance(f, Finding) for f in findings))
        for f in findings:
            self.assertEqual(f.stage, "warnings", f.stage)
            self.assertNotEqual(f.status, laws.CLEAN, f.message)
            self.assertNotIn(f.status, _PROOF, f.message)
            self.assertFalse(laws.is_proof(f.status), f.status)

    def _run(self, paths, which, run_side_effect):
        with mock.patch("helix.adapters.shutil.which", side_effect=which), \
             mock.patch("helix.adapters.subprocess.run", side_effect=run_side_effect) as run:
            out = run_compiler(paths, Config())
        return out, run

    def test_missing_compiler_is_notrun_never_clean(self):
        with mock.patch("helix.adapters.shutil.which", return_value=None), \
             mock.patch("helix.adapters.subprocess.run") as run:
            out = run_compiler([C_FILE], Config())
        run.assert_not_called()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "warnings")
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.UNKNOWN)
        self.assertFalse(laws.is_proof(f.status))
        self.assertTrue(
            "PATH" in f.message or "not found" in f.message,
            msg=f.message,
        )
        self.assertEqual((f.extra or {}).get("install"), "install gcc or clang")
        self._never_proof(out)

    def test_present_no_c_files_is_unknown_not_silence(self):
        which = _which_map({"gcc": GCC})
        out, run = self._run(
            [Path("readme.md"), Path("hdr.h"), Path("notes.txt")],
            which,
            lambda *_a, **_k: _proc(),
        )
        run.assert_not_called()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.stage, "warnings")
        self.assertEqual(f.status, laws.UNKNOWN)
        self.assertIn("no .c/.cc/.cpp/.cxx files in scope", f.message.lower())
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertNotEqual(f.status, laws.NOTRUN)
        self._never_proof(out)

        empty, run2 = self._run([], which, lambda *_a, **_k: _proc())
        run2.assert_not_called()
        self.assertEqual(empty[0].status, laws.UNKNOWN)
        self._never_proof(empty)

    def test_parsed_warning_is_failed_never_proved(self):
        stderr = "planted.c:3:5: warning: overflow [-Woverflow]\n"
        out, run = self._run(
            [C_FILE],
            _which_map({"gcc": GCC}),
            lambda *_a, **_k: _proc(stderr=stderr),
        )
        self.assertTrue(run.called)
        cmd = run.call_args[0][0]
        self.assertEqual(cmd[0], GCC)
        self.assertIn("-Wall", cmd)
        self.assertIn("-Wextra", cmd)
        self.assertIn("-fsyntax-only", cmd)
        self.assertNotIn("-w", cmd)
        for flag in cmd:
            self.assertFalse(flag.startswith("-Wno-"), msg=flag)
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.FAILED)
        self.assertEqual(f.cls, "compiler-warning")
        self.assertEqual(f.line, 3)
        self.assertIn("overflow", f.message)
        self.assertEqual(f.strength, laws.STRENGTH_SOME)
        self._never_proof(out)

    def test_parsed_error_is_failed(self):
        stderr = "planted.c:1:1: error: expected ';' before '}' token\n"
        out, _ = self._run(
            [C_FILE],
            _which_map({"gcc": GCC}),
            lambda *_a, **_k: _proc(stderr=stderr, rc=1),
        )
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.FAILED)
        self.assertEqual(out[0].cls, "compiler-error")
        self._never_proof(out)

    def test_silence_is_empty_not_clean_not_proof(self):
        out, run = self._run(
            [C_FILE],
            _which_map({"gcc": GCC}),
            lambda *_a, **_k: _proc(),
        )
        self.assertTrue(run.called)
        self.assertEqual(out, [])
        self.assertNotIn(laws.CLEAN, {f.status for f in out})
        self.assertNotIn(laws.PROVED, {f.status for f in out})
        self.assertFalse(any(laws.is_proof(f.status) for f in out))

    def test_unmatched_nonzero_exit_is_failed_not_dropped(self):
        out, run = self._run(
            [C_FILE],
            _which_map({"gcc": GCC}),
            lambda *_a, **_k: _proc(stderr="fatal: cannot exec cc1", rc=1),
        )
        self.assertTrue(run.called)
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.FAILED)
        self.assertEqual(f.cls, "compiler-error")
        self.assertIn("cannot exec", f.message)
        self._never_proof(out)

    def test_timeout_is_timeout_never_proved(self):
        def boom(cmd, **_k):
            raise subprocess.TimeoutExpired(cmd, 30)

        out, run = self._run([C_FILE], _which_map({"gcc": GCC}), boom)
        run.assert_called_once()
        self.assertEqual(len(out), 1)
        f = out[0]
        self.assertEqual(f.status, laws.TIMEOUT)
        self.assertIn("timeout", f.message.lower())
        self._never_proof(out)

    def test_gcc_and_clang_union_dedupes_same_diagnostic(self):
        stderr = "planted.c:2:1: warning: unused [-Wunused]\n"
        calls: list[list[str]] = []

        def fake_run(cmd, **_k):
            calls.append(list(cmd))
            return _proc(stderr=stderr)

        out, run = self._run(
            [C_FILE],
            _which_map({"gcc": GCC, "clang": CLANG}),
            fake_run,
        )
        self.assertEqual(run.call_count, 2)
        compilers = {c[0] for c in calls}
        self.assertEqual(compilers, {GCC, CLANG})
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.FAILED)
        self.assertEqual(out[0].cls, "compiler-warning")
        self._never_proof(out)

    def test_gcc_and_clang_keep_distinct_diagnostics(self):
        def fake_run(cmd, **_k):
            cc = cmd[0]
            if cc == GCC:
                return _proc(stderr="planted.c:1:1: warning: gcc-only [-Wfoo]\n")
            return _proc(stderr="planted.c:1:1: warning: clang-only [-Wbar]\n")

        out, run = self._run(
            [C_FILE],
            _which_map({"gcc": GCC, "clang": CLANG}),
            fake_run,
        )
        self.assertEqual(run.call_count, 2)
        messages = {f.message for f in out}
        self.assertIn("gcc-only [-Wfoo]", messages)
        self.assertIn("clang-only [-Wbar]", messages)
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self._never_proof(out)

    def test_cpp_units_use_cxx_std_not_c11(self):
        captured: list[list[str]] = []

        def fake_run(cmd, **_k):
            captured.append(list(cmd))
            return _proc()

        self._run(
            [Path("unit.cpp")],
            _which_map({"gcc": GCC}),
            fake_run,
        )
        self.assertTrue(captured)
        self.assertIn("-std=c++11", captured[0])
        self.assertNotIn("-std=c11", captured[0])

        captured.clear()
        self._run(
            [Path("unit.cxx")],
            _which_map({"gcc": GCC}),
            fake_run,
        )
        self.assertTrue(captured)
        self.assertIn("-std=c++11", captured[-1])
        self.assertNotIn("-std=c11", captured[-1])
        self.assertTrue(any(str(a).endswith("unit.cxx") for a in captured[-1]))

    def test_same_resolved_gcc_clang_is_not_double_run(self):
        def fake_run(cmd, **_k):
            return _proc()

        out, run = self._run(
            [C_FILE],
            _which_map({"gcc": GCC, "clang": GCC}),
            fake_run,
        )
        self.assertEqual(run.call_count, 1)
        self.assertEqual(out, [])

    @unittest.skipUnless(HAS_CC, "no C compiler")
    def test_shift_ub_is_failed_not_proved(self):
        out = run_compiler([TD / "shift_ub.c"], Config())
        self.assertTrue(out, msg="expected at least one compiler warning/error")
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self.assertTrue(all(f.stage == "warnings" for f in out))
        self._never_proof(out)

    @unittest.skipUnless(HAS_CC, "no C compiler")
    def test_abs_ok_silence_is_not_clean_or_proved(self):
        out = run_compiler([TD / "abs_ok.c"], Config())
        self.assertNotIn(laws.CLEAN, {f.status for f in out})
        self.assertNotIn(laws.PROVED, {f.status for f in out})
        self.assertFalse(any(laws.is_proof(f.status) for f in out))


if __name__ == "__main__":
    unittest.main()
