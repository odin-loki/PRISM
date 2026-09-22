"""KLEE adapter honesty: missing binary is NOTRUN; a run is never a proof.

python -m unittest tests.test_klee_adapter
"""

from __future__ import annotations

import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.adapters_extra import _run_klee, run_optional_tools
from prism.config import Config

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"
KLEE_EXE = r"C:\tools\klee"
CLANG_EXE = r"C:\tools\clang.exe"


def _no_adapter(*_a, **_k):
    return None


def _c_file() -> Path:
    p = TD / "klee_fork.c"
    if not p.is_file():
        p = next(TD.glob("*.c"))
    return p


def _clang_writes_bc(cmd, klee_proc, err_name: str | None = None):
    joined = [str(x) for x in cmd]
    if "-emit-llvm" in joined:
        outp = Path(joined[joined.index("-o") + 1])
        outp.write_bytes(b"BC")
        return mock.Mock(returncode=0, stdout="", stderr="")
    if err_name:
        bc = Path(joined[-1])
        dump = bc.parent / err_name
        dump.parent.mkdir(parents=True, exist_ok=True)
        dump.write_text("klee error path\n", encoding="utf-8")
    return klee_proc


class TestKleeAdapter(unittest.TestCase):
    def _never_proved(self, findings: list) -> None:
        statuses = {f.status for f in findings}
        self.assertNotIn(laws.PROVED, statuses)
        self.assertNotIn(laws.PROVED_UNBOUNDED, statuses)
        self.assertNotIn(laws.PROVED_ASSUMING, statuses)
        self.assertNotIn(laws.CLEAN, statuses)
        self.assertFalse(any(laws.is_proof(f.status) for f in findings))

    def test_missing_klee_via_run_optional_tools_is_notrun(self):
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            findings = run_optional_tools([_c_file()], Config())
        klee = [f for f in findings if f.stage == "klee"]
        self.assertTrue(klee)
        self.assertTrue(all(f.status == laws.NOTRUN for f in klee))
        self._never_proved(klee)
        self.assertIn("not found", klee[0].message)

    def test_present_no_c_files_is_unknown(self):
        out = _run_klee(KLEE_EXE, [TD / "README.md"] if (TD / "README.md").is_file() else [], Config())
        self.assertTrue(out)
        self.assertTrue(all(f.stage == "klee" for f in out))
        self.assertTrue(all(f.status == laws.UNKNOWN for f in out))
        self.assertIn("no .c files", out[0].message)
        self._never_proved(out)

    def test_present_no_clang_is_unknown_not_a_verdict(self):
        with mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            out = _run_klee(KLEE_EXE, [_c_file()], Config())
        self.assertTrue(out)
        self.assertEqual(out[0].status, laws.UNKNOWN)
        self.assertIn("no bitcode toolchain (not a verdict)", out[0].message)
        self.assertNotEqual(out[0].status, laws.PROVED)
        self._never_proved(out)

    def test_bitcode_compile_fail_is_not_proved(self):
        fail = mock.Mock(returncode=1, stdout="", stderr="clang: error: no bitcode")

        def fake_run(cmd, timeout, cwd=None):
            return fail

        with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG_EXE), \
             mock.patch("prism.adapters_extra._run", side_effect=fake_run):
            out = _run_klee(KLEE_EXE, [_c_file()], Config())
        self._never_proved(out)
        if out:
            self.assertTrue(all(f.status == laws.UNKNOWN for f in out))
            self.assertIn("not a verdict", out[0].message.lower())
        else:
            self.assertEqual(out, [])

    def test_klee_error_or_err_dump_is_failed_never_proved(self):
        cases = (
            ("text", mock.Mock(returncode=1, stdout="", stderr="KLEE: ERROR: invalid pointer"), None),
            ("err-file", mock.Mock(returncode=0, stdout="KLEE: done\n", stderr=""), "error.err"),
            ("ptr-err", mock.Mock(returncode=0, stdout="KLEE: done\n", stderr=""), "klee-out/test000001.ptr.err"),
            ("nested-ptr-err", mock.Mock(returncode=0, stdout="KLEE: done\n", stderr=""),
             "klee-out/klee-last/test000001.ptr.err"),
        )
        for label, klee_proc, err_name in cases:
            with self.subTest(label=label):
                def fake_run(cmd, timeout, cwd=None, _proc=klee_proc, _err=err_name):
                    return _clang_writes_bc(cmd, _proc, err_name=_err)

                with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG_EXE), \
                     mock.patch("prism.adapters_extra._run", side_effect=fake_run):
                    out = _run_klee(KLEE_EXE, [_c_file()], Config())
                self.assertTrue(out)
                self.assertTrue(all(f.stage == "klee" for f in out))
                self.assertTrue(all(f.status == laws.FAILED for f in out), msg=[f.status for f in out])
                self._never_proved(out)

    def test_klee_ran_no_error_dump_is_unknown_not_a_proof(self):
        klee_proc = mock.Mock(returncode=0, stdout="KLEE: done: generated tests = 2\n", stderr="")

        def fake_run(cmd, timeout, cwd=None):
            return _clang_writes_bc(cmd, klee_proc)

        with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG_EXE), \
             mock.patch("prism.adapters_extra._run", side_effect=fake_run):
            out = _run_klee(KLEE_EXE, [_c_file()], Config())
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.UNKNOWN for f in out))
        self.assertIn("not a proof", out[0].message.lower())
        self.assertNotEqual(out[0].status, laws.PROVED)
        self._never_proved(out)

    def test_ktest_only_dump_is_unknown_not_failed(self):
        """A .ktest path dump is not an error file; rglob *.err is the hit."""
        klee_proc = mock.Mock(returncode=0, stdout="KLEE: done\n", stderr="")

        def fake_run(cmd, timeout, cwd=None):
            joined = [str(x) for x in cmd]
            if "-emit-llvm" in joined:
                outp = Path(joined[joined.index("-o") + 1])
                outp.write_bytes(b"BC")
                return mock.Mock(returncode=0, stdout="", stderr="")
            bc = Path(joined[-1])
            (bc.parent / "test000001.ktest").write_bytes(b"KTEST")
            return klee_proc

        with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG_EXE), \
             mock.patch("prism.adapters_extra._run", side_effect=fake_run):
            out = _run_klee(KLEE_EXE, [_c_file()], Config())
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.UNKNOWN for f in out))
        self.assertIn("not a proof", out[0].message.lower())
        self._never_proved(out)

    def test_doctest_klee_is_notrun_never_unknown(self):
        doctest = mock.Mock(
            returncode=0,
            stdout="[doctest] doctest version is 2.4.11\nUnknown option: --max-time\n",
            stderr="",
        )

        def fake_run(cmd, timeout, cwd=None):
            return _clang_writes_bc(cmd, doctest)

        with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG_EXE), \
             mock.patch("prism.adapters_extra._run", side_effect=fake_run):
            out = _run_klee(KLEE_EXE, [_c_file()], Config())
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.NOTRUN for f in out))
        self.assertIn("not klee", out[0].message.lower())
        self._never_proved(out)


if __name__ == "__main__":
    unittest.main()
