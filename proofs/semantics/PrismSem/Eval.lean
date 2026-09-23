/-
PRISM PIR — expression semantics and the definition of undefined behaviour
(roadmap Part 5.2; the UB predicate is what Part 8.2 "Property
instrumentation" is measured against).
-/
import PrismSem.Syntax

namespace PrismSem

/-- A (total, well-typed) environment: every variable `x : iw` has a value in
`BitVec w`.  Initial environments are the program inputs. -/
def Env : Type := (w : Nat) → String → BitVec w

/-- Update variable `x : iw`. -/
def Env.set (ρ : Env) (w : Nat) (x : String) (v : BitVec w) : Env :=
  fun w' y => if h : w = w' then (if y = x then h ▸ v else ρ w' y) else ρ w' y

@[simp] theorem Env.set_same (ρ : Env) (w : Nat) (x : String) (v : BitVec w) :
    ρ.set w x v w x = v := by
  simp [Env.set]

theorem Env.set_other (ρ : Env) (w w' : Nat) (x y : String) (v : BitVec w)
    (h : w ≠ w' ∨ y ≠ x) : ρ.set w x v w' y = ρ w' y := by
  unfold Env.set
  split
  · rename_i hw; subst hw; simp at h; simp [h]
  · rfl

/-- Truth value of an `i1`. -/
def truth (v : BitVec 1) : Bool := v.getLsbD 0

@[simp] theorem truth_ofBool (b : Bool) : truth (BitVec.ofBool b) = b := by
  simp [truth]

@[simp] theorem truth_one : truth 1#1 = true := by decide
@[simp] theorem truth_zero : truth 0#1 = false := by decide

@[simp] theorem truth_or (a b : BitVec 1) : truth (a ||| b) = (truth a || truth b) := by
  simp [truth]

@[simp] theorem truth_and (a b : BitVec 1) : truth (a &&& b) = (truth a && truth b) := by
  simp [truth]

@[simp] theorem truth_xor_one (a : BitVec 1) : truth (a ^^^ 1#1) = !truth a := by
  simp [truth]

theorem eq_ofBool_truth (a : BitVec 1) : a = BitVec.ofBool (truth a) := by
  have : ∀ a : BitVec 1, a = BitVec.ofBool (truth a) := by decide
  exact this a

def evalBin {w : Nat} : BinOp → BitVec w → BitVec w → BitVec w
  | .add, a, b => a + b
  | .sub, a, b => a - b
  | .mul, a, b => a * b
  | .udiv, a, b => a / b
  | .sdiv, a, b => a.sdiv b
  | .urem, a, b => a % b
  | .srem, a, b => a.srem b
  | .shl, a, b => a <<< b
  | .lshr, a, b => a >>> b
  | .ashr, a, b => a.sshiftRight' b
  | .and, a, b => a &&& b
  | .or, a, b => a ||| b
  | .xor, a, b => a ^^^ b

def evalPred {w : Nat} : Pred → BitVec w → BitVec w → Bool
  | .eq, a, b => a == b
  | .ne, a, b => a != b
  | .ult, a, b => a.ult b
  | .ule, a, b => a.ule b
  | .ugt, a, b => b.ult a
  | .uge, a, b => b.ule a
  | .slt, a, b => a.slt b
  | .sle, a, b => a.sle b
  | .sgt, a, b => b.slt a
  | .sge, a, b => b.sle a

def evalOvf {w : Nat} : OvfOp → BitVec w → BitVec w → Bool
  | .sadd, a, b => BitVec.saddOverflow a b
  | .ssub, a, b => BitVec.ssubOverflow a b
  | .smul, a, b => BitVec.smulOverflow a b
  | .uadd, a, b => BitVec.uaddOverflow a b
  | .usub, a, b => BitVec.usubOverflow a b
  | .umul, a, b => BitVec.umulOverflow a b

/-- Value of an expression.  Evaluation is total; whether the evaluation
performed an operation with undefined behaviour is the separate predicate
`ubE`. -/
def evalE (ρ : Env) : {w : Nat} → Expr w → BitVec w
  | _, .const v => v
  | _, .var w x => ρ w x
  | _, .bin op _ a b => evalBin op (evalE ρ a) (evalE ρ b)
  | _, .icmp p a b => BitVec.ofBool (evalPred p (evalE ρ a) (evalE ρ b))
  | _, .ovf op a b => BitVec.ofBool (evalOvf op (evalE ρ a) (evalE ρ b))
  | _, .select c a b => if truth (evalE ρ c) then evalE ρ a else evalE ρ b
  | _, .zext v a => (evalE ρ a).setWidth v
  | _, .sext v a => (evalE ρ a).signExtend v
  | _, .trunc v a => (evalE ρ a).setWidth v

/-- The undefined-behaviour condition of one operation, given its operand
values.  This is the *definition* of UB for PIR arithmetic:

* `add/sub/mul nsw`: signed overflow (`BitVec.saddOverflow`, ...);
* `add/sub/mul nuw`: unsigned overflow;
* `udiv/urem` (not `total`): divisor zero;
* `sdiv/srem` (not `total`): divisor zero, or `INT_MIN / -1`;
* `shl/lshr/ashr` (not `total`): shift amount `>= w` (as unsigned). -/
def ubBin {w : Nat} (op : BinOp) (fl : Flags) (a b : BitVec w) : Bool :=
  match op with
  | .add => (fl.nsw && BitVec.saddOverflow a b) || (fl.nuw && BitVec.uaddOverflow a b)
  | .sub => (fl.nsw && BitVec.ssubOverflow a b) || (fl.nuw && BitVec.usubOverflow a b)
  | .mul => (fl.nsw && BitVec.smulOverflow a b) || (fl.nuw && BitVec.umulOverflow a b)
  | .udiv | .urem => !fl.total && b == 0#w
  | .sdiv | .srem => !fl.total && (b == 0#w || (a == BitVec.intMin w && b == BitVec.allOnes w))
  | .shl | .lshr | .ashr => !fl.total && decide (w ≤ b.toNat)
  | .and | .or | .xor => false

/-- `ubE ρ e`: evaluating `e` in `ρ` performs an operation with undefined
behaviour.  All operands are evaluated (including both arms of `select`). -/
def ubE (ρ : Env) : {w : Nat} → Expr w → Bool
  | _, .const _ => false
  | _, .var _ _ => false
  | _, .bin op fl a b => ubE ρ a || ubE ρ b || ubBin op fl (evalE ρ a) (evalE ρ b)
  | _, .icmp _ a b => ubE ρ a || ubE ρ b
  | _, .ovf _ a b => ubE ρ a || ubE ρ b
  | _, .select c a b => ubE ρ c || ubE ρ a || ubE ρ b
  | _, .zext _ a => ubE ρ a
  | _, .sext _ a => ubE ρ a
  | _, .trunc _ a => ubE ρ a

/-! ### UB-free copies and syntactic UB conditions -/

/-- Erase all UB-carrying flags: `nsw`/`nuw` off, `total` on.  Same value,
never UB. -/
def Expr.erase : {w : Nat} → Expr w → Expr w
  | _, .const v => .const v
  | _, .var w x => .var w x
  | _, .bin op _ a b => .bin op { nsw := false, nuw := false, total := true } a.erase b.erase
  | _, .icmp p a b => .icmp p a.erase b.erase
  | _, .ovf op a b => .ovf op a.erase b.erase
  | _, .select c a b => .select c.erase a.erase b.erase
  | _, .zext v a => .zext v a.erase
  | _, .sext v a => .sext v a.erase
  | _, .trunc v a => .trunc v a.erase

@[simp] theorem evalE_erase (ρ : Env) {w : Nat} (e : Expr w) :
    evalE ρ e.erase = evalE ρ e := by
  induction e <;> simp_all [Expr.erase, evalE]

@[simp] theorem ubE_erase (ρ : Env) {w : Nat} (e : Expr w) :
    ubE ρ e.erase = false := by
  induction e with
  | bin op fl a b iha ihb => cases op <;> simp_all [Expr.erase, ubE, ubBin]
  | _ => simp_all [Expr.erase, ubE]

/-- The UB condition of a single operation as a PIR expression over the
(already UB-free) operand expressions. -/
def ubBinExpr {w : Nat} (op : BinOp) (fl : Flags) (a b : Expr w) : Expr 1 :=
  let ovfPair (s u : OvfOp) : Expr 1 :=
    Expr.or' (if fl.nsw then .ovf s a b else Expr.ff) (if fl.nuw then .ovf u a b else Expr.ff)
  let zero : Expr 1 := .icmp .eq b (.const 0#w)
  match op with
  | .add => ovfPair .sadd .uadd
  | .sub => ovfPair .ssub .usub
  | .mul => ovfPair .smul .umul
  | .udiv | .urem => if fl.total then Expr.ff else zero
  | .sdiv | .srem =>
      if fl.total then Expr.ff else
      Expr.or' zero
        (Expr.and' (.icmp .eq a (.const (BitVec.intMin w))) (.icmp .eq b (.const (BitVec.allOnes w))))
  | .shl | .lshr | .ashr =>
      if fl.total then Expr.ff else .icmp .uge b (.const (BitVec.ofNat w w))
  | .and | .or | .xor => Expr.ff

/-- `ubExpr e : i1` is the PIR expression that is true exactly when
evaluating `e` has undefined behaviour (theorem `ubExpr_correct`); it is
itself UB-free (theorem `ubE_ubExpr`).  This is what the instrumentation
asserts the negation of. -/
def ubExpr : {w : Nat} → Expr w → Expr 1
  | _, .const _ => Expr.ff
  | _, .var _ _ => Expr.ff
  | _, .bin op fl a b => Expr.or' (Expr.or' (ubExpr a) (ubExpr b)) (ubBinExpr op fl a.erase b.erase)
  | _, .icmp _ a b => Expr.or' (ubExpr a) (ubExpr b)
  | _, .ovf _ a b => Expr.or' (ubExpr a) (ubExpr b)
  | _, .select c a b => Expr.or' (Expr.or' (ubExpr c) (ubExpr a)) (ubExpr b)
  | _, .zext _ a => ubExpr a
  | _, .sext _ a => ubExpr a
  | _, .trunc _ a => ubExpr a

theorem ubBinExpr_correct (ρ : Env) {w : Nat} (op : BinOp) (fl : Flags) (a b : Expr w) :
    truth (evalE ρ (ubBinExpr op fl a b)) = ubBin op fl (evalE ρ a) (evalE ρ b) := by
  cases op <;> cases fl with
  | mk nsw nuw total =>
    cases nsw <;> cases nuw <;> cases total <;>
      simp [ubBinExpr, ubBin, evalE, evalBin, evalPred, evalOvf, Expr.or', Expr.and', Expr.ff,
        BitVec.ule]

theorem ubE_ubBinExpr (ρ : Env) {w : Nat} (op : BinOp) (fl : Flags) (a b : Expr w) :
    ubE ρ (ubBinExpr op fl a.erase b.erase) = false := by
  cases op <;> cases fl with
  | mk nsw nuw total =>
    cases nsw <;> cases nuw <;> cases total <;>
      simp [ubBinExpr, ubE, ubBin, Expr.or', Expr.and', Expr.ff]

/-- **Syntactic UB condition is exact.** -/
theorem ubExpr_correct (ρ : Env) {w : Nat} (e : Expr w) :
    truth (evalE ρ (ubExpr e)) = ubE ρ e := by
  induction e with
  | bin op fl a b iha ihb =>
    simp [ubExpr, ubE, evalE, evalBin, Expr.or', iha, ihb, ubBinExpr_correct]
  | _ => simp_all [ubExpr, ubE, evalE, evalBin, Expr.or', Expr.ff]

/-- **The UB check cannot itself have UB.** -/
theorem ubE_ubExpr (ρ : Env) {w : Nat} (e : Expr w) : ubE ρ (ubExpr e) = false := by
  induction e with
  | bin op fl a b iha ihb =>
    simp [ubExpr, ubE, ubBin, Expr.or', iha, ihb, ubE_ubBinExpr]
  | _ => simp_all [ubExpr, ubE, ubBin, Expr.or', Expr.ff]

end PrismSem
