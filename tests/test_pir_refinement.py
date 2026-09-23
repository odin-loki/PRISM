"""LLVM -> PIR refinement proofs and the C++ translator stay in step (roadmap 8.2, 8.5).

proofs/refinement proves the translation correct for a Lean translator that
mirrors src/prism/pir/translate.cpp; tools/pir_lean_check.py checks, per run,
that the C++ output is exactly that translator's output. These tests lock the
static parts of that correspondence (check property names and taxonomy
classes, operator names) without Lean, and run the checker when it is built.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REFINE = ROOT / "proofs" / "refinement"
CHECKER = REFINE / ".lake" / "build" / "bin" / "pir_lean_check"


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


class StaticParity(unittest.TestCase):
    """Things a change to translate.cpp must also change in the Lean model."""

    def setUp(self) -> None:
        self.cpp = _read(ROOT / "src" / "prism" / "pir" / "translate.cpp")
        self.lean = _read(REFINE / "PrismRefine" / "Translate.lean")
        self.check = _read(REFINE / "PrismRefine" / "Check.lean")
        self.pir = _read(ROOT / "src" / "prism" / "pir" / "pir.cpp")

    def binop_body(self) -> str:
        start = self.cpp.index("void binop(")
        return self.cpp[start:self.cpp.index("void inst(", start)]

    def test_every_lean_check_is_a_cpp_check(self):
        # (property, class) pairs the Lean translator inserts
        lean_pairs = set(re.findall(r'"([a-z0-9+*\-]+)" "([A-Z][A-Z\-]+)"', self.lean))
        self.assertGreaterEqual(len(lean_pairs), 15)
        for prop, cls in lean_pairs:
            self.assertIn(f'"{prop}"', self.cpp, msg=prop)
            self.assertIn(f'"{cls}"', self.cpp, msg=cls)

    def test_every_cpp_binop_check_is_modelled(self):
        # every property name translate.cpp's binop() uses appears in Translate.lean
        props = set(re.findall(r'"([a-z0-9+*\-]+)",\s*"(?:INT|UB)-[A-Z\-]+"', self.binop_body()))
        props |= {"div0", "mod0"}  # written as op == "udiv" ? "div0" : "mod0"
        self.assertGreaterEqual(len(props), 15)
        for prop in props:
            self.assertIn(f'"{prop}"', self.lean, msg=f"{prop} not modelled in Translate.lean")

    def test_every_binop_flag_is_modelled_or_refused(self):
        flags = set(re.findall(r'has_flag\(in, "([a-z]+)"\)', self.cpp))
        for fl in flags:
            self.assertIn(fl, {"nsw", "nuw", "exact", "disjoint", "nneg"}, msg=fl)
        export = _read(ROOT / "src" / "prism" / "pir" / "export_lean.cpp")
        # samesign is a poison flag translate.cpp ignores: the exporter must
        # refuse it rather than let the checker claim a proof for it
        self.assertIn('fl != "nneg"', export)

    def test_fragment_operator_names_match_op_name(self):
        names = dict(re.findall(r'case Op::(\w+): return "([^"]+)";', self.pir))
        used = set(re.findall(r"Op::(\w+)", self.binop_body()))
        used |= {"Eq", "Ne", "Ult", "Ule", "Ugt", "Uge", "Slt", "Sle", "Sgt", "Sge",
                 "Select", "ZExt", "SExt", "Trunc"}
        for op in used:
            self.assertIn(op, names, msg=op)
            self.assertIn(f'"{names[op]}"', self.check, msg=f"{op} ({names[op]}) not parsed by Check.lean")


@unittest.skipUnless(CHECKER.is_file(), "NOTRUN: pir_lean_check not built (cd proofs/refinement && lake build)")
class CheckerFixtures(unittest.TestCase):
    def test_fixtures(self):
        for f in sorted((REFINE / "fixtures").glob("*.pirl")):
            r = subprocess.run([str(CHECKER), str(f)], capture_output=True, text=True, check=False)
            want = _read(f.with_suffix(".expected")).strip()
            self.assertEqual(r.stdout.strip().splitlines()[-1], want, msg=f.name)
            self.assertEqual(r.returncode, 1 if "mismatch=0" not in want else 0, msg=f.name)


@unittest.skipUnless(CHECKER.is_file() and os.environ.get("PRISM_BIN"),
                     "NOTRUN: needs PRISM_BIN and a built pir_lean_check")
class EndToEnd(unittest.TestCase):
    def test_tests_pir_agrees_with_the_proved_translator(self):
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "pir_lean_check.py"),
                            str(ROOT / "tests" / "pir"), "--bin", os.environ["PRISM_BIN"],
                            "--checker", str(CHECKER)],
                           capture_output=True, text=True, check=False, cwd=ROOT)
        self.assertEqual(r.returncode, 0, msg=r.stdout + r.stderr)
        m = re.search(r"total: agree=(\d+) .* mismatch=(\d+)", r.stdout)
        self.assertIsNotNone(m, msg=r.stdout)
        assert m is not None
        self.assertEqual(int(m.group(2)), 0)
        self.assertGreaterEqual(int(m.group(1)), 30)


if __name__ == "__main__":
    unittest.main()
