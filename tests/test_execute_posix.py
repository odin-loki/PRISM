"""C++ POSIX run_argv source-contract (fork/execvp).

WSL-shaped honesty: src/prism/stages/platform.cpp compile+run is fork/execvp, not a
"process helper not implemented" stub. execute_cex still never PROVED;
rlef_repair without an LLM stays NOTRUN. Unique vs tests/cpp/test_main.cpp.

python -m unittest tests.test_execute_posix
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STAGES = ROOT / "src" / "prism" / "stages"
PLATFORM = STAGES / "platform.cpp"  # run_argv
LLM = STAGES / "llm.cpp"  # sandbox_run, llm_httpish
COMMON = STAGES / "common.cpp"  # nr
EXECUTE = STAGES / "execute_cex.cpp"
RLEF = STAGES / "rlef.cpp"
FUSE = STAGES / "fuse.cpp"  # fuzz_function
STUB = "process helper not implemented"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _read_all_stages() -> str:
    """Every stage implementation (src/prism/stages/*.cpp), concatenated."""
    return "\n".join(_read(p) for p in sorted(STAGES.glob("*.cpp")))


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//.*?$", "", src, flags=re.M)


def _brace_body(src: str, sig: str) -> str:
    i = src.find(sig)
    if i < 0:
        raise AssertionError(f"missing {sig!r} in {STAGES.name}/")
    brace = src.find("{", i)
    if brace < 0:
        raise AssertionError(f"no body after {sig!r}")
    depth = 0
    for j, ch in enumerate(src[brace:], brace):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return src[brace : j + 1]
    raise AssertionError(f"unbalanced braces after {sig!r}")


def _posix_run_argv(src: str) -> str:
    body = _brace_body(src, "ProcRun run_argv(")
    win = body.find("#ifdef _WIN32")
    els = body.find("#else", win)
    end = body.find("#endif", els)
    if win < 0 or els < 0 or end < 0:
        raise AssertionError("POSIX #else of run_argv is missing")
    return body[els:end]


class TestPosixRunArgvContract(unittest.TestCase):
    def test_posix_run_argv_contains_fork_and_execvp(self):
        posix = _strip_comments(_posix_run_argv(_read(PLATFORM)))
        self.assertRegex(posix, r"\bfork\s*\(")
        self.assertRegex(posix, r"\bexecvp\s*\(")
        self.assertIn("waitpid", posix)

    def test_posix_run_argv_is_not_process_helper_stub(self):
        src = _read(PLATFORM)
        body = _brace_body(src, "ProcRun run_argv(")
        posix = _posix_run_argv(src)
        self.assertNotIn(STUB, body)
        self.assertNotIn(STUB, posix)
        self.assertNotIn(STUB, _strip_comments(posix).lower())
        code = _strip_comments(posix)
        self.assertRegex(code, r"\bfork\s*\(")
        self.assertRegex(code, r"\bexecvp\s*\(")
        self.assertNotIn("not implemented", code.lower())

    def test_sandbox_run_compile_and_run_use_run_argv(self):
        body = _brace_body(_read(LLM), "nlohmann::json sandbox_run(")
        calls = [m.start() for m in re.finditer(r"\brun_argv\s*\(", body)]
        self.assertGreaterEqual(len(calls), 2, msg="compile then run must both use run_argv")
        self.assertIn('"-o"', body)
        self.assertIn("cr.rc", body)
        self.assertIn("rr.rc", body)


class TestExecuteCexHonestyContract(unittest.TestCase):
    def test_execute_cex_source_never_assigns_proved(self):
        body = _brace_body(_read(EXECUTE), "std::vector<Finding> execute_cex(")
        code = _strip_comments(body)
        self.assertNotIn("laws::PROVED", code)
        self.assertNotIn('status = std::string(laws::PROVED)', code)
        self.assertIn("laws::CRASH", code)
        self.assertIn("laws::CLEAN", code)
        self.assertIn("not a proof", code)
        self.assertIn("concrete-replay", code)

    def test_rlef_repair_missing_llm_is_notrun_source(self):
        nr_fn = _brace_body(_read(COMMON), "Finding nr(")
        self.assertIn("laws::NOTRUN", nr_fn)
        self.assertNotIn("laws::CLEAN", nr_fn)
        self.assertNotIn("laws::PROVED", nr_fn)
        repair = _brace_body(_read(RLEF), "std::vector<Finding> rlef_repair(")
        head = repair.split("which_cc()", 1)[0]
        self.assertIn("engine.available()", head)
        self.assertIn('nr("repair"', head)
        self.assertIn("LLM_UNAVAILABLE_MSG", head)
        self.assertNotIn("laws::PROVED", head)
        self.assertNotIn("laws::CLEAN", head)

    def test_rlef_repair_httpish_notrun_on_best_score_not_history_size(self):
        repair = _brace_body(_read(RLEF), "std::vector<Finding> rlef_repair(")
        self.assertIn("llm_httpish(r.error) && best_score < 0", repair)
        self.assertNotIn("history.size() == 1", repair)

    def test_llm_httpish_includes_connection_aborted(self):
        src = _read(LLM)
        start = src.find("bool llm_httpish(")
        self.assertGreaterEqual(start, 0)
        body = src[start : start + 1200]
        self.assertIn("connection aborted", body)
        self.assertIn("connection refused", body)

    def test_fuzz_function_binary_fuzz_after_concrete_before_clean(self):
        src = _read(FUSE)
        start = src.find("Finding fuzz_function(")
        self.assertGreaterEqual(start, 0)
        body = src[start : start + 16000]
        self.assertIn("compile_afl_harness", body)
        self.assertIn('extra["oracle"] = "binary"', body)
        self.assertIn('extra["binary"] = "compile-failed"', body)
        self.assertIn("binary_iters", body)
        concrete_at = body.find('extra["oracle"] = "concrete"')
        binary_at = body.find('extra["oracle"] = "binary"')
        self.assertGreater(concrete_at, 0)
        self.assertGreater(binary_at, concrete_at)
        self.assertIn("laws::CLEAN", body)
        self.assertNotIn("(void)src;", body)


class TestHarnessParsefailContract(unittest.TestCase):
    def test_harness_for_parsefail_is_prism_mapper_not_generic_stub(self):
        rest = _read_all_stages()
        self.assertNotIn("is not that model", rest)
        bmc = (ROOT / "src" / "prism" / "bmc.cpp").read_text(encoding="utf-8")
        start = bmc.find("std::optional<std::string> harness_for_parsefail(")
        self.assertGreaterEqual(start, 0)
        body = bmc[start : start + 4000]
        self.assertIn("throw unencoded", body)
        self.assertIn("return std::nullopt", bmc[start:])
        hdr = (ROOT / "include" / "prism" / "stages.hpp").read_text(encoding="utf-8")
        self.assertIn("harness_for_parsefail", hdr)


if __name__ == "__main__":
    unittest.main()
