"""The Lean loop-cut proof and the pir encoder it models stay in step (roadmap 8.2, M8).

proofs/techniques/PrismTechniques/LoopCut.lean proves that Houdini's loop-cut
encoding (src/prism/pir/houdini.inc, docs/PIR.md "Loop invariants") is sound
for structured programs with nested and sequential loops, and that the
encoder's per-statement guard (encode.cpp, soundness bug S9) is the
sequential semantics of `assume`. The proof models three facts of the C++
encoder; these tests fail if one of them changes without the model:

* a check is guarded by the node's reach strengthened by the assumes before
  it, never by a later one (`r = r && cond`, `exit_reach`);
* the invariants assumed at a havocked header are guarded by "the header is
  reached and no property was violated before it" (`noviol(props_before)`);
* a base query assumes the loops encoded before the header, a step query
  the loops encoded up to the back edge (topological order: `seq`).
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TECH = ROOT / "proofs" / "techniques"
LOOPCUT = TECH / "PrismTechniques" / "LoopCut.lean"


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def _strip_comments(text: str) -> str:
    return re.sub(r"--[^\n]*", "", re.sub(r"/-.*?-/", "", text, flags=re.S))


class LeanModel(unittest.TestCase):
    def test_main_theorems_present_and_audited(self):
        src = _strip_comments(_read(LOOPCUT))
        audit = _read(TECH / "PrismTechniques" / "Audit.lean")
        for thm in ("encViol_iff", "encExit_iff", "old_viol_imp", "s9_old_encoding_misses", "exec_cut",
                    "cexec_grun", "grun_complete", "loopcut_sound", "houdini_loopcut_sound"):
            self.assertRegex(src, rf"\btheorem {thm}\b", msg=thm)
            self.assertIn(f"#assert_axioms LoopCut.{thm}", audit, msg=thm)
        self.assertIn("import PrismTechniques.LoopCut", _read(TECH / "PrismTechniques.lean"))

    def test_no_escape_hatch(self):
        src = _strip_comments(_read(LOOPCUT))
        self.assertNotRegex(src, r"\b(sorry|admit|native_decide|bv_decide)\b")
        self.assertNotRegex(src, r"(?m)^\s*axiom\b")

    def test_model_has_nested_multi_exit_loops(self):
        src = _read(LOOPCUT)
        # loops with break/continue (several exits and latches), nested freely
        for ctor in ("| loop (L : Nat) (body : Prog S)", "| brk", "| cont", "| ret"):
            self.assertIn(ctor, src, msg=ctor)
        # the havoc assumption is guarded by "no violation before" (and no failed goal)
        self.assertIn("(¬ v ∧ ¬ q1 → Inv L e s)", src)


class EncoderParity(unittest.TestCase):
    def setUp(self) -> None:
        self.enc = _read(ROOT / "src" / "prism" / "pir" / "encode.cpp")
        self.hou = _read(ROOT / "src" / "prism" / "pir" / "houdini.inc")

    def test_sequential_assume_guard(self):
        # Part 1 (S9): the running guard r; a check uses it, an assume strengthens it
        # for what follows, and the node's edges start from its final value
        self.assertIn("case Stmt::Check: props.push_back(PropInst{&s, id, r && is1(lookup(s.args[0], id))}); break;",
                      self.enc)
        self.assertIn("case Stmt::Assume: r = r && is1(lookup(s.args[0], id)); break;", self.enc)
        self.assertIn("exit_reach[static_cast<std::size_t>(id)] = r;", self.enc)
        self.assertIn("const auto& r = exit_reach[static_cast<std::size_t>(from)];", self.enc)

    def test_havoc_assumption_guarded_by_no_violation_before(self):
        # Part 2: sel[i] -> (reach(header) && noviol(props before it) -> invariant at the havoc)
        guarded = re.findall(
            r"z3::implies\(\s*\*sel\[i\],\s*z3::implies\(e\.reach\[static_cast<std::size_t>\(CL\.node\)\]"
            r"\s*&&\s*noviol\(CL\.props_before\)",
            self.hou,
        )
        self.assertGreaterEqual(len(guarded), 2, msg="search and final assumptions both guarded")

    def test_queries_assume_the_loops_before_their_point(self):
        # base: loops encoded before the header; step: loops encoded up to the back edge
        self.assertIn("[&](std::size_t M) { return e.cut_loops[M].seq < seqL; }", self.hou)
        self.assertIn("[&](std::size_t M) { return e.cut_loops[M].seq <= seqT; }", self.hou)
        # the goals: base in the entry state, step in each back edge's next state,
        # both only where no property was violated before
        self.assertIn("e.reach[static_cast<std::size_t>(CL.node)] && noviol(CL.props_before)", self.hou)
        self.assertIn("lt.guard && noviol(lt.props_before)", self.hou)
        # only the fixpoint is used: the final query assumes every loop's survivors
        self.assertIn("selectors([](std::size_t) { return true; }, all_sel);", self.hou)


if __name__ == "__main__":
    unittest.main()
