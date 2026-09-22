"""Sanitizer adapter: missing compiler/sanitizer = NOTRUN, never CLEAN from a probe.

python -m unittest tests.test_sanitize
"""

from __future__ import annotations

import shutil
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.config import Config
from prism.models import Finding
from prism.sanitize import (
    _AS_FLAGS,
    _UB_FLAGS,
    _has_sanitizer_lib,
    _is_mingw,
    _probe_sanitizer,
    _sanitizer_hit,
    _sanitizer_runtime_unusable,
    run_sanitize,
)

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"
HAS_CC = bool(shutil.which("gcc") or shutil.which("clang"))


class TestSanitize(unittest.TestCase):
    def test_signature_returns_list_of_findings(self):
        with mock.patch("prism.sanitize.shutil.which", return_value=None):
            out = run_sanitize([], Config())
        self.assertIsInstance(out, list)
        self.assertTrue(out)
        self.assertTrue(all(isinstance(f, Finding) for f in out))

    def test_missing_compiler_is_notrun(self):
        with mock.patch("prism.sanitize.shutil.which", return_value=None):
            out = run_sanitize([TD / "abs_ok.c"], Config())
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].stage, "sanitize")
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertIn("not on PATH", out[0].message)
        self.assertTrue((out[0].extra or {}).get("install"))
        self.assertNotEqual(out[0].status, laws.PROVED)
        self.assertFalse(laws.is_proof(out[0].status))

    def test_unsupported_ubsan_is_notrun(self):
        with mock.patch("prism.sanitize._find_cc", return_value="/bin/gcc"), \
             mock.patch("prism.sanitize._probe_sanitizer", return_value=False):
            out = run_sanitize([TD / "abs_ok.c"], Config())
        asan = [f for f in out if (f.extra or {}).get("sanitizer") == "asan"]
        ubsan = [f for f in out if (f.extra or {}).get("sanitizer") == "ubsan"]
        tsan = [f for f in out if (f.extra or {}).get("sanitizer") == "tsan"]
        self.assertEqual(len(asan), 1)
        self.assertEqual(asan[0].status, laws.NOTRUN)
        self.assertIn("no ASan", asan[0].message)
        self.assertEqual(len(ubsan), 1)
        self.assertEqual(ubsan[0].status, laws.NOTRUN)
        self.assertIn("no UBSan", ubsan[0].message)
        self.assertEqual(len(tsan), 1)
        self.assertEqual(tsan[0].status, laws.NOTRUN)
        statuses = {f.status for f in out}
        self.assertNotIn(laws.CLEAN, statuses)
        self.assertNotIn(laws.PROVED, statuses)
        self.assertFalse(any(laws.is_proof(f.status) for f in out))

    def test_supported_ubsan_clean_is_not_a_proof(self):
        with mock.patch("prism.sanitize._find_cc", return_value="/bin/gcc"), \
             mock.patch("prism.sanitize._probe_sanitizer", side_effect=lambda _cc, flags: flags[0] == "-fsanitize=undefined"), \
             mock.patch("prism.sanitize._compile_and_run", return_value=(laws.CLEAN, "ran under sanitizer with exit 0 (not a proof of absence)", "")):
            out = run_sanitize([TD / "abs_ok.c"], Config())
        ubsan = [f for f in out if (f.extra or {}).get("sanitizer") == "ubsan" and f.file]
        self.assertTrue(ubsan)
        self.assertEqual(ubsan[0].status, laws.CLEAN)
        self.assertIn("not a proof", ubsan[0].message)
        self.assertNotEqual(ubsan[0].status, laws.PROVED)
        self.assertFalse(laws.is_proof(ubsan[0].status))

    def test_sanitizer_abort_is_failed(self):
        with mock.patch("prism.sanitize._find_cc", return_value="/bin/gcc"), \
             mock.patch("prism.sanitize._probe_sanitizer", side_effect=lambda _cc, flags: flags[0] == "-fsanitize=undefined"), \
             mock.patch(
                 "prism.sanitize._compile_and_run",
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
            self.assertIn((f.extra or {}).get("sanitizer"), {"asan", "ubsan", "tsan"})
            self.assertNotEqual(f.status, laws.PROVED)

    def test_is_mingw_from_path(self):
        self.assertTrue(_is_mingw(r"C:\mingw64\bin\gcc.exe"))
        self.assertTrue(_is_mingw(r"C:\msys64\mingw64\bin\gcc.exe"))

    def test_is_mingw_from_dumpmachine_ucrt(self):
        with mock.patch("prism.sanitize._dumpmachine", return_value="x86_64-w64-mingw32"):
            self.assertTrue(_is_mingw(r"C:\msys64\ucrt64\bin\gcc.exe"))
        with mock.patch("prism.sanitize._dumpmachine", return_value="x86_64-linux-gnu"):
            self.assertFalse(_is_mingw("/usr/bin/gcc"))

    def test_print_file_name_echo_is_not_a_sanitizer_lib(self):
        proc = mock.Mock(returncode=0, stdout="libubsan.a\n", stderr="")
        with mock.patch("prism.sanitize._run", return_value=proc):
            self.assertFalse(_has_sanitizer_lib("gcc", _UB_FLAGS))

    def test_mingw_without_libubsan_probe_skips_compile(self):
        with mock.patch("prism.sanitize._has_sanitizer_lib", return_value=False), \
             mock.patch("prism.sanitize._compile_ok") as compile_ok:
            self.assertFalse(_probe_sanitizer(r"C:\mingw64\bin\gcc.exe", _UB_FLAGS))
            compile_ok.assert_not_called()

    def test_mingw_without_libubsan_is_notrun_never_clean_or_proved(self):
        """MinGW gcc that links a binary without libubsan is NOTRUN, not CLEAN."""
        with mock.patch("prism.sanitize._find_cc", return_value=r"C:\mingw64\bin\gcc.exe"), \
             mock.patch("prism.sanitize._has_sanitizer_lib", return_value=False), \
             mock.patch("prism.sanitize._compile_and_run") as compile_and_run:
            out = run_sanitize([TD / "abs_ok.c"], Config())
        compile_and_run.assert_not_called()
        self.assertTrue(out)
        statuses = {f.status for f in out}
        self.assertEqual(statuses, {laws.NOTRUN})
        self.assertNotIn(laws.CLEAN, statuses)
        self.assertNotIn(laws.PROVED, statuses)
        self.assertFalse(any(laws.is_proof(f.status) for f in out))
        msgs = " ".join(f.message for f in out)
        self.assertIn("no ASan", msgs)
        self.assertIn("no UBSan", msgs)
        self.assertIn("no TSan", msgs)

    def test_flag_accept_without_ubsan_fire_is_not_supported(self):
        """Compiling `int main(){return 0;}` with -fsanitize=* is not a sanitizer."""
        clean = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch("prism.sanitize._is_mingw", return_value=False), \
             mock.patch("prism.sanitize._compile_ok", return_value=True), \
             mock.patch("prism.sanitize._run", return_value=clean):
            self.assertFalse(_probe_sanitizer("/usr/bin/clang", _UB_FLAGS))

    def test_flag_accept_without_asan_fire_is_not_supported(self):
        """Compiling `int main(){return 0;}` with -fsanitize=address is not a sanitizer."""
        clean = mock.Mock(returncode=0, stdout="", stderr="")
        with mock.patch("prism.sanitize._is_mingw", return_value=False), \
             mock.patch("prism.sanitize._compile_ok", return_value=True), \
             mock.patch("prism.sanitize._run", return_value=clean):
            self.assertFalse(_probe_sanitizer("/usr/bin/clang", _AS_FLAGS))

    def test_supported_asan_clean_is_not_a_proof(self):
        with mock.patch("prism.sanitize._find_cc", return_value="/bin/gcc"), \
             mock.patch("prism.sanitize._probe_sanitizer", side_effect=lambda _cc, flags: flags[0] == "-fsanitize=address"), \
             mock.patch("prism.sanitize._compile_and_run", return_value=(laws.CLEAN, "ran under sanitizer with exit 0 (not a proof of absence)", "")):
            out = run_sanitize([TD / "abs_ok.c"], Config())
        asan = [f for f in out if (f.extra or {}).get("sanitizer") == "asan" and f.file]
        self.assertTrue(asan)
        self.assertEqual(asan[0].status, laws.CLEAN)
        self.assertIn("not a proof", asan[0].message)
        self.assertNotEqual(asan[0].status, laws.PROVED)
        self.assertFalse(laws.is_proof(asan[0].status))

    def test_asan_abort_is_failed(self):
        with mock.patch("prism.sanitize._find_cc", return_value="/bin/gcc"), \
             mock.patch("prism.sanitize._probe_sanitizer", side_effect=lambda _cc, flags: flags[0] == "-fsanitize=address"), \
             mock.patch(
                 "prism.sanitize._compile_and_run",
                 return_value=(laws.FAILED, "AddressSanitizer: heap-buffer-overflow", "asan trace"),
             ):
            out = run_sanitize([TD / "oob_write.c"], Config())
        asan = [f for f in out if (f.extra or {}).get("sanitizer") == "asan" and f.file]
        self.assertEqual(asan[0].status, laws.FAILED)

    def test_asan_hit_text(self):
        self.assertTrue(_sanitizer_hit("ERROR: AddressSanitizer: heap-buffer-overflow", "", 1))
        self.assertTrue(_sanitizer_hit("", "AddressSanitizer: heap-use-after-free", 1))

    def test_print_file_name_echo_is_not_asan_lib(self):
        proc = mock.Mock(returncode=0, stdout="libasan.a\n", stderr="")
        with mock.patch("prism.sanitize._run", return_value=proc):
            self.assertFalse(_has_sanitizer_lib("gcc", _AS_FLAGS))

    def test_mingw_without_libasan_probe_skips_compile(self):
        with mock.patch("prism.sanitize._has_sanitizer_lib", return_value=False), \
             mock.patch("prism.sanitize._compile_ok") as compile_ok:
            self.assertFalse(_probe_sanitizer(r"C:\mingw64\bin\gcc.exe", _AS_FLAGS))
            compile_ok.assert_not_called()

    def test_tsan_unexpected_mapping_is_notrun_not_failed(self):
        mapping = "FATAL: ThreadSanitizer: unexpected memory mapping 0x7f00"
        self.assertTrue(_sanitizer_runtime_unusable(mapping))
        self.assertFalse(_sanitizer_hit(mapping, "", -1))
        self.assertNotEqual(laws.FAILED, laws.NOTRUN)
        with mock.patch("prism.sanitize._find_cc", return_value="/usr/bin/clang"), \
             mock.patch("prism.sanitize._is_mingw", return_value=False), \
             mock.patch("prism.sanitize._probe_sanitizer", return_value=True), \
             mock.patch(
                 "prism.sanitize._compile_and_run",
                 return_value=(
                     laws.NOTRUN,
                     "sanitizer runtime unusable (unexpected memory mapping); not a defect finding",
                     mapping,
                 ),
             ):
            out = run_sanitize([TD / "abs_ok.c"], Config())
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.NOTRUN for f in out))
        self.assertFalse(any(f.status == laws.FAILED for f in out))
        self.assertFalse(any(laws.is_proof(f.status) for f in out))

    def test_compile_oserror_is_notrun_never_error(self):
        with mock.patch("prism.sanitize._find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("prism.sanitize._probe_sanitizer", return_value=True), \
             mock.patch("prism.sanitize._compile_and_run", return_value=(
                 laws.NOTRUN, "gcc vanished", "",
             )):
            out = run_sanitize([TD / "abs_ok.c"], Config())
        self.assertTrue(out)
        self.assertTrue(all(f.status == laws.NOTRUN for f in out))
        self.assertFalse(any(f.status == laws.ERROR for f in out))
        self.assertFalse(any(f.status == laws.CLEAN for f in out))
        self.assertFalse(any(laws.is_proof(f.status) for f in out))

    def test_compile_and_run_oserror_maps_to_notrun(self):
        from prism.sanitize import _compile_and_run
        with mock.patch("prism.sanitize._run", side_effect=OSError("exec format error")):
            st, msg, _ev = _compile_and_run("/usr/bin/gcc", TD / "abs_ok.c", _UB_FLAGS, 8.0)
        self.assertEqual(st, laws.NOTRUN)
        self.assertNotEqual(st, laws.ERROR)
        self.assertNotEqual(st, laws.CLEAN)
        self.assertFalse(laws.is_proof(st))
        self.assertIn("exec format", msg)


if __name__ == "__main__":
    unittest.main()
