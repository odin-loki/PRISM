/-
End-to-end demonstration of certified mode (roadmap 3.2) with the proved
bit-blaster: bit-blast a formula with `toCNF`, run CaDiCaL (the copy shipped
with the Lean toolchain) to get an LRAT certificate, and run core Lean's
verified LRAT checker on it.  When the checker accepts, `certified_unsat`
turns that into "the formula is unsatisfiable".

This is an *executed* check (compiled code), not a kernel proof: the theorem
`certified_unsat` is proved, and this program evaluates its premise
`LRAT.check cert (toCNF φ) = true` for concrete formulas.

Usage: `lake exe certified-demo [path/to/cadical]`
-/
import PrismTechniques.BitblastEncode
import Std.Tactic.BVDecide.LRAT.Parser

open PrismTechniques.Bitblast
open Std.Tactic.BVDecide

/-- 8-bit variables `x` (input bits 0..7) and `y` (bits 8..15). -/
def x8 : BVExpr 8 := .var 0
def y8 : BVExpr 8 := .var 8
/-- 6-bit variables for the (harder) multiplier commutativity check. -/
def x6 : BVExpr 6 := .var 0
def y6 : BVExpr 6 := .var 6

/-- `a ↔ b` for 1-bit formulas, as "they differ" (so the check is unsat). -/
def differ (a b : BVExpr 1) : BVExpr 1 := .xor a b

/-- The formulas: `(name, φ, expectUnsat)`. -/
def cases : List (String × BVExpr 1 × Bool) :=
  [ ("x <u x", .ult x8 x8, true),
    ("x + y != y + x", .not (.eq (.add x8 y8) (.add y8 x8)), true),
    ("x <s y && y <s x", .and (.slt x8 y8) (.slt y8 x8), true),
    ("(x & y) != ~(~x | ~y)", .not (.eq (.and x8 y8) (.not (.or (.not x8) (.not y8)))), true),
    ("ite(x <u y, x, y) >u y", .ult y8 (.ite (.ult x8 y8) x8 y8), true),
    ("x * 3 != x + x + x", .not (.eq (.mul x8 (.const 3#8)) (.add (.add x8 x8) x8)), true),
    ("x * y != y * x (6-bit)", .not (.eq (.mul x6 y6) (.mul y6 x6)), true),
    ("x - y != x + -y", .not (.eq (.sub x8 y8) (.add x8 (.neg y8))), true),
    ("x << 1 != x + x (shift by a variable-width amount)",
      .not (.eq (.shl x8 (.const 1#8)) (.add x8 x8)), true),
    ("(x >>a 7) != -(x >>l 7)", .not (.eq (.ashrC 7 x8) (.neg (.lshrC 7 x8))), true),
    ("saddo(x,y) differs from sext9 x + sext9 y != sext9 (x + y)",
      differ (.saddo x8 y8) (.not (.eq (.add (.sext 9 x8) (.sext 9 y8)) (.sext 9 (.add x8 y8)))),
      true),
    ("ssubo(x,y) differs from sext9 x - sext9 y != sext9 (x - y)",
      differ (.ssubo x8 y8) (.not (.eq (.sub (.sext 9 x8) (.sext 9 y8)) (.sext 9 (.sub x8 y8)))),
      true),
    ("umulo(x,y) differs from the high half of zext x * zext y being nonzero (6-bit)",
      differ (.umulo x6 y6)
        (.not (.eq (.extract 6 6 (.mul (.zext 12 x6) (.zext 12 y6))) (.const 0#6))), true),
    ("smulo(x,y) differs from sext12 x * sext12 y != sext12 (x * y) (6-bit)",
      differ (.or (.smulHi x6 y6) (.smulLo x6 y6))
        (.not (.eq (.mul (.sext 12 x6) (.sext 12 y6)) (.sext 12 (.mul x6 y6)))), true),
    ("x != (x udiv y) * y + (x urem y) (6-bit)",
      .not (.eq x6 (.add (.mul (.udiv x6 y6) y6) (.urem x6 y6))), true),
    ("x != (x sdiv y) * y + (x srem y) (6-bit)",
      .not (.eq x6 (.add (.mul (.sdiv x6 y6) y6) (.srem x6 y6))), true),
    ("x udiv 0 != ~0 (SMT-LIB division by zero)",
      .not (.eq (.udiv x8 (.const 0#8)) (.const (BitVec.allOnes 8))), true),
    ("concat (extract 4 4 x) (extract 0 4 x) != x",
      .not (.eq (.concat (.extract 4 4 x8) (.extract 0 4 x8)) x8), true),
    ("x <u y (satisfiable)", .ult x8 y8, false),
    ("x * y = 35 (satisfiable)", .eq (.mul x8 y8) (.const 35#8), false) ]

def runCase (cadical : String) (dir : System.FilePath) (name : String) (φ : BVExpr 1)
    (expectUnsat : Bool) (idx : Nat) : IO Bool := do
  let cnf := toCNF φ
  let cnfPath := dir / s!"case{idx}.cnf"
  let lratPath := dir / s!"case{idx}.lrat"
  IO.FS.writeFile cnfPath (Std.Sat.CNF.dimacs cnf)
  let out ← IO.Process.output {
    cmd := cadical,
    args := #[cnfPath.toString, lratPath.toString, "--lrat", "--binary=false", "--quiet",
      "--unsat"] }
  -- CaDiCaL exits 20 for UNSAT, 10 for SAT
  if out.exitCode == 20 then
    let cert ← LRAT.loadLRATProof lratPath
    let ok := LRAT.check cert cnf
    IO.println s!"{name}: UNSAT, {cnf.clauses.size} clauses, {cert.size} LRAT steps, verified checker: {ok}"
    return ok && expectUnsat
  else if out.exitCode == 10 then
    IO.println s!"{name}: SAT ({cnf.clauses.size} clauses) — no certificate, nothing to check"
    return !expectUnsat
  else
    IO.println s!"{name}: solver failed (exit {out.exitCode})"
    return false

def main (args : List String) : IO UInt32 := do
  let cadical := args.headD "cadical"
  let dir : System.FilePath := ".lake/certified-demo"
  IO.FS.createDirAll dir
  let mut allOk := true
  let mut idx := 0
  for (name, φ, expectUnsat) in cases do
    let ok ← runCase cadical dir name φ expectUnsat idx
    allOk := allOk && ok
    idx := idx + 1
  return if allOk then 0 else 1
