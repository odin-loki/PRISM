"""C++ run_klee dump-scan source-contract. python -m unittest tests.test_prism_klee -q

helix.adapters_extra._run_klee is law. After C++ dump scan lands, adapters.cpp
run_klee must treat a dump whose filename contains "error" OR whose extension
is .err as FAILED — so test000001.ptr.err is a hit. Filename-only "error"
misses *.ptr.err. Silence with no dump is UNKNOWN, never a proof.
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


class TestCppKleeDumpScanSourceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.src = _read()
        cls.code = _strip_comments(cls.src)
        cls.body = _brace_body(cls.src, "std::vector<Finding> run_klee(")

    def test_dump_hit_is_filename_error_or_err_extension(self):
        """test000001.ptr.err has no 'error' in the name; .err still FAILED."""
        self.assertIn('name.find("error")', self.body)
        self.assertIn('ext == ".err"', self.body)
        self.assertRegex(
            self.body,
            r'name\.find\("error"\).*ext == "\.err"|ext == "\.err".*name\.find\("error"\)',
            re.S,
        )
        self.assertIn("||", self.body)
        # A name-only scan would drop KLEE's testNNNNNN.ptr.err dumps.
        self.assertNotRegex(
            self.body,
            r'if\s*\(\s*name\.find\("error"\)\s*!=\s*std::string::npos\s*\)\s*crash\s*=\s*true',
            msg="filename 'error' alone misses test000001.ptr.err",
        )

    def test_dump_or_klee_error_text_is_failed_never_proof(self):
        self.assertIn('kr.text.find("KLEE: ERROR")', self.body)
        self.assertIn("laws::FAILED", self.body)
        self.assertIn("if (crash)", self.body)
        crash = self.body.split("if (crash)", 1)[1]
        crash = crash.split("} else {", 1)[0]
        self.assertIn("laws::FAILED", crash)
        self.assertNotIn("laws::CLEAN", crash)
        self.assertNotIn("laws::PROVED", crash)
        for proof in _PROOF:
            self.assertNotIn(proof, crash)

    def test_no_dump_is_unknown_not_a_proof(self):
        self.assertIn("not a proof", self.body)
        self.assertIn("laws::UNKNOWN", self.body)
        self.assertNotIn("laws::CLEAN", self.body)
        for proof in _PROOF:
            self.assertNotIn(proof, self.body)


if __name__ == "__main__":
    unittest.main()
