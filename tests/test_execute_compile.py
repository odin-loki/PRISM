"""Compile+run paths: sandbox_run, run_compiler, interpreter_loop, rlef_repair.

Honesty: missing gcc/clang is NOTRUN never CLEAN. A planted crash is
CRASH/FAILED not PROVED. Best RLEF score is not PROVED. LLM silence is
HYPOTHESIS/NOTRUN. No live LLM required.

python -m unittest tests.test_execute_compile
"""

from __future__ import annotations

import shutil
import unittest
from pathlib import Path
from unittest import mock

from prism import laws, sandbox
from prism.adapters import run_compiler
from prism.agent import (
    CC_INSTALL,
    CC_MISSING_MSG,
    execute_cex,
    find_cc,
    interpreter_loop,
    rlef_repair,
    rlef_reward,
    sandbox_run,
    sandbox_verdict,
)
from prism.ai import LLM_INSTALL, LLM_UNAVAILABLE_MSG
from prism.config import Config
from prism.models import Finding, FunctionInfo

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"
HAS_CC = bool(shutil.which("gcc") or shutil.which("clang"))
ABORT_PLANT = TD / "sandbox_abort.c"


def _abort_harness() -> str:
    return ABORT_PLANT.read_text(encoding="utf-8") + "\nint main(void) { sandbox_abort(1); return 0; }\n"


def _engine(available: bool = True, text: str = "", error=None):
    engine = mock.Mock()
    engine.available.return_value = available
    engine.complete.return_value = mock.Mock(error=error, text=text, backend="mock")
    return engine


class TestSandboxRun(unittest.TestCase):
    def setUp(self):
        # Law 9: these tests drive the execute path on purpose (--allow-exec).
        self.addCleanup(sandbox.set_allowed, sandbox.set_allowed(True))

    def test_missing_compiler(self):
        with mock.patch("prism.agent.shutil.which", return_value=None):
            result = sandbox_run("int main(void){return 0;}")
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"], "no compiler")
        self.assertIsNone(result["code"])
        self.assertEqual(sandbox_verdict(result), laws.NOTRUN)
        self.assertNotEqual(sandbox_verdict(result), laws.CLEAN)
        self.assertFalse(laws.is_proof(sandbox_verdict(result)))

    def test_sandbox_verdict_never_proved(self):
        self.assertEqual(sandbox_verdict({"ok": True, "error": None}), laws.CLEAN)
        self.assertEqual(
            sandbox_verdict({"ok": False, "error": "crash", "code": -6}),
            laws.CRASH,
        )
        self.assertEqual(
            sandbox_verdict({"ok": False, "error": "exit", "code": 1}),
            laws.FAILED,
        )
        for result in (
            {"ok": True, "error": None},
            {"ok": False, "error": "crash"},
            {"ok": False, "error": "exit"},
            {"ok": False, "error": "compile"},
            {"ok": False, "error": "no compiler"},
        ):
            self.assertNotEqual(sandbox_verdict(result), laws.PROVED)
            self.assertFalse(laws.is_proof(sandbox_verdict(result)))

    @unittest.skipUnless(HAS_CC, "no C compiler")
    def test_tiny_snippet_compiles_and_runs(self):
        src = (
            '#include <stdio.h>\n'
            "int main(void) {\n"
            '    puts("prism-ok");\n'
            "    return 0;\n"
            "}\n"
        )
        result = sandbox_run(src, timeout=5.0)
        self.assertTrue(result["ok"], msg=result)
        self.assertEqual(result["code"], 0)
        self.assertIn("prism-ok", result["stdout"])
        self.assertEqual(result["error"], None)
        self.assertEqual(sandbox_verdict(result), laws.CLEAN)
        self.assertNotEqual(sandbox_verdict(result), laws.PROVED)

    @unittest.skipUnless(HAS_CC, "no C compiler")
    def test_compile_error_captured(self):
        result = sandbox_run("int main(void){ return no_such_symbol; }")
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"], "compile")
        self.assertTrue(result["stderr"] or result["stdout"])
        self.assertEqual(sandbox_verdict(result), laws.FAILED)

    @unittest.skipUnless(HAS_CC, "no C compiler")
    def test_nonzero_exit_is_failure(self):
        result = sandbox_run("int main(void){ return 42; }")
        self.assertFalse(result["ok"])
        self.assertEqual(result["error"], "exit")
        self.assertEqual(result["code"], 42)
        self.assertEqual(sandbox_verdict(result), laws.FAILED)
        self.assertNotEqual(sandbox_verdict(result), laws.PROVED)

    @unittest.skipUnless(HAS_CC, "no C compiler")
    def test_planted_abort_is_crash_or_failed_not_proved(self):
        self.assertTrue(ABORT_PLANT.is_file())
        result = sandbox_run(_abort_harness(), timeout=5.0)
        self.assertFalse(result["ok"], msg=result)
        self.assertIn(result["error"], {"crash", "exit"})
        verdict = sandbox_verdict(result)
        self.assertIn(verdict, {laws.CRASH, laws.FAILED})
        self.assertNotEqual(verdict, laws.PROVED)
        self.assertNotEqual(verdict, laws.CLEAN)
        self.assertFalse(laws.is_proof(verdict))


class TestRunCompiler(unittest.TestCase):
    def test_missing_compiler_is_notrun(self):
        with mock.patch("prism.adapters.shutil.which", return_value=None):
            out = run_compiler([TD / "shift_ub.c"], Config())
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertTrue(
            "PATH" in out[0].message or "not found" in out[0].message,
            msg=out[0].message,
        )

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
    def setUp(self):
        # Law 9: these tests drive the execute path on purpose (--allow-exec).
        self.addCleanup(sandbox.set_allowed, sandbox.set_allowed(True))

    def test_no_compiler_is_notrun(self):
        engine = _engine(text="int main(void){ return 0; }")
        with mock.patch("prism.agent.find_cc", return_value=None):
            out = interpreter_loop(engine, "write a test harness", rounds=2)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertEqual(out[0].message, CC_MISSING_MSG)
        self.assertEqual(out[0].extra.get("install"), CC_INSTALL)
        engine.complete.assert_not_called()

    def test_no_llm_is_notrun_with_install(self):
        engine = _engine(available=False)
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"):
            out = interpreter_loop(engine, "write a test harness", rounds=2)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertEqual(out[0].message, LLM_UNAVAILABLE_MSG)
        self.assertEqual(out[0].extra.get("install"), LLM_INSTALL)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        engine.complete.assert_not_called()

    def test_silence_is_hypothesis_not_proved(self):
        engine = _engine(text="   ")
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("prism.agent.sandbox_run") as sandbox:
            out = interpreter_loop(engine, "test property", rounds=2)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.HYPOTHESIS)
        self.assertNotEqual(out[0].status, laws.PROVED)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        sandbox.assert_not_called()
        self.assertEqual(engine.complete.call_count, 2)

    def test_successful_round_returns_clean(self):
        engine = _engine(text="int main(void){ return 0; }")
        ok = {"ok": True, "error": None, "stdout": "", "stderr": "", "code": 0}
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("prism.agent.sandbox_run", return_value=ok):
            out = interpreter_loop(engine, "test property", rounds=3)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.CLEAN)
        self.assertNotEqual(out[0].status, laws.PROVED)
        self.assertFalse(laws.is_proof(out[0].status))
        self.assertEqual(engine.complete.call_count, 1)

    def test_exhausted_rounds_is_failed_not_notrun(self):
        engine = _engine(text="int main(void){ return 1; }")
        bad = {"ok": False, "error": "exit", "stdout": "", "stderr": "", "code": 1}
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("prism.agent.sandbox_run", return_value=bad):
            out = interpreter_loop(engine, "test property", rounds=2)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.FAILED)
        self.assertEqual(engine.complete.call_count, 2)

    def test_planted_crash_is_crash_not_proved(self):
        engine = _engine(text=_abort_harness())
        crash = {
            "ok": False, "error": "crash", "stdout": "",
            "stderr": "Aborted", "code": -6,
        }
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("prism.agent.sandbox_run", return_value=crash):
            out = interpreter_loop(engine, "demonstrate abort", rounds=3)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.CRASH)
        self.assertNotEqual(out[0].status, laws.PROVED)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(out[0].status))
        self.assertEqual(engine.complete.call_count, 1)


class TestRlefRepairHonesty(unittest.TestCase):
    def setUp(self):
        # Law 9: these tests drive the execute path on purpose (--allow-exec).
        self.addCleanup(sandbox.set_allowed, sandbox.set_allowed(True))

    def test_no_llm_is_notrun_with_install(self):
        engine = _engine(available=False)
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"):
            out = rlef_repair(engine, "int x;", "FAILED", rounds=2)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertEqual(out[0].message, LLM_UNAVAILABLE_MSG)
        self.assertEqual(out[0].extra.get("install"), LLM_INSTALL)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertNotEqual(out[0].status, laws.PROVED)
        engine.complete.assert_not_called()

    def test_no_compiler_is_notrun(self):
        engine = _engine(text="int main(void){ return 0; }")
        with mock.patch("prism.agent.find_cc", return_value=None):
            out = rlef_repair(engine, "int x;", "FAILED", rounds=2)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertEqual(out[0].extra.get("install"), CC_INSTALL)
        self.assertIn("gcc/clang", out[0].message)
        engine.complete.assert_not_called()

    def test_silence_is_hypothesis_not_proved(self):
        engine = _engine(text="")
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("prism.agent.sandbox_run") as sandbox:
            out = rlef_repair(engine, "int x;", "FAILED", rounds=2)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.HYPOTHESIS)
        self.assertNotEqual(out[0].status, laws.PROVED)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        sandbox.assert_not_called()
        self.assertEqual(engine.complete.call_count, 2)

    def test_http_error_before_any_patch_is_notrun(self):
        engine = _engine(error="HTTP Error 502: Bad Gateway", text="")
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("prism.agent.sandbox_run") as sandbox:
            out = rlef_repair(engine, "int x;", "FAILED", rounds=2)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.ERROR)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertNotEqual(out[0].status, laws.HYPOTHESIS)
        self.assertEqual(out[0].extra.get("install"), LLM_INSTALL)
        self.assertFalse(laws.is_proof(out[0].status))
        sandbox.assert_not_called()

    def test_best_score_is_clean_not_proved(self):
        engine = _engine(text="int main(void){ return 0; }")
        ok = {"ok": True, "error": None, "stdout": "", "stderr": "", "code": 0}
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("prism.agent.sandbox_run", return_value=ok):
            out = rlef_repair(
                engine, "int broken;", "FAILED", rounds=2, bmc_oracle=lambda _src: None,
            )
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.CLEAN)
        self.assertNotEqual(out[0].status, laws.PROVED)
        self.assertFalse(laws.is_proof(out[0].status))
        self.assertIn("best score", out[0].message)
        self.assertIn("not a proof", out[0].message)

    def test_crashing_patch_is_not_proved(self):
        engine = _engine(text=_abort_harness())
        crash = {
            "ok": False, "error": "crash", "stdout": "",
            "stderr": "Aborted", "code": -6,
        }
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("prism.agent.sandbox_run", return_value=crash):
            out = rlef_repair(
                engine, "int broken;", "CRASH", rounds=2, bmc_oracle=lambda _src: None,
            )
        self.assertEqual(len(out), 1)
        self.assertIn(out[0].status, {laws.HYPOTHESIS, laws.FAILED, laws.CRASH})
        self.assertNotEqual(out[0].status, laws.PROVED)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(out[0].status))

    def test_bmc_failed_is_negative_not_clean(self):
        engine = _engine(text="int main(void){ return 0; }")
        ok = {"ok": True, "error": None, "stdout": "", "stderr": "", "code": 0}
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("prism.agent.sandbox_run", return_value=ok):
            out = rlef_repair(
                engine, "int broken;", "FAILED", rounds=2,
                bmc_oracle=lambda _src: laws.FAILED,
            )
        self.assertEqual(len(out), 1)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertNotEqual(out[0].status, laws.PROVED)
        self.assertFalse(laws.is_proof(out[0].status))
        self.assertEqual(out[0].status, laws.HYPOTHESIS)
        hist = out[0].extra.get("history") or []
        self.assertTrue(hist)
        self.assertEqual(hist[0].get("bmc"), laws.FAILED)
        self.assertLess(hist[0].get("score"), 3)

    def test_bmc_proved_is_terminal_success(self):
        engine = _engine(text="int abs_ok(int x){ return x < 0 ? -x : x; }")
        ok = {"ok": True, "error": None, "stdout": "", "stderr": "", "code": 0}
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("prism.agent.sandbox_run", return_value=ok):
            out = rlef_repair(
                engine, "int broken;", "FAILED", rounds=2,
                bmc_oracle=lambda _src: laws.PROVED,
            )
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.PROVED)
        self.assertTrue(laws.is_proof(out[0].status))
        self.assertIn("terminal success", out[0].message)
        self.assertEqual(engine.complete.call_count, 1)

    def test_bmc_bounded_is_not_proved(self):
        engine = _engine(text="int main(void){ return 0; }")
        ok = {"ok": True, "error": None, "stdout": "", "stderr": "", "code": 0}
        with mock.patch("prism.agent.find_cc", return_value="/usr/bin/gcc"), \
             mock.patch("prism.agent.sandbox_run", return_value=ok):
            out = rlef_repair(
                engine, "int broken;", "FAILED", rounds=2,
                bmc_oracle=lambda _src: laws.BOUNDED,
            )
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.CLEAN)
        self.assertNotEqual(out[0].status, laws.PROVED)
        self.assertNotEqual(out[0].status, laws.BOUNDED)
        self.assertFalse(laws.is_proof(out[0].status))
        with self.assertRaises(ValueError):
            laws.refuse_merge(laws.PROVED, laws.BOUNDED)


class TestRlefReward(unittest.TestCase):
    def test_bmc_failed_negative_vs_proved_terminal(self):
        ok = {"ok": True, "error": None}
        score, terminal = rlef_reward(ok, None)
        self.assertEqual(score, 3)
        self.assertIsNone(terminal)
        score_fail, term_fail = rlef_reward(ok, laws.FAILED)
        self.assertEqual(score_fail, 1)
        self.assertIsNone(term_fail)
        self.assertLess(score_fail, score)
        score_p, term_p = rlef_reward(ok, laws.PROVED)
        self.assertEqual(term_p, laws.PROVED)
        self.assertTrue(laws.is_proof(term_p))
        score_b, term_b = rlef_reward(ok, laws.BOUNDED)
        self.assertIsNone(term_b)
        self.assertFalse(laws.is_proof(laws.BOUNDED))
        with self.assertRaises(ValueError):
            laws.refuse_merge(laws.PROVED, laws.BOUNDED)

    def test_fuzzer_clean_is_not_a_proof(self):
        ok = {"ok": True, "error": None}
        score, terminal = rlef_reward(ok, laws.CLEAN)
        self.assertEqual(score, 3)
        self.assertIsNone(terminal)
        self.assertFalse(laws.is_proof(laws.CLEAN))
        with self.assertRaises(ValueError):
            laws.refuse_merge(laws.CLEAN, laws.PROVED)

    def test_crash_is_not_rewarded_as_clean(self):
        crash = {"ok": False, "error": "crash"}
        score, terminal = rlef_reward(crash, laws.FAILED)
        self.assertLess(score, 0)
        self.assertIsNone(terminal)
        self.assertFalse(laws.is_proof(sandbox_verdict(crash)))


class TestExecuteCexHonesty(unittest.TestCase):
    def test_empty_fails_is_notrun(self):
        out = execute_cex([], [], llm=False, engine=None)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertNotEqual(out[0].status, laws.CLEAN)
        self.assertFalse(laws.is_proof(out[0].status))

    def test_llm_down_is_notrun_for_llm_half(self):
        fail = Finding(
            stage="bmc", status=laws.FAILED, file="div_param.c",
            function="div_param", line=2, cls="INT-DIV-ZERO",
            message="div by zero", strength=laws.STRENGTH_PROVES,
            counterexample="x=1,y=0",
        )
        engine = _engine(available=False)
        out = execute_cex([fail], [], llm=True, engine=engine)
        self.assertTrue(out)
        llm_half = [f for f in out if f.extra.get("half") == "llm" or f.message == LLM_UNAVAILABLE_MSG]
        self.assertEqual(len(llm_half), 1)
        self.assertEqual(llm_half[0].status, laws.NOTRUN)
        self.assertNotEqual(llm_half[0].status, laws.CLEAN)
        self.assertEqual(llm_half[0].extra.get("install"), LLM_INSTALL)
        self.assertFalse(any(laws.is_proof(f.status) for f in out))
        engine.complete.assert_not_called()

    def test_no_llm_skips_interpreter(self):
        fail = Finding(
            stage="bmc", status=laws.FAILED, file="x.c", function="foo",
            line=1, cls="INT-DIV-ZERO", message="x", strength=laws.STRENGTH_PROVES,
            counterexample="x=0",
        )
        engine = _engine(available=False)
        out = execute_cex([fail], [], llm=False, engine=engine)
        self.assertEqual(out[0].status, laws.NOTRUN)
        self.assertEqual(out[0].message, "no FAILED/CRASH cex to replay")
        engine.complete.assert_not_called()

    def test_pointer_cex_is_needs_harness_never_error_or_clean(self):
        fail = Finding(
            stage="bmc", status=laws.FAILED, file="walk.c",
            function="walk", line=1, cls="PTR-NULL-DEREF",
            message="null deref", strength=laws.STRENGTH_PROVES,
            counterexample="p=0",
        )
        fn = FunctionInfo(
            file="walk.c", name="walk", kind="POINTER", line=1,
            signature="int walk(char *p)", body="return *p;",
            params=[("char *", "p")],
        )
        out = execute_cex([fail], [fn], llm=False, engine=None)
        self.assertEqual(len(out), 1)
        rec = out[0]
        self.assertEqual(rec.status, laws.NEEDS_HARNESS)
        self.assertNotEqual(rec.status, laws.ERROR)
        self.assertNotEqual(rec.status, laws.CLEAN)
        self.assertNotEqual(rec.status, laws.PROVED)
        self.assertFalse(laws.is_proof(rec.status))
        self.assertIn("POINTER", rec.message)


class TestFindCc(unittest.TestCase):
    def test_find_cc_delegates_to_which(self):
        with mock.patch("prism.agent.shutil.which", side_effect=lambda n: "/bin/gcc" if n == "gcc" else None):
            self.assertEqual(find_cc(), "/bin/gcc")


if __name__ == "__main__":
    unittest.main()
