/-
Non-vacuity checks: concrete programs on which the definitions compute the
expected outcome, and on which the encoder's formula evaluates as the
theorems say.  These are evaluated by the Lean kernel (`decide`/`rfl`), not
by `native_decide`, so they add no axioms.
-/
import PrismSem.MemEncode

namespace PrismSem.Examples

open PrismSem

/-- Input environment with `x : i8 = v`, everything else 0. -/
def envX (v : BitVec 8) : Env := Env.set (fun _ _ => 0) 8 "x" v

/-- `y := x +nsw 1` — C `y = x + 1` on `signed char` after promotion is not
UB, but on `int` it is; here the `i8` add is flagged `nsw`. -/
def incr : Stmt := .assign "y" (.bin .add { nsw := true } (.var 8 "x") (.const 1#8))

/-- Signed overflow is UB at `x = 127` … -/
example : (run 0 incr (envX 127#8)).isUb = true := by decide
/-- … and not at `x = 5`. -/
example : ubE (envX 5#8) (.bin .add { nsw := true } (.var 8 "x") (.const 1#8)) = false := by decide

/-- The encoder's bounded VC is true exactly at the overflowing input. -/
example : truth (evalE (envX 127#8) (vcBounded 0 incr)) = true := by decide
example : truth (evalE (envX 5#8) (vcBounded 0 incr)) = false := by decide

/-- `x / (x - x)` : division by zero on every input. -/
def divz : Stmt :=
  .assign "y" (.bin .udiv {} (.var 8 "x") (.bin .sub {} (.var 8 "x") (.var 8 "x")))

example : truth (evalE (envX 3#8) (vcBounded 0 divz)) = true := by decide

/-- `while (x != 0) x := x - 1` needs `x` iterations. -/
def countdown : Stmt :=
  .loop (.icmp .ne (.var 8 "x") (.const 0#8)) (.assign "x" (.bin .sub {} (.var 8 "x") (.const 1#8)))

/-- With bound 3, input 3 finishes and input 4 violates the unwinding
assertion. -/
example : truth (evalE (envX 3#8) (encode 3 countdown).uw) = false := by decide
example : truth (evalE (envX 4#8) (encode 3 countdown).uw) = true := by decide

/-- The instrumented program fails its inserted check at the overflowing
input. -/
example : truth (evalE (envX 127#8) (encode 0 (instr incr)).fl) = true := by decide

end PrismSem.Examples
