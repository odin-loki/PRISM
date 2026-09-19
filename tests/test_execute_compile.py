"""Compile+run paths: sandbox_run, run_compiler, interpreter_loop compiler gate.

python -m unittest tests.test_execute_compile
"""

from __future__ import annotations

import shutil
import unittest
from pathlib import Path
from unittest import mock

from helix import laws
from helix.adapters import run_compiler
from helix.agent import find_cc, interpreter_loop, sandbox_run
from helix.config import Config

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"
HAS_CC = bool(shutil.which("gcc") or shutil.which("clang"))


class TestSandboxRun(unittest.TestCase):
    def test_missing_compiler(self):
        with mock.patch("helix.agent.shutil.which", return_value=None):
            result = sandbox_run("int main(void){return 0;}")
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"], "no compiler")
        self.assertIsNone(result["code"])

    @unittest.skipUnless(HAS_CC, "no C compiler")
    def test_tiny_snippet_compiles_and_runs(self):
        src = (
            '#include <stdio.h>\n'
            "int main(void) {\n"
            '    puts("helix-ok");\n'
            "    return 0;\n"
            "}\n"
        )
        result = sandbox_run(src, timeout=5.0)
        self.assertTrue(result["ok"], msg=result)
        self.assertEqual(result["code"], 0)
        self.assertIn("helix-ok", result["stdout"])
        self.assertEqual(result["error"], None)

    @unittest.skipUnless(HAS_CC, "no C compiler")
    def test_compile_error_captured(self):
        result = sandbox_run("int main(void){ return no_such_symbol; }")
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"], "compile")
        self.assertTrue(result["stderr"] or result["stdout"])

    @unittest.skipUnless(HAS_CC, "no C compiler")
    def test_nonzero_exit_is_failure(self):
        result = sandbox_run("int main(void){ return 42; }")
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"], "exit")
        self.assertEqual(result["code"], 42)


class TestRunCompiler(unittest.TestCase):
    def test_missing_compiler_is_notrun(self):
        with mock.patch("helix.adapters.shutil.which", return_value=None):
            out = run_compiler([TD / "shift_ub.c"], Config())
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertIn("not on PATH", out[0].message)

    @unittest.skipUnless(HAS_CC, "no C compiler")
    def test_testdata_syntax_check(self):
        out = run_compiler([TD / "shift_ub.c"], Config())
        self.assertTrue(
            out,
            msg="expected at least one compiler warning/error for shift_ub.c",
        )
        self.assertTrue(all(f.status == laws.FAILED for f in out))
        self.assertTrue(any(f.stage == "warnings" for f in out))


class TestInterpreterLoopCompilerGate(unittest.TestCase):
    def test_no_compiler_is_notrun(self):
        engine = mock.Mock()
        engine.available.return_value = True
        with mock.patch("helix.agent.find_cc", return_value=None):
            out = interpreter_loop(engine, "write a test harness", rounds=2)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertIn("gcc/clang", out[0].message)
        engine.complete.assert_not_called()

    def test_successful_round_returns_clean(self):
        engine = mock.Mock()
        engine.available.return_value = True
        engine.complete.return_value = mock.Mock(
            error=None,
            text="int main(void){ return 0; }",
            backend="mock",
        )
        ok = {"ok": True, "error": None, "stdout": "", "stderr": "", "code": 0}
        with mock.patch("helix.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("helix.agent.sandbox_run", return_value=ok):
            out = interpreter_loop(engine, "test property", rounds=3)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.CLEAN)
        self.assertEqual(engine.complete.call_count, 1)

    def test_exhausted_rounds_is_failed_not_notrun(self):
        engine = mock.Mock()
        engine.available.return_value = True
        engine.complete.return_value = mock.Mock(
            error=None,
            text="int main(void){ return 1; }",
            backend="mock",
        )
        bad = {"ok": False, "error": "exit", "stdout": "", "stderr": "", "code": 1}
        with mock.patch("helix.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("helix.agent.sandbox_run", return_value=bad):
            out = interpreter_loop(engine, "test property", rounds=2)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.FAILED)
        self.assertEqual(engine.complete.call_count, 2)


class TestFindCc(unittest.TestCase):
    def test_find_cc_delegates_to_which(self):
        with mock.patch("helix.agent.shutil.which", side_effect=lambda n: "/bin/gcc" if n == "gcc" else None):
            self.assertEqual(find_cc(), "/bin/gcc")


if __name__ == "__main__":
    unittest.main()
