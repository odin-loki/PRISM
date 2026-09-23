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

def XPRes.then : XPRes → (Store → World → XPRes) → XPRes
  | .ok σ t, f => f σ t
  | .fail, _ => .fail
  | .blocked, _ => .blocked

theorem xStmts_append (P : PFunc) (ω : Nat → Nat) : ∀ (σ : Store) (t : World) (xs ys : List PStmt),
    xStmts P ω σ t (xs ++ ys) = (xStmts P ω σ t xs).then (fun σ' t' => xStmts P ω σ' t' ys)
  | σ, t, [], ys => rfl
  | σ, t, .assign d op args :: r, ys => xStmts_append P ω _ t r ys
  | σ, t, .havoc d :: r, ys => xStmts_append P ω _ _ r ys
  | σ, t, .check a _ _ :: r, ys => by
    simp only [List.cons_append, xStmts]; split
    · rfl
    · exact xStmts_append P ω σ t r ys
  | σ, t, .assume a :: r, ys => by
    simp only [List.cons_append, xStmts]; split
    · exact xStmts_append P ω σ t r ys
    · rfl

def PRes.toX : PRes → World → XPRes
  | .ok σ, t => .ok σ t
  | .fail, _ => .fail
  | .blocked, _ => .blocked

/-- Without `havoc`, the oracle plays no part. -/
theorem xStmts_noHavoc (P : PFunc) (ω : Nat → Nat) : ∀ (σ : Store) (t : World) (l : List PStmt),
    (∀ d, PStmt.havoc d ∉ l) → xStmts P ω σ t l = (pStmts P σ l).toX t
  | σ, t, [], _ => rfl
  | σ, t, .assign d op args :: r, h => by
    simp only [xStmts, pStmts]
    exact xStmts_noHavoc P ω _ t r (fun d' hd => h d' (List.mem_cons_of_mem _ hd))
  | σ, t, .havoc d :: r, h => absurd (List.mem_cons_self) (h d)
  | σ, t, .check a _ _ :: r, h => by
    simp only [xStmts, pStmts]; split
    · rfl
    · exact xStmts_noHavoc P ω σ t r (fun d' hd => h d' (List.mem_cons_of_mem _ hd))
  | σ, t, .assume a :: r, h => by
    simp only [xStmts, pStmts]; split
    · exact xStmts_noHavoc P ω σ t r (fun d' hd => h d' (List.mem_cons_of_mem _ hd))
    · rfl

theorem emitAll_noHavoc : ∀ (cs : List Chk) (k d : Nat), PStmt.havoc d ∉ (emitAll k cs).1
  | [], _, _ => by simp [emitAll]
  | c :: cs, k, d => by
    simp only [emitAll, List.mem_append, not_or]
    refine ⟨?_, emitAll_noHavoc cs _ d⟩
    cases c <;> simp [Chk.emit]

/-- `emit_all` (`Refine.lean`) with the oracle. -/
theorem emitX_all (P : PFunc) (ω : Nat → Nat) (cs : List Chk) (σ : Store) (k : Nat) (t : World)
    (hT : TempsOK P k (emitAll k cs).2) (hb : ∀ c ∈ cs, c.below k) :
    (cs.any (Chk.bad σ) = true → xStmts P ω σ t (emitAll k cs).1 = .fail) ∧
    (cs.any (Chk.bad σ) = false → ∃ σ', xStmts P ω σ t (emitAll k cs).1 = .ok σ' t ∧
      ∀ i, i < k → σ' i = σ i) := by
  rw [xStmts_noHavoc P ω σ t _ (emitAll_noHavoc cs k)]
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
      rw [xStmts_append]
      cases hcnd : (binUB op w x y || cUB op fl w x y || binPoison op fl w x y)
      · rw [hcnd] at hbad
        obtain ⟨σ', e, hag⟩ := hem.2 hbad
        simp only [hcnd, Bool.false_eq_true, ite_false, Res.bind, SimX, e, XPRes.then, xStmts, hwd,
          evalOp, Inst.draws, Nat.add_zero]
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
      simp only [Res.bind, SimX, xStmts, hwd, evalOp, hAw rfl, hAx, hBy, Inst.draws, Nat.add_zero]
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
      simp only [Res.bind, SimX, xStmts, hwd, evalOp, hAx, hBy, hCz, Inst.draws, Nat.add_zero]
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
        subst hc'; exact ⟨hAb, trivial⟩)
      rw [xStmts_append]
      cases hcnd : castPoison ck nneg fw x
      · rw [hcnd] at hbad
        obtain ⟨σ', e, hag⟩ := hem.2 hbad
        simp only [hcnd, Bool.false_eq_true, ite_false, Res.bind, SimX, e, XPRes.then, xStmts, hwd, evalOp,
          Inst.draws, Nat.add_zero]
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
      simp only [sSInst, SimX, List.cons_append, List.nil_append, xStmts, evalOp, Arg.setW, Arg.get,
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
      simp only [Res.bind, SimX, xStmts, evalOp, Arg.setW_get, hAv, hwd]
      exact ⟨_, rfl, RelX.set hR hl hu hsh _, agree_set ha0 hlo _⟩

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
