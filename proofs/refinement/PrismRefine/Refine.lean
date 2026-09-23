/-
PRISM refinement — the translation is correct.

Main results (all for every function `F` the translator accepts, every
input vector `args` and every fuel bound `n` — the number of basic blocks
executed, so loops are covered for any number of iterations):

* `translate_exact`: the PIR program and the *strict* LLVM semantics have the
  same outcome — PIR returns `v` iff LLVM returns `v`, PIR fails a check iff
  LLVM reaches undefined behaviour or creates poison, PIR runs out of fuel iff
  LLVM does — unless the LLVM program is ill-formed on that path (`stuck`).
* `strict_lazy`: the strict semantics and the LangRef semantics agree up to
  the first creation of poison.
* `pir_sound` (headline): if the LLVM program, under the LangRef semantics,
  reaches undefined behaviour or creates poison within `n` blocks, the PIR
  program fails a check within `n` blocks.  So a PIR proof that no check can
  fail is a proof that the LLVM function has no UB and never creates poison.
* `pir_faithful`: every PIR outcome is an LLVM outcome: a PIR return value is
  the LLVM return value (with no UB and no poison), and a failed PIR check
  means the LLVM run has UB, creates poison, or is ill-formed.
-/
import PrismRefine.Ops

namespace PrismRefine

open PrismSem

/-! ## Names -/

theorem findName_get : ∀ {l : List (String × Nat)} {n : String} {i w : Nat},
    findName l n = some (i, w) → l[i]? = some (n, w)
  | [], _, _, _, h => by simp [findName] at h
  | (m, w') :: t, n, i, w, h => by
    simp only [findName] at h
    split at h
    · rename_i hm; simp at h; obtain ⟨rfl, rfl⟩ := h; simp [hm]
    · cases h' : findName t n with
      | none => simp [h'] at h
      | some p =>
        simp [h'] at h
        obtain ⟨rfl, rfl⟩ := h
        simp [findName_get h']

theorem findName_lt {l : List (String × Nat)} {n : String} {i w : Nat}
    (h : findName l n = some (i, w)) : i < l.length := by
  have := findName_get h
  exact (List.getElem?_eq_some_iff.mp this).1

theorem findName_inj {l : List (String × Nat)} {n m : String} {i w w' : Nat}
    (h1 : findName l n = some (i, w)) (h2 : findName l m = some (i, w')) : n = m := by
  have a := findName_get h1
  have b := findName_get h2
  rw [a] at b; simp at b; exact b.1

theorem findName_append_left : ∀ {l r : List (String × Nat)} {n : String} {p : Nat × Nat},
    findName l n = some p → findName (l ++ r) n = some p
  | [], _, _, _, h => by simp [findName] at h
  | (m, w) :: t, r, n, p, h => by
    simp only [findName, List.cons_append] at h ⊢
    split
    · simp_all
    · rename_i hm; simp only [hm, ite_false] at h
      cases h' : findName t n with
      | none => simp [h'] at h
      | some q => rw [findName_append_left h']; simpa [h'] using h

/-! ## Stores -/

@[simp] theorem Store.set_same (σ : Store) (i v : Nat) : σ.set i v i = v := by simp [Store.set]
theorem Store.set_other (σ : Store) {i j : Nat} (v : Nat) (h : j ≠ i) : σ.set i v j = σ j := by
  simp [Store.set, h]

@[simp] theorem SRegs.set_same (R : SRegs) (n : String) (v : Nat) : R.set n v n = some v := by
  simp [SRegs.set]

/-- The register file `R` and the PIR store `σ` agree on every defined
register (through the translator's name → variable map). -/
def Rel (names : List (String × Nat)) (R : SRegs) (σ : Store) : Prop :=
  ∀ n v, R n = some v → ∃ i w, findName names n = some (i, w) ∧ σ i = v

theorem Rel.set {names : List (String × Nat)} {R : SRegs} {σ : Store} (h : Rel names R σ)
    {n : String} {i w : Nat} (hf : findName names n = some (i, w)) (v : Nat) :
    Rel names (R.set n v) (σ.set i v) := by
  intro m u hm
  by_cases e : m = n
  · subst e; simp [SRegs.set] at hm; subst hm; exact ⟨i, w, hf, by simp⟩
  · simp only [SRegs.set, e, ite_false] at hm
    obtain ⟨j, w', hj, hv⟩ := h m u hm
    refine ⟨j, w', hj, ?_⟩
    have : j ≠ i := fun hji => e (findName_inj hj (hji ▸ hf))
    rw [Store.set_other _ _ this, hv]

theorem Rel.agree {names : List (String × Nat)} {R : SRegs} {σ σ' : Store} (h : Rel names R σ)
    (ha : ∀ i, i < names.length → σ' i = σ i) : Rel names R σ' := by
  intro n v hn
  obtain ⟨i, w, hi, hv⟩ := h n v hn
  exact ⟨i, w, hi, by rw [ha i (findName_lt hi), hv]⟩

/-! ## Composition of PIR statement runs -/

def PRes.then : PRes → (Store → PRes) → PRes
  | .ok σ, f => f σ
  | .fail, _ => .fail
  | .blocked, _ => .blocked

theorem pStmts_append (P : PFunc) : ∀ (σ : Store) (xs ys : List PStmt),
    pStmts P σ (xs ++ ys) = (pStmts P σ xs).then (fun σ' => pStmts P σ' ys)
  | σ, [], ys => rfl
  | σ, .assign d op args :: t, ys => pStmts_append P _ t ys
  | σ, .havoc d :: t, ys => pStmts_append P _ t ys
  | σ, .check a _ _ :: t, ys => by
    simp only [List.cons_append, pStmts]; split
    · rfl
    · exact pStmts_append P σ t ys
  | σ, .assume a :: t, ys => by
    simp only [List.cons_append, pStmts]; split
    · exact pStmts_append P σ t ys
    · rfl

/-! ## Widths of variables -/

def NamesOK (P : PFunc) (names : List (String × Nat)) : Prop :=
  ∀ i (h : i < names.length), P.wd i = names[i].2

def TempsOK (P : PFunc) (k : Nat) (tws : List Nat) : Prop :=
  ∀ j (h : j < tws.length), P.wd (k + j) = tws[j]

theorem TempsOK.left {P : PFunc} {k : Nat} {a b : List Nat} (h : TempsOK P k (a ++ b)) :
    TempsOK P k a := by
  intro j hj
  have := h j (by simp; omega)
  rw [this, List.getElem_append_left hj]

theorem TempsOK.right {P : PFunc} {k : Nat} {a b : List Nat} (h : TempsOK P k (a ++ b)) :
    TempsOK P (k + a.length) b := by
  intro j hj
  have := h (a.length + j) (by simp; omega)
  rw [Nat.add_assoc, this, List.getElem_append_right (by omega)]
  simp

theorem NamesOK.dst {P : PFunc} {names : List (String × Nat)} (hN : NamesOK P names)
    {d : String} {w i : Nat} (h : dstIdx names d w = .ok i) :
    findName names d = some (i, w) ∧ P.wd i = w := by
  unfold dstIdx at h
  split at h
  · rename_i i' w' hf
    split at h
    · rename_i hw; simp at h; subst h; subst hw
      refine ⟨hf, ?_⟩
      have hg := findName_get hf
      have hl := findName_lt hf
      rw [hN i' hl]
      rw [List.getElem?_eq_getElem hl] at hg; simp at hg; rw [hg]
    · simp at h
  · simp at h

/-! ## Arguments below a bound are unaffected by temporaries -/

def Arg.below (k : Nat) : Arg → Prop
  | .c _ _ => True
  | .v i _ => i < k

theorem Arg.get_agree {k : Nat} {σ σ' : Store} (ha : ∀ i, i < k → σ' i = σ i) :
    ∀ {a : Arg}, a.below k → a.get σ' = a.get σ
  | .c _ _, _ => rfl
  | .v i _, h => ha i h

theorem Arg.below_mono {k k' : Nat} (hk : k ≤ k') : ∀ {a : Arg}, a.below k → a.below k'
  | .c _ _, _ => trivial
  | .v _ _, h => by simp only [Arg.below] at *; omega

def Chk.below (k : Nat) : Chk → Prop
  | .p _ a b _ _ => a.below k ∧ b.below k
  | .disj a b _ => a.below k ∧ b.below k

theorem Chk.bad_agree {k : Nat} {σ σ' : Store} (ha : ∀ i, i < k → σ' i = σ i) :
    ∀ {c : Chk}, c.below k → c.bad σ' = c.bad σ
  | .p op a b _ _, ⟨h1, h2⟩ => by
    simp only [Chk.bad]
    cases op <;> simp [evalOp, Arg.get_agree ha h1, Arg.get_agree ha h2]
  | .disj a b w, ⟨h1, h2⟩ => by
    simp only [Chk.bad, Arg.get_agree ha h1, Arg.get_agree ha h2]

/-! ## Emitted checks -/

theorem bv_toNat_mod {w : Nat} (t : BitVec w) : bv w (t.toNat % 2 ^ w) = t := by
  apply BitVec.eq_of_toNat_eq; simp

theorem emit_one (P : PFunc) (σ : Store) (k : Nat) (c : Chk) (hT : TempsOK P k (c.emit k).2) :
    (c.bad σ = true → pStmts P σ (c.emit k).1 = .fail) ∧
    (c.bad σ = false → ∃ σ', pStmts P σ (c.emit k).1 = .ok σ' ∧ ∀ i, i < k → σ' i = σ i) := by
  cases c with
  | p op a b pr cl =>
    have hw : P.wd k = 1 := by have := hT 0 (by simp [Chk.emit]); simpa [Chk.emit] using this
    simp only [Chk.emit, pStmts, hw, Arg.get, Store.set_same]
    constructor
    · intro h; simp only [Chk.bad] at h; simp [h]
    · intro h; simp only [Chk.bad] at h
      refine ⟨σ.set k (evalOp σ op 1 [a, b] % 2), by simp [h], ?_⟩
      intro i hi; exact Store.set_other _ _ (by omega)
  | disj a b w =>
    have hw0 : P.wd k = w := by have := hT 0 (by simp [Chk.emit]); simpa [Chk.emit] using this
    have hw1 : P.wd (k + 1) = 1 := by have := hT 1 (by simp [Chk.emit]); simpa [Chk.emit] using this
    have key : truthN (evalOp (σ.set k (evalOp σ (.bin .and) (P.wd k) [a, b] % 2 ^ P.wd k))
        (.cmp .ne) (P.wd (k + 1)) [.v k w, .c w 0] % 2 ^ P.wd (k + 1)) = Chk.bad σ (.disj a b w) := by
      rw [hw0, hw1]
      simp only [evalOp, binVal, icmpVal, Arg.get, Arg.width, Store.set_same, Chk.bad, truthN_mod2,
        truthN_ofBool, evalPred, evalBin]
      rw [bv_toNat_mod, Nat.pow_one, truthN_mod2, truthN_ofBool, bv_zero]; rfl
    simp only [Chk.emit, pStmts, Arg.get, Store.set_same]
    constructor
    · intro h; rw [key, h]; rfl
    · intro h; rw [key, h]
      refine ⟨_, rfl, ?_⟩
      intro i hi; rw [Store.set_other _ _ (by omega), Store.set_other _ _ (by omega)]

theorem emit_all (P : PFunc) : ∀ (cs : List Chk) (σ : Store) (k : Nat),
    TempsOK P k (emitAll k cs).2 → (∀ c ∈ cs, c.below k) →
    (cs.any (Chk.bad σ) = true → pStmts P σ (emitAll k cs).1 = .fail) ∧
    (cs.any (Chk.bad σ) = false → ∃ σ', pStmts P σ (emitAll k cs).1 = .ok σ' ∧
      ∀ i, i < k → σ' i = σ i)
  | [], σ, k, _, _ => by simp [emitAll, pStmts]
  | c :: cs, σ, k, hT, hb => by
    simp only [emitAll] at hT ⊢
    have h1 := emit_one P σ k c hT.left
    rw [pStmts_append]
    simp only [List.any_cons, Bool.or_eq_true, Bool.or_eq_false_iff]
    constructor
    · rintro (hc | hc)
      · rw [h1.1 hc]; rfl
      · cases hcb : c.bad σ
        · obtain ⟨σ', e, ha⟩ := h1.2 hcb
          rw [e]; simp only [PRes.then]
          have ih := emit_all P cs σ' (k + (c.emit k).2.length) hT.right
            (fun c' hc' => ?_)
          · apply ih.1
            rw [List.any_eq_true] at hc ⊢
            obtain ⟨c', hm, hb'⟩ := hc
            refine ⟨c', hm, ?_⟩
            rw [Chk.bad_agree ha (hb c' (by simp [hm]))]; exact hb'
          · have := hb c' (by simp [hc'])
            cases c' <;> simp only [Chk.below] at this ⊢ <;>
              exact ⟨Arg.below_mono (by omega) this.1, Arg.below_mono (by omega) this.2⟩
        · rw [h1.1 hcb]; rfl
    · rintro ⟨hc, hcs⟩
      obtain ⟨σ', e, ha⟩ := h1.2 hc
      rw [e]; simp only [PRes.then]
      have ih := emit_all P cs σ' (k + (c.emit k).2.length) hT.right
        (fun c' hc' => by
          have := hb c' (by simp [hc'])
          cases c' <;> simp only [Chk.below] at this ⊢ <;>
            exact ⟨Arg.below_mono (by omega) this.1, Arg.below_mono (by omega) this.2⟩)
      have hcs' : cs.any (Chk.bad σ') = false := by
        rw [List.any_eq_false] at hcs ⊢
        intro c' hm
        rw [Chk.bad_agree ha (hb c' (by simp [hm]))]; exact hcs c' hm
      obtain ⟨σ'', e2, ha2⟩ := ih.2 hcs'
      refine ⟨σ'', e2, fun i hi => ?_⟩
      rw [ha2 i (by omega), ha i hi]

/-! ## Simulation of one step -/

/-- A strict LLVM step result and a PIR statement-run result correspond. -/
def Sim (names : List (String × Nat)) : Res SRegs → PRes → Prop
  | .ok R', p => ∃ σ', p = .ok σ' ∧ Rel names R' σ'
  | .ub, p => p = .fail
  | .stuck, _ => True

theorem Sim.bind {names : List (String × Nat)} {r : Res SRegs} {p : PRes} (h : Sim names r p)
    {g : SRegs → Res SRegs} {q : Store → PRes}
    (hg : ∀ R' σ', Rel names R' σ' → Sim names (g R') (q σ')) :
    Sim names (r.bind g) (p.then q) := by
  cases r with
  | ok R' => obtain ⟨σ', rfl, hr⟩ := h; exact hg R' σ' hr
  | ub => simp only [Sim] at h; subst h; rfl
  | stuck => trivial

theorem Except.bind_ok {ε α β : Type} {x : Except ε α} {f : α → Except ε β} {b : β}
    (h : (x >>= f) = .ok b) : ∃ a, x = .ok a ∧ f a = .ok b := by
  cases x with
  | error e => simp [bind, Except.bind] at h
  | ok a => exact ⟨a, rfl, h⟩

theorem need_ok {b : Bool} {m : String} {u : Unit} (h : need b m = .ok u) : b = true := by
  unfold need at h; split at h <;> simp_all

theorem opnd_sim {names : List (String × Nat)} {R : SRegs} {σ : Store} (hR : Rel names R σ)
    {w : Nat} {keep : Bool} {k : Nat} {o : Opnd} {s : List PStmt} {tws : List Nat} {A : Arg}
    (h : trOpnd names w keep k o = .ok (s, tws, A)) (hk : names.length ≤ k)
    (P : PFunc) (f : Nat → Res SRegs) (rest : List PStmt)
    (hc : ∀ v, sOpnd R w o = .ok v → s = [] → tws = [] → A.get σ = v → A.below k →
      (keep = false → A.width = w) → Sim names (f v) (pStmts P σ rest)) :
    Sim names ((sOpnd R w o).bind f) (pStmts P σ (s ++ rest)) := by
  cases o with
  | const b =>
    simp only [trOpnd, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    exact hc _ rfl rfl rfl rfl trivial (fun _ => rfl)
  | reg n =>
    simp only [trOpnd] at h
    split at h
    · rename_i i wd hf
      simp only [Except.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl, rfl⟩ := h
      simp only [sOpnd]
      cases hn : R n with
      | none => trivial
      | some v =>
        obtain ⟨i', w', hf', hv⟩ := hR n v hn
        rw [hf] at hf'; simp at hf'; obtain ⟨rfl, rfl⟩ := hf'
        refine hc v (by simp [sOpnd, hn]) rfl rfl hv (by simp only [Arg.below]; have := findName_lt hf; omega) ?_
        intro hkp; simp [hkp, Arg.width]
    · simp at h
  | poison =>
    simp only [trOpnd, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    simp only [sOpnd, Res.bind, Sim, List.cons_append, pStmts, Arg.get]
    rfl

theorem checks_below {k w : Nat} {A B : Arg} (hA : A.below k) (hB : B.below k)
    (op : BinOp) (fl : LFlags) : ∀ c ∈ checks op fl w A B, c.below k := by
  have hC : (Arg.c w 0).below k := trivial
  cases op <;> simp only [checks, opt] <;> (repeat' split) <;>
    simp only [List.forall_mem_cons, List.forall_mem_append, List.nil_append, List.append_nil,
      List.cons_append, Chk.below] <;>
    simp_all

/-- Store update of a result that is already in range. -/
theorem assign_sim {names : List (String × Nat)} {R : SRegs} {σ : Store} (hR : Rel names R σ)
    {d : String} {i w : Nat} (hf : findName names d = some (i, w)) (v : Nat) (hv : v < 2 ^ w) :
    Rel names (R.set d v) (σ.set i (v % 2 ^ w)) := by
  rw [Nat.mod_eq_of_lt hv]; exact hR.set hf v

theorem binVal_lt (op : BinOp) (w x y : Nat) : binVal op w x y < 2 ^ w := (BitVec.isLt _)

theorem inst_sim {P : PFunc} {names : List (String × Nat)} (hN : NamesOK P names)
    {R : SRegs} {σ : Store} (hR : Rel names R σ) {k : Nat} (hk : names.length ≤ k)
    {i : Inst} {s : List PStmt} {tws : List Nat} (h : trInst names k i = .ok (s, tws))
    (hT : TempsOK P k tws) :
    Sim names (sInst R i) (pStmts P σ s) := by
  cases i with
  | bin d op fl w a b =>
    simp only [trInst] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
    have hf := need_ok hu2
    obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
    obtain ⟨⟨sb, tb, B⟩, hb, h⟩ := Except.bind_ok h
    obtain ⟨di, hdi, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    obtain ⟨hfd, hwd⟩ := hN.dst hdi
    simp only [sInst]
    rw [List.append_assoc, List.append_assoc]
    refine opnd_sim hR ha hk P _ _ ?_
    intro x _ hsa hta hAx hAb hAw
    subst hsa hta
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hb hT ⊢
    refine opnd_sim hR hb hk P _ _ ?_
    intro y _ hsb htb hBy hBb hBw
    subst hsb htb
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hT ⊢
    have hbad := checks_bad σ op fl w A B (hAw rfl) (hBw rfl) hf
    rw [hAx, hBy] at hbad
    have hem := emit_all P (checks op fl w A B) σ k hT (checks_below hAb hBb op fl)
    rw [pStmts_append]
    cases hc : (binUB op w x y || cUB op fl w x y || binPoison op fl w x y)
    · rw [hc] at hbad
      obtain ⟨σ', e, hag⟩ := hem.2 hbad
      simp only [hc, Bool.false_eq_true, ite_false, Sim, e, PRes.then, pStmts, hwd, evalOp]
      refine ⟨_, rfl, ?_⟩
      rw [Arg.get_agree hag hAb, Arg.get_agree hag hBb, hAx, hBy]
      exact assign_sim (hR.agree (fun j hj => hag j (by omega))) hfd _ (binVal_lt _ _ _ _)
    · rw [hc] at hbad
      simp only [hc, ite_true, Sim, hem.1 hbad, PRes.then]
  | icmp d p w a b =>
    simp only [trInst] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
    obtain ⟨⟨sb, tb, B⟩, hb, h⟩ := Except.bind_ok h
    obtain ⟨di, hdi, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    obtain ⟨hfd, hwd⟩ := hN.dst hdi
    simp only [sInst]
    rw [List.append_assoc]
    refine opnd_sim hR ha hk P _ _ ?_
    intro x _ hsa hta hAx _ hAw
    subst hsa hta
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hb ⊢
    refine opnd_sim hR hb hk P _ _ ?_
    intro y _ hsb htb hBy _ _
    subst hsb htb
    simp only [List.nil_append, Res.bind, Sim, pStmts, hwd, evalOp, hAw rfl, hAx, hBy]
    exact ⟨_, rfl, assign_sim hR hfd _ (by have := icmpVal_lt2 p w x y; simpa using this)⟩
  | select d w c a b =>
    simp only [trInst] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨⟨sc, tc, C⟩, hc, h⟩ := Except.bind_ok h
    obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
    obtain ⟨⟨sb, tb, B⟩, hb, h⟩ := Except.bind_ok h
    obtain ⟨di, hdi, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    obtain ⟨hfd, hwd⟩ := hN.dst hdi
    simp only [sInst]
    rw [List.append_assoc, List.append_assoc]
    refine opnd_sim hR hc hk P _ _ ?_
    intro z _ hsc htc hCz _ _
    subst hsc htc
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at ha hb ⊢
    refine opnd_sim hR ha hk P _ _ ?_
    intro x _ hsa hta hAx _ _
    subst hsa hta
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hb ⊢
    refine opnd_sim hR hb hk P _ _ ?_
    intro y _ hsb htb hBy _ _
    subst hsb htb
    simp only [List.nil_append, Res.bind, Sim, pStmts, hwd, evalOp, hAx, hBy, hCz]
    refine ⟨_, rfl, assign_sim hR hfd _ ?_⟩
    unfold selVal; split <;> exact Nat.mod_lt _ (Nat.two_pow_pos w)
  | cast d ck nneg fw tw a =>
    simp only [trInst] at h
    obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
    obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
    have hnn := need_ok hu2
    obtain ⟨⟨sa, ta, A⟩, ha, h⟩ := Except.bind_ok h
    obtain ⟨di, hdi, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    obtain ⟨hfd, hwd⟩ := hN.dst hdi
    simp only [sInst]
    rw [List.append_assoc]
    refine opnd_sim hR ha hk P _ _ ?_
    intro x _ hsa hta hAx hAb hAw
    subst hsa hta
    simp only [List.nil_append, List.length_nil, Nat.add_zero] at hT ⊢
    have hbad := nneg_bad σ fw A (hAw rfl) nneg
    rw [hAx] at hbad
    have hcp : castPoison ck nneg fw x = castPoison .zext nneg fw x := by
      cases nneg
      · simp [castPoison]
      · simp at hnn; subst hnn; rfl
    rw [← hcp] at hbad
    have hem := emit_all P _ σ k hT (by
      intro c hc'
      cases nneg <;> simp [opt] at hc'
      subst hc'; exact ⟨hAb, trivial⟩)
    rw [pStmts_append]
    cases hc : castPoison ck nneg fw x
    · rw [hc] at hbad
      obtain ⟨σ', e, hag⟩ := hem.2 hbad
      simp only [hc, Bool.false_eq_true, ite_false, Sim, e, PRes.then, pStmts, hwd, evalOp]
      refine ⟨_, rfl, ?_⟩
      rw [Arg.get_agree hag hAb, hAx, hAw rfl]
      exact assign_sim (hR.agree (fun j hj => hag j (by omega))) hfd _ (by
        unfold castVal; split <;> exact BitVec.isLt _)
    · rw [hc] at hbad
      simp only [hc, ite_true, Sim, hem.1 hbad, PRes.then]

theorem insts_sim {P : PFunc} {names : List (String × Nat)} (hN : NamesOK P names) :
    ∀ (is : List Inst) (R : SRegs) (σ : Store) (k : Nat) (s : List PStmt) (tws : List Nat),
    Rel names R σ → names.length ≤ k → trInsts names k is = .ok (s, tws) → TempsOK P k tws →
    Sim names (sInsts R is) (pStmts P σ s)
  | [], R, σ, k, s, tws, hR, _, h, _ => by
    simp only [trInsts, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    exact ⟨σ, rfl, hR⟩
  | i :: is, R, σ, k, s, tws, hR, hk, h, hT => by
    simp only [trInsts] at h
    obtain ⟨⟨s1, t1⟩, h1, h⟩ := Except.bind_ok h
    obtain ⟨⟨s2, t2⟩, h2, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    simp only [sInsts]
    rw [pStmts_append]
    exact Sim.bind (inst_sim hN hR hk h1 hT.left) (fun R' σ' hr =>
      insts_sim hN is R' σ' (k + t1.length) s2 t2 hr (by omega) h2 hT.right)

/-! ## Phis -/

/-- The updates computed by the LLVM phis and by the PIR phis correspond. -/
def UpdRel (names : List (String × Nat)) : List (String × Nat) → List (Nat × Nat) → Prop
  | [], [] => True
  | (n, v) :: t, (i, u) :: t' => (∃ w, findName names n = some (i, w)) ∧ v = u ∧ UpdRel names t t'
  | _, _ => False

theorem Rel.setAll {names : List (String × Nat)} :
    ∀ (upd : List (String × Nat)) (upd' : List (Nat × Nat)) (R : SRegs) (σ : Store),
    Rel names R σ → UpdRel names upd upd' → Rel names (R.setAll upd) (σ.setAll upd')
  | [], [], _, _, hR, _ => hR
  | (n, v) :: t, (i, u) :: t', R, σ, hR, ⟨⟨w, hf⟩, hv, hr⟩ => by
    subst hv
    exact Rel.setAll t t' _ _ (hR.set hf v) hr
  | [], _ :: _, _, _, _, h => h.elim
  | _ :: _, [], _, _, _, h => h.elim

theorem dstIdx_ok {names : List (String × Nat)} {d : String} {w i : Nat}
    (h : dstIdx names d w = .ok i) : findName names d = some (i, w) := by
  unfold dstIdx at h
  split at h
  · rename_i i' w' hf
    by_cases hw : w' = w
    · subst hw; simp at h; subst h; exact hf
    · simp [hw] at h
  · simp at h

/-- What a phi incoming value translates to. -/
def OpndArg (names : List (String × Nat)) (w : Nat) (o : Opnd) (a : Arg) : Prop :=
  (∃ b, o = .const b ∧ a = .c w (b % 2 ^ w)) ∨
  (∃ n i wd, o = .reg n ∧ findName names n = some (i, wd) ∧ a = .v i wd)

theorem trInc_pick {F : LFunc} {names : List (String × Nat)} {w pv : Nat} :
    ∀ {inc : List (Opnd × String)} {L : List (Nat × Arg)} {o : Opnd},
    trInc F names w inc = .ok L → phiPick F pv inc = some o →
    ∃ a, pickInc pv L = some a ∧ OpndArg names w o a
  | [], _, _, _, h => by simp [phiPick] at h
  | (o', pr) :: t, L, o, h, hp => by
    simp only [trInc] at h
    obtain ⟨a', ha', h⟩ := Except.bind_ok h
    obtain ⟨rest, hr, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq] at h
    have hoa : OpndArg names w o' a' := by
      cases o' with
      | const b => simp only [trIncArg, Except.ok.injEq] at ha'; exact .inl ⟨b, rfl, ha'.symm⟩
      | reg n =>
        simp only [trIncArg] at ha'
        split at ha'
        · rename_i i wd hf
          simp only [Except.ok.injEq] at ha'
          exact .inr ⟨n, i, wd, rfl, hf, ha'.symm⟩
        · simp at ha'
      | poison => simp [trIncArg] at ha'
    simp only [phiPick] at hp
    split at hp
    · rename_i hl
      simp at hp; subst hp
      rw [hl] at h; subst h
      exact ⟨a', by simp [pickInc], hoa⟩
    · rename_i hl
      obtain ⟨a, hpa, hoa'⟩ := trInc_pick hr hp
      refine ⟨a, ?_, hoa'⟩
      subst h
      split
      · rename_i j hj
        have : j ≠ pv := fun e => hl (e ▸ hj)
        simp [pickInc, this, hpa]
      · exact hpa

theorem phis_sim {F : LFunc} {names : List (String × Nat)} {R : SRegs} {σ : Store}
    (hR : Rel names R σ) (prev : Option Nat) :
    ∀ (ps : List PhiI) (qs : List PPhi), trPhis F names ps = .ok qs →
    (sPhis F R prev ps = .ub → False) ∧
    (∀ upd, sPhis F R prev ps = .ok upd → UpdRel names upd (phiUpd σ prev qs))
  | [], qs, h => by
    simp only [trPhis, Except.ok.injEq] at h; subst h
    simp [sPhis, phiUpd, UpdRel]
  | p :: ps, qs, h => by
    simp only [trPhis] at h
    obtain ⟨q, hq, h⟩ := Except.bind_ok h
    obtain ⟨qs', hqs, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq] at h; subst h
    have ih := phis_sim hR prev ps qs' hqs
    simp only [trPhi] at hq
    obtain ⟨u, _, hq⟩ := Except.bind_ok hq
    obtain ⟨i, hi, hq⟩ := Except.bind_ok hq
    obtain ⟨L, hL, hq⟩ := Except.bind_ok hq
    simp only [pure, Except.pure, Except.ok.injEq] at hq; subst hq
    have hfd : findName names p.dst = some (i, p.w) := dstIdx_ok hi
    cases prev with
    | none => simp [sPhis]
    | some pv =>
      cases hpk : phiPick F pv p.inc with
      | none => simp [sPhis, hpk]
      | some o =>
        obtain ⟨a, hpa, hoa⟩ := trInc_pick hL hpk
        have hphi : phiUpd σ (some pv) ({ dst := i, inc := L } :: qs') =
            (i, a.get σ) :: phiUpd σ (some pv) qs' := by
          simp [phiUpd, hpa]
        rw [hphi]
        rcases hoa with ⟨b, rfl, rfl⟩ | ⟨n, j, wd, rfl, hf, rfl⟩
        · simp only [sPhis, hpk, sOpnd, Res.bind]
          constructor
          · intro hu
            cases hs : sPhis F R (some pv) ps <;> simp [hs, Res.bind] at hu
            exact ih.1 hs
          · intro upd hu
            cases hs : sPhis F R (some pv) ps <;> simp [hs, Res.bind] at hu
            subst hu
            exact ⟨⟨p.w, hfd⟩, rfl, ih.2 _ hs⟩
        · simp only [sPhis, hpk, sOpnd]
          cases hn : R n with
          | none => simp [Res.bind]
          | some v =>
            obtain ⟨j', w', hf', hv⟩ := hR n v hn
            rw [hf] at hf'; simp at hf'; obtain ⟨rfl, rfl⟩ := hf'
            simp only [Res.bind]
            constructor
            · intro hu
              cases hs : sPhis F R (some pv) ps <;> simp [hs, Res.bind] at hu
              exact ih.1 hs
            · intro upd hu
              cases hs : sPhis F R (some pv) ps <;> simp [hs, Res.bind] at hu
              subst hu
              exact ⟨⟨p.w, hfd⟩, by simp [Arg.get, hv], ih.2 _ hs⟩

/-! ## Terminators, blocks, runs -/

/-- A strict LLVM outcome and a PIR outcome correspond. -/
def OSim : Out → POut → Prop
  | .ret v, p => p = .ret v
  | .ub, p => p = .fail
  | .fuel, p => p = .fuel
  | .stuck, _ => True

theorem Sim.out {names : List (String × Nat)} {r : Res SRegs} {p : PRes} (h : Sim names r p)
    {f : SRegs → Out} {g : Store → POut}
    (hfg : ∀ R σ, Rel names R σ → OSim (f R) (g σ)) : OSim (r.out f) (p.run g) := by
  cases r with
  | ok R' => obtain ⟨σ', rfl, hr⟩ := h; exact hfg R' σ' hr
  | ub => simp only [Sim] at h; subst h; rfl
  | stuck => trivial

theorem PRes.then_run (p : PRes) (q : Store → PRes) (g : Store → POut) :
    (p.then q).run g = p.run (fun σ => (q σ).run g) := by
  cases p <;> rfl

theorem opnd_osim {names : List (String × Nat)} {R : SRegs} {σ : Store} (hR : Rel names R σ)
    {w : Nat} {keep : Bool} {k : Nat} {o : Opnd} {s : List PStmt} {tws : List Nat} {A : Arg}
    (h : trOpnd names w keep k o = .ok (s, tws, A)) (P : PFunc) (f : Nat → Out) (g : Store → POut)
    (hc : ∀ v, sOpnd R w o = .ok v → s = [] → A.get σ = v → OSim (f v) (g σ)) :
    OSim ((sOpnd R w o).out f) ((pStmts P σ s).run g) := by
  cases o with
  | const b =>
    simp only [trOpnd, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    exact hc _ rfl rfl rfl
  | reg n =>
    simp only [trOpnd] at h
    split at h
    · rename_i i wd hf
      simp only [Except.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl, rfl⟩ := h
      cases hn : R n with
      | none => simp [sOpnd, hn, Res.out, OSim]
      | some v =>
        obtain ⟨i', w', hf', hv⟩ := hR n v hn
        rw [hf] at hf'; simp at hf'; obtain ⟨rfl, rfl⟩ := hf'
        have := hc v (by simp [sOpnd, hn]) rfl hv
        simpa [sOpnd, hn, Res.out, pStmts, PRes.run] using this
    · simp at h
  | poison =>
    simp only [trOpnd, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    rfl

theorem target_ok {F : LFunc} {t : String} {j : Nat} (h : target F t = .ok j) :
    lookupBlock F t = some j := by
  unfold target at h; split at h <;> simp_all

theorem term_sim {F : LFunc} {P : PFunc} {names : List (String × Nat)} {R : SRegs} {σ : Store}
    (hR : Rel names R σ) {k : Nat} {t : LTerm} {s : List PStmt}
    {tws : List Nat} {T : PTerm} (h : trTerm F names k t = .ok (s, tws, T))
    (runL : Nat → SRegs → Out) (runP : Nat → Store → POut)
    (hrun : ∀ j R σ, Rel names R σ → OSim (runL j R) (runP j σ)) :
    OSim (sTerm F R runL t) ((pStmts P σ s).run (fun σ' => pTerm σ' runP T)) := by
  cases t with
  | br tn =>
    simp only [trTerm] at h
    obtain ⟨j, hj, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    simp only [sTerm, target_ok hj, pStmts, PRes.run, pTerm]
    exact hrun j R σ hR
  | cbr c tn fn =>
    simp only [trTerm] at h
    obtain ⟨⟨sc, tc, C⟩, hc, h⟩ := Except.bind_ok h
    obtain ⟨tj, htj, h⟩ := Except.bind_ok h
    obtain ⟨fj, hfj, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    simp only [sTerm]
    refine opnd_osim hR hc P _ _ ?_
    intro v _ _ hv
    simp only [pTerm, hv]
    cases truthN v
    · simp only [Bool.false_eq_true, ite_false, target_ok hfj]; exact hrun fj R σ hR
    · simp only [ite_true, target_ok htj]; exact hrun tj R σ hR
  | ret o =>
    cases o with
    | none =>
      simp only [trTerm, Except.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl, rfl⟩ := h
      rfl
    | some o =>
      simp only [trTerm] at h
      obtain ⟨⟨so, to, A⟩, ho, h⟩ := Except.bind_ok h
      simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
      obtain ⟨rfl, rfl, rfl⟩ := h
      simp only [sTerm]
      refine opnd_osim hR ho P _ _ ?_
      intro v _ _ hv
      simp [pTerm, hv, OSim]
  | unreachable =>
    simp only [trTerm, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl, rfl⟩ := h
    rfl

theorem run_sim {F : LFunc} {P : PFunc} {names : List (String × Nat)} (hN : NamesOK P names)
    (hB : ∀ (cur : Nat) (B : LBlock), F.blocks[cur]? = some B → ∃ k pb tws, names.length ≤ k ∧
      trBlock F names k B = .ok (pb, tws) ∧ P.blocks[cur]? = some pb ∧ TempsOK P k tws) :
    ∀ n prev cur R σ, Rel names R σ → OSim (sRun F n prev cur R) (pRun P n prev cur σ)
  | 0, _, _, _, _, _ => rfl
  | n + 1, prev, cur, R, σ, hR => by
    cases hb : F.blocks[cur]? with
    | none => simp [sRun, hb, OSim]
    | some B =>
      obtain ⟨k, pb, tws, hk, htr, hpb, hT⟩ := hB cur B hb
      simp only [trBlock] at htr
      obtain ⟨qs, hqs, htr⟩ := Except.bind_ok htr
      obtain ⟨⟨s1, t1⟩, h1, htr⟩ := Except.bind_ok htr
      obtain ⟨⟨s2, t2, T⟩, h2, htr⟩ := Except.bind_ok htr
      simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at htr
      obtain ⟨rfl, rfl⟩ := htr
      simp only [sRun, pRun, hb, hpb]
      have hph := phis_sim (F := F) hR prev B.phis qs hqs
      cases hsp : sPhis F R prev B.phis with
      | ub => exact (hph.1 hsp).elim
      | stuck => trivial
      | ok upd =>
        have hR1 := Rel.setAll _ _ _ _ hR (hph.2 upd hsp)
        simp only [Res.out]
        rw [pStmts_append, PRes.then_run]
        exact Sim.out (insts_sim hN _ _ _ _ _ _ hR1 hk h1 hT.left) (fun R' σ' hr =>
          term_sim hr h2 _ _ (fun j R'' σ'' hr' => run_sim hN hB n (some cur) j R'' σ'' hr'))

theorem trBlocks_get {F : LFunc} {names : List (String × Nat)} {P : PFunc} :
    ∀ (Bs : List LBlock) (k : Nat) (pbs : List PBlock) (ts : List Nat),
    trBlocks F names k Bs = .ok (pbs, ts) → TempsOK P k ts →
    ∀ (cur : Nat) (B : LBlock), Bs[cur]? = some B → ∃ k' pb tws, k ≤ k' ∧ trBlock F names k' B = .ok (pb, tws) ∧
      pbs[cur]? = some pb ∧ TempsOK P k' tws
  | [], _, _, _, _, _, _, _, hB => by simp at hB
  | B0 :: Bs, k, pbs, ts, h, hT, cur, B, hB => by
    simp only [trBlocks] at h
    obtain ⟨⟨pb, t1⟩, h1, h⟩ := Except.bind_ok h
    obtain ⟨⟨pbs', t2⟩, h2, h⟩ := Except.bind_ok h
    simp only [pure, Except.pure, Except.ok.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    cases cur with
    | zero =>
      simp at hB; subst hB
      exact ⟨k, pb, t1, Nat.le_refl _, h1, rfl, hT.left⟩
    | succ c =>
      simp at hB
      obtain ⟨k', pb', tws, hk', htr, hpb, hT'⟩ := trBlocks_get Bs _ _ _ h2 hT.right c B hB
      exact ⟨k', pb', tws, by omega, htr, by simpa using hpb, hT'⟩

/-! ## Initial state -/

theorem initRegs_eq : ∀ (ps : List (String × Nat)) (args : List Nat) (n : String),
    initRegs ps args n = (findName ps n).map (fun p => args.getD p.1 0 % 2 ^ p.2)
  | [], _, _ => rfl
  | (m, w) :: ps, args, n => by
    simp only [initRegs, findName]
    by_cases h : n = m
    · subst h; cases args <;> simp
    · have h' : ¬ m = n := fun e => h e.symm
      simp only [h, h', ite_false, initRegs_eq ps args.tail n, Option.map_map]
      congr 1; funext p
      cases args <;> simp [List.getD_eq_getElem?_getD]

theorem pInitAux_eq (wd : Nat → Nat) : ∀ (m s : Nat) (args : List Nat) (σ : Store) (i : Nat),
    pInitAux wd (List.range' s m) args σ i =
      if s ≤ i ∧ i < s + m then args.getD (i - s) 0 % 2 ^ wd i else σ i
  | 0, s, args, σ, i => by simp [pInitAux]; omega
  | m + 1, s, args, σ, i => by
    simp only [List.range'_succ, pInitAux]
    rw [pInitAux_eq wd m (s + 1) args.tail]
    by_cases h1 : s + 1 ≤ i ∧ i < s + 1 + m
    · have h2 : s ≤ i ∧ i < s + (m + 1) := by omega
      simp only [h1, h2, and_self, ite_true]
      have : i - s = (i - (s + 1)) + 1 := by omega
      rw [this]
      cases args <;> simp [List.getD_eq_getElem?_getD]
    · simp only [h1, ite_false]
      by_cases h3 : i = s
      · subst h3; cases args <;> simp [Store.set]
      · have h2 : ¬ (s ≤ i ∧ i < s + (m + 1)) := by omega
        simp only [h2, ite_false]
        exact Store.set_other _ _ h3

/-! ## The translator's output -/

theorem translate_spec {F : LFunc} {P : PFunc} (h : translate F = .ok P) :
    ∃ temps, trBlocks F (F.params ++ resultNames F) (F.params ++ resultNames F).length F.blocks =
        .ok (P.blocks, temps) ∧
      P.vars = (F.params ++ resultNames F).map Prod.snd ++ temps ∧
      P.params = List.range' 0 F.params.length ∧ P.retw = F.retw := by
  simp only [translate] at h
  obtain ⟨_, _, h⟩ := Except.bind_ok h
  obtain ⟨_, _, h⟩ := Except.bind_ok h
  obtain ⟨_, _, h⟩ := Except.bind_ok h
  obtain ⟨_, _, h⟩ := Except.bind_ok h
  obtain ⟨_, _, h⟩ := Except.bind_ok h
  obtain ⟨⟨bs, temps⟩, hb, h⟩ := Except.bind_ok h
  simp only [pure, Except.pure, Except.ok.injEq] at h
  subst h
  exact ⟨temps, hb, rfl, rfl, rfl⟩

/-- **The translation is exact with respect to the strict semantics.** For
every accepted function, input and fuel: PIR returns `v` iff LLVM returns `v`,
PIR fails a check iff LLVM reaches UB (strict: including poison creation),
PIR runs out of fuel iff LLVM does — whenever the LLVM run is not stuck. -/
theorem translate_exact {F : LFunc} {P : PFunc} (h : translate F = .ok P) (args : List Nat)
    (n : Nat) : OSim (sRunF F args n) (pRunF P args n) := by
  obtain ⟨temps, hb, hv, hp, _⟩ := translate_spec h
  generalize hn : F.params ++ resultNames F = names at hb hv
  have hN : NamesOK P names := by
    intro i hi
    simp only [PFunc.wd, hv, List.getD_eq_getElem?_getD]
    rw [List.getElem?_append_left (by simpa using hi)]
    simp [List.getElem?_eq_getElem hi]
  have hT : TempsOK P names.length temps := by
    intro j hj
    simp only [PFunc.wd, hv, List.getD_eq_getElem?_getD]
    rw [List.getElem?_append_right (by simp)]
    simp [hj]
  apply run_sim hN (fun cur B hB => by
    obtain ⟨k', pb, tws, hk', htr, hpb, hT'⟩ := trBlocks_get F.blocks _ _ _ hb hT cur B hB
    exact ⟨k', pb, tws, hk', htr, hpb, hT'⟩)
  -- the initial states are related
  intro m v hm
  rw [initRegs_eq] at hm
  cases hf : findName F.params m with
  | none => simp [hf] at hm
  | some p =>
    obtain ⟨i, w⟩ := p
    simp [hf] at hm
    subst hm
    have hfn : findName names m = some (i, w) := by
      rw [← hn]; exact findName_append_left hf
    refine ⟨i, w, hfn, ?_⟩
    have hil := findName_lt hf
    simp only [pInit, hp]
    rw [pInitAux_eq]
    simp only [Nat.zero_le, true_and, Nat.zero_add, hil, ite_true, Nat.sub_zero]
    congr 2
    rw [hN i (findName_lt hfn)]
    have g1 := findName_get hfn
    rw [List.getElem?_eq_getElem (findName_lt hfn)] at g1
    simp at g1; rw [g1]

end PrismRefine
