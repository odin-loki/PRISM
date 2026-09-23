"""C++ run_optional_tools probe source-contract. python -m unittest tests.test_prism_optional -q

prism.adapters_extra.run_optional_tools is law. After C++ leftover honesty
lands, adapters.cpp must map a present-but-unusable exe to NOTRUN: probe_exe
catch and !probed call notrun(...), never laws::ERROR / CLEAN / PROVED.
Dispatch run failure may still be ERROR; that is a later catch.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

from prism import laws

ROOT = Path(__file__).resolve().parents[1]
ADAPTERS = ROOT / "src" / "prism" / "adapters.cpp"
_PROOF = {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}


def _read() -> str:
    return ADAPTERS.read_text(encoding="utf-8")


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//.*?$", "", src, flags=re.M)


def _brace_body(src: str, sig: str) -> str:
    i = src.find(sig)
    if i < 0:
        raise AssertionError(f"missing {sig!r} in {ADAPTERS.name}")
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


def _between(src: str, start: str, end: str) -> str:
    i = src.find(start)
    if i < 0:
        raise AssertionError(f"missing {start!r}")
    j = src.find(end, i)
    if j < 0:
        raise AssertionError(f"missing {end!r} after {start!r}")
    return src[i:j]


class TestCppOptionalProbeSourceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = _read()
        cls.code = _strip_comments(cls.src)
        cls.body = _brace_body(cls.src, "std::vector<Finding> run_optional_tools(")
        cls.probe = _between(cls.body, "probe_exe(", "dispatch_optional(")

    def test_probe_catch_is_notrun_never_error(self):
        catch = _between(self.probe, "catch (", "if (!probed)")
        self.assertIn("notrun(tool.stage, first_name, install)", catch)
        self.assertIn("probe failed", catch)
        self.assertNotIn("laws::ERROR", catch)
        self.assertNotIn("laws::CLEAN", catch)
        for proof in _PROOF:
            self.assertNotIn(proof, catch)

    def test_shell_not_found_text_is_not_a_help_page(self):
        probe = _brace_body(self.src, "std::optional<ProcResult> probe_exe(")
        self.assertIn("probe_looks_missing", probe)
        helper = _brace_body(self.src, "bool probe_looks_missing(")
        self.assertIn("126", helper)
        self.assertIn("127", helper)
        self.assertIn("not found", helper)
        self.assertNotIn("laws::ERROR", helper)
        # Doctest/Catch2 answering --help is missing, even on rc 0/1. Scan first.
        self.assertIn("doctest version", helper)
        self.assertIn("catch2 v", helper)
        self.assertIn("unknown option: --timeout", helper)
        self.assertLess(helper.find("doctest version"), helper.find("r.rc == 0"))

    def test_cbmc_unknown_option_is_notrun_never_error(self):
        body = _brace_body(self.src, "std::vector<Finding> run_cbmc(")
        self.assertIn("unknown option", body)
        self.assertIn("doctest version", body)
        self.assertIn("laws::NOTRUN", body)
        self.assertIn("not CBMC", body)
        self.assertIn('adapter_install("cbmc")', body)
        self.assertLess(body.find("unknown option"), body.find("VERIFICATION SUCCESSFUL"))
        successful = body[body.find("VERIFICATION SUCCESSFUL"):body.find("VERIFICATION FAILED")]
        self.assertIn("laws::BOUNDED", successful)
        self.assertNotIn("laws::PROVED", successful)
        self.assertNotIn("laws::CLEAN", body)
        self.assertNotIn("laws::ERROR", body[body.find("unknown option"):body.find("VERIFICATION SUCCESSFUL")])

    def test_unanswered_help_is_notrun_never_error(self):
        none = self.probe.split("if (!probed)", 1)[1]
        self.assertIn("notrun(tool.stage, first_name, install)", none)
        self.assertIn("did not answer --help", none)
        self.assertNotIn("laws::ERROR", none)
        self.assertNotIn("laws::CLEAN", none)
        for proof in _PROOF:
            self.assertNotIn(proof, none)

    def test_unusable_exe_is_never_clean_or_proved(self):
        self.assertIn("probe_exe", self.probe)
        self.assertNotIn("laws::CLEAN", self.probe)
        for proof in _PROOF:
            self.assertNotIn(proof, self.probe)
        # Dispatch OSError/system_error is NOTRUN; other run failures may ERROR.
        self.assertNotIn("laws::ERROR", self.probe)

    def test_dispatch_system_error_is_notrun_never_error(self):
        body = self.body
        start = body.find("dispatch_optional(")
        self.assertGreaterEqual(start, 0)
        slice_ = body[start : start + 1800]
        self.assertIn("std::system_error", slice_)
        sys_at = slice_.find("std::system_error")
        err_at = slice_.find("laws::ERROR")
        self.assertGreater(sys_at, 0)
        self.assertGreater(err_at, sys_at)
        sys_catch = slice_[sys_at : err_at]
        self.assertIn("notrun", sys_catch)
        self.assertIn("unusable", sys_catch)
        self.assertNotIn("laws::ERROR", sys_catch)
        self.assertNotIn("laws::CLEAN", sys_catch)

    def test_infer_missing_compiler_is_notrun(self):
        infer = _brace_body(self.src, "std::vector<Finding> run_infer(")
        self.assertIn("infer present but gcc/clang not on PATH", infer)
        self.assertIn("NOTRUN", infer)
        self.assertIn("install gcc or clang", infer)
        self.assertNotIn("laws::ERROR, \"\", \"infer present but gcc/clang not on PATH\"", infer)


if __name__ == "__main__":
    unittest.main()
