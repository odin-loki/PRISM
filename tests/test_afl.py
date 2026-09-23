"""prism.afl honesty: missing AFL is None (not CLEAN); CLEAN is not a proof.

Python engine is law. tests.test_afl_flag covers PRISM_AFL / fuse opt-in — do not
duplicate those cases here.

python -m unittest tests.test_afl tests.test_afl_flag -q
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from prism import laws, sandbox
from prism.afl import afl_available, run_afl_fuzz
from prism.models import FunctionInfo

_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}
AFL = r"C:\tools\afl-fuzz.exe"
GCC = r"C:\tools\gcc.exe"


def _scalar(name: str = "inc") -> FunctionInfo:
    return FunctionInfo(
        file="planted.c",
        name=name,
        kind="SCALAR",
        line=1,
        signature=f"int {name}(int x)",
        params=[("int", "x")],
        body="return x + 1;",
    )


def _pointer(name: str = "copy") -> FunctionInfo:
    return FunctionInfo(
        file="planted.c",
        name=name,
        kind="POINTER",
        line=1,
        signature=f"void {name}(int *p)",
        params=[("int *", "p")],
        body="*p = 1;",
    )


def _which_afl_only(name: str):
    if name in ("afl-fuzz", "afl-fuzz.exe"):
        return AFL
    return None


def _which_afl_and_gcc(name: str):
    if name in ("afl-fuzz", "afl-fuzz.exe"):
        return AFL
    if name in ("gcc", "clang"):
        return GCC
    return None


def _never_proof(status: str) -> None:
    assert status not in _PROOF
    assert not laws.is_proof(status)


class TestAflAvailable(unittest.TestCase):
    def test_afl_available_none_when_which_none(self):
        with mock.patch("prism.afl.shutil.which", return_value=None) as which:
            self.assertIsNone(afl_available())
        self.assertTrue(which.called)
        names = [c.args[0] for c in which.call_args_list]
        self.assertIn("afl-fuzz", names)
        self.assertIn("afl-fuzz.exe", names)


class TestRunAflFuzzHonesty(unittest.TestCase):
    def setUp(self):
        # Law 9: these tests drive the execute path on purpose (--allow-exec).
        self.addCleanup(sandbox.set_allowed, sandbox.set_allowed(True))

    def test_missing_afl_returns_none_not_clean(self):
        src = Path("planted.c")
        with mock.patch("prism.afl.shutil.which", return_value=None), \
             mock.patch("prism.afl._compile") as compile_, \
             mock.patch("prism.afl.subprocess.run") as run:
            rec = run_afl_fuzz(_scalar(), src)
        self.assertIsNone(rec)
        self.assertNotEqual(rec, laws.CLEAN)
        self.assertIsNot(rec, laws.CLEAN)
        compile_.assert_not_called()
        run.assert_not_called()

    def test_pointer_returns_none_not_clean(self):
        src = Path("planted.c")
        with mock.patch("prism.afl.shutil.which", side_effect=_which_afl_and_gcc), \
             mock.patch("prism.afl._compile") as compile_, \
             mock.patch("prism.afl.subprocess.run") as run:
            rec = run_afl_fuzz(_pointer(), src)
        self.assertIsNone(rec)
        self.assertNotEqual(rec, laws.CLEAN)
        compile_.assert_not_called()
        run.assert_not_called()

    def test_missing_compiler_with_afl_present_is_notrun_never_clean(self):
        src = Path("planted.c")
        with mock.patch("prism.afl.shutil.which", side_effect=_which_afl_only), \
             mock.patch("prism.afl._compile") as compile_, \
             mock.patch("prism.afl.subprocess.run") as run:
            rec = run_afl_fuzz(_scalar(), src)
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.NOTRUN)
        self.assertNotEqual(rec.status, laws.ERROR)
        self.assertNotEqual(rec.status, laws.PROVED)
        self.assertNotEqual(rec.status, laws.CLEAN)
        self.assertNotIn(rec.status, _PROOF)
        self.assertFalse(laws.is_proof(rec.status))
        self.assertNotEqual((rec.extra or {}).get("engine"), "afl")
        self.assertNotIn("engine", rec.extra or {})
        self.assertEqual((rec.extra or {}).get("install"), "install gcc or clang")
        self.assertIn("compiler", rec.message.lower())
        compile_.assert_not_called()
        run.assert_not_called()

    def test_mocked_crash_is_crash_engine_afl_not_proof(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / "planted.c"
            src.write_text("int inc(int x) { return x + 1; }\n", encoding="utf-8")
            work = root / "aflwork"

            def plant_crash(*_a, **_k):
                crash_dir = work / "out" / "crashes"
                crash_dir.mkdir(parents=True, exist_ok=True)
                (crash_dir / "id:000000").write_bytes(b"\x01\x02\x03\x04")
                return mock.Mock(returncode=0, stdout=b"", stderr=b"")

            with mock.patch("prism.afl.shutil.which", side_effect=_which_afl_and_gcc), \
                 mock.patch("prism.afl._compile", return_value=(True, "")), \
                 mock.patch("prism.afl.subprocess.run", side_effect=plant_crash):
                rec = run_afl_fuzz(_scalar(), src, timeout=1.0, work=work)
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.CRASH)
        self.assertEqual((rec.extra or {}).get("engine"), "afl")
        self.assertNotEqual(rec.status, laws.PROVED)
        self.assertNotEqual(rec.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(rec.status))
        _never_proof(rec.status)

    def test_mocked_successful_run_is_clean_not_a_proof(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / "planted.c"
            src.write_text("int inc(int x) { return x + 1; }\n", encoding="utf-8")
            work = root / "aflwork"
            with mock.patch("prism.afl.shutil.which", side_effect=_which_afl_and_gcc), \
                 mock.patch("prism.afl._compile", return_value=(True, "")), \
                 mock.patch("prism.afl.subprocess.run", return_value=mock.Mock(
                     returncode=0, stdout=b"", stderr=b"",
                 )):
                rec = run_afl_fuzz(_scalar(), src, timeout=1.0, work=work)
        self.assertIsNotNone(rec)
        self.assertEqual(rec.status, laws.CLEAN)
        self.assertEqual((rec.extra or {}).get("engine"), "afl")
        self.assertNotEqual(rec.status, laws.PROVED)
        self.assertNotIn(rec.status, _PROOF)
        self.assertFalse(laws.is_proof(rec.status))
        self.assertIn("not a proof", rec.message.lower())
        _never_proof(rec.status)


class TestAflCompileSanitizerComments(unittest.TestCase):
    def test_compile_afl_harness_comments_document_sanitizer_fallback(self):
        from prism.afl import _compile_afl_harness
        import inspect

        src = inspect.getsource(_compile_afl_harness)
        self.assertIn("-fsanitize=address,undefined", src)
        self.assertIn("-fsanitize=undefined", src)
        self.assertIn("-fsanitize=address", src)
        self.assertIn("-fno-sanitize-recover=address,undefined", src)
        self.assertIn("-fno-sanitize-recover=undefined", src)
        self.assertIn("-fno-sanitize-recover=address", src)
        self.assertIn("bare", src.lower())
        self.assertIn("TSan", src)
        self.assertIn("not a fake", src.lower())

    def test_compile_missing_compiler_message_is_notrun_not_engine_afl(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            src = root / "planted.c"
            src.write_text("int inc(int x) { return x + 1; }\n", encoding="utf-8")
            work = root / "aflwork"
            with mock.patch("prism.afl.shutil.which", side_effect=_which_afl_and_gcc), \
                 mock.patch("prism.afl._compile", return_value=(False, "no C compiler on PATH")), \
                 mock.patch("prism.afl.subprocess.run") as run:
                rec = run_afl_fuzz(_scalar(), src, timeout=1.0, work=work)
        self.assertEqual(rec.status, laws.NOTRUN)
        self.assertNotEqual(rec.status, laws.ERROR)
        self.assertNotEqual(rec.status, laws.CLEAN)
        self.assertNotEqual((rec.extra or {}).get("engine"), "afl")
        self.assertNotIn("engine", rec.extra or {})
        run.assert_not_called()
        _never_proof(rec.status)


if __name__ == "__main__":
    unittest.main()
