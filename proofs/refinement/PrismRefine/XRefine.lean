/-
PRISM refinement, extended fragment — the translation is correct.

For every module `M`, analysed function `F`, PIR function `P` and
certificate `C` with `validB M F P C` (the check `pir_lean_check` runs on
every `agree-ext` function), every argument vector, every oracle `ω` (the
arbitrary values of `freeze undef`, shared by both sides) and every fuel
bound:

* `translateX_exact`: strict LLVM returns `v` ⇒ PIR returns `v`; strict
  LLVM reaches UB ⇒ a PIR check fails; strict LLVM runs out of fuel ⇒ PIR
  does (strict *stuck*: no claim).

The proof is a lock-step simulation: one machine step (a block segment of
the top frame, `XLlvm.lean`) is one PIR block.  The invariant (`InvX`) maps
the top frame to its instance in the certificate — its registers to the
instance's `Arg`s (`RelX`, as `Rel` in `Refine.lean` but through the
instance's name map), its position to the PIR block of the segment, and the
PIR predecessor to what the phis (or the continuation phis) of that block
need — and every suspended caller to the instance that called (`TailX`):
their registers are still related because an instance only writes
variables at or above its `lo`, and every caller reads only below its `hi
≤` the callee's `lo`.
-/
import PrismRefine.Refine
import PrismRefine.XPir
import PrismRefine.XValid
import PrismRefine.XMemSim
import PrismRefine.XMemOps

namespace PrismRefine

open PrismSem

/-! ## Lookups and arguments -/

theorem look_mem {α : Type} : ∀ {l : List (String × α)} {n : String} {a : α},
    look l n = some a → (n, a) ∈ l
  | [], _, _, h => by simp [look] at h
  | (m, b) :: t, n, a, h => by
    simp only [look] at h
    split at h
    · rename_i hm; subst hm; simp at h; subst h; simp
    · exact List.mem_cons_of_mem _ (look_mem h)

theorem Ctx.uniq_env {c : Ctx} {d : String} {i : Nat} (h : c.uniq d i = true) {m : String} {a : Arg}
    (hm : look c.env m = some a) (hne : m ≠ d) : a.var? ≠ some i := by
  unfold Ctx.uniq at h
  rw [Bool.and_eq_true, List.all_eq_true] at h
  have := h.1 _ (look_mem hm)
  simp only [Bool.or_eq_true, beq_iff_eq, bne_iff_ne, ne_eq] at this
  rcases this with h1 | h1
  · exact absurd h1 hne
  · exact h1

theorem Ctx.uniq_sh {c : Ctx} {d : String} {i : Nat} (h : c.uniq d i = true) {m : String} {s : Arg}
    (hm : look c.sh m = some s) : s.var? ≠ some i := by
  unfold Ctx.uniq at h
  rw [Bool.and_eq_true, List.all_eq_true, List.all_eq_true] at h
  have := h.2 _ (look_mem hm)
  simpa using this

theorem Ctx.uniqS_env {c : Ctx} {d : String} {s : Nat} (h : c.uniqS d s = true) {m : String} {a : Arg}
    (hm : look c.env m = some a) : a.var? ≠ some s := by
  unfold Ctx.uniqS at h
  rw [Bool.and_eq_true, List.all_eq_true] at h
  have := h.1 _ (look_mem hm)
  simpa using this

theorem Ctx.uniqS_sh {c : Ctx} {d : String} {s : Nat} (h : c.uniqS d s = true) {m : String} {a : Arg}
    (hm : look c.sh m = some a) (hne : m ≠ d) : a.var? ≠ some s := by
  unfold Ctx.uniqS at h
  rw [Bool.and_eq_true, List.all_eq_true, List.all_eq_true] at h
  have := h.2 _ (look_mem hm)
  simp only [Bool.or_eq_true, beq_iff_eq, bne_iff_ne, ne_eq] at this
  rcases this with h1 | h1
  · exact absurd h1 hne
  · exact h1

theorem Arg.lt_below {m : Nat} : ∀ {a : Arg}, a.lt m = true → a.below m
  | .c _ _, _ => trivial
  | .v _ _, h => by simpa [Arg.lt, Arg.below] using h

@[simp] theorem Arg.setW_get (a : Arg) (w : Nat) (σ : Store) : (a.setW w).get σ = a.get σ := by
  cases a <;> rfl

@[simp] theorem Arg.setW_width (a : Arg) (w : Nat) : (a.setW w).width = w := by
  cases a <;> rfl

theorem Arg.setW_below {k : Nat} (w : Nat) : ∀ {a : Arg}, a.below k → (a.setW w).below k
  | .c _ _, h => h
  | .v _ _, h => h

theorem Arg.get_set_ne {i : Nat} (σ : Store) (v : Nat) : ∀ {a : Arg}, a.var? ≠ some i →
    a.get (σ.set i v) = a.get σ
  | .c _ _, _ => rfl
  | .v j _, h => by
    simp only [Arg.var?, ne_eq, Option.some.injEq] at h
    exact Store.set_other _ _ h

theorem Ctx.shOf_some {c : Ctx} {n : String} {s : Arg} (h : c.shOf n = some s) : look c.sh n = some s := by
  unfold Ctx.shOf at h; split at h <;> simp_all

theorem Ctx.shOf_var {c : Ctx} {n : String} {i w : Nat} (h : look c.env n = some (.v i w)) :
    c.shOf n = look c.sh n := by
  simp [Ctx.shOf, h]

/-! ## Registers and variables -/

/-- A register value and its image (as `ShOK` in `Refine.lean`, for an
`Arg`). -/
def ShOKX (sh : Option Arg) (σ : Store) (a : Arg) : SV → Prop
  | .val v => a.get σ = v ∧ ∀ s, sh = some s → truthN (s.get σ) = false
  | .ind => ∃ s, sh = some s ∧ truthN (s.get σ) = true

def RelX (c : Ctx) (R : SRegs) (σ : Store) : Prop :=
  ∀ n x, R n = some x → ∃ a, look c.env n = some a ∧ ShOKX (c.shOf n) σ a x

/-- What the certificate check guarantees of an instance's context. -/
structure CtxOK (P : PFunc) (c : Ctx) : Prop where
  vars : c.vars = P.vars
  env : ∀ n a, look c.env n = some a → a.below c.hi
  sh : ∀ n s, look c.sh n = some s → s.below c.hi
  lohi : c.lo ≤ c.hi

theorem ShOKX.transfer {sh : Option Arg} {σ σ' : Store} {a : Arg} {x : SV} (h : ShOKX sh σ a x)
    (ha : a.get σ' = a.get σ) (hs : ∀ s, sh = some s → s.get σ' = s.get σ) : ShOKX sh σ' a x := by
  cases x with
  | val v =>
    obtain ⟨h1, h2⟩ := h
    exact ⟨by rw [ha, h1], fun s hs' => by rw [hs s hs']; exact h2 s hs'⟩
  | ind =>
    obtain ⟨s, hs', h1⟩ := h
    exact ⟨s, hs', by rw [hs s hs']; exact h1⟩

theorem RelX.agree {P : PFunc} {c : Ctx} (hc : CtxOK P c) {R : SRegs} {σ σ' : Store}
    (h : RelX c R σ) {m : Nat} (hm : c.hi ≤ m) (ha : ∀ j, j < m → σ' j = σ j) : RelX c R σ' := by
  intro n x hn
  obtain ⟨a, hl, hx⟩ := h n x hn
  refine ⟨a, hl, hx.transfer (Arg.get_agree ha (Arg.below_mono hm (hc.env n a hl))) ?_⟩
  intro s hs
  exact Arg.get_agree ha (Arg.below_mono hm (hc.sh n s (Ctx.shOf_some hs)))

/-- The indices written for register `d`: its variable and its shadow
variable. -/
def OwnsX (c : Ctx) (d : String) (i j : Nat) : Prop := j = i ∨ ∃ w, look c.sh d = some (.v j w)

theorem RelX.put {c : Ctx} {R : SRegs} {σ σ' : Store} (h : RelX c R σ) {d : String} {i w : Nat}
    (hd : look c.env d = some (.v i w)) (hu : c.uniq d i = true)
    (hus : ∀ s w', look c.sh d = some (.v s w') → c.uniqS d s = true)
    {x : SV} (hx : ShOKX (c.shOf d) σ' (.v i w) x) (hag : ∀ j, ¬ OwnsX c d i j → σ' j = σ j) :
    RelX c (R.put d x) σ' := by
  intro m y hm
  by_cases e : m = d
  · subst e; simp only [SRegs.put, ite_true, Option.some.injEq] at hm; subst hm; exact ⟨_, hd, hx⟩
  · simp only [SRegs.put, e, ite_false] at hm
    obtain ⟨a, hl, hy⟩ := h m y hm
    refine ⟨a, hl, hy.transfer ?_ ?_⟩
    · cases a with
      | c _ _ => rfl
      | v j _ =>
        simp only [Arg.get]; apply hag
        rintro (hj | ⟨w', hs⟩)
        · exact Ctx.uniq_env hu hl e (by simp [Arg.var?, hj])
        · exact Ctx.uniqS_env (hus _ _ hs) hl rfl
    · intro s hs
      have hs' := Ctx.shOf_some hs
      cases s with
      | c _ _ => rfl
      | v j _ =>
        simp only [Arg.get]; apply hag
        rintro (hj | ⟨w', hs2⟩)
        · exact Ctx.uniq_sh hu hs' (by simp [Arg.var?, hj])
        · exact Ctx.uniqS_sh (hus _ _ hs2) hs' e rfl

theorem RelX.set {c : Ctx} {R : SRegs} {σ : Store} (h : RelX c R σ) {d : String} {i w : Nat}
    (hd : look c.env d = some (.v i w)) (hu : c.uniq d i = true) (hsh : look c.sh d = none) (v : Nat) :
    RelX c (R.set d v) (σ.set i v) := by
  refine RelX.put h hd hu (fun s w' hs => by rw [hsh] at hs; cases hs) ?_ ?_
  · rw [Ctx.shOf_var hd, hsh]; exact ⟨by simp [Arg.get], fun s hs => by cases hs⟩
  · intro j hj
    exact Store.set_other _ _ (fun e => hj (.inl e))

theorem dstX_ok {c : Ctx} {d : String} {w i : Nat} (h : dstX c d w = .ok i) :
    look c.env d = some (.v i w) ∧ c.lo ≤ i ∧ i < c.hi ∧ c.vars.getD i 0 = w ∧ c.uniq d i = true := by
  unfold dstX at h
  split at h
  · rename_i i' w' hl
    split at h
    · rename_i hc
      simp only [Except.ok.injEq] at h; subst h
      obtain ⟨rfl, h1, h2, h3, h4⟩ := hc
      exact ⟨hl, h1, h2, h3, h4⟩
    · simp at h
  · simp at h

theorem CtxOK.wd {P : PFunc} {c : Ctx} (hc : CtxOK P c) {i w : Nat} (h : c.vars.getD i 0 = w) :
    P.wd i = w := by
  rw [← h, hc.vars]; rfl

/-! ## Statement runs with the oracle -/

def PRes.toX : PRes → World → XPRes
  | .ok σ, t => .ok σ t
  | .fail, _ => .fail
  | .blocked, _ => .blocked

def POp.isMem : POp → Bool
  | .objSize | .objLive | .objKind | .objAlign => true
  | _ => false

/-- Statements that neither draw from the oracle nor touch the memory. -/
def PStmt.pure : PStmt → Bool
  | .assign _ op _ => !op.isMem
  | .check .. | .assume .. => true
  | _ => false

theorem evalOpM_pure (m : Mem) (σ : Store) {op : POp} (h : op.isMem = false) (w : Nat) (args : List Arg) :
    evalOpM m σ op w args = evalOp σ op w args := by
  cases op <;> simp [POp.isMem] at h <;> rfl

/-- Pure statements run as in `Pir.lean`. -/
theorem xStmts_pure (P : PFunc) (ω : Nat → Nat) : ∀ (σ : Store) (t : World) (l : List PStmt),
    (∀ s ∈ l, s.pure = true) → xStmts P ω σ t l = (pStmts P σ l).toX t
  | σ, t, [], _ => rfl
  | σ, t, .assign d op args :: r, h => by
    have hop : op.isMem = false := by simpa [PStmt.pure] using h _ List.mem_cons_self
    simp only [xStmts, pStmts, evalOpM_pure _ _ hop]
    exact xStmts_pure P ω _ t r (fun s hs => h s (List.mem_cons_of_mem _ hs))
  | σ, t, .havoc d :: r, h => by simp [PStmt.pure] at h
  | σ, t, .check a _ _ :: r, h => by
    simp only [xStmts, pStmts]; split
    · rfl
    · exact xStmts_pure P ω σ t r (fun s hs => h s (List.mem_cons_of_mem _ hs))
  | σ, t, .assume a :: r, h => by
    simp only [xStmts, pStmts]; split
    · exact xStmts_pure P ω σ t r (fun s hs => h s (List.mem_cons_of_mem _ hs))
    · rfl
  | σ, t, .alloc .. :: r, h => by simp [PStmt.pure] at h
  | σ, t, .load .. :: r, h => by simp [PStmt.pure] at h
  | σ, t, .store .. :: r, h => by simp [PStmt.pure] at h
  | σ, t, .free .. :: r, h => by simp [PStmt.pure] at h
  | σ, t, .memcpy .. :: r, h => by simp [PStmt.pure] at h
  | σ, t, .memset .. :: r, h => by simp [PStmt.pure] at h

/-- A check whose test operator is not a memory query. -/
def Chk.opOK : Chk → Bool
  | .p op _ _ _ _ => !op.isMem
  | .disj .. => true

theorem emitAll_pure : ∀ (cs : List Chk) (k : Nat), (∀ c ∈ cs, c.opOK = true) →
    ∀ s ∈ (emitAll k cs).1, s.pure = true
  | [], _, _ => by simp [emitAll]
  | c :: cs, k, h => by
    simp only [emitAll, List.mem_append]
    intro s hs
    rcases hs with hs | hs
    · have := h c List.mem_cons_self
      cases c with
      | p op a b pr cl =>
        simp [Chk.emit] at hs
        rcases hs with rfl | rfl <;> simp_all [PStmt.pure, Chk.opOK, POp.isMem]
      | disj a b w =>
        simp [Chk.emit] at hs
        rcases hs with rfl | rfl | rfl <;> simp [PStmt.pure, POp.isMem]
    · exact emitAll_pure cs _ (fun c' hc' => h c' (List.mem_cons_of_mem _ hc')) s hs

theorem checks_opOK (op : BinOp) (fl : LFlags) (w : Nat) (A B : Arg) :
    ∀ c ∈ checks op fl w A B, c.opOK = true := by
  cases op <;> simp only [checks, opt] <;> (repeat' split) <;>
    simp [Chk.opOK, POp.isMem]

/-- `emit_all` (`Refine.lean`) with the oracle. -/
theorem emitX_all (P : PFunc) (ω : Nat → Nat) (cs : List Chk) (σ : Store) (k : Nat) (t : World)
    (hT : TempsOK P k (emitAll k cs).2) (hb : ∀ c ∈ cs, c.below k) (hop : ∀ c ∈ cs, c.opOK = true) :
    (cs.any (Chk.bad σ) = true → xStmts P ω σ t (emitAll k cs).1 = .fail) ∧
    (cs.any (Chk.bad σ) = false → ∃ σ', xStmts P ω σ t (emitAll k cs).1 = .ok σ' t ∧
      ∀ i, i < k → σ' i = σ i) := by
  rw [xStmts_pure P ω σ t _ (emitAll_pure cs k hop)]
  have h := emit_all P cs σ k hT hb
  refine ⟨fun hc => by rw [h.1 hc]; rfl, fun hc => ?_⟩
  obtain ⟨σ', e, ha⟩ := h.2 hc
  exact ⟨σ', by rw [e]; rfl, ha⟩

/-! ## One instruction -/

/-- A strict step and a PIR statement run correspond; `σ0` is the store at
the start of the segment, which the run changes only at or above `lo`. -/
def SimX (c : Ctx) (σ0 : Store) : Res (SRegs × World) → XPRes → Prop
  | .ok (R', t'), p => ∃ σ', p = .ok σ' t' ∧ RelX c R' σ' ∧ ∀ j, j < c.lo → σ' j = σ0 j
  | .ub, p => p = .fail
  | .stuck, _ => True

theorem Res.bind_assoc {α β γ : Type} (r : Res α) (f : α → Res β) (g : β → Res γ) :
    (r.bind f).bind g = r.bind (fun a => (f a).bind g) := by
  cases r <;> rfl

theorem SimX.bind {c : Ctx} {σ0 : Store} {r : Res (SRegs × World)} {p : XPRes} (h : SimX c σ0 r p)
    {g : SRegs × World → Res (SRegs × World)} {q : Store → World → XPRes}
    (hg : ∀ R' t' σ', RelX c R' σ' → (∀ j, j < c.lo → σ' j = σ0 j) → SimX c σ0 (g (R', t')) (q σ' t')) :
    SimX c σ0 (r.bind g) (p.then q) := by
  cases r with
  | ok a =>
    obtain ⟨R', t'⟩ := a
    obtain ⟨σ', rfl, hr, ha⟩ := h
    exact hg R' t' σ' hr ha
  | ub => simp only [SimX] at h; subst h; rfl
  | stuck => trivial

theorem shChecks_pass (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) {sh : Option Arg}
    (hsh : ∀ a, sh = some a → truthN (a.get σ) = false) (rest : List PStmt) :
    xStmts P ω σ t (shChecks sh ++ rest) = xStmts P ω σ t rest := by
  cases sh with
  | none => rfl
  | some a => simp [shChecks, xStmts, hsh a rfl]

theorem opndX_sim {P : PFunc} {ω : Nat → Nat} {c : Ctx} {R : SRegs} {σ σ0 : Store}
    (hR : RelX c R σ) {w : Nat} {keep : Bool} {k : Nat} {t : World} {o : Opnd} {s : List PStmt}
    {tws : List Nat} {A : Arg} (h : trOpndX c w keep k o = .ok (s, tws, A)) (hk : c.hi ≤ k)
    (f : Nat → Res (SRegs × World)) (rest : List PStmt)
    (hf : ∀ v, sOpnd R w o = .ok v → tws = [] → A.get σ = v → A.below k →
      (keep = false → A.width = w) → SimX c σ0 (f v) (xStmts P ω σ t rest)) :
    SimX c σ0 ((sOpnd R w o).bind f) (xStmts P ω σ t (s ++ rest)) := by
  cases o with
  | const b =>
    simp only [trOpndX, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    exact hf _ rfl rfl rfl trivial (fun _ => rfl)
  | reg n =>
    simp only [trOpndX] at h
    split at h
    · rename_i a hl
      split at h
      · rename_i hlt
        simp only [Except.ok.injEq, Prod.mk.injEq] at h
        obtain ⟨rfl, rfl, rfl⟩ := h
        have hab : a.below k := Arg.below_mono hk (Arg.lt_below hlt)
        cases hn : R n with
        | none => simp [sOpnd, hn, Res.bind, SimX]
        | some x =>
          obtain ⟨a', hl', hx⟩ := hR n x hn
          rw [hl] at hl'; cases hl'
          cases x with
          | val v =>
            obtain ⟨hv, hsh⟩ := hx
            rw [shChecks_pass P ω σ t hsh rest]
            have hs : sOpnd R w (.reg n) = .ok v := by simp [sOpnd, hn]
            rw [hs]
            refine hf v hs rfl ?_ ?_ ?_
            · cases keep <;> simp [hv]
            · cases keep
              · exact Arg.setW_below w hab
              · exact hab
            · intro hkp; subst hkp; simp
          | ind =>
            obtain ⟨s', hs', ht⟩ := hx
            simp only [sOpnd, hn, Res.bind, SimX, hs', shChecks, List.cons_append, xStmts, ht, ite_true]
      · simp at h
    · simp at h
  | poison =>
    simp only [trOpndX, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    simp only [sOpnd, Res.bind, SimX, List.cons_append, xStmts, Arg.get]
    rfl

/-- A result written with a defined value (no shadow). -/
theorem assignX {c : Ctx} {R : SRegs} {σ : Store}
    (hR : RelX c R σ) {d : String} {w i : Nat} (hd : dstX c d w = .ok i) (hsh : look c.sh d = none)
    (v : Nat) (hv : v < 2 ^ w) :
    RelX c (R.set d v) (σ.set i (v % 2 ^ w)) := by
  obtain ⟨hl, _, _, _, hu⟩ := dstX_ok hd
  rw [Nat.mod_eq_of_lt hv]
  exact RelX.set hR hl hu hsh v

theorem dstX_wd {P : PFunc} {c : Ctx} (hc : CtxOK P c) {d : String} {w i : Nat} (hd : dstX c d w = .ok i) :
    P.wd i = w := hc.wd (dstX_ok hd).2.2.2.1

theorem dstX_lo {c : Ctx} {d : String} {w i : Nat} (hd : dstX c d w = .ok i) : c.lo ≤ i :=
  (dstX_ok hd).2.1

theorem dstX_hi {c : Ctx} {d : String} {w i : Nat} (hd : dstX c d w = .ok i) : i < c.hi :=
  (dstX_ok hd).2.2.1

/-- Agreement below `lo` survives a write at or above `lo`. -/
theorem agree_set {σ σ0 : Store} {lo i : Nat} (ha : ∀ j, j < lo → σ j = σ0 j) (hi : lo ≤ i) (v : Nat) :
    ∀ j, j < lo → σ.set i v j = σ0 j := by
  intro j hj; rw [Store.set_other _ _ (by omega)]; exact ha j hj

theorem selVal_lt (w z x y : Nat) : selVal w z x y < 2 ^ w := by
  unfold selVal; split <;> exact Nat.mod_lt _ (Nat.two_pow_pos w)

theorem castVal_lt (k : CastK) (fw tw x : Nat) : castVal k fw tw x < 2 ^ tw := by
  unfold castVal; split <;> exact BitVec.isLt _

/-- The value and "initialised" argument of a `store` (`trStoreVal`). -/
theorem storeVal_sim {P : PFunc} {ω : Nat → Nat} {c : Ctx} {R : SRegs} {σ : Store}
    (hR : RelX c R σ) {w k : Nat} (hk : c.hi ≤ k) {v : FOpnd} {sv : List PStmt} {tv : List Nat} {V I : Arg}
    (h : trStoreVal c w k v = .ok (sv, tv, V, I)) (hT : TempsOK P k tv) (W : World) :
    (∀ vv init W1, sStoreVal ω R W w v = .ok (vv, init, W1) →
      ∃ σ1, xStmts P ω σ W sv = .ok σ1 W1 ∧ (∀ j, j < k → σ1 j = σ j) ∧
        V.below (k + tv.length) ∧ I.below (k + tv.length) ∧ truthN (I.get σ1) = init ∧
        (init = true → V.get σ1 = vv)) ∧
    sStoreVal ω R W w v ≠ .ub := by
  cases v with
  | undef =>
    simp only [trStoreVal, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl, rfl⟩ := h
    have hw : P.wd k = w := by simpa using hT 0 (by simp)
    refine ⟨fun vv init W1 e => ?_, by simp [sStoreVal]⟩
    simp only [sStoreVal, Res.ok.injEq, Prod.mk.injEq] at e
    obtain ⟨rfl, rfl, rfl⟩ := e
    refine ⟨_, rfl, fun j hj => Store.set_other _ _ (by omega), by simp [Arg.below], trivial, rfl,
      fun h => by cases h⟩
  | o o =>
    cases o with
    | poison =>
      simp only [trStoreVal, Except.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl, rfl, rfl⟩ := h
      refine ⟨fun vv init W1 e => ?_, by simp [sStoreVal]⟩
      simp only [sStoreVal, Res.ok.injEq, Prod.mk.injEq] at e
      obtain ⟨rfl, rfl, rfl⟩ := e
      refine ⟨_, rfl, fun j hj => Store.set_other _ _ (by omega), by simp [Arg.below], trivial, rfl,
        fun h => by cases h⟩
    | const b =>
      simp only [trStoreVal, Except.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl, rfl, rfl⟩ := h
      refine ⟨fun vv init W1 e => ?_, by simp [sStoreVal]⟩
      simp only [sStoreVal, Res.ok.injEq, Prod.mk.injEq] at e
      obtain ⟨rfl, rfl, rfl⟩ := e
      exact ⟨σ, rfl, fun _ _ => rfl, trivial, trivial, rfl, fun _ => rfl⟩
    | reg n =>
      simp only [trStoreVal] at h
      split at h
      · rename_i a hl
        split at h
        · rename_i hlt
          have hab : a.below k := Arg.below_mono hk (Arg.lt_below hlt)
          cases hn : R n with
          | none => exact ⟨fun _ _ _ e => by simp [sStoreVal, hn] at e, by simp [sStoreVal, hn]⟩
          | some x =>
            obtain ⟨a', hl', hx⟩ := hR n x hn
            rw [hl] at hl'; cases hl'
            refine ⟨fun vv init W1 e => ?_, by cases x <;> simp [sStoreVal, hn]⟩
            split at h
            · rename_i s hs
              split at h
              · rename_i hw1
                simp only [Except.ok.injEq, Prod.mk.injEq] at h
                obtain ⟨rfl, rfl, rfl, rfl⟩ := h
                have hwk : P.wd k = 1 := by simpa using hT 0 (by simp)
                have hsw : s.width = 1 := by simpa using hw1
                have hv : truthN (icmpVal .eq 1 (s.get σ) 0) = !truthN (s.get σ) := by
                  rw [truthN_icmpVal, pred_eq]
                  simp only [truthN]
                  rcases Nat.mod_two_eq_zero_or_one (s.get σ) with h | h <;> simp [h]
                have hW : W1 = W := by
                  cases x <;> simp only [sStoreVal, hn, Res.ok.injEq, Prod.mk.injEq] at e <;> exact e.2.2.symm
                subst hW
                refine ⟨_, rfl, ?_, by simpa using Arg.below_mono (by omega) hab, by simp [Arg.below], ?_⟩
                · intro j hj; exact Store.set_other _ _ (by omega)
                cases x with
                | val v =>
                  obtain ⟨hv1, hv2⟩ := hx
                  simp only [sStoreVal, hn, Res.ok.injEq, Prod.mk.injEq] at e
                  obtain ⟨rfl, rfl, -⟩ := e
                  simp only [xStmts, evalOpM, evalOp, Arg.get_v', Arg.get_c', Store.set_same, hwk, hsw, xStmts_nil]
                  refine ⟨?_, fun _ => ?_⟩
                  · rw [truthN_mod2, hv, hv2 s hs]; rfl
                  · rw [Arg.get_agree (fun j hj => Store.set_other _ _ (by omega)) hab, hv1]
                | ind =>
                  obtain ⟨s', hs', ht⟩ := hx
                  rw [hs] at hs'; cases hs'
                  simp only [sStoreVal, hn, Res.ok.injEq, Prod.mk.injEq] at e
                  obtain ⟨rfl, rfl, -⟩ := e
                  simp only [xStmts, evalOpM, evalOp, Arg.get_v', Arg.get_c', Store.set_same, hwk, hsw, xStmts_nil]
                  refine ⟨?_, fun h => by cases h⟩
                  rw [truthN_mod2, hv, ht]; rfl
              · simp at h
            · rename_i hs
              simp only [Except.ok.injEq, Prod.mk.injEq] at h
              obtain ⟨rfl, rfl, rfl, rfl⟩ := h
              cases x with
              | val v =>
                obtain ⟨hv1, _⟩ := hx
                simp only [sStoreVal, hn, Res.ok.injEq, Prod.mk.injEq] at e
                obtain ⟨rfl, rfl, rfl⟩ := e
                exact ⟨σ, rfl, fun _ _ => rfl, by simpa using hab, trivial, rfl, fun _ => hv1⟩
              | ind =>
                obtain ⟨s', hs', _⟩ := hx
                rw [hs] at hs'; cases hs'
        · simp at h
      · simp at h

theorem World.store_uninit (t : World) (p v v' w : Nat) : t.store p v w false = t.store p v' w false := by
  simp [World.store, Mem.writeN]

@[simp] theorem Res.ok_bind {α β : Type} (a : α) (f : α → Res β) : (Res.ok a).bind f = f a := rfl

/-- An index operand the translator accepted is an `IArg`. -/
theorem iarg_of {c : Ctx} {R : SRegs} {σ : Store} {d : String} {i : Nat} (hu : c.uniq d i = true)
    {w k : Nat} (hw : okW w = true) {o : Opnd} {s1 : List PStmt} {t1 : List Nat} {A : Arg}
    (h1 : trOpndX c w false k o = .ok (s1, t1, A)) (hnc : ∀ n w' b, o = .reg n → A ≠ .c w' b)
    (hod : o ≠ .reg d) :
    ∀ v, sOpnd R w o = .ok v → A.get σ = v → A.below k → A.width = w → IArg σ k i o w A v := by
  intro v hv hAv hAb hAw
  have hw64 : w ≤ 64 := by simp only [okW, Bool.and_eq_true, decide_eq_true_eq] at hw; omega
  cases o with
  | const b =>
    simp only [trOpndX, Except.ok.injEq, Prod.mk.injEq] at h1
    obtain ⟨rfl, rfl, rfl⟩ := h1
    exact ⟨hAv, hAw, hw64, hAb, ⟨_, rfl⟩, by simp [Arg.var?]⟩
  | reg n =>
    simp only [trOpndX] at h1
    split at h1
    · rename_i a hl
      split at h1
      · simp only [Bool.false_eq_true, ite_false, Except.ok.injEq, Prod.mk.injEq] at h1
        obtain ⟨rfl, rfl, rfl⟩ := h1
        have hne : n ≠ d := fun e => hod (by rw [e])
        have hvi := Ctx.uniq_env hu hl hne
        cases a with
        | c w' b => exact absurd rfl (hnc n w b rfl)
        | v j w' =>
          exact ⟨hAv, hAw, hw64, hAb, ⟨j, rfl⟩, by simpa [Arg.setW, Arg.var?] using hvi⟩
      · cases h1
    · cases h1
  | poison => simp [sOpnd] at hv

/-- The statements and temporaries of `gEnd` continue the loop's. -/
theorem gEnd_ext (k i : Nat) (B : Arg) (inb : Bool) (g : GSt) :
    (∃ l, (gEnd k i B inb g).1 = g.s ++ l) ∧ (∃ l, (gEnd k i B inb g).2 = g.ts ++ l) := by
  have fin : ∀ g' : GSt, GExt g g' → ∀ δ : Arg,
      (∃ l, (if g'.var.isNone && g'.cst == 0 then (g'.s ++ [.assign i .copy [B]], g'.ts)
        else (g'.s ++ gFinS i (k + g'.ts.length) B δ inb, g'.ts ++ gFinT inb)).1 = g.s ++ l) ∧
      (∃ l, (if g'.var.isNone && g'.cst == 0 then (g'.s ++ [.assign i .copy [B]], g'.ts)
        else (g'.s ++ gFinS i (k + g'.ts.length) B δ inb, g'.ts ++ gFinT inb)).2 = g.ts ++ l) := by
    intro g' ⟨⟨l1, e1⟩, ⟨l2, e2⟩⟩ δ
    split
    · exact ⟨⟨l1 ++ _, by rw [e1, List.append_assoc]⟩, ⟨l2, e2⟩⟩
    · exact ⟨⟨l1 ++ _, by rw [e1, List.append_assoc]⟩, ⟨l2 ++ _, by rw [e2, List.append_assoc]⟩⟩
  unfold gEnd
  split
  · split
    · exact fin _ (gAddVarT_gext _ _ _) _
    · exact fin _ (GExt.refl g) _
  · exact fin _ (GExt.refl g) _

/-- The index operands of a `getelementptr` (`trIdxOps`) and their values. -/
theorem idxOps_sim {P : PFunc} {ω : Nat → Nat} {c : Ctx} {R : SRegs} {σ σ0 : Store}
    (hR : RelX c R σ) {d : String} {i : Nat} (hu : c.uniq d i = true) {t : World} :
    ∀ (ix : List GIdx) (k : Nat) (si : List PStmt) (ti : List Nat) (As : List Arg),
    trIdxOps c k ix = .ok (si, ti, As) → c.hi ≤ k →
    (∀ o ∈ ix.filterMap (fun g => g.opnd.map (·.1)), o ≠ .reg d) →
    ∀ (f : List Nat → Res (SRegs × World)) (rest : List PStmt),
    (∀ vs, sIdxVals R ix = .ok vs → ti = [] → IdxR σ k i ix As vs → SimX c σ0 (f vs) (xStmts P ω σ t rest)) →
    SimX c σ0 ((sIdxVals R ix).bind f) (xStmts P ω σ t (si ++ rest))
  | [], k, si, ti, As, h, _, _, f, rest, hf => by
    simp only [trIdxOps, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    exact hf [] rfl rfl .nil
  | .field off :: ix, k, si, ti, As, h, hk, hd, f, rest, hf => by
    simp only [trIdxOps, GIdx.opnd] at h
    obtain ⟨⟨s2, t2, As2⟩, h2, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    simp only [sIdxVals, GIdx.opnd, Res.bind_assoc, Res.ok_bind]
    refine idxOps_sim hR hu ix k s2 t2 As2 h2 hk (fun o ho => hd o (by simpa [GIdx.opnd] using ho)) _ rest ?_
    intro vs hvs ht hI
    exact hf (0 :: vs) (by simp [sIdxVals, GIdx.opnd, hvs, Res.bind]) ht (.field hI)
  | .first o w sc :: ix, k, si, ti, As, h, hk, hd, f, rest, hf => by
    simp only [trIdxOps, GIdx.opnd] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨⟨s1, t1, A⟩, h1, h⟩ := Except.bind_ok h
    simp only at h
    split at h
    · simp [throw, throwThe, MonadExceptOf.throw, bind, Except.bind] at h
    rename_i hnc'
    obtain ⟨⟨s2, t2, As2⟩, h2, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    have hod : o ≠ .reg d := hd o (by simp [GIdx.opnd])
    have hnc : ∀ n w' b, o = .reg n → A ≠ .c w' b := fun n w' b ho hA => hnc' n w' b ho hA
    have hia := iarg_of (σ := σ) (R := R) hu (need_ok hu1) h1 hnc hod
    simp only [sIdxVals, GIdx.opnd, Res.bind_assoc]
    rw [List.append_assoc]
    refine opndX_sim hR h1 hk _ _ ?_
    intro v hv ht1 hAv hAb hAw
    subst ht1
    simp only [List.length_nil, Nat.add_zero] at h2
    simp only [Res.bind_assoc, Res.ok_bind]
    refine idxOps_sim hR hu ix k s2 t2 As2 h2 hk (fun o' ho => hd o' (by obtain ⟨a, ha, hfa⟩ := List.mem_filterMap.mp ho; exact List.mem_filterMap.mpr ⟨a, List.mem_cons_of_mem _ ha, hfa⟩)) _ rest ?_
    intro vs hvs ht hI
    exact hf (v :: vs) (by simp [sIdxVals, GIdx.opnd, hv, hvs, Res.bind]) (by simp [ht])
      (.first (hia v hv hAv hAb (hAw rfl)) hI)
  | .arr o w sc n use :: ix, k, si, ti, As, h, hk, hd, f, rest, hf => by
    simp only [trIdxOps, GIdx.opnd] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨⟨s1, t1, A⟩, h1, h⟩ := Except.bind_ok h
    simp only at h
    split at h
    · simp [throw, throwThe, MonadExceptOf.throw, bind, Except.bind] at h
    rename_i hnc'
    obtain ⟨⟨s2, t2, As2⟩, h2, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    have hod : o ≠ .reg d := hd o (by simp [GIdx.opnd])
    have hnc : ∀ n w' b, o = .reg n → A ≠ .c w' b := fun n w' b ho hA => hnc' n w' b ho hA
    have hia := iarg_of (σ := σ) (R := R) hu (need_ok hu1) h1 hnc hod
    simp only [sIdxVals, GIdx.opnd, Res.bind_assoc]
    rw [List.append_assoc]
    refine opndX_sim hR h1 hk _ _ ?_
    intro v hv ht1 hAv hAb hAw
    subst ht1
    simp only [List.length_nil, Nat.add_zero] at h2
    simp only [Res.bind_assoc, Res.ok_bind]
    refine idxOps_sim hR hu ix k s2 t2 As2 h2 hk (fun o' ho => hd o' (by obtain ⟨a, ha, hfa⟩ := List.mem_filterMap.mp ho; exact List.mem_filterMap.mpr ⟨a, List.mem_cons_of_mem _ ha, hfa⟩)) _ rest ?_
    intro vs hvs ht hI
    exact hf (v :: vs) (by simp [sIdxVals, GIdx.opnd, hv, hvs, Res.bind]) (by simp [ht])
      (.arr (hia v hv hAv hAb (hAw rfl)) hI)

theorem bv_beq (w x : Nat) (y : BitVec w) : (bv w x == y) = decide (x % 2 ^ w = y.toNat) := by
  rw [Bool.eq_iff_iff]; simp [BitVec.toNat_eq]

theorem unChecks_bad (σ : Store) (uk : UnK) (flag : Bool) (w : Nat) (A : Arg) (hA : A.width = w) :
    (unChecks uk flag w A).any (Chk.bad σ) = unPoison uk flag w (A.get σ) := by
  cases flag
  · cases uk <;> simp [unChecks, opt, unPoison]
  · cases uk <;> simp only [unChecks, opt, ite_true, List.any_cons, List.any_nil, Bool.or_false, bad_cmp, hA,
      unPoison, Bool.true_and, pred_eq] <;> (try rw [Bool.eq_iff_iff]) <;>
      simp [BitVec.toNat_eq, BitVec.toNat_intMin] <;>
      exact ⟨fun h => of_decide_eq_true h, fun h => decide_eq_true h⟩

theorem unChecks_below {k : Nat} {A : Arg} (hA : A.below k) (uk : UnK) (flag : Bool) (w : Nat) :
    ∀ c ∈ unChecks uk flag w A, c.below k := by
  intro c hc
  cases flag <;> cases uk <;> simp [unChecks, opt] at hc <;> subst hc <;> exact ⟨hA, trivial⟩

theorem unChecks_opOK (uk : UnK) (flag : Bool) (w : Nat) (A : Arg) :
    ∀ c ∈ unChecks uk flag w A, c.opOK = true := by
  intro c hc
  cases flag <;> cases uk <;> simp [unChecks, opt] at hc <;> subst hc <;> rfl

theorem World.store_mod (t : World) (p v w : Nat) (init : Bool) :
    t.store (p % 2 ^ 64) v w init = t.store p v w init := by
  simp [World.store, Nat.mod_mod]

theorem globStores_run (P : PFunc) (ω : Nat → Nat) (i p : Nat) (hp : p < 2 ^ 64) :
    ∀ (st : List (Nat × Nat × Nat)) (σ : Store) (t : World) (k : Nat), i < k → σ i = p →
    TempsOK P k (globStores i k st).2 → (∀ x ∈ st, x.1 < 2 ^ 64) →
    ∃ σ', xStmts P ω σ t (globStores i k st).1 = .ok σ' (globW p t st) ∧ ∀ j, j < k → σ' j = σ j
  | [], σ, t, k, _, _, _, _ => ⟨σ, rfl, fun _ _ => rfl⟩
  | (o, w, v) :: st, σ, t, k, hik, hσ, hT, ho => by
    have ho' : ∀ x ∈ st, x.1 < 2 ^ 64 := fun x hx => ho x (List.mem_cons_of_mem _ hx)
    have hol : o < 2 ^ 64 := ho (o, w, v) List.mem_cons_self
    by_cases h0 : o = 0
    · subst h0
      simp only [globStores, ite_true] at hT ⊢
      obtain ⟨σ', e, ag⟩ := globStores_run P ω i p hp st σ (t.store p (v % 2 ^ w) w true) k hik hσ hT ho'
      refine ⟨σ', ?_, ag⟩
      simp only [xStmts, Arg.get_v', Arg.get_c', Arg.width_c', hσ, truthN, show (1 : Nat) % 2 = 1 from rfl,
        beq_self_eq_true]
      simp only [globW, Nat.add_zero, Nat.mod_eq_of_lt hp]
      exact e
    · simp only [globStores, h0, ite_false] at hT ⊢
      have wk : P.wd k = 64 := by have := hT 0 (by simp); simpa [List.getElem_cons_zero] using this
      have hT' : TempsOK P (k + 1) (globStores i (k + 1) st).2 := by
        have := hT.right (a := [64]); simpa using this
      have hq : (p + o) % 2 ^ 64 < 2 ^ 64 := Nat.mod_lt _ (Nat.two_pow_pos 64)
      obtain ⟨σ', e, ag⟩ := globStores_run P ω i p hp st (σ.set k ((p + o) % 2 ^ 64))
        (t.store ((p + o) % 2 ^ 64) (v % 2 ^ w) w true) (k + 1) (by omega) (by simp [set_apply, show i ≠ k by omega, hσ])
        hT' ho'
      refine ⟨σ', ?_, fun j hj => by rw [ag j (by omega)]; simp [set_apply, show j ≠ k by omega]⟩
      simp only [xStmts, evalOpM, evalOp, wk, Arg.get_v', Arg.get_c', c64_get, Arg.width_c', hσ, add64,
        Nat.mod_mod, Nat.mod_eq_of_lt hol, Store.set_same, truthN, show (1 : Nat) % 2 = 1 from rfl,
        beq_self_eq_true]
      simp only [globW]
      exact e

theorem store_simX {P : PFunc} {ω : Nat → Nat} {c : Ctx} (hc : CtxOK P c) {R : SRegs}
    {σ σ0 : Store} (hR : RelX c R σ) (ha0 : ∀ j, j < c.lo → σ j = σ0 j) {k : Nat} {t : World} (hk : c.hi ≤ k)
    {w : Nat} {v : FOpnd} {p : Opnd} {al : Nat} {s : List PStmt} {tws : List Nat}
    (h : trStore c k w v p al = .ok (s, tws)) (hT : TempsOK P k tws) :
    SimX c σ0 (sStoreR ω R t w v p al) (xStmts P ω σ t s) := by
    simp only [trStore] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
    obtain ⟨⟨sv, tv, V, I⟩, hv, h⟩ := Except.bind_ok h
    obtain ⟨⟨sp, tp, A⟩, hp, h⟩ := Except.bind_ok h
    have hw := need_ok hu1
    have hal := need_ok hu2
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    have hn8 : (w + 7) / 8 ≤ 8 := by simp only [okW, Bool.and_eq_true, decide_eq_true_eq] at hw; omega
    have hsv := storeVal_sim (ω := ω) hR hk hv (hT.left.left) t
    simp only [sStoreR]
    cases hs : sStoreVal ω R t w v with
    | stuck => trivial
    | ub => exact absurd hs hsv.2
    | ok r =>
      obtain ⟨vv, init, W1⟩ := r
      obtain ⟨σ1, e1, ag1, hVb, hIb, hI, hV⟩ := hsv.1 vv init W1 hs
      simp only [Res.bind]
      rw [List.append_assoc, List.append_assoc, xStmts_append, e1, XPRes.then]
      have hR1 : RelX c R σ1 := RelX.agree hc hR hk ag1
      have ha1 : ∀ j, j < c.lo → σ1 j = σ0 j := fun j hj => by
        rw [ag1 j (by have := hc.lohi; omega)]; exact ha0 j hj
      refine opndX_sim hR1 hp (by omega) _ _ ?_
      intro pv _ htp hAp hAb _
      subst htp
      have hTa : TempsOK P (k + tv.length) (accessChecks (k + tv.length) A ((w + 7) / 8) true al).2 := by
        have := hT.right; simpa using this
      simp only [List.length_nil, Nat.add_zero] at hTa ⊢
      have hac := accessChecks_run P ω σ1 W1 (k + tv.length) A hAb ((w + 7) / 8) hn8 true al hal hTa
      rw [hAp] at hac
      rw [xStmts_append]
      cases hb : accessBad W1.mem (pv % 2 ^ 64) ((w + 7) / 8) true al
      · obtain ⟨σ2, e2, ag2⟩ := hac.2 hb
        have hA2 : A.get σ2 = pv := by rw [Arg.get_agree ag2 hAb, hAp]
        have hV2 : V.get σ2 = V.get σ1 := Arg.get_agree ag2 hVb
        have hI2 : I.get σ2 = I.get σ1 := Arg.get_agree ag2 hIb
        simp only [hb, Bool.false_eq_true, ite_false, e2, XPRes.then, xStmts, xStmts_nil, Arg.setW_get,
          Arg.setW_width, hA2, hV2, hI2, hI, SimX]
        refine ⟨σ2, ?_, RelX.agree hc hR1 (m := k) hk (fun j hj => ag2 j (by omega)), fun j hj => ?_⟩
        · cases init
          · rw [World.store_uninit]
          · rw [hV rfl]
        · rw [ag2 j (by have := hc.lohi; omega)]; exact ha1 j hj
      · simp only [hb, ite_true, SimX, hac.1 hb, XPRes.then]

theorem sinstX_sim {P : PFunc} {ω : Nat → Nat} {c : Ctx} (hc : CtxOK P c) {R : SRegs}
    {σ σ0 : Store} (hR : RelX c R σ) (ha0 : ∀ j, j < c.lo → σ j = σ0 j) {k : Nat} {t : World} (hk : c.hi ≤ k)
    {i : SInst} {s : List PStmt} {tws : List Nat} (h : trSInstX c k i = .ok (s, tws))
    (hT : TempsOK P k tws) :
    SimX c σ0 (sSInst ω R t i) (xStmts P ω σ t s) := by
  cases i with
  | i x =>
    cases x with
    | bin d op fl w a b =>
      simp only [trSInstX, trInstX] at h
      obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
      obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
      have hf := need_ok hu2
      obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
      obtain ⟨⟨sb, tb, B⟩, hb, h⟩ := Except.bind_ok h
      obtain ⟨di, hdi, h⟩ := Except.bind_ok h
      obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
      have hsh := isNone_eq (need_ok hu3)
      simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl⟩ := h
      have hwd := dstX_wd hc hdi
      simp only [sSInst, sInst, Res.bind_assoc]
      rw [List.append_assoc, List.append_assoc]
      refine opndX_sim hR ha hk _ _ ?_
      intro x _ hta hAx hAb hAw
      subst hta
      simp only [List.nil_append, List.length_nil, Nat.add_zero] at hb hT ⊢
      refine opndX_sim hR hb hk _ _ ?_
      intro y _ htb hBy hBb hBw
      subst htb
      simp only [List.nil_append, List.length_nil, Nat.add_zero] at hT ⊢
      have hbad := checks_bad σ op fl w A B (hAw rfl) (hBw rfl) hf
      rw [hAx, hBy] at hbad
      have hem := emitX_all P ω (checks op fl w A B) σ k t hT (checks_below hAb hBb op fl)
        (checks_opOK op fl w A B)
      rw [xStmts_append]
      cases hcnd : (binUB op w x y || cUB op fl w x y || binPoison op fl w x y)
      · rw [hcnd] at hbad
        obtain ⟨σ', e, hag⟩ := hem.2 hbad
        simp only [hcnd, Bool.false_eq_true, ite_false, Res.bind, SimX, e, XPRes.then, xStmts, hwd,
          evalOpM, evalOp, Inst.draws, Nat.add_zero]
        refine ⟨_, rfl, ?_, ?_⟩
        · rw [Arg.get_agree hag hAb, Arg.get_agree hag hBb, hAx, hBy]
          exact assignX (RelX.agree hc hR hk hag) hdi hsh _ (binVal_lt _ _ _ _)
        · exact agree_set (fun j hj => by
            rw [hag j (by have := dstX_lo hdi; have := dstX_hi hdi; omega)]; exact ha0 j hj)
            (dstX_lo hdi) _
      · rw [hcnd] at hbad
        simp only [hcnd, ite_true, Res.bind, SimX, hem.1 hbad, XPRes.then]
    | icmp d p w a b =>
      simp only [trSInstX, trInstX] at h
      obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
      obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
      obtain ⟨⟨sb, tb, B⟩, hb, h⟩ := Except.bind_ok h
      obtain ⟨di, hdi, h⟩ := Except.bind_ok h
      obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
      have hsh := isNone_eq (need_ok hu3)
      simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl⟩ := h
      have hwd := dstX_wd hc hdi
      simp only [sSInst, sInst, Res.bind_assoc]
      rw [List.append_assoc]
      refine opndX_sim hR ha hk _ _ ?_
      intro x _ hta hAx _ hAw
      subst hta
      simp only [List.nil_append, List.length_nil, Nat.add_zero] at hb ⊢
      refine opndX_sim hR hb hk _ _ ?_
      intro y _ htb hBy _ _
      subst htb
      simp only [Res.bind, SimX, xStmts, hwd, evalOpM, evalOp, hAw rfl, hAx, hBy, Inst.draws, Nat.add_zero]
      refine ⟨_, rfl, assignX hR hdi hsh _ (by have := icmpVal_lt2 p w x y; simpa using this), ?_⟩
      exact agree_set ha0 (dstX_lo hdi) _
    | select d w cnd a b =>
      simp only [trSInstX, trInstX] at h
      obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
      obtain ⟨⟨sc, tc, C⟩, hcn, h⟩ := Except.bind_ok h
      obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
      obtain ⟨⟨sb, tb, B⟩, hb, h⟩ := Except.bind_ok h
      obtain ⟨di, hdi, h⟩ := Except.bind_ok h
      obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
      have hsh := isNone_eq (need_ok hu3)
      simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl⟩ := h
      have hwd := dstX_wd hc hdi
      simp only [sSInst, sInst, Res.bind_assoc]
      rw [List.append_assoc, List.append_assoc]
      refine opndX_sim hR hcn hk _ _ ?_
      intro z _ htc hCz _ _
      subst htc
      simp only [List.nil_append, List.length_nil, Nat.add_zero] at ha hb ⊢
      refine opndX_sim hR ha hk _ _ ?_
      intro x _ hta hAx _ _
      subst hta
      simp only [List.nil_append, List.length_nil, Nat.add_zero] at hb ⊢
      refine opndX_sim hR hb hk _ _ ?_
      intro y _ htb hBy _ _
      subst htb
      simp only [Res.bind, SimX, xStmts, hwd, evalOpM, evalOp, hAx, hBy, hCz, Inst.draws, Nat.add_zero]
      exact ⟨_, rfl, assignX hR hdi hsh _ (selVal_lt w z x y), agree_set ha0 (dstX_lo hdi) _⟩
    | cast d ck nneg fw tw a =>
      simp only [trSInstX, trInstX] at h
      obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
      obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
      have hnn := need_ok hu2
      obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
      obtain ⟨di, hdi, h⟩ := Except.bind_ok h
      obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
      have hsh := isNone_eq (need_ok hu3)
      simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl⟩ := h
      have hwd := dstX_wd hc hdi
      simp only [sSInst, sInst, Res.bind_assoc]
      rw [List.append_assoc]
      refine opndX_sim hR ha hk _ _ ?_
      intro x _ hta hAx hAb hAw
      subst hta
      simp only [List.nil_append, List.length_nil, Nat.add_zero] at hT ⊢
      have hbad := nneg_bad σ fw A (hAw rfl) nneg
      rw [hAx] at hbad
      have hcp : castPoison ck nneg fw x = castPoison .zext nneg fw x := by
        cases nneg
        · simp [castPoison]
        · simp at hnn; subst hnn; rfl
      rw [← hcp] at hbad
      have hem := emitX_all P ω _ σ k t hT (by
        intro c' hc'
        cases nneg <;> simp [opt] at hc'
        subst hc'; exact ⟨hAb, trivial⟩) (by
        intro c' hc'
        cases nneg <;> simp [opt] at hc'
        subst hc'; rfl)
      rw [xStmts_append]
      cases hcnd : castPoison ck nneg fw x
      · rw [hcnd] at hbad
        obtain ⟨σ', e, hag⟩ := hem.2 hbad
        simp only [hcnd, Bool.false_eq_true, ite_false, Res.bind, SimX, e, XPRes.then, xStmts, hwd, evalOpM,
          evalOp, Inst.draws, Nat.add_zero]
        refine ⟨_, rfl, ?_, ?_⟩
        · rw [Arg.get_agree hag hAb, hAx, hAw rfl]
          exact assignX (RelX.agree hc hR hk hag) hdi hsh _ (castVal_lt _ _ _ _)
        · exact agree_set (fun j hj => by
            rw [hag j (by have := dstX_lo hdi; have := dstX_hi hdi; omega)]; exact ha0 j hj)
            (dstX_lo hdi) _
      · rw [hcnd] at hbad
        simp only [hcnd, ite_true, Res.bind, SimX, hem.1 hbad, XPRes.then]
    | uninit d w =>
      simp only [trSInstX, trInstX] at h
      obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
      obtain ⟨di, hdi, h⟩ := Except.bind_ok h
      obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
      have hsh : look c.sh d = some (.c 1 1) := by simpa using need_ok hu3
      simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl⟩ := h
      obtain ⟨hl, hlo, _, _, hu⟩ := dstX_ok hdi
      simp only [sSInst, sInst, Res.bind, SimX, xStmts, Inst.draws]
      refine ⟨_, rfl, RelX.put (x := .ind) hR hl hu (fun s w' hs => by rw [hsh] at hs; cases hs)
        ⟨.c 1 1, by rw [Ctx.shOf_var hl, hsh], rfl⟩ ?_, agree_set ha0 hlo _⟩
      intro j hj
      exact Store.set_other _ _ (fun e => hj (.inl e))
  | freeze d w a =>
    simp only [trSInstX] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
    obtain ⟨di, hdi, h⟩ := Except.bind_ok h
    obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
    have hsh := isNone_eq (need_ok hu3)
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    have hwd := dstX_wd hc hdi
    obtain ⟨hl, hlo, hhi, _, hu⟩ := dstX_ok hdi
    cases a with
    | undef =>
      simp only [trFOpnd, Except.ok.injEq, Prod.mk.injEq] at ha
      obtain ⟨rfl, rfl, rfl⟩ := ha
      have hwk : P.wd k = w := by have := hT 0 (by simp); simpa using this
      simp only [sSInst, SimX, List.cons_append, List.nil_append, xStmts, evalOpM, evalOp, Arg.setW, Arg.get,
        Store.set_same, hwk, hwd, Nat.mod_mod]
      refine ⟨_, rfl, ?_, ?_⟩
      · exact RelX.set (RelX.agree hc hR (Nat.le_refl _) (fun j hj => Store.set_other _ _ (by omega)))
          hl hu hsh _
      · intro j hj
        rw [Store.set_other _ _ (by omega), Store.set_other _ _ (by omega)]
        exact ha0 j hj
    | o o =>
      simp only [trFOpnd] at ha
      simp only [sSInst, Res.bind_assoc]
      refine opndX_sim hR ha hk _ _ ?_
      intro v _ hta hAv _ _
      subst hta
      simp only [Res.bind, SimX, xStmts, evalOpM, evalOp, Arg.setW_get, hAv, hwd]
      exact ⟨_, rfl, RelX.set hR hl hu hsh _, agree_set ha0 hlo _⟩
  | alloca d size al =>
    simp only [trSInstX] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
    obtain ⟨di, hdi, h⟩ := Except.bind_ok h
    obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
    have hsh := isNone_eq (need_ok hu3)
    have hsz := need_ok hu2
    simp only [decide_eq_true_eq] at hsz
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    have hwd := dstX_wd hc hdi
    simp only [sSInst, Mem.alloc, World.allocW, SimX, xStmts, hwd, Arg.get, xStmts_nil, ↓reduceIte]
    refine ⟨_, rfl, ?_, agree_set ha0 (dstX_lo hdi) _⟩
    exact assignX hR hdi hsh _ (Nat.mod_lt _ (Nat.two_pow_pos 64))
  | load d w p al =>
    simp only [trSInstX] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
    obtain ⟨⟨sp, tp, A⟩, hp, h⟩ := Except.bind_ok h
    obtain ⟨di, hdi, h⟩ := Except.bind_ok h
    obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
    have hsh := isNone_eq (need_ok hu3)
    have hw := need_ok hu1
    have hal := need_ok hu2
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    have hwd := dstX_wd hc hdi
    have hn8 : (w + 7) / 8 ≤ 8 := by simp only [okW, Bool.and_eq_true, decide_eq_true_eq] at hw; omega
    simp only [sSInst]
    rw [List.append_assoc]
    refine opndX_sim hR hp hk _ _ ?_
    intro pv _ htp hAp hAb _
    subst htp
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hT ⊢
    have hac := accessChecks_run P ω σ t k A hAb ((w + 7) / 8) hn8 false al hal hT.left
    rw [hAp] at hac
    rw [xStmts_append]
    cases hb : accessBad t.mem (pv % 2 ^ 64) ((w + 7) / 8) false al
    · obtain ⟨σ', e, hag⟩ := hac.2 hb
      have hu : P.wd (k + (accessChecks k A ((w + 7) / 8) false al).2.length) = 1 := by
        have := hT.right 0 (by simp); simpa using this
      have hlt : di < k := by have := dstX_hi hdi; omega
      have hPA : A.get σ' = pv := by rw [Arg.get_agree hag hAb, hAp]
      simp only [e, XPRes.then, xStmts, hwd, hu, hPA, Store.set_same, Arg.get_v', ite_false, Bool.false_eq_true,
        hb, xStmts_nil]
      split
      · rename_i hany
        simp only [hany, ite_true, SimX]
        simp [truthN]
      · rename_i hany
        simp only [Bool.not_eq_true] at hany
        simp only [hany, Bool.false_eq_true, ite_false, Nat.zero_mod, truthN, SimX]
        refine ⟨_, rfl, ?_, ?_⟩
        · refine RelX.agree hc (m := k) ?_ hk (fun j hj => Store.set_other _ _ (by omega))
          have := assignX (RelX.agree hc hR hk hag) hdi hsh
            (bytesVal ((loadCells t.mem pv w).map (·.getD 0)) % 2 ^ w) (Nat.mod_lt _ (Nat.two_pow_pos w))
          rwa [Nat.mod_mod] at this
        · intro j hj
          have := dstX_lo hdi
          rw [Store.set_other _ _ (by omega), Store.set_other _ _ (by omega), hag j (by omega)]
          exact ha0 j hj
    · simp only [hb, ite_true, SimX, hac.1 hb, XPRes.then]
  | store w v p al =>
    simp only [trSInstX] at h
    exact store_simX hc hR ha0 hk h hT
  | gep d inb base ix =>
    simp only [trSInstX] at h
    obtain ⟨u0, hu0, h⟩ := Except.bind_ok h
    obtain ⟨⟨sb, tb, B⟩, hb, h⟩ := Except.bind_ok h
    obtain ⟨⟨si, ti, As⟩, hi, h⟩ := Except.bind_ok h
    obtain ⟨di, hdi, h⟩ := Except.bind_ok h
    obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
    obtain ⟨g, hg, h⟩ := Except.bind_ok h
    have hsh := isNone_eq (need_ok hu3)
    have hus := need_ok hu0
    simp only [gepUses, List.all_cons, Bool.and_eq_true, bne_iff_ne, ne_eq, List.all_eq_true] at hus
    obtain ⟨hbd, hixd⟩ := hus
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    obtain ⟨hl, hlo, hhi, hvw, hu⟩ := dstX_ok hdi
    have hwd := dstX_wd hc hdi
    simp only [sSInst]
    rw [List.append_assoc]
    refine opndX_sim hR hb hk _ _ ?_
    intro bv hbv htb hBv hBb _
    subst htb
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hi hg hT ⊢
    refine idxOps_sim hR hu ix k si ti As hi hk (fun o ho => hixd o ho) _ _ ?_
    intro vs hvs hti hI
    subst hti
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hg hT ⊢
    have hBi : B.var? ≠ some di := by
      cases base with
      | const b => simp only [trOpndX, Except.ok.injEq, Prod.mk.injEq] at hb; rw [← hb.2.2]; simp [Arg.var?]
      | reg n =>
        simp only [trOpndX] at hb
        split at hb
        · rename_i a hla
          split at hb
          · simp only [ite_true, Except.ok.injEq, Prod.mk.injEq] at hb
            rw [← hb.2.2]
            exact Ctx.uniq_env hu hla (fun e => hbd (by rw [e]))
          · cases hb
        · cases hb
      | poison => simp [sOpnd] at hbv
    have hdk : di < k := by omega
    have hG0 : GRel P ω t k di σ { s := [], ts := [], cst := 0, var := none } { cst := 0, var := none } σ :=
      ⟨rfl, fun _ _ => rfl, rfl, trivial, hdk⟩
    obtain ⟨⟨le, hle⟩, ⟨lt, hlt⟩⟩ := gEnd_ext k di B inb g
    have hTg : TempsOK P k g.ts := by rw [hlt] at hT; exact hT.left
    have hL := gLoop_sim hI hG0 hg hTg
    simp only [gepVal, Res.bind_assoc]
    cases hf : gFold { cst := 0, var := none } (ix.zip vs) with
    | ok acc =>
      rw [hf] at hL
      obtain ⟨σg, hGg⟩ := hL
      have he := gEnd_sim hGg hBb hBi hwd inb hT
      rw [hBv] at he
      simp only [Res.ok_bind]
      cases hr : gFinish t.mem inb bv acc with
      | ok r =>
        rw [hr] at he
        obtain ⟨σ', e, h1, h2⟩ := he
        simp only [Res.ok_bind, SimX, e]
        refine ⟨σ', rfl, ?_, fun j hj => ?_⟩
        · refine RelX.agree hc (m := k) (RelX.set hR hl hu hsh r) hk (fun j hj => ?_)
          by_cases hji : j = di
          · subst hji; rw [h1, Store.set_same]
          · rw [h2 j hj hji, Store.set_other _ _ hji]
        · rw [h2 j (by omega) (by omega)]; exact ha0 j hj
      | ub => rw [hr] at he; simp only [Res.bind, SimX]; exact he
      | stuck => trivial
    | ub =>
      rw [hf] at hL
      simp only [Res.bind, SimX]
      rw [hle]
      exact xStmts_fail_append hL _
    | stuck => trivial
  | mm d mk w a b =>
    simp only [trSInstX] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
    obtain ⟨⟨sb, tb, B⟩, hb, h⟩ := Except.bind_ok h
    obtain ⟨di, hdi, h⟩ := Except.bind_ok h
    obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
    have hsh := isNone_eq (need_ok hu3)
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    have hwd := dstX_wd hc hdi
    simp only [sSInst, sMMV, Res.bind_assoc]
    rw [List.append_assoc]
    refine opndX_sim hR ha hk _ _ ?_
    intro x _ hta hAx _ _
    subst hta
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hb ⊢
    refine opndX_sim hR hb hk _ _ ?_
    intro y _ htb hBy _ _
    subst htb
    simp only [Res.bind, SimX, xStmts, hwd, evalOpM, evalOp, hAx, hBy, Res.ok_bind, xStmts_nil]
    refine ⟨_, rfl, ?_, agree_set ha0 (dstX_lo hdi) _⟩
    have := assignX hR hdi hsh (mmVal mk w x y % 2 ^ w) (Nat.mod_lt _ (Nat.two_pow_pos w))
    rwa [Nat.mod_mod] at this
  | un d uk w a flag =>
    simp only [trSInstX] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
    obtain ⟨u4, hu4, h⟩ := Except.bind_ok h
    obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
    obtain ⟨di, hdi, h⟩ := Except.bind_ok h
    obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
    have hsh := isNone_eq (need_ok hu3)
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    have hwd := dstX_wd hc hdi
    simp only [sSInst, sUnV, Res.bind_assoc]
    rw [List.append_assoc]
    refine opndX_sim hR ha hk _ _ ?_
    intro x _ hta hAx hAb hAw
    subst hta
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hT ⊢
    have hbad := unChecks_bad σ uk flag w A (hAw rfl)
    rw [hAx] at hbad
    have hem := emitX_all P ω _ σ k t hT (unChecks_below hAb uk flag w) (unChecks_opOK uk flag w A)
    rw [xStmts_append]
    cases hcnd : unPoison uk flag w x
    · rw [hcnd] at hbad
      obtain ⟨σ', e, hag⟩ := hem.2 hbad
      simp only [Bool.false_eq_true, ite_false, Res.bind, SimX, e, XPRes.then, xStmts, hwd, evalOpM,
        evalOp, Res.ok_bind, xStmts_nil]
      refine ⟨_, rfl, ?_, ?_⟩
      · rw [Arg.get_agree hag hAb, hAx]
        have := assignX (RelX.agree hc hR hk hag) hdi hsh (unVal uk w x % 2 ^ w) (Nat.mod_lt _ (Nat.two_pow_pos w))
        rwa [Nat.mod_mod] at this
      · exact agree_set (fun j hj => by
          rw [hag j (by have := dstX_lo hdi; have := dstX_hi hdi; omega)]; exact ha0 j hj)
          (dstX_lo hdi) _
    · rw [hcnd] at hbad
      simp only [ite_true, Res.bind, SimX, hem.1 hbad, XPRes.then]
  | expect d w a =>
    simp only [trSInstX] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
    obtain ⟨di, hdi, h⟩ := Except.bind_ok h
    obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
    have hsh := isNone_eq (need_ok hu3)
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    have hwd := dstX_wd hc hdi
    obtain ⟨hl, hlo, _, _, hu⟩ := dstX_ok hdi
    simp only [sSInst, sExpV, Res.bind_assoc]
    refine opndX_sim hR ha hk _ _ ?_
    intro v _ hta hAv _ _
    subst hta
    simp only [Res.bind, SimX, xStmts, evalOpM, evalOp, hAv, hwd, Res.ok_bind, xStmts_nil]
    exact ⟨_, rfl, RelX.set hR hl hu hsh _, agree_set ha0 hlo _⟩
  | xv d w src idx =>
    simp only [trSInstX] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
    split at h
    · rename_i a hla
      obtain ⟨u4, hu4, h⟩ := Except.bind_ok h
      obtain ⟨u5, hu5, h⟩ := Except.bind_ok h
      obtain ⟨di, hdi, h⟩ := Except.bind_ok h
      obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
      have hsh := isNone_eq (need_ok hu3)
      have hpsh := isNone_eq (need_ok hu5)
      simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl⟩ := h
      have hwd := dstX_wd hc hdi
      obtain ⟨hl, hlo, _, _, hu⟩ := dstX_ok hdi
      simp only [sSInst, sXvV]
      cases hn : R (pairReg src idx) with
      | none => trivial
      | some x =>
        obtain ⟨a', hl', hx⟩ := hR _ x hn
        rw [hla] at hl'; cases hl'
        cases x with
        | val v =>
          obtain ⟨hv, _⟩ := hx
          simp only [Res.bind, SimX, xStmts, evalOpM, evalOp, hv, hwd, xStmts_nil]
          exact ⟨_, rfl, RelX.set hR hl hu hsh _, agree_set ha0 hlo _⟩
        | ind =>
          obtain ⟨s', hs', _⟩ := hx
          have := Ctx.shOf_some hs'
          rw [hpsh] at this; cases this
    · simp [throw, throwThe, MonadExceptOf.throw] at h
  | ovf d ok w a b =>
    simp only [trSInstX] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
    obtain ⟨⟨sb, tb, B⟩, hb, h⟩ := Except.bind_ok h
    obtain ⟨i0, hi0, h⟩ := Except.bind_ok h
    obtain ⟨i1, hi1, h⟩ := Except.bind_ok h
    obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
    obtain ⟨u4, hu4, h⟩ := Except.bind_ok h
    have hsh := need_ok hu3
    have hvv := need_ok hu4
    simp only [Bool.and_eq_true, Option.isNone_iff_eq_none] at hsh
    simp only [Bool.and_eq_true, bne_iff_ne, ne_eq] at hvv
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    have hw0 := dstX_wd hc hi0
    have hw1 := dstX_wd hc hi1
    simp only [sSInst, sOvfV, Res.bind_assoc]
    rw [List.append_assoc]
    refine opndX_sim hR ha hk _ _ ?_
    intro x _ hta hAx _ hAw
    subst hta
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hb ⊢
    refine opndX_sim hR hb hk _ _ ?_
    intro y _ htb hBy _ hBw
    subst htb
    simp only [Res.bind, SimX, xStmts, evalOpM, evalOp, hw0, hw1, Res.ok_bind, xStmts_nil,
      Arg.get_set_ne _ _ hvv.1, Arg.get_set_ne _ _ hvv.2, hAx, hBy, testVal, hAw rfl, BitVec.toNat_ofBool,
      Nat.pow_one]
    refine ⟨_, rfl, ?_, agree_set (agree_set ha0 (dstX_lo hi0) _) (dstX_lo hi1) _⟩
    have h1 := assignX hR hi0 hsh.1 _ (binVal_lt (ovfBin ok) w x y)
    have h2 := assignX h1 hi1 hsh.2 (ovfTest ok w x y).toNat (by cases ovfTest ok w x y <;> decide)
    have hb : ∀ b : Bool, b.toNat % 2 = b.toNat := fun b => by cases b <;> decide
    rw [Nat.pow_one, hb] at h2
    rw [hb]
    exact h2
  | lstart n p =>
    simp only [trSInstX] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    simp only [sSInst]
    exact store_simX hc hR ha0 hk h hT
  | lend p =>
    simp only [trSInstX] at h
    obtain ⟨⟨sp, tp, A⟩, hp, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    simp only [sSInst, sLend, Res.bind_assoc]
    refine opndX_sim hR hp hk _ _ ?_
    intro pv _ htp hAp _ _
    subst htp
    simp only [Res.bind, SimX, xStmts, hAp, xStmts_nil, Res.ok_bind]
    exact ⟨σ, rfl, hR, ha0⟩
  | glob d size al kd ini st =>
    simp only [trSInstX] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
    obtain ⟨di, hdi, h⟩ := Except.bind_ok h
    obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
    have hsh := isNone_eq (need_ok hu3)
    have hoff : ∀ x ∈ st, x.1 < 2 ^ 64 := by
      have := need_ok hu2
      simp only [List.all_eq_true, decide_eq_true_eq] at this
      exact fun x hx => this x hx
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    have hwd := dstX_wd hc hdi
    obtain ⟨hl, hlo, hhi, _, hu⟩ := dstX_ok hdi
    have hp := World.allocW_lt t ω (size % 2 ^ 64) kd al ini
    obtain ⟨σ', e, ag⟩ := globStores_run P ω di _ hp st
      (σ.set di ((t.allocW ω (size % 2 ^ 64) kd al ini).2 % 2 ^ 64))
      (t.allocW ω (size % 2 ^ 64) kd al ini).1 k (by omega)
      (by simp [set_apply, Nat.mod_eq_of_lt hp]) hT hoff
    simp only [sSInst, globAlloc, SimX, xStmts, hwd, Arg.get_c']
    rw [e]
    refine ⟨σ', rfl, ?_, fun j hj => ?_⟩
    · refine RelX.agree hc (RelX.set hR hl hu hsh _) (m := k) hk (fun j hj => ?_)
      rw [ag j hj, Nat.mod_eq_of_lt hp]
    · rw [ag j (by have := hc.lohi; omega), Store.set_other _ _ (by omega)]; exact ha0 j hj
  | memcpy d s len lw mv =>
    simp only [trSInstX] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨⟨sd, td, D⟩, hd, h⟩ := Except.bind_ok h
    obtain ⟨⟨ss, ts, S⟩, hs, h⟩ := Except.bind_ok h
    obtain ⟨⟨sn, tn, L⟩, hn, h⟩ := Except.bind_ok h
    obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
    have hLw : L.width = lw := by simpa using need_ok hu2
    have hw := need_ok hu1
    have hlw : lw ≤ 64 := by simp only [okW, Bool.and_eq_true, decide_eq_true_eq] at hw; omega
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    simp only [sSInst, sMemcpy, Res.bind_assoc, List.append_assoc]
    refine opndX_sim hR hd hk _ _ ?_
    intro dv _ htd hDv hDb _
    subst htd
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hs hn hT ⊢
    refine opndX_sim hR hs hk _ _ ?_
    intro sv _ hts hSv hSb _
    subst hts
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hn hT ⊢
    refine opndX_sim hR hn hk _ _ ?_
    intro n0 _ htn hLv hLb _
    subst htn
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hT ⊢
    have hm := memcpy_run P ω σ t k D S L hDb hSb hLb (by omega) mv hT
    simp only [hDv, hSv, hLv, hLw] at hm
    cases hb : cpyBad t.mem (dv % 2 ^ 64) (sv % 2 ^ 64) (n0 % 2 ^ lw) mv
    · obtain ⟨σ', e, ag⟩ := hm.2 hb
      simp only [Bool.false_eq_true, ite_false, Res.bind]
      rw [e]
      exact ⟨σ', rfl, RelX.agree hc hR hk ag, fun j hj => by
        rw [ag j (by have := hc.lohi; omega)]; exact ha0 j hj⟩
    · simp only [ite_true, Res.bind, SimX]
      exact hm.1 hb
  | memset d b len lw =>
    simp only [trSInstX] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨⟨sd, td, D⟩, hd, h⟩ := Except.bind_ok h
    obtain ⟨⟨sb, tb, B⟩, hb, h⟩ := Except.bind_ok h
    obtain ⟨u3, hu3, h⟩ := Except.bind_ok h
    obtain ⟨⟨sn, tn, L⟩, hn, h⟩ := Except.bind_ok h
    obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
    have hLw : L.width = lw := by simpa using need_ok hu2
    have hw := need_ok hu1
    have hlw : lw ≤ 64 := by simp only [okW, Bool.and_eq_true, decide_eq_true_eq] at hw; omega
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    simp only [sSInst, sMemset, Res.bind_assoc, List.append_assoc]
    refine opndX_sim hR hd hk _ _ ?_
    intro dv _ htd hDv hDb _
    subst htd
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hb hn hT ⊢
    refine opndX_sim hR hb hk _ _ ?_
    intro bv _ htb hBv hBb _
    subst htb
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hn hT ⊢
    refine opndX_sim hR hn hk _ _ ?_
    intro n0 _ htn hLv hLb _
    subst htn
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hT ⊢
    have hm := memset_run P ω σ t k D B L hDb hBb hLb (by omega) hT
    simp only [hDv, hBv, hLv, hLw] at hm
    cases hbad : (n0 % 2 ^ lw != 0 && accessBad t.mem (dv % 2 ^ 64) (n0 % 2 ^ lw) true 1)
    · obtain ⟨σ', e, ag⟩ := hm.2 hbad
      simp only [hbad, Bool.false_eq_true, ite_false, Res.bind]
      rw [e]
      exact ⟨σ', rfl, RelX.agree hc hR hk ag, fun j hj => by
        rw [ag j (by have := hc.lohi; omega)]; exact ha0 j hj⟩
    · simp only [hbad, ite_true, Res.bind, SimX]
      exact hm.1 hbad

theorem sinstsX_sim {P : PFunc} {ω : Nat → Nat} {c : Ctx} (hc : CtxOK P c) {σ0 : Store} :
    ∀ (is : List SInst) (R : SRegs) (σ : Store) (k : Nat) (t : World) (s : List PStmt) (tws : List Nat),
    RelX c R σ → (∀ j, j < c.lo → σ j = σ0 j) → c.hi ≤ k → trSInstsX c k is = .ok (s, tws) →
    TempsOK P k tws → SimX c σ0 (sSInsts ω R t is) (xStmts P ω σ t s)
  | [], R, σ, k, t, s, tws, hR, ha, _, h, _ => by
    simp only [trSInstsX, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    exact ⟨σ, rfl, hR, ha⟩
  | i :: is, R, σ, k, t, s, tws, hR, ha, hk, h, hT => by
    simp only [trSInstsX] at h
    obtain ⟨⟨s1, t1⟩, h1, h⟩ := Except.bind_ok h
    obtain ⟨⟨s2, t2⟩, h2, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    simp only [sSInsts]
    rw [xStmts_append]
    exact SimX.bind (sinstX_sim hc hR ha hk h1 hT.left) (fun R' t' σ' hr ha' =>
      sinstsX_sim hc is R' σ' (k + t1.length) t' s2 t2 hr ha' (by omega) h2 hT.right)

end PrismRefine
