/-
PRISM refinement, extended fragment — PIR with nondeterministic `havoc`.

`Pir.lean` runs `havoc d` as `d := 0` (the C++ interpreter's choice).  The
verifier treats a havocked variable as arbitrary, so for the extended
fragment (`freeze`/`undef`, where the LLVM value is itself arbitrary) the PIR
semantics draws it from the same oracle as the LLVM semantics
(`XLlvm.lean`): the `t`-th `havoc` of a run stores `ω t mod 2^width`.
Everything else is `Pir.lean`'s semantics (`evalOp`, phis, terminators).
-/
import PrismRefine.Pir

namespace PrismRefine

open PrismSem

inductive XPRes where
  | ok (σ : Store) (t : Nat)
  | fail
  | blocked

def xStmts (P : PFunc) (ω : Nat → Nat) : Store → Nat → List PStmt → XPRes
  | σ, t, [] => .ok σ t
  | σ, t, .assign d op args :: r => xStmts P ω (σ.set d (evalOp σ op (P.wd d) args % 2 ^ P.wd d)) t r
  | σ, t, .havoc d :: r => xStmts P ω (σ.set d (ω t % 2 ^ P.wd d)) (t + 1) r
  | σ, t, .check a _ _ :: r => if truthN (a.get σ) then .fail else xStmts P ω σ t r
  | σ, t, .assume a :: r => if truthN (a.get σ) then xStmts P ω σ t r else .blocked

def XPRes.run : XPRes → (Store → Nat → POut) → POut
  | .ok σ t, f => f σ t
  | .fail, _ => .fail
  | .blocked, _ => .blocked

def xpRun (P : PFunc) (ω : Nat → Nat) : Nat → Nat → Option Nat → Nat → Store → POut
  | 0, _, _, _, _ => .fuel
  | n + 1, t, prev, cur, σ =>
    match P.blocks[cur]? with
    | none => .stop
    | some B =>
      (xStmts P ω (σ.setAll (phiUpd σ prev B.phis)) t B.stmts).run fun σ' t' =>
        pTerm σ' (fun j σ'' => xpRun P ω n t' (some cur) j σ'') B.term

def xpRunF (P : PFunc) (args : List Nat) (ω : Nat → Nat) (fuel : Nat) : POut :=
  xpRun P ω fuel 0 none 0 (pInit P args)

end PrismRefine
