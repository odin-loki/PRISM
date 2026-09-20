"""C++ run_sanitize source-contract. python -m unittest tests.test_prism_sanitize -q

helix.sanitize.run_sanitize is law. adapters.cpp must probe ASan, UBSan, and
TSan. Flag-accept is not a sanitizer: UBSan must fire planted signed overflow,
ASan must fire planted heap OOB. CLEAN is not a proof. Missing compiler or
sanitizer is NOTRUN, never CLEAN/PROVED.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

from helix import laws

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


class TestCppSanitizeSourceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = _read()
        cls.code = _strip_comments(cls.src)
        cls.run_body = _brace_body(cls.src, "std::vector<Finding> run_sanitize(")
        cls.hit = _brace_body(cls.src, "bool sanitizer_hit(")
        cls.probe = _brace_body(cls.src, "bool probe_sanitizer(")

    def test_probes_asan_ubsan_tsan(self):
        self.assertIn('-fsanitize=address', self.run_body)
        self.assertIn('-fsanitize=undefined', self.run_body)
        self.assertIn('-fsanitize=thread', self.run_body)
        self.assertIn('"asan"', self.run_body)
        self.assertIn('"ubsan"', self.run_body)
        self.assertIn('"tsan"', self.run_body)
        self.assertIn("compiler has no ASan", self.run_body)
        self.assertIn("compiler has no UBSan", self.run_body)
        self.assertIn("compiler has no TSan", self.run_body)

    def test_hit_recognizes_addresssanitizer(self):
        self.assertIn("addresssanitizer", self.hit)
        self.assertIn("undefinedbehaviorsanitizer", self.hit)
        self.assertIn("threadsanitizer", self.hit)

    def test_ubsan_and_asan_must_actually_fire(self):
        self.assertIn("2147483647", self.probe)
        self.assertIn("malloc(1)", self.probe)
        self.assertIn("p[8]", self.probe)
        self.assertIn("fsanitize=undefined", self.probe)
        self.assertIn("fsanitize=address", self.probe)

    def test_clean_is_not_a_proof_and_missing_is_notrun(self):
        self.assertIn("not a proof of absence", self.src)
        self.assertIn("gcc/clang not on PATH", self.run_body)
        self.assertIn("NOTRUN", self.run_body)
        for st in _PROOF:
            self.assertNotIn(st, self.run_body)

    def test_unexpected_mapping_is_notrun(self):
        self.assertIn("unexpected memory mapping", self.src)
        self.assertIn("NOTRUN", self.src)

    def test_mingw_without_sanitizer_lib_is_not_a_probe(self):
        self.assertIn("is_mingw", self.src)
        self.assertIn("has_sanitizer_lib", self.src)
        self.assertIn("-print-file-name=", self.src)
        self.assertIn("mingw", self.probe.lower())
        code = _strip_comments(self.probe)
        self.assertIn("is_mingw", code)
        self.assertIn("has_sanitizer_lib", code)
        self.assertLess(code.find("is_mingw"), code.find("compile_ok"))

    def test_compile_failed_to_start_is_notrun_never_error(self):
        run = _brace_body(self.src, "std::tuple<std::string, std::string, std::string> compile_and_run_san(")
        self.assertIn("sanitizer compile failed to start", run)
        start = run.find("if (comp.failed)")
        self.assertGreaterEqual(start, 0)
        end = run.find("if (comp.rc != 0)", start)
        self.assertGreater(end, start)
        slice_ = run[start:end]
        self.assertIn("NOTRUN", slice_)
        self.assertNotIn("ERROR", slice_)
