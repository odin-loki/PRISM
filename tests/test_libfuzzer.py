"""libFuzzer adapter honesty: missing clang/flag is NOTRUN, never a proof.

Python engine `_libfuzzer_probe` is law. libfuzzer is not a PATH binary in
OPTIONAL_TOOLS; `run_optional_tools` always appends a clang
`-fsanitize=fuzzer` probe. Missing clang or an unsupported fuzzer flag
is NOTRUN (the probe did not run). A present clang that compiles the
tiny LLVMFuzzerTestOneInput stub is UNKNOWN — a toolchain finding, not
a verdict on the user's code. Never CLEAN, never PROVED.

C++ `libfuzzer_probe` in src/prism/adapters.cpp must match that
mapping (source contract; Python engine is the engine).

python -m unittest tests.test_libfuzzer
"""

from __future__ import annotations

import inspect
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from prism import laws, sandbox
from prism.adapters_extra import (
    OPTIONAL_TOOLS,
    _LIBFUZZER_INSTALL,
    _libfuzzer_probe,
    _run_libfuzzer,
    run_optional_tools,
)
from prism.config import Config
from prism.models import Finding, FunctionInfo

ROOT = Path(__file__).resolve().parents[1]
_CPP = ROOT / "src" / "prism" / "adapters.cpp"
CLANG = r"C:\tools\clang.exe"
C_FILE = Path("planted.c")
_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}


def _no_adapter(*_a, **_k):
    return None


def _proc(stdout: str = "", stderr: str = "", rc: int = 0):
    return mock.Mock(returncode=rc, stdout=stdout, stderr=stderr)


def _cpp_libfuzzer_src() -> str:
    text = _CPP.read_text(encoding="utf-8")
    start = text.find("Finding libfuzzer_probe")
    if start < 0:
        return ""
    rest = text[start:]
    end = rest.find("\nstd::vector<fs::path> glob_ext")
    return rest if end < 0 else rest[:end]


class TestLibfuzzerProbe(unittest.TestCase):
    def _never_proof(self, findings) -> None:
        self.assertTrue(findings)
        for f in findings:
            if isinstance(f, Finding):
                self.assertEqual(f.stage, "libfuzzer", f.stage)
                self.assertEqual(f.strength, laws.STRENGTH_FINDS, f.strength)
            self.assertNotEqual(f.status, laws.PROVED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_UNBOUNDED, f.message)
            self.assertNotEqual(f.status, laws.PROVED_ASSUMING, f.message)
            self.assertNotIn(f.status, _PROOF, f.message)
            self.assertNotEqual(f.status, laws.CLEAN, f.message)
            self.assertFalse(laws.is_proof(f.status), f.status)

    def test_libfuzzer_is_probe_not_optional_path_binary(self):
        stages = [spec[0] for spec in OPTIONAL_TOOLS]
        self.assertNotIn("libfuzzer", stages)
        self.assertIn("out.append(_libfuzzer_probe(cfg))", inspect.getsource(run_optional_tools))

    def test_missing_clang_via_run_optional_tools_is_notrun(self):
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=None), \
             mock.patch("prism.adapters_extra._run") as run:
            findings = run_optional_tools([C_FILE], Config())
        run.assert_not_called()
        lf = next(f for f in findings if f.stage == "libfuzzer")
        self.assertEqual(lf.status, laws.NOTRUN)
        self.assertIn("clang not on PATH", lf.message)
        self.assertEqual((lf.extra or {}).get("install"), _LIBFUZZER_INSTALL)
        self.assertIn("third_party/MANIFEST.toml", (lf.extra or {}).get("install", ""))
        self.assertNotEqual(lf.status, laws.CLEAN)
        self.assertNotEqual(lf.status, laws.PROVED)
        self.assertFalse(laws.is_proof(lf.status))
        self._never_proof([lf])

    def test_missing_clang_direct_probe_is_notrun(self):
        with mock.patch("prism.adapters_extra.shutil.which", return_value=None), \
             mock.patch("prism.adapters_extra._run") as run:
            f = _libfuzzer_probe(Config())
        run.assert_not_called()
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertEqual(f.stage, "libfuzzer")
        self.assertIn("clang not on PATH", f.message)
        self.assertFalse(laws.is_proof(f.status))
        self._never_proof([f])

    def test_unsupported_fsanitize_fuzzer_is_notrun(self):
        unsupported = _proc(stderr="error: unsupported argument '-fsanitize=fuzzer'", rc=1)
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
             mock.patch("prism.adapters_extra._run", return_value=unsupported) as run:
            findings = run_optional_tools([], Config())
        run.assert_called()
        cmd = run.call_args[0][0]
        self.assertEqual(cmd[0], CLANG)
        self.assertIn("-fsanitize=fuzzer", cmd)
        lf = next(f for f in findings if f.stage == "libfuzzer")
        self.assertEqual(lf.status, laws.NOTRUN)
        self.assertIn("libfuzzer", lf.message.lower())
        self.assertIn("-fsanitize=fuzzer", lf.message)
        self.assertNotEqual(lf.status, laws.CLEAN)
        self.assertNotEqual(lf.status, laws.UNKNOWN)
        self.assertFalse(laws.is_proof(lf.status))
        self._never_proof([lf])

    def test_unknown_and_unrecognized_flag_is_notrun(self):
        cases = (
            "error: unknown argument: '-fsanitize=fuzzer'",
            "clang: error: unrecognized command line option '-fsanitize=fuzzer'",
        )
        for stderr in cases:
            with self.subTest(stderr=stderr):
                with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
                     mock.patch("prism.adapters_extra._run", return_value=_proc(stderr=stderr, rc=1)) as run:
                    f = _libfuzzer_probe(Config())
                self.assertEqual(run.call_count, 1, "keyword reject must not fall through to a second probe")
                self.assertEqual(f.status, laws.NOTRUN)
                self.assertNotEqual(f.status, laws.CLEAN)
                self.assertFalse(laws.is_proof(f.status))
                self._never_proof([f])

    def test_generic_compile_fail_second_probe_fail_is_notrun(self):
        fail = _proc(stderr="clang: error: linker command failed", rc=1)
        with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
             mock.patch("prism.adapters_extra._run", return_value=fail) as run:
            f = _libfuzzer_probe(Config())
        self.assertGreaterEqual(run.call_count, 2)
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertIn("libfuzzer", f.message.lower())
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(f.status))
        self._never_proof([f])

    def test_successful_probe_is_unknown_not_a_code_verdict(self):
        ok = _proc(rc=0)
        with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=_no_adapter), \
             mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
             mock.patch("prism.adapters_extra._run", return_value=ok) as run:
            findings = run_optional_tools([C_FILE], Config())
        run.assert_called()
        cmd = run.call_args[0][0]
        self.assertIn("-fsanitize=fuzzer", cmd)
        lf = next(f for f in findings if f.stage == "libfuzzer")
        self.assertEqual(lf.status, laws.UNKNOWN)
        self.assertIn("not a code verdict", lf.message.lower())
        self.assertIn(CLANG, lf.message)
        self.assertNotEqual(lf.status, laws.CLEAN)
        self.assertNotEqual(lf.status, laws.PROVED)
        self.assertFalse(laws.is_proof(lf.status))
        self.assertEqual(lf.file, "")
        self._never_proof([lf])

    def test_probe_timeout_is_notrun_never_error(self):
        with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
             mock.patch("prism.adapters_extra._run", side_effect=subprocess.TimeoutExpired("clang", 12)):
            f = _libfuzzer_probe(Config())
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertNotEqual(f.status, laws.ERROR)
        self.assertIn("probe failed", f.message.lower())
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(f.status))
        self._never_proof([f])

    def test_never_is_proof_on_any_mocked_path(self):
        paths = []
        with mock.patch("prism.adapters_extra.shutil.which", return_value=None):
            paths.append(_libfuzzer_probe(Config()))
        with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
             mock.patch("prism.adapters_extra._run", return_value=_proc(
                 stderr="error: unsupported argument '-fsanitize=fuzzer'", rc=1,
             )):
            paths.append(_libfuzzer_probe(Config()))
        with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
             mock.patch("prism.adapters_extra._run", return_value=_proc(rc=0)):
            paths.append(_libfuzzer_probe(Config()))
        with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
             mock.patch("prism.adapters_extra._run", side_effect=OSError("clang vanished")):
            paths.append(_libfuzzer_probe(Config()))
        self.assertEqual(len(paths), 4)
        for f in paths:
            self.assertFalse(laws.is_proof(f.status), f.status)
            self.assertNotEqual(f.status, laws.CLEAN)
            self.assertNotIn(f.status, _PROOF)
        self._never_proof(paths)

    def test_prism_and_cpp_source_never_emit_clean_or_proved(self):
        py = inspect.getsource(_libfuzzer_probe)
        self.assertIn("laws.NOTRUN", py)
        self.assertIn("laws.UNKNOWN", py)
        self.assertIn("-fsanitize=fuzzer", py)
        self.assertIn("not a code verdict", py)
        self.assertNotIn("laws.CLEAN", py)
        self.assertNotIn("laws.PROVED", py)

        stub = _cpp_libfuzzer_src()
        self.assertTrue(stub, "adapters.cpp must define libfuzzer_probe")
        self.assertIn("laws::NOTRUN", stub)
        self.assertIn("laws::UNKNOWN", stub)
        self.assertIn("-fsanitize=fuzzer", stub)
        self.assertIn("not a code verdict", stub)
        self.assertNotIn("laws::CLEAN", stub)
        self.assertNotIn("laws::PROVED", stub)
        self.assertNotIn("PROVED-UNBOUNDED", stub)


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


class TestRunLibfuzzerHonesty(unittest.TestCase):
    def setUp(self):
        # Law 9: these tests drive the execute path on purpose (--allow-exec).
        self.addCleanup(sandbox.set_allowed, sandbox.set_allowed(True))

    def test_missing_clang_is_notrun_never_engine_libfuzzer(self):
        with mock.patch("prism.adapters_extra.shutil.which", return_value=None), \
             mock.patch("prism.adapters_extra._run") as run:
            f = _run_libfuzzer(_scalar(), Path("planted.c"))
        run.assert_not_called()
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.ERROR)
        self.assertNotEqual((f.extra or {}).get("engine"), "libfuzzer")
        self.assertNotIn("engine", f.extra or {})
        self.assertEqual((f.extra or {}).get("install"), _LIBFUZZER_INSTALL)
        self.assertFalse(laws.is_proof(f.status))

    def test_pointer_is_needs_harness_never_error(self):
        with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
             mock.patch("prism.adapters_extra._run") as run:
            f = _run_libfuzzer(_pointer(), Path("planted.c"))
        run.assert_not_called()
        self.assertEqual(f.status, laws.NEEDS_HARNESS)
        self.assertNotEqual(f.status, laws.ERROR)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertIn("POINTER", f.message)
        self.assertNotEqual((f.extra or {}).get("engine"), "libfuzzer")
        self.assertFalse(laws.is_proof(f.status))

    def test_unsupported_fuzzer_flag_is_notrun_not_engine(self):
        unsupported = _proc(stderr="error: unsupported argument '-fsanitize=fuzzer'", rc=1)
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "planted.c"
            src.write_text("int inc(int x) { return x + 1; }\n", encoding="utf-8")
            work = Path(td) / "lf"
            with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
                 mock.patch("prism.adapters_extra._run", return_value=unsupported) as run:
                f = _run_libfuzzer(_scalar(), src, timeout=1.0, work=work)
        self.assertGreaterEqual(run.call_count, 1)
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual(f.status, laws.ERROR)
        self.assertNotEqual((f.extra or {}).get("engine"), "libfuzzer")
        self.assertFalse(laws.is_proof(f.status))

    def test_compile_oserror_is_notrun_never_error(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "planted.c"
            src.write_text("int inc(int x) { return x + 1; }\n", encoding="utf-8")
            work = Path(td) / "lf"
            with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
                 mock.patch("prism.adapters_extra._compile_libfuzzer",
                            side_effect=OSError("clang vanished")):
                f = _run_libfuzzer(_scalar(), src, timeout=1.0, work=work)
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertNotEqual(f.status, laws.ERROR)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual((f.extra or {}).get("engine"), "libfuzzer")
        self.assertEqual((f.extra or {}).get("install"), _LIBFUZZER_INSTALL)
        self.assertFalse(laws.is_proof(f.status))

    def test_compile_timeout_is_timeout_never_error(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "planted.c"
            src.write_text("int inc(int x) { return x + 1; }\n", encoding="utf-8")
            work = Path(td) / "lf"
            with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
                 mock.patch("prism.adapters_extra._compile_libfuzzer",
                            side_effect=subprocess.TimeoutExpired("clang", 30)):
                f = _run_libfuzzer(_scalar(), src, timeout=1.0, work=work)
        self.assertEqual(f.status, laws.TIMEOUT)
        self.assertNotEqual(f.status, laws.ERROR)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(f.status))

    def test_run_oserror_is_notrun_never_error(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "planted.c"
            src.write_text("int inc(int x) { return x + 1; }\n", encoding="utf-8")
            work = Path(td) / "lf"
            calls = {"n": 0}

            def fake_run(cmd, timeout, cwd=None):
                calls["n"] += 1
                if any(str(a).startswith("-max_total_time") for a in cmd):
                    raise OSError("exec format error")
                return _proc(rc=0)

            with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
                 mock.patch("prism.adapters_extra._run", side_effect=fake_run), \
                 mock.patch("prism.adapters_extra._run_harness", side_effect=fake_run):
                f = _run_libfuzzer(_scalar(), src, timeout=1.0, work=work)
        self.assertGreaterEqual(calls["n"], 1)
        self.assertEqual(f.status, laws.NOTRUN)
        self.assertNotEqual(f.status, laws.ERROR)
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertNotEqual((f.extra or {}).get("engine"), "libfuzzer")
        self.assertIn("unusable", f.message)
        self.assertFalse(laws.is_proof(f.status))

    def test_mocked_clean_is_not_a_proof(self):
        ok = _proc(rc=0)
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "planted.c"
            src.write_text("int inc(int x) { return x + 1; }\n", encoding="utf-8")
            work = Path(td) / "lf"
            with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
                 mock.patch("prism.adapters_extra._run", return_value=ok), \
                 mock.patch("prism.adapters_extra._run_harness", return_value=ok):
                f = _run_libfuzzer(_scalar(), src, timeout=1.0, work=work)
        self.assertEqual(f.status, laws.CLEAN)
        self.assertEqual((f.extra or {}).get("engine"), "libfuzzer")
        self.assertIn("not a proof", f.message.lower())
        self.assertNotEqual(f.status, laws.PROVED)
        self.assertFalse(laws.is_proof(f.status))

    def test_mocked_crash_artifact_is_crash_not_proof(self):
        ok = _proc(rc=0)

        def run_side_effect(cmd, timeout, cwd=None):
            if any(str(a).startswith("-max_total_time") for a in cmd):
                dest = Path(cwd) if cwd is not None else Path(".")
                (dest / "crash-deadbeef").write_bytes(b"\x01\x02\x03\x04")
            return ok

        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / "planted.c"
            src.write_text("int inc(int x) { return x + 1; }\n", encoding="utf-8")
            work = Path(td) / "lf"
            with mock.patch("prism.adapters_extra.shutil.which", return_value=CLANG), \
                 mock.patch("prism.adapters_extra._run", side_effect=run_side_effect), \
                 mock.patch("prism.adapters_extra._run_harness", side_effect=run_side_effect):
                f = _run_libfuzzer(_scalar(), src, timeout=1.0, work=work)
        self.assertEqual(f.status, laws.CRASH)
        self.assertEqual((f.extra or {}).get("engine"), "libfuzzer")
        self.assertNotEqual(f.status, laws.CLEAN)
        self.assertFalse(laws.is_proof(f.status))

    def test_run_optional_tools_still_probes_not_campaigns(self):
        src = inspect.getsource(run_optional_tools)
        self.assertIn("out.append(_libfuzzer_probe(cfg))", src)
        self.assertNotIn("_run_libfuzzer(", src)


if __name__ == "__main__":
    unittest.main()
