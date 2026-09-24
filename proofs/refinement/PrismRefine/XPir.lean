/-
PRISM refinement, extended fragment — PIR with nondeterministic `havoc`.

`Pir.lean` runs `havoc d` as `d := 0` (the C++ interpreter's choice).  The
verifier treats a havocked variable as arbitrary, so for the extended
fragment (`freeze`/`undef`, where the LLVM value is itself arbitrary) the PIR
semantics draws it from the same oracle as the LLVM semantics
(`XLlvm.lean`): the `t`-th `havoc` of a run stores `ω t mod 2^width`.
Everything else is `Pir.lean`'s semantics (`evalOp`, phis, terminators).
-/
import PrismRefine.XMem

namespace PrismRefine

open PrismSem

inductive XPRes where
  | ok (σ : Store) (t : World)
  | fail
  | blocked

/-- The value of an operator; the memory queries read the memory (at the
64-bit pointer, as the C++ engine's `uint64_t`). -/
def evalOpM (m : Mem) (σ : Store) (op : POp) (w : Nat) (args : List Arg) : Nat :=
  match op, args with
  | .objSize, [a] => m.size (a.get σ % 2 ^ 64)
  | .objLive, [a] => if m.live (a.get σ % 2 ^ 64) then 1 else 0
  | .objKind, [a] => m.kind (a.get σ % 2 ^ 64)
  | .objAlign, [a] => m.align (a.get σ % 2 ^ 64)
  | _, _ => evalOp σ op w args



def xStmts (P : PFunc) (ω : Nat → Nat) : Store → World → List PStmt → XPRes
  | σ, t, [] => .ok σ t
  | σ, t, .assign d op args :: r => xStmts P ω (σ.set d (evalOpM t.mem σ op (P.wd d) args % 2 ^ P.wd d)) t r
  | σ, t, .havoc d :: r => xStmts P ω (σ.set d (ω t.t % 2 ^ P.wd d)) (t.adv 1) r
  | σ, t, .check a _ _ :: r => if truthN (a.get σ) then .fail else xStmts P ω σ t r
  | σ, t, .assume a :: r => if truthN (a.get σ) then xStmts P ω σ t r else .blocked
  | σ, t, .alloc d size kind init align :: r =>
    xStmts P ω (σ.set d ((t.allocW ω (size.get σ % 2 ^ 64) kind align init).2 % 2 ^ P.wd d))
      (t.allocW ω (size.get σ % 2 ^ 64) kind align init).1 r
  | σ, t, .load d u p :: r =>
    xStmts P ω ((σ.set d (bytesVal ((loadCells t.mem (p.get σ) (P.wd d)).map (·.getD 0)) % 2 ^ P.wd d)).set u
      ((if (loadCells t.mem (p.get σ) (P.wd d)).any (·.isNone) then 1 else 0) % 2 ^ P.wd u)) t r
  | σ, t, .store p v init :: r =>
    xStmts P ω σ (t.store (p.get σ) (v.get σ) v.width (truthN (init.get σ))) r
  | σ, t, .free p :: r => xStmts P ω σ { t with mem := t.mem.free (p.get σ % 2 ^ 64) } r
  | σ, t, .memcpy d s n :: r =>
    xStmts P ω σ { t with mem := t.mem.copy (d.get σ % 2 ^ 64) (s.get σ % 2 ^ 64) (n.get σ % 2 ^ 64) } r
  | σ, t, .memset d b n :: r =>
    xStmts P ω σ { t with mem := t.mem.fill (d.get σ % 2 ^ 64) (b.get σ % 256) (n.get σ % 2 ^ 64) } r

def XPRes.run : XPRes → (Store → World → POut) → POut
  | .ok σ t, f => f σ t
  | .fail, _ => .fail
  | .blocked, _ => .blocked

def xpRun (P : PFunc) (ω : Nat → Nat) : Nat → World → Option Nat → Nat → Store → POut
  | 0, _, _, _, _ => .fuel
  | n + 1, t, prev, cur, σ =>
    match P.blocks[cur]? with
    | none => .stop
    | some B =>
      (xStmts P ω (σ.setAll (phiUpd σ prev B.phis)) t B.stmts).run fun σ' t' =>
        pTerm σ' (fun j σ'' => xpRun P ω n t' (some cur) j σ'') B.term

def xpRunF (P : PFunc) (args : List Nat) (ω : Nat → Nat) (fuel : Nat) : POut :=
  xpRun P ω fuel World.init none 0 (pInit P args)

end PrismRefine
