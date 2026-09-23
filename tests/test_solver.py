"""Solver library (roadmap 3.1-3.3) source locks.

The library is C++ only (roadmap D8: the Python engine is frozen as an
oracle). Its behaviour is tested by the doctest cases in
tests/cpp/test_main.cpp ("solver: ..."). These checks keep the promises
that the C++ sources and docs/TRUSTED_BASE.md make to each other from
drifting apart.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOLVER = ROOT / "src" / "prism" / "solver"
HEADER = ROOT / "include" / "prism" / "solver.hpp"
TRUSTED = ROOT / "docs" / "TRUSTED_BASE.md"
CMAKE = ROOT / "CMakeLists.txt"


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8", errors="replace")


class TestSolverSources(unittest.TestCase):
    def test_sources_are_listed_not_globbed(self) -> None:
        cmake = _read(CMAKE)
        for f in sorted(SOLVER.glob("*.cpp")):
            self.assertIn(f"src/prism/solver/{f.name}", cmake)
        self.assertNotRegex(cmake, r"GLOB[^)]*solver")

    def test_cuda_walker_only_with_prism_cuda(self) -> None:
        cmake = _read(CMAKE)
        at = cmake.index("src/cuda/probsat.cu")
        self.assertGreater(at, cmake.index("if(PRISM_CUDA)"))
        self.assertLess(at, cmake.index("if(PRISM_LLAMA"))

    def test_certified_status_is_one_literal(self) -> None:
        hdr = _read(HEADER)
        # One spelling: the verdict module owns it; the solver aliases it.
        self.assertIn("kProvedCertified = laws::PROVED_CERTIFIED", hdr)
        self.assertNotIn('"PROVED-CERTIFIED"', hdr)
        for f in SOLVER.glob("*.cpp"):
            self.assertNotIn('"PROVED-CERTIFIED"', _read(f), f.name)

    def test_certified_is_set_only_after_the_checker(self) -> None:
        src = _read(SOLVER / "portfolio.cpp")
        sets = [m.start() for m in re.finditer(r"res\.certified = true", src)]
        self.assertEqual(len(sets), 2, "certified is set on the fresh path and the re-checked cache hit")
        for at in sets:
            window = src[max(0, at - 400) : at]
            self.assertIn("ck.certified", window)

    def test_checker_verdict_is_the_exact_line(self) -> None:
        src = _read(SOLVER / "util.cpp")
        self.assertIn('has_line(p.out, "s VERIFIED UNSAT")', src)

    def test_sat_answers_are_model_checked(self) -> None:
        src = _read(SOLVER / "portfolio.cpp")
        self.assertIn("validate_model(c, formula, msg.model", src)
        self.assertIn("validate_model(c, formula, m, &why)", src)  # cached counterexamples too

    def test_local_search_never_reports_unsat(self) -> None:
        src = _read(SOLVER / "portfolio.cpp")
        sls = src[src.index("case MemberKind::Sls:") :]
        sls = sls[: sls.index("break;")]
        self.assertNotIn("Kind::Unsat", sls)


class TestLeanBitblaster(unittest.TestCase):
    """Certified mode's Lean-proved bit-blaster (roadmap 3.2 step 2, 5.4, 8.2)."""

    def test_lean_path_needs_both_checkers(self) -> None:
        src = _read(SOLVER / "portfolio.cpp")
        body = src[src.index("Certify run_checkers(") :]
        body = body[: body.index("c.certified = true;")]
        cake = body.index('find_tool("cake_lpr"')
        lean = body.index("check_lrat_dag(plan.chk")
        self.assertLess(cake, lean, "cake_lpr decides first, then Lean's checker must accept too")
        self.assertIn('"not certified: prism-lrat-check not found (NOTRUN)"', body)

    def test_certificate_says_which_bitblaster(self) -> None:
        src = _read(SOLVER / "portfolio.cpp")
        self.assertIn('"bitblast: lean-proved (toCNF_equisat), prism-bitblast "', src)
        self.assertIn('"bitblast: z3 tactics, unproved (Lean bit-blaster not used: "', src)
        self.assertIn("c.info = plan.desc + ", src)

    def test_executables_are_built_and_found(self) -> None:
        lake = _read(ROOT / "proofs" / "techniques" / "lakefile.toml")
        for exe in ("prism-bitblast", "prism-lrat-check"):
            self.assertIn(f'name = "{exe}"', lake)
            self.assertIn(f'"{exe}"', lake.split("defaultTargets", 1)[1].split("\n", 1)[0])
        self.assertIn("PRISM_LEAN_BIN_DIR", _read(CMAKE))
        self.assertIn("proofs/techniques/.lake/build/bin", _read(CMAKE))

    def test_executables_run_the_proved_functions(self) -> None:
        tech = ROOT / "proofs" / "techniques"
        bb = _read(tech / "BitblastMain.lean")
        self.assertIn("Std.Sat.CNF.dimacs (dagCNF g)", bb)
        self.assertIn("defsOK 0 g.defs", bb)
        chk = _read(tech / "LratCheckMain.lean")
        self.assertIn("verdict (checkDag g cert)", chk)
        self.assertIn("verdict (LRAT.check cert cnf)", chk)
        enc = _read(tech / "PrismTechniques" / "BitblastEncode.lean")
        self.assertIn("def dagCNF (g : Dag) : CNF Nat := toCNF g.toExpr", enc)
        self.assertIn("theorem checkDag_sound", enc)
        audit = _read(tech / "PrismTechniques" / "Audit.lean")
        for thm in ("toCNF_equisat", "certified_unsat", "checkDag_sound", "udivOp_spec", "smulHiOp_spec"):
            self.assertIn(f"#assert_axioms Bitblast.{thm}", audit)


class TestTrustedBaseDocument(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.doc = _read(TRUSTED)

    def test_names_the_real_bit_blaster(self) -> None:
        for s in ("simplify", "bit-blast", "tseitin-cnf", "None of these is a proof", "bv_decide"):
            self.assertIn(s, self.doc)

    def test_lists_roadmap_8_3_rows(self) -> None:
        for row in (
            "Clang (C/C++ to LLVM IR)",
            "Lean kernel",
            "Lean compiler",
            "The C++ compiler that builds PRISM",
            "Hardware and operating system",
            "Formal LLVM semantics",
        ):
            self.assertIn(row, self.doc)

    def test_checker_and_fallback_documented(self) -> None:
        for s in ("cake_lpr", "s VERIFIED UNSAT", "never quietly upgraded", "lrat-check", "SHA-256"):
            self.assertIn(s, self.doc)

    def test_lean_path_trusted_base_documented(self) -> None:
        for s in (
            "bitblast: lean-proved",
            "prism-lrat-check --dag",
            "checkDag_sound",
            "Z3 → S-expression serializer",
            "Lean compiler",
            "Both must accept",
            "tests/cpp/test_leanbb.cpp",
        ):
            self.assertIn(s, self.doc)


if __name__ == "__main__":
    unittest.main()
