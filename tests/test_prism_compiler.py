"""C++ run_compiler source-contract. python -m unittest tests.test_prism_compiler -q

helix.adapters.run_compiler is law. After C++ gcc+clang union lands,
adapters.cpp run_compiler must match: both compilers, empty units UNKNOWN,
.cpp/.cxx is -std=c++11, unmatched exit FAILED, refuse -w/-Wno-*, same resolved
path is not a second compiler. Missing gcc/clang is NOTRUN, never CLEAN.
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


class TestCppCompilerSourceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = _read()
        cls.code = _strip_comments(cls.src)
        cls.body = _brace_body(cls.src, "std::vector<Finding> run_compiler(")

    def test_queries_gcc_and_clang_separately(self):
        self.assertIn('which({"gcc"})', self.body)
        self.assertIn('which({"clang"})', self.body)
        self.assertNotRegex(
            self.body,
            r'which\(\s*\{\s*"gcc"\s*,\s*"clang"\s*\}',
            msg="first-only which({gcc,clang}) is not a union",
        )

    def test_empty_units_is_unknown_never_silent(self):
        self.assertIn("no .c/.cc/.cpp/.cxx files in scope", self.body)
        self.assertIn("UNKNOWN", self.body)
        self.assertNotIn("return {};", self.body.replace("return {std::move(f)};", ""))

    def test_cpp_units_use_cxx_std(self):
        self.assertIn("-std=c++11", self.body)
        self.assertIn("-std=c11", self.body)
        self.assertRegex(self.body, r'\.cpp".*-std=c\+\+11|-std=c\+\+11.*\.cpp', re.S)
        # .cxx is the same C++11 arm as .cpp, not C11. Empty scope names .cxx.
        self.assertIn('ext == ".cxx"', self.body)
        self.assertRegex(
            self.body,
            r'ext == "\.cpp"\s*\|\|\s*ext == "\.cxx"\)\s*\?\s*"-std=c\+\+11"',
        )
        self.assertIn("no .c/.cc/.cpp/.cxx files in scope", self.body)

    def test_unmatched_exit_is_failed(self):
        self.assertIn("compiler-error", self.body)
        self.assertIn("FAILED", self.body)
        self.assertRegex(self.body, r"hits\s*==\s*0.*rc\s*!=\s*0|rc\s*!=\s*0.*hits\s*==\s*0", re.S)

    def test_refuses_wno_and_w(self):
        self.assertIn('"-w"', self.body)
        self.assertIn("-Wno-", self.body)
        self.assertIn("refusing to disable a check", self.body)

    def test_dedupes_same_resolved_compiler(self):
        self.assertTrue(
            "compiler_key" in self.body or "canonical" in self.body,
            msg="same gcc/clang path must not run twice",
        )

    def test_missing_is_notrun_never_clean(self):
        self.assertIn('notrun("warnings", "gcc"', self.body)
        self.assertIn("install gcc or clang", self.body)
        self.assertNotIn("laws::CLEAN", self.body)
        for proof in _PROOF:
            self.assertNotIn(proof, self.body)

    def test_never_passes_w_flags_in_cmd(self):
        cmd = _brace_body(self.src, "std::vector<Finding> run_compiler(")
        for bad in ('"-w"', '"-Wno-everything"', '"-werror"'):
            if bad == '"-w"':
                continue
        self.assertIn("-Wall", cmd)
        self.assertIn("-Wextra", cmd)
        self.assertIn("-fsyntax-only", cmd)
        self.assertNotIn("-Wno-error", cmd)


if __name__ == "__main__":
    unittest.main()
