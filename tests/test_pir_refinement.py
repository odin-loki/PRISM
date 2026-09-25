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

    def test_every_lean_memory_check_is_a_cpp_check(self):
        # the extended fragment's memory checks (XTranslate.lean) are
        # MemTr::access_checks / MemTr::gep checks (translate_mem.cpp)
        xlean = _read(REFINE / "PrismRefine" / "XTranslate.lean")
        mem = _read(ROOT / "src" / "prism" / "pir" / "translate_mem.cpp") + self.cpp
        pairs = set(re.findall(r'"([a-z0-9+*\-]+)" "([A-Z][A-Z\-]+)"', xlean))
        pairs |= set(re.findall(r'\("([a-z0-9\-]+)", "([A-Z][A-Z\-]+)"\)', xlean))
        self.assertGreaterEqual(len(pairs), 8)
        for prop, cls in pairs:
            self.assertIn(f'"{prop}"', mem, msg=prop)
            self.assertIn(f'"{cls}"', mem, msg=cls)

    def test_every_cpp_binop_check_is_modelled(self):
        # every property name translate.cpp's binop() uses appears in Translate.lean
        props = set(re.findall(r'"([a-z0-9+*\-]+)",\s*"(?:INT|UB)-[A-Z\-]+"', self.binop_body()))
        props |= {"div0", "mod0"}  # written as op == "udiv" ? "div0" : "mod0"
        self.assertGreaterEqual(len(props), 15)
        for prop in props:
            self.assertIn(f'"{prop}"', self.lean, msg=f"{prop} not modelled in Translate.lean")

    def test_every_binop_flag_is_modelled_or_refused(self):
        flags = set(re.findall(r'has_flag\(in, "([a-z]+)"\)', self.cpp))
        export = _read(ROOT / "src" / "prism" / "pir" / "export_lean.cpp")
        for fl in flags:
            if fl == "inbounds":
                # getelementptr's flag. getelementptr is in the extended fragment
                # (proofs/refinement XTranslate.lean gEnd, proved in XMemSim.lean):
                # the exporter passes inbounds and refuses every other GEP flag,
                # and the Lean translator models both forms.
                self.assertIn('if (fl != "inbounds") throw Unsupported{"getelementptr " + fl};', export)
                self.assertIn("if inb then", _read(REFINE / "PrismRefine" / "XTranslate.lean"))
                continue
            if fl == "samesign":
                # icmp samesign (refinement finding 2): translate.cpp checks it
                # (UB-POISON when the signs differ), the Lean model has no such
                # flag, so the exporter must keep refusing it (not in its
                # allowlist below): the checker never claims a proof for it
                self.assertIn('check(cur, p2(cur, Op::Ne, na, nb), "samesign", "UB-POISON"', self.cpp)
                continue
            self.assertIn(fl, {"nsw", "nuw", "exact", "disjoint", "nneg"}, msg=fl)
        # the exporter's flag allowlist for the other instructions is exactly the modelled set
        self.assertEqual(set(re.findall(r'fl != "([a-z]+)"', export)) - {"inbounds"},
                         {"nsw", "nuw", "exact", "disjoint", "nneg"})
        self.assertNotIn('fl != "samesign"', export)

    def test_fragment_operator_names_match_op_name(self):
        names = dict(re.findall(r'case Op::(\w+): return "([^"]+)";', self.pir))
        used = set(re.findall(r"Op::(\w+)", self.binop_body()))
        used |= {"Eq", "Ne", "Ult", "Ule", "Ugt", "Uge", "Slt", "Sle", "Sgt", "Sge",
                 "Select", "ZExt", "SExt", "Trunc"}
        for op in used:
            self.assertIn(op, names, msg=op)
            self.assertIn(f'"{names[op]}"', self.check, msg=f"{op} ({names[op]}) not parsed by Check.lean")

    def test_intrinsic_operators_match_op_name(self):
        # the intrinsics the extended fragment models (XTranslate.lean `mm`, `un`)
        names = dict(re.findall(r'case Op::(\w+): return "([^"]+)";', self.pir))
        for op in ("SMax", "SMin", "UMax", "UMin", "Abs", "Ctlz", "Cttz", "Ctpop", "Bswap"):
            self.assertIn(op, names, msg=op)
            self.assertIn(f'"{names[op]}"', self.check, msg=f"{op} not parsed by Check.lean")
        for prefix in ("llvm.smax.", "llvm.abs.", "llvm.ctlz.", "llvm.ctpop.", "llvm.bswap.", "llvm.memcpy.",
                       "llvm.memmove.", "llvm.memset.", "llvm.lifetime.start", "llvm.lifetime.end"):
            self.assertIn(prefix, self.cpp, msg=prefix)

    def test_globals_share_one_definition(self):
        # the variables the translator preassigns for globals and the `L glob`
        # lines the exporter writes come from the same function
        mem = _read(ROOT / "src" / "prism" / "pir" / "translate_mem.cpp")
        export = _read(ROOT / "src" / "prism" / "pir" / "export_lean.cpp")
        self.assertIn("entry_globals(t_.module(), lay_, f, t_.options().globals_initial)", mem)
        self.assertIn("pirmem::entry_globals(m, lay, f, opt.globals_initial)", export)
        self.assertIn("mt.preassign_globals(f)", self.cpp)
        self.assertIn("mt.emit_entry_globals()", self.cpp)

    def test_pointer_harness_and_heap_share_definitions(self):
        # a contract-bound pointer parameter's object size, and whether the
        # hidden allocation-failed flag exists, are computed by the same
        # functions for the translator and for the Lean export
        mem = _read(ROOT / "src" / "prism" / "pir" / "translate_mem.cpp")
        export = _read(ROOT / "src" / "prism" / "pir" / "export_lean.cpp")
        self.assertIn("uint64_t contract_elem_bytes(", mem)
        self.assertIn("pirmem::contract_elem_bytes(tr.mt.layout(), f, p, c)", self.cpp)
        self.assertIn("pirmem::contract_elem_bytes(lay, f, p, *c)", export)
        self.assertIn("bool reaches_alloc_failed(", mem)
        self.assertIn("pirmem::reaches_alloc_failed(m, f)", self.cpp)
        self.assertIn("pirmem::reaches_alloc_failed(m, f)", export)

    def test_pointer_and_heap_checks_are_modelled(self):
        # the pointer comparison, stack-escape and release checks of
        # translate_mem.cpp are in the Lean translator, with the kinds the
        # models use (heap 2, new 5, new[] 6)
        xlean = _read(REFINE / "PrismRefine" / "XTranslate.lean")
        mem = _read(ROOT / "src" / "prism" / "pir" / "translate_mem.cpp")
        for prop, cls in (("ptr-cmp", "PTR-COMPARE"), ("stack-escape", "MEM-STACK-ESCAPE"),
                          ("free-invalid", "MEM-INVALID-FREE"), ("free-mismatch", "MEM-MISMATCHED-FREE"),
                          ("double-free", "MEM-DOUBLE-FREE")):
            self.assertIn(f'"{prop}" "{cls}"', xlean, msg=prop)
            self.assertIn(f'"{prop}", "{cls}"', mem, msg=prop)
        enum = _read(ROOT / "include" / "prism" / "pir.hpp")
        self.assertIn("Heap = 2", enum)
        self.assertIn("New = 5, NewArr = 6", enum)
        self.assertIn(".c 8 2], .assign (j + 12) (.cmp .eq) [.v (j + 2) 8, .c 8 5]", xlean)


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
        # the extended fragment (freeze, calls, memory) is checked too
        ext = re.search(r"total: agree=\d+ agree-ext=(\d+)", r.stdout)
        self.assertIsNotNone(ext, msg=r.stdout)
        assert ext is not None
        # with the intrinsics, memcpy/memset and globals: 28 functions measured;
        # the pir stage's time budget can leave files unexported on a loaded machine
        self.assertGreaterEqual(int(ext.group(1)), 15)


if __name__ == "__main__":
    unittest.main()
