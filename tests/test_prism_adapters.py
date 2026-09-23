"""C++ adapter honesty source-contract. python -m unittest tests.test_prism_adapters -q

Python engine adapters.py / adapters_extra.py is law. Missing tool is NOTRUN.
Fake Catch2/doctest is NOTRUN, never PROVED/CLEAN/ERROR. Empty diagnostics
are UNKNOWN, never a proof. POINTER is not this module (fuse).
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


class TestCppAdapterHonestySourceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = _read()
        cls.code = _strip_comments(cls.src)

    def test_is_fake_adapter_matches_python(self):
        body = _brace_body(self.src, "bool is_fake_adapter(")
        self.assertIn("doctest version", body)
        self.assertIn("catch2 v", body)
        self.assertIn("unknown option", body)
        self.assertNotIn("laws::CLEAN", body)
        self.assertNotIn("laws::PROVED", body)

    def test_tool_unusable_is_missing_not_verdict(self):
        body = _brace_body(self.src, "bool tool_unusable(")
        self.assertIn("is_fake_adapter", body)
        self.assertIn("126", body)
        self.assertIn("127", body)
        self.assertIn("is not recognized as", body)
        self.assertIn("cannot execute", body)
        self.assertNotIn("laws::CLEAN", body)

    def test_cppcheck_empty_diagnostics_unknown_fake_notrun(self):
        body = _brace_body(self.src, "std::vector<Finding> run_cppcheck_unstamped(")  # + tool_sha wrapper
        self.assertIn("no diagnostics (not a proof)", body)
        self.assertIn("not cppcheck", body)
        self.assertIn("tool_unusable", body)
        self.assertIn("laws::UNKNOWN", body)
        self.assertIn("laws::NOTRUN", body)
        self.assertNotIn("laws::CLEAN", body)
        for proof in _PROOF:
            self.assertNotIn(proof, body)

    def test_esbmc_fake_is_notrun_never_proved(self):
        body = _brace_body(self.src, "std::vector<Finding> run_esbmc_unstamped(")  # + tool_sha wrapper
        self.assertIn("not ESBMC", body)
        self.assertIn("tool_unusable", body)
        self.assertIn("laws::NOTRUN", body)
        fake = body[body.find("tool_unusable") : body.find("VERIFICATION SUCCESSFUL")]
        self.assertIn("laws::NOTRUN", fake)
        self.assertNotIn("laws::ERROR", fake)
        self.assertNotIn("laws::CLEAN", fake)
        self.assertNotIn("laws::PROVED", fake)

    def test_dafny_fake_is_notrun_never_proved(self):
        body = _brace_body(self.src, "std::vector<Finding> run_dafny_unstamped(")  # + tool_sha wrapper
        self.assertIn("not Dafny", body)
        self.assertIn("tool_unusable", body)
        self.assertIn("laws::NOTRUN", body)
        fake = body[body.find("tool_unusable") : body.find("r.rc == 0")]
        self.assertIn("laws::NOTRUN", fake)
        self.assertNotIn("laws::PROVED", fake)
        self.assertNotIn("laws::CLEAN", fake)

    def test_cbmc_fake_is_notrun_before_successful(self):
        body = _brace_body(self.src, "std::vector<Finding> run_cbmc(")
        self.assertIn("not CBMC", body)
        self.assertIn("unknown option", body)
        self.assertIn("doctest version", body)
        self.assertLess(body.find("unknown option"), body.find("VERIFICATION SUCCESSFUL"))
        successful = body[body.find("VERIFICATION SUCCESSFUL") : body.find("VERIFICATION FAILED")]
        self.assertIn("laws::BOUNDED", successful)
        self.assertNotIn("laws::PROVED", successful)

    def test_optional_runners_fake_is_notrun(self):
        cases = (
            ("std::vector<Finding> run_clang_tidy(", "not clang-tidy"),
            ("std::vector<Finding> run_spatch(", "not Coccinelle"),
            ("std::vector<Finding> run_infer(", "not Infer"),
            ("std::vector<Finding> run_frama_c(", "not Frama-C"),
            ("std::vector<Finding> run_klee(", "not KLEE"),
            ("std::vector<Finding> run_semgrep(", "not semgrep"),
            ("std::vector<Finding> run_strix(", "not Strix"),
        )
        for sig, needle in cases:
            with self.subTest(sig=sig):
                body = _brace_body(self.src, sig)
                self.assertIn(needle, body)
                self.assertIn("laws::NOTRUN", body)
                self.assertNotIn("laws::CLEAN", body)
                self.assertNotIn("laws::PROVED", body)

    def test_probe_not_found_is_missing_on_any_rc(self):
        body = _brace_body(self.src, "bool probe_looks_missing(")
        self.assertIn("is not recognized as", body)
        self.assertIn("cannot execute", body)
        self.assertIn("command not found", body)
        self.assertLess(body.find("not found"), body.find("r.rc == 0"))
        self.assertNotIn("laws::CLEAN", body)
        self.assertNotIn("laws::ERROR", body)

    def test_clang_tidy_fake_before_warning_parse(self):
        body = _brace_body(self.src, "std::vector<Finding> run_clang_tidy(")
        fake = body.find("is_fake_adapter")
        warn = body.find(": warning:")
        self.assertGreaterEqual(fake, 0)
        self.assertGreaterEqual(warn, 0)
        self.assertLess(fake, warn, msg="Catch2/doctest must be NOTRUN before : warning: hits")

    def test_framac_stub_stderr_is_not_present(self):
        body = _brace_body(self.src, "bool frama_c_probe_present(")
        self.assertIn("probe_looks_missing", body)
        self.assertIn("tool_unusable", body)
        self.assertIn("frama", body)
        self.assertNotIn("laws::CLEAN", body)
        self.assertNotIn("present at", body)
        opt = _brace_body(self.src, "std::vector<Finding> run_optional_tools(")
        self.assertIn("frama_c_probe_present", opt)
        self.assertIn("did not answer --help", opt)
        gate = opt[opt.find("frama_c_probe_present") : opt.find("dispatch_optional")]
        self.assertIn("frama_c_probe_present", gate)
        self.assertIn("did not answer --help", gate)
        self.assertNotIn("laws::CLEAN", gate)
        self.assertNotIn("help_ok", gate)

    def test_sanitizer_start_fail_is_notrun(self):
        body = _brace_body(
            self.src, "std::tuple<std::string, std::string, std::string> compile_and_run_san("
        )
        self.assertIn("sanitizer compile failed to start", body)
        self.assertIn("sanitizer run failed to start", body)
        start = body.find("if (comp.failed)")
        self.assertGreaterEqual(start, 0)
        slice_ = body[start : body.find("if (comp.rc != 0)", start)]
        self.assertIn("NOTRUN", slice_)
        self.assertNotIn("ERROR", slice_)
        self.assertNotIn("CLEAN", slice_)

    def test_klee_ptr_err_is_failed(self):
        body = _brace_body(self.src, "std::vector<Finding> run_klee(")
        self.assertIn('ext == ".err"', body)
        self.assertIn('name.find("error")', body)
        self.assertIn("laws::FAILED", body)
        crash = body.split("if (crash)", 1)[1].split("} else {", 1)[0]
        self.assertIn("laws::FAILED", crash)
        self.assertNotIn("laws::CLEAN", crash)
        for proof in _PROOF:
            self.assertNotIn(proof, crash)


if __name__ == "__main__":
    unittest.main()
