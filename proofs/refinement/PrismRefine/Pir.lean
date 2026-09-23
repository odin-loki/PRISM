/-
PRISM refinement — the PIR control-flow graph exactly as the C++ engine
represents it (`include/prism/pir.hpp`: `Arg`, `Stmt`, `Phi`, `Term`,
`Block`, `Function`) and its concrete semantics, mirroring the C++
interpreter `prism::pir::interpret` (`src/prism/pir/pir.cpp`):

* variables are indices into `vars` (their widths); parameters come first;
* a block runs its phis as a parallel assignment from the predecessor (the
  first entry for that predecessor; none: the variable keeps its value), then
  its statements, then its terminator;
* `assign d op args` stores `eval_op(...) mod 2^width(d)`; `havoc d` stores
  `0` (the interpreter's choice); `check a` is a violation when bit 0 of `a`
  is 1; `assume a` blocks when it is 0;
* a jump to a block that does not exist stops the run (`Stopped`).

Only the operators the translator emits for the LLVM fragment are modelled;
the checker (`Check.lean`) rejects any other operator.  Arithmetic,
comparison and cast values are `PrismSem.evalBin` / `evalPred` (proofs/
semantics); the i1 test operators (`sadd.ovf`, `shl.nsw.ovf`, ...) are
written as the C++ `eval_op` computes them, and `Ops.lean` proves each one
equals the LangRef condition it is meant to test.
-/
import PrismRefine.Llvm

namespace PrismRefine

open PrismSem

/-- PIR operators (the fragment's subset of `prism::pir::Op`). -/
inductive POp where
  | bin (op : BinOp)
  | cmp (p : Pred)
  | select
  | cast (k : CastK)
  | ovf (o : OvfOp)
  | sdivOvf | shiftOob | shlSOvf | shlNswOvf | shlNuwOvf
  | lostBitsL | lostBitsA | inexactU | inexactS
  /-- `copy` (`Op::Copy`): the translation of `freeze` (`XTranslate.lean`). -/
  | copy
  /-- memory queries (`Op::ObjSize` …): read the memory, so only the
  extended semantics (`XPir.lean`) gives them a value -/
  | objSize | objLive | objKind | objAlign
  deriving DecidableEq, Repr, Inhabited

/-- `Arg`: a constant `(width, bits)` (bits already masked) or variable
`(index, width)`. -/
inductive Arg where
  | c (w bits : Nat)
  | v (i w : Nat)
  deriving DecidableEq, Repr, Inhabited

def Arg.width : Arg → Nat
  | .c w _ => w
  | .v _ w => w

inductive PStmt where
  | assign (d : Nat) (op : POp) (args : List Arg)
  | havoc (d : Nat)
  /-- `check a` with its property name and taxonomy class (as in the C++
  `Stmt::prop` / `Stmt::cls`; the semantics ignores them). -/
  | check (a : Arg) (prop cls : String)
  | assume (a : Arg)
  /-- memory statements (`Stmt::Alloc`, `Load`, `Store`; `XPir.lean`):
  `alloc d size kind init align`; `load d u p` (`u`: some loaded byte is
  uninitialised); `store p v init` -/
  | alloc (d : Nat) (size : Arg) (kind init align : Nat)
  | load (d u : Nat) (p : Arg)
  | store (p v init : Arg)
  deriving DecidableEq, Repr, Inhabited

structure PPhi where
  dst : Nat
  inc : List (Nat × Arg)
  deriving DecidableEq, Repr, Inhabited

inductive PTerm where
  | jmp (t : Nat)
  | br (c : Arg) (t f : Nat)
  | ret (v : Option Arg)
  | stop
  deriving DecidableEq, Repr, Inhabited

structure PBlock where
  phis : List PPhi
  stmts : List PStmt
  term : PTerm
  deriving DecidableEq, Repr, Inhabited

structure PFunc where
  vars : List Nat
  params : List Nat
  retw : Nat
  blocks : List PBlock
  deriving DecidableEq, Repr, Inhabited

abbrev Store := Nat → Nat

def Store.set (σ : Store) (i v : Nat) : Store := fun j => if j = i then v else σ j

def Arg.get (σ : Store) : Arg → Nat
  | .c _ b => b
  | .v i _ => σ i

/-! ### The i1 test operators, as `eval_op` computes them (`xw` = width of
the first argument). -/

def ovfTest (o : OvfOp) (xw x y : Nat) : Bool :=
  let a := bv xw x
  let b := bv xw y
  match o with
  | .sadd => (a + b).toInt != a.toInt + b.toInt
  | .ssub => (a - b).toInt != a.toInt - b.toInt
  | .smul => (a * b).toInt != a.toInt * b.toInt
  | .uadd => decide ((a + b).toNat < a.toNat)
  | .usub => decide (a.toNat < b.toNat)
  | .umul => decide (2 ^ xw - 1 < a.toNat * b.toNat)

def testVal (op : POp) (xw x y : Nat) : Bool :=
  let a := bv xw x
  let b := bv xw y
  match op with
  | .ovf o => ovfTest o xw x y
  | .sdivOvf => a == BitVec.intMin xw && b == BitVec.allOnes xw
  | .shiftOob => decide (xw ≤ b.toNat)
  | .shlSOvf => decide (xw ≤ b.toNat) || a.msb || (a >>> (xw - 1 - b.toNat)) != 0#xw
  | .shlNswOvf => decide (xw ≤ b.toNat) || (a <<< b.toNat).sshiftRight b.toNat != a
  | .shlNuwOvf => decide (xw ≤ b.toNat) || (a <<< b.toNat) >>> b.toNat != a
  | .lostBitsL => decide (xw ≤ b.toNat) || (a >>> b.toNat) <<< b.toNat != a
  | .lostBitsA => decide (xw ≤ b.toNat) || (a.sshiftRight b.toNat) <<< b.toNat != a
  | .inexactU => b != 0#xw && a % b != 0#xw
  | .inexactS =>
    if b == 0#xw then false
    else if a == BitVec.intMin xw && b == BitVec.allOnes xw then false
    else a.srem b != 0#xw
  | _ => false

/-- `eval_op(op, w, args)`: `w` is the width of the destination. -/
def evalOp (σ : Store) (op : POp) (w : Nat) (args : List Arg) : Nat :=
  match op, args with
  | .bin o, [a, b] => binVal o w (a.get σ) (b.get σ)
  | .cmp p, [a, b] => icmpVal p a.width (a.get σ) (b.get σ)
  | .select, [c, a, b] => selVal w (c.get σ) (a.get σ) (b.get σ)
  | .cast k, [a] => castVal k a.width w (a.get σ)
  | .copy, [a] => a.get σ
  | op, [a, b] => (BitVec.ofBool (testVal op a.width (a.get σ) (b.get σ))).toNat
  | _, _ => 0

/-- Width of variable `d`. -/
def PFunc.wd (P : PFunc) (d : Nat) : Nat := P.vars.getD d 0

inductive PRes where
  | ok (σ : Store)
  | fail
  | blocked

def pStmts (P : PFunc) (σ : Store) : List PStmt → PRes
  | [] => .ok σ
  | .assign d op args :: t => pStmts P (σ.set d (evalOp σ op (P.wd d) args % 2 ^ P.wd d)) t
  | .havoc d :: t => pStmts P (σ.set d 0) t
  | .check a _ _ :: t => if truthN (a.get σ) then .fail else pStmts P σ t
  | .assume a :: t => if truthN (a.get σ) then pStmts P σ t else .blocked
  -- the base fragment has no memory: no claim is made about these (`XPir.lean` runs them)
  | .alloc .. :: _ => .blocked
  | .load .. :: _ => .blocked
  | .store .. :: _ => .blocked

def pickInc (prev : Nat) : List (Nat × Arg) → Option Arg
  | [] => none
  | (p, a) :: t => if p = prev then some a else pickInc prev t

/-- Phi updates computed from the store on entry (parallel assignment). -/
def phiUpd (σ : Store) (prev : Option Nat) : List PPhi → List (Nat × Nat)
  | [] => []
  | ph :: t =>
    match prev.bind (fun pv => pickInc pv ph.inc) with
    | some a => (ph.dst, a.get σ) :: phiUpd σ prev t
    | none => phiUpd σ prev t

def Store.setAll (σ : Store) : List (Nat × Nat) → Store
  | [] => σ
  | (i, v) :: t => (σ.set i v).setAll t

/-- Observable outcome of a PIR run (`InterpResult::Status`). -/
inductive POut where
  | ret (v : Option Nat)
  | fail
  | blocked
  | stop
  | fuel
  deriving DecidableEq, Repr

/-- Continue a statement-run result into an outcome. -/
def PRes.run : PRes → (Store → POut) → POut
  | .ok σ, f => f σ
  | .fail, _ => .fail
  | .blocked, _ => .blocked

def pTerm (σ : Store) (run : Nat → Store → POut) : PTerm → POut
  | .jmp t => run t σ
  | .br c t f => run (if truthN (c.get σ) then t else f) σ
  | .ret none => .ret none
  | .ret (some a) => .ret (some (a.get σ))
  | .stop => .stop

def pRun (P : PFunc) : Nat → Option Nat → Nat → Store → POut
  | 0, _, _, _ => .fuel
  | n + 1, prev, cur, σ =>
    match P.blocks[cur]? with
    | none => .stop
    | some B =>
      (pStmts P (σ.setAll (phiUpd σ prev B.phis)) B.stmts).run fun σ' =>
        pTerm σ' (fun j σ'' => pRun P n (some cur) j σ'') B.term

/-- Initial store (`interpret`: `val[params[i]] = args[i] & mask(width)`,
everything else 0). -/
def pInitAux (wd : Nat → Nat) : List Nat → List Nat → Store → Store
  | [], _, σ => σ
  | p :: ps, args, σ => pInitAux wd ps args.tail (σ.set p (args.headD 0 % 2 ^ wd p))

def pInit (P : PFunc) (args : List Nat) : Store := pInitAux P.wd P.params args (fun _ => 0)

def pRunF (P : PFunc) (args : List Nat) (fuel : Nat) : POut :=
  pRun P fuel none 0 (pInit P args)

end PrismRefine
