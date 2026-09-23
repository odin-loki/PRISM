"""Lean floating-point and concurrency proofs stay in step with the C++ (roadmap 8.2, M8, M10).

proofs/refinement/PrismRefine/FloatOps.lean proves that the floating-point
conditions PRISM encodes are IEEE 754's (FLOAT-OVERFLOW, FLOAT-INVALID up to
signalling NaNs, FLOAT-DIV-ZERO up to 0/0 and inf/0) and that FLOAT-CAST-OVF
is exactly C11's out-of-range condition. It does so about Lean definitions
that mirror the C++ condition expressions operator by operator.
proofs/techniques/PrismTechniques/LazySeqN.lean proves coverage and
soundness of the slot schedule src/prism/conc/lazy.cpp runs.

These tests lock both sides without Lean: if a C++ condition or the schedule
changes, the matching test fails until the Lean definition (and its proof) is
updated with it.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLOAT_OPS = ROOT / "proofs" / "refinement" / "PrismRefine" / "FloatOps.lean"
LAZY_N = ROOT / "proofs" / "techniques" / "PrismTechniques" / "LazySeqN.lean"


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


def _norm(s: str) -> str:
    return re.sub(r"\s+", " ", s).strip()


def _body(text: str, start: str, end: str) -> str:
    i = text.index(start)
    return text[i:text.index(end, i + len(start))]


class FloatChecksMirror(unittest.TestCase):
    """`FpTr::checks` (translate_fp.cpp) <-> prismDivZero / prismInvalid / prismOverflow."""

    def setUp(self) -> None:
        cpp = _read(ROOT / "src" / "prism" / "pir" / "translate_fp.cpp")
        self.checks = _norm(_body(cpp, "void FpTr::checks(", "\nbool FpTr::inst("))
        self.cpp = cpp
        self.lean = _norm(_read(FLOAT_OPS))

    def test_cpp_conditions_are_the_modelled_ones(self):
        # each C++ condition expression, verbatim (whitespace-normalised)
        for expr in [
            "Arg n = p(b, Op::FIsNaN, {a});",
            "Arg fin = p(b, Op::Xor, {p(b, Op::Or, {n, p(b, Op::FIsInf, {a})}), Arg::c(1, 1)});",
            "any_nan = any_nan ? p(b, Op::Or, {*any_nan, n}) : n;",
            "all_finite = all_finite ? p(b, Op::And, {*all_finite, fin}) : fin;",
            "if (op == Op::FDiv) { divz = p(b, Op::FIsZero, {args[1]});",
            't_.check(b, p(b, Op::And, {*divz, p(b, Op::Xor, {p(b, Op::FIsNaN, {args[0]}), Arg::c(1, 1)})}), '
            '"fp-div0", "FLOAT-DIV-ZERO",',
            't_.check(b, p(b, Op::And, {p(b, Op::FIsNaN, {r}), p(b, Op::Xor, {*any_nan, Arg::c(1, 1)})}), '
            '"fp-invalid", "FLOAT-INVALID",',
            "Arg ovf = p(b, Op::And, {p(b, Op::FIsInf, {r}), *all_finite});",
            "if (divz) ovf = p(b, Op::And, {ovf, p(b, Op::Xor, {*divz, Arg::c(1, 1)})});",
            't_.check(b, ovf, "fp-overflow", "FLOAT-OVERFLOW",',
        ]:
            self.assertIn(_norm(expr), self.checks,
                          msg=f"translate_fp.cpp changed: update FloatOps.lean and its proofs ({expr})")

    def test_lean_definitions_mirror_them(self):
        for d in [
            "def pFin (a : Val) : Bool := (a.isNaN || a.isInf) ^^ true",
            "def prismDivZero (x y : Val) : Bool := y.isZero && (x.isNaN ^^ true)",
            "def prismInvalid (x y r : Val) : Bool := r.isNaN && ((x.isNaN || y.isNaN) ^^ true)",
            "def prismOverflow (op : BinOp) (x y r : Val) : Bool := "
            "if op = .div then (r.isInf && (pFin x && pFin y)) && (y.isZero ^^ true) "
            "else r.isInf && (pFin x && pFin y)",
        ]:
            self.assertIn(_norm(d), self.lean, msg=d)

    def test_binary_operators_covered(self):
        # the proofs cover fadd fsub fmul fdiv; frem also gets the checks but is not modelled
        self.assertIn(_norm("inductive BinOp where | add | sub | mul | div"), self.lean)
        bin_map = _body(self.cpp, "static const std::map<std::string, Op> bin{", "};")
        for op, cop in [("fadd", "FAdd"), ("fsub", "FSub"), ("fmul", "FMul"), ("fdiv", "FDiv")]:
            self.assertIn(f'{{"{op}", Op::{cop}}}', bin_map)
        self.assertIn("checks(cur, it->second, w, {a, b}, r, c.line);", self.cpp)

    def test_main_theorems_present_and_audited(self):
        audit = _read(ROOT / "proofs" / "refinement" / "Audit.lean")
        for thm in ["roundQ_nearest", "roundF_correct", "sub_correct", "mul_correct", "div_correct",
                    "prism_overflow_eq", "prism_invalid_eq", "ieee_invalid_eq", "prism_divzero_eq",
                    "ieee_divzero_imp_prism", "cast_ovf_iff"]:
            self.assertRegex(self.lean, rf"theorem {thm}\b", msg=thm)
            self.assertIn(f"#print axioms PrismRefine.Float.{thm}\n", audit, msg=thm)
        self.assertIn("import PrismRefine.FloatOps", _read(ROOT / "proofs" / "refinement" / "PrismRefine.lean"))


class FloatCastMirror(unittest.TestCase):
    """FLOAT-CAST-OVF in the encoder and the interpreter <-> prismCastOvf."""

    def test_encoder_condition(self):
        enc = _norm(_body(_read(ROOT / "src" / "prism" / "pir" / "encode.cpp"),
                          "case Op::FToSIOvf:", "case Op::FLibm:"))
        for expr in [
            "auto t = z3::expr(c, Z3_mk_fpa_round_to_integral(c, rm(Z3_mk_fpa_rtz(c)), x));",
            "z3::expr lo = s.op == Op::FToSIOvf ? fnum(-std::ldexp(1.0, static_cast<int>(k) - 1), fw) "
            ": z3::expr(c, Z3_mk_fpa_zero(c, fsort(fw), false));",
            "z3::expr hi = fnum(std::ldexp(1.0, static_cast<int>(s.op == Op::FToSIOvf ? k - 1 : k)), fw);",
            "auto below = s.op == Op::FToSIOvf ? flt(t, lo) : (flt(t, lo));",
            "auto above = z3::expr(c, Z3_mk_fpa_geq(c, t, hi));",
            "return b2bv(is_nan(x) || is_inf(x) || below || above);",
        ]:
            self.assertIn(_norm(expr), enc, msg=f"encode.cpp changed: update prismCastOvf ({expr})")
        # pow2Val models fnum as a round-to-nearest numeral of the format
        self.assertIn("z3::expr fnum(double d, unsigned w) { return z3::expr(c, Z3_mk_fpa_numeral_double(c, d, "
                      "fsort(w))); }", _read(ROOT / "src" / "prism" / "pir" / "encode.cpp"))

    def test_interpreter_condition(self):
        fp = _norm(_body(_read(ROOT / "src" / "prism" / "pir" / "fp.cpp"), "case Op::FToSIOvf:", "case Op::FLibm:"))
        for expr in [
            "if (std::isnan(x) || std::isinf(x)) return 1;",
            "double t = std::trunc(x);",
            "if (op == Op::FToSIOvf) return t < -std::ldexp(1.0, k - 1) || t >= std::ldexp(1.0, k - 1);",
            "return t < 0 || t >= std::ldexp(1.0, k);",
        ]:
            self.assertIn(_norm(expr), fp, msg=expr)

    def test_lean_definition(self):
        lean = _norm(_read(FLOAT_OPS))
        self.assertIn(_norm("""def prismCastOvf (f : Fmt) (signed : Bool) (k : Nat) (x : Val) : Bool :=
  let t := rtz f x
  let lo := if signed then (pow2Val f (k - 1)).neg else .fin false 0
  let hi := pow2Val f (if signed then k - 1 else k)
  x.isNaN || x.isInf || Val.lt t lo || Val.le hi t"""), lean)
        self.assertIn(_norm("match round f (2 ^ (j + f.D)) with | some r => .fin false r | none => .inf false"),
                      lean)
        self.assertIn(_norm("theorem cast_ovf_iff (f : Fmt) (signed : Bool) (k : Nat) (x : FP f) (hw : x.wf) :"
                            " prismCastOvf f signed k (decode x) = true ↔ ¬ inRange f signed k (decode x)"), lean)


class LazyScheduleMirror(unittest.TestCase):
    """src/prism/conc/lazy.cpp's slot schedule <-> LazySeqN.prismSched."""

    def test_cpp_schedule(self):
        cpp = _norm(_read(ROOT / "src" / "prism" / "conc" / "lazy.cpp"))
        self.assertIn(_norm("""for (int r = 0; r < opt.rounds; ++r) {
            for (std::size_t t = 0; t < N; ++t) run_slot(r, static_cast<int>(t));"""), cpp,
                      msg="lazy.cpp round-robin changed: update LazySeqN.rr / prismSched")
        self.assertIn("run_slot(opt.rounds, 0);", cpp, msg="main's final slot changed: update prismSched")
        self.assertIn('res.extra["context_switch_bound"] = std::to_string(opt.rounds * N);', cpp)

    def test_lean_schedule(self):
        lean = _norm(_read(LAZY_N))
        self.assertIn("def rr (N K : Nat) : List Nat := (List.replicate K (List.range N)).flatten", lean)
        self.assertIn("def prismSched (N K : Nat) : List Nat := rr N K ++ [0]", lean)
        self.assertIn("(prismSched N K).length = K * N + 1", lean)
        audit = _read(ROOT / "proofs" / "techniques" / "PrismTechniques" / "Audit.lean")
        for thm in ["slots_of_star", "star_of_slots", "slots_mono", "length_prismSched", "rr_covers_runs",
                    "lazy_sound", "lazy_covers_runs", "lazy_covers", "lazy_covers_two",
                    "per_thread_bound_not_enough"]:
            self.assertRegex(lean, rf"theorem {thm}\b", msg=thm)
            self.assertIn(f"#assert_axioms LazySeqN.{thm}\n", audit, msg=thm)
        self.assertIn("import PrismTechniques.LazySeqN",
                      _read(ROOT / "proofs" / "techniques" / "PrismTechniques.lean"))


if __name__ == "__main__":
    unittest.main()
