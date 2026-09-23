/-
PRISM PIR — syntax (roadmap Part 2.4, Part 5.2).

This is a Lean model of the *design* of PIR, the PRISM intermediate
representation: a small typed language whose only value type (in this layer)
is the fixed-width bitvector `BitVec w`.  Booleans are `BitVec 1`, exactly as
LLVM's `i1`.

Expressions are indexed by their width, so every expression is well typed by
construction; there is no separate type checker to trust.

Variables are identified by a name *and* a width (`Expr.var w x`); `x : i8`
and `x : i32` are different variables.  This is what makes the environment a
total, well-typed function (see `PrismSem.Env`).
-/

namespace PrismSem

/-- Two-operand bitvector operations (LLVM `binop` opcodes). -/
inductive BinOp where
  | add | sub | mul
  | udiv | sdiv | urem | srem
  | shl | lshr | ashr
  | and | or | xor
  deriving DecidableEq, Repr

/-- Integer comparison predicates (LLVM `icmp`). -/
inductive Pred where
  | eq | ne | ult | ule | ugt | uge | slt | sle | sgt | sge
  deriving DecidableEq, Repr

/-- Overflow predicates (LLVM `*.with.overflow` intrinsics).  They are total
and have no undefined behaviour; the UB instrumentation uses them to *state*
the overflow condition of an `nsw`/`nuw` operation as a PIR expression. -/
inductive OvfOp where
  | sadd | ssub | smul | uadd | usub | umul
  deriving DecidableEq, Repr

/-- Operation flags.

* `nsw`/`nuw` on `add`/`sub`/`mul` (LLVM `nsw`/`nuw`): a flagged operation
  whose result wraps is *undefined behaviour* (C signed overflow is emitted as
  `nsw`).  Unflagged operations wrap silently (two's complement).
* `total` on `udiv`/`sdiv`/`urem`/`srem`/`shl`/`lshr`/`ashr`: by default
  (`total = false`) division by zero, `INT_MIN / -1` and a shift amount
  `>= w` are undefined behaviour, as in C.  `total = true` selects the
  SMT-LIB-style total operation (no UB; value as Lean's `BitVec`).  The front
  end never emits `total`; PRISM's own instrumentation uses it so that the
  inserted checks cannot themselves have UB (see `Expr.erase`). -/
structure Flags where
  nsw : Bool := false
  nuw : Bool := false
  total : Bool := false
  deriving DecidableEq, Repr

/-- PIR expressions of width `w`. -/
inductive Expr : Nat → Type where
  /-- A bitvector literal. -/
  | const {w : Nat} (v : BitVec w) : Expr w
  /-- A variable `x : iw`. -/
  | var (w : Nat) (x : String) : Expr w
  /-- `a op b` with optional `nsw`/`nuw` flags. -/
  | bin {w : Nat} (op : BinOp) (fl : Flags) (a b : Expr w) : Expr w
  /-- `icmp p a b : i1`. -/
  | icmp {w : Nat} (p : Pred) (a b : Expr w) : Expr 1
  /-- Overflow test `op.with.overflow(a, b).1 : i1`. -/
  | ovf {w : Nat} (op : OvfOp) (a b : Expr w) : Expr 1
  /-- `select c a b`.  Both arms are evaluated (LLVM `select` operands are
  already-computed values), so UB in either arm is UB of the select. -/
  | select {w : Nat} (c : Expr 1) (a b : Expr w) : Expr w
  /-- `zext`, `sext`, `trunc` to width `v`. -/
  | zext {w : Nat} (v : Nat) (a : Expr w) : Expr v
  | sext {w : Nat} (v : Nat) (a : Expr w) : Expr v
  | trunc {w : Nat} (v : Nat) (a : Expr w) : Expr v

/-- Who inserted an assertion: the user (`assert` in the source, or a
contract) or PRISM's UB instrumentation. -/
inductive Tag where
  | user
  | ub
  deriving DecidableEq, Repr

/-- PIR statements (control-flow layer; memory is in `PrismSem.Memory`). -/
inductive Stmt where
  | skip
  /-- `x : iw := e` -/
  | assign {w : Nat} (x : String) (e : Expr w)
  /-- `assert c` — failure is an error outcome. -/
  | assert (t : Tag) (c : Expr 1)
  /-- `assume c` — a violated assumption blocks the execution (it is not an
  error and not a normal result). -/
  | assume (c : Expr 1)
  | seq (s t : Stmt)
  | ite (c : Expr 1) (s t : Stmt)
  /-- `while c do body`. -/
  | loop (c : Expr 1) (body : Stmt)
  /-- `return` (the environment at that point is the result). -/
  | ret

/-! ### Boolean helpers on `i1` expressions (used by the encoder). -/

namespace Expr

def tt : Expr 1 := .const 1#1
def ff : Expr 1 := .const 0#1
def and' (a b : Expr 1) : Expr 1 := .bin .and {} a b
def or' (a b : Expr 1) : Expr 1 := .bin .or {} a b
def not' (a : Expr 1) : Expr 1 := .bin .xor {} a tt

end Expr

/-! ### Syntactic fragments, used to state the layered theorems. -/

/-- Straight-line code: no branches, no loops. -/
def Stmt.StraightLine : Stmt → Prop
  | .seq s t => s.StraightLine ∧ t.StraightLine
  | .ite .. => False
  | .loop .. => False
  | _ => True

/-- Loop-free code: branches allowed, loops not. -/
def Stmt.LoopFree : Stmt → Prop
  | .seq s t => s.LoopFree ∧ t.LoopFree
  | .ite _ s t => s.LoopFree ∧ t.LoopFree
  | .loop .. => False
  | _ => True

/-- The program contains no assertion tagged `.ub` (the user never writes
PRISM's own instrumentation tag). -/
def Stmt.NoUbTags : Stmt → Prop
  | .assert t _ => t = .user
  | .seq s t => s.NoUbTags ∧ t.NoUbTags
  | .ite _ s t => s.NoUbTags ∧ t.NoUbTags
  | .loop _ b => b.NoUbTags
  | _ => True

end PrismSem
