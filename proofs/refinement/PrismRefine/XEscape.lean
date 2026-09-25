/-
PRISM refinement, extended fragment — returning a pointer: the stack-escape
check (`MemTr::stack_escape_check`, MEM-STACK-ESCAPE).

The analysed function returning a pointer into one of its own stack objects
(the `alloca`s of its entry block before the first call, `Frame::allocas`)
returns a dangling pointer (C17 6.2.4p2); PRISM reports it at the return.
`escChk_run`: the statements the translator emits fail exactly when the
returned pointer is not null and its object is one of those objects;
`escHit_ok`: the LLVM semantics' condition (`escHit`, `XLlvm.lean`) is that
condition on the related registers.
-/
import PrismRefine.XRefine

namespace PrismRefine

open PrismSem

/-! ### One statement each -/

theorem xs_lshr48 (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (j : Nat) (A : Arg) (r : List PStmt)
    (w : P.wd j = 64) :
    xStmts P ω σ t (.assign j (.bin .lshr) [A, c64 48] :: r) =
      xStmts P ω (σ.set j (ptrObj (A.get σ % 2 ^ 64))) t r := by
  rw [xStmts_assign]; simp only [evalOpM, evalOp, w, c64_get, lshr48, ptrObj_mod]

theorem xs_eq64 (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (j : Nat) (X Y : Arg) (r : List PStmt)
    (w : P.wd j = 1) (hX : X.width = 64) :
    xStmts P ω σ t (.assign j (.cmp .eq) [X, Y] :: r) =
      xStmts P ω (σ.set j (decide (X.get σ % 2 ^ 64 = Y.get σ % 2 ^ 64)).toNat) t r := by
  rw [xStmts_assign]; simp only [evalOpM, evalOp, w, hX, icmpVal_eq', pred_eq, boolToNat_mod1]

theorem xs_ne64 (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (j : Nat) (X Y : Arg) (r : List PStmt)
    (w : P.wd j = 1) (hX : X.width = 64) :
    xStmts P ω σ t (.assign j (.cmp .ne) [X, Y] :: r) =
      xStmts P ω (σ.set j (!decide (X.get σ % 2 ^ 64 = Y.get σ % 2 ^ 64)).toNat) t r := by
  rw [xStmts_assign]; simp only [evalOpM, evalOp, w, hX, icmpVal_eq', pred_ne, boolToNat_mod1]

theorem xs_or1 (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (j : Nat) (X Y : Arg) (r : List PStmt)
    (w : P.wd j = 1) {a b : Bool} (hX : X.get σ = a.toNat) (hY : Y.get σ = b.toNat) :
    xStmts P ω σ t (.assign j (.bin .or) [X, Y] :: r) = xStmts P ω (σ.set j (a || b).toNat) t r := by
  rw [xStmts_assign]; simp only [evalOpM, evalOp, w, hX, hY, or1, boolToNat_mod1]

theorem xs_and1 (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (j : Nat) (X Y : Arg) (r : List PStmt)
    (w : P.wd j = 1) {a b : Bool} (hX : X.get σ = a.toNat) (hY : Y.get σ = b.toNat) :
    xStmts P ω σ t (.assign j (.bin .and) [X, Y] :: r) = xStmts P ω (σ.set j (a && b).toNat) t r := by
  rw [xStmts_assign]; simp only [evalOpM, evalOp, w, hX, hY, and1, boolToNat_mod1]

theorem get_set3 {A : Arg} {j : Nat} (hA : A.below j) (σ : Store) {i : Nat} (hi : j ≤ i) (v : Nat) :
    A.get (σ.set i v) = A.get σ := Arg.get_set_ge hA σ v hi

theorem any_congr_mem {α : Type} {p q : α → Bool} : ∀ {l : List α}, (∀ a ∈ l, p a = q a) → l.any p = l.any q
  | [], _ => rfl
  | a :: l, h => by
    simp only [List.any_cons, h a List.mem_cons_self, any_congr_mem (fun x hx => h x (List.mem_cons_of_mem _ hx))]

/-! ### The loop over the objects -/

/-- The running `or` of `escLoop`: every statement is pure, and the final
hit variable holds `b0 || (some object is r0)`. -/
theorem escLoop_run (P : PFunc) (ω : Nat → Nat) (t : World) (ro : Arg) (r0 : Nat) (hr0 : r0 < 2 ^ 64) :
    ∀ (As : List Arg) (j : Nat) (hit : Option Arg) (b0 : Bool) (σ : Store),
    ro.below j → ro.width = 64 → ro.get σ = r0 → (∀ a ∈ As, a.below j) →
    (∀ h, hit = some h → h.below j ∧ h.get σ = b0.toNat) → (hit = none → b0 = false) →
    TempsOK P j (escLoop ro j hit As).2.1 →
    ∃ σ', xStmts P ω σ t (escLoop ro j hit As).1 = .ok σ' t ∧ (∀ i, i < j → σ' i = σ i) ∧
      (∀ h, (escLoop ro j hit As).2.2 = some h →
        h.below (j + (escLoop ro j hit As).2.1.length) ∧
        h.get σ' = (b0 || As.any (fun a => r0 == ptrObj (a.get σ % 2 ^ 64))).toNat) ∧
      ((escLoop ro j hit As).2.2 = none → hit = none ∧ As = [])
  | [], j, hit, b0, σ, _, _, _, _, hh, _, _ => by
    refine ⟨σ, by simp [escLoop], fun _ _ => rfl, fun h e => ?_, fun e => ⟨by simpa [escLoop] using e, rfl⟩⟩
    simp only [escLoop] at e ⊢
    obtain ⟨h1, h2⟩ := hh h e
    exact ⟨by simpa using h1, by simpa using h2⟩
  | a :: As, j, none, b0, σ, hro, hrw, hrg, hA, _, hb0, hT => by
    have hE : escLoop ro j none (a :: As) =
        ([.assign j (.bin .lshr) [a, c64 48], .assign (j + 1) (.cmp .eq) [ro, .v j 64]] ++
          (escLoop ro (j + 2) (some (.v (j + 1) 1)) As).1,
         [64, 1] ++ (escLoop ro (j + 2) (some (.v (j + 1) 1)) As).2.1,
         (escLoop ro (j + 2) (some (.v (j + 1) 1)) As).2.2) := rfl
    rw [hE] at hT ⊢
    have w0 : P.wd j = 64 := by simpa using hT.left 0 (by decide)
    have w1 : P.wd (j + 1) = 1 := by simpa using hT.left 1 (by decide)
    have hTr : TempsOK P (j + 2) (escLoop ro (j + 2) (some (.v (j + 1) 1)) As).2.1 := hT.right
    have hab : a.below j := hA a List.mem_cons_self
    have hro1 : ro.get ((σ.set j (ptrObj (a.get σ % 2 ^ 64))).set (j + 1)
        (decide (r0 = ptrObj (a.get σ % 2 ^ 64))).toNat) = r0 := by
      rw [get_set3 hro _ (by omega), get_set3 hro _ (by omega), hrg]
    obtain ⟨σ', e1, ag, hh, hn⟩ := escLoop_run P ω t ro r0 hr0 As (j + 2) (some (.v (j + 1) 1))
      (decide (r0 = ptrObj (a.get σ % 2 ^ 64)))
      ((σ.set j (ptrObj (a.get σ % 2 ^ 64))).set (j + 1) (decide (r0 = ptrObj (a.get σ % 2 ^ 64))).toNat)
      (Arg.below_mono (by omega) hro) hrw hro1
      (fun x hx => Arg.below_mono (by omega) (hA x (List.mem_cons_of_mem _ hx)))
      (fun h hh => by
        simp only [Option.some.injEq] at hh; subst hh
        exact ⟨by simp [Arg.below], by simp [set_apply]⟩)
      (fun h => by cases h) hTr
    have hany : As.any (fun x => r0 == ptrObj (x.get ((σ.set j (ptrObj (a.get σ % 2 ^ 64))).set (j + 1)
          (decide (r0 = ptrObj (a.get σ % 2 ^ 64))).toNat) % 2 ^ 64)) =
        As.any (fun x => r0 == ptrObj (x.get σ % 2 ^ 64)) := by
      apply any_congr_mem; intro x hx
      have hx' := hA x (List.mem_cons_of_mem _ hx)
      rw [get_set3 hx' _ (by omega), get_set3 hx' _ (by omega)]
    generalize escLoop ro (j + 2) (some (.v (j + 1) 1)) As = L at hT e1 hh hn ⊢
    refine ⟨σ', ?_, fun i hi => ?_, fun h hs => ?_, fun hs => absurd (hn hs).1 (by simp)⟩
    · rw [List.cons_append, List.cons_append, List.nil_append, xs_lshr48 P ω σ t j a _ w0,
        xs_eq64 P ω _ t (j + 1) ro _ _ w1 hrw, get_set3 hro _ (Nat.le_refl j), hrg, Arg.get_v',
        show σ.set j (ptrObj (a.get σ % 2 ^ 64)) j = ptrObj (a.get σ % 2 ^ 64) by simp [set_apply],
        Nat.mod_eq_of_lt hr0, ptrObj_mod]
      exact e1
    · rw [ag i (by omega)]; simp only [set_apply, show i ≠ j by omega, show i ≠ j + 1 by omega, ite_false]
    · obtain ⟨h1, h2⟩ := hh h hs
      refine ⟨Arg.below_mono (by simp only [List.length_append, List.length_cons, List.length_nil]; omega) h1, ?_⟩
      rw [h2, hany, hb0 rfl, List.any_cons, Bool.false_or]
      congr 1
  | a :: As, j, some h0, b0, σ, hro, hrw, hrg, hA, hh0, _, hT => by
    have hE : escLoop ro j (some h0) (a :: As) =
        ([.assign j (.bin .lshr) [a, c64 48], .assign (j + 1) (.cmp .eq) [ro, .v j 64],
          .assign (j + 2) (.bin .or) [h0, .v (j + 1) 1]] ++ (escLoop ro (j + 3) (some (.v (j + 2) 1)) As).1,
         [64, 1, 1] ++ (escLoop ro (j + 3) (some (.v (j + 2) 1)) As).2.1,
         (escLoop ro (j + 3) (some (.v (j + 2) 1)) As).2.2) := rfl
    rw [hE] at hT ⊢
    have w0 : P.wd j = 64 := by simpa using hT.left 0 (by decide)
    have w1 : P.wd (j + 1) = 1 := by simpa using hT.left 1 (by decide)
    have w2 : P.wd (j + 2) = 1 := by simpa using hT.left 2 (by decide)
    have hTr : TempsOK P (j + 3) (escLoop ro (j + 3) (some (.v (j + 2) 1)) As).2.1 := hT.right
    obtain ⟨hb, hg⟩ := hh0 h0 rfl
    have hro1 : ro.get (((σ.set j (ptrObj (a.get σ % 2 ^ 64))).set (j + 1)
        (decide (r0 = ptrObj (a.get σ % 2 ^ 64))).toNat).set (j + 2)
        (b0 || decide (r0 = ptrObj (a.get σ % 2 ^ 64))).toNat) = r0 := by
      rw [get_set3 hro _ (by omega), get_set3 hro _ (by omega), get_set3 hro _ (by omega), hrg]
    obtain ⟨σ', e1, ag, hh, hn⟩ := escLoop_run P ω t ro r0 hr0 As (j + 3) (some (.v (j + 2) 1))
      (b0 || decide (r0 = ptrObj (a.get σ % 2 ^ 64)))
      (((σ.set j (ptrObj (a.get σ % 2 ^ 64))).set (j + 1)
        (decide (r0 = ptrObj (a.get σ % 2 ^ 64))).toNat).set (j + 2)
        (b0 || decide (r0 = ptrObj (a.get σ % 2 ^ 64))).toNat)
      (Arg.below_mono (by omega) hro) hrw hro1
      (fun x hx => Arg.below_mono (by omega) (hA x (List.mem_cons_of_mem _ hx)))
      (fun h hh => by
        simp only [Option.some.injEq] at hh; subst hh
        exact ⟨by simp [Arg.below], by simp [set_apply]⟩)
      (fun h => by cases h) hTr
    have hany : As.any (fun x => r0 == ptrObj (x.get (((σ.set j (ptrObj (a.get σ % 2 ^ 64))).set (j + 1)
          (decide (r0 = ptrObj (a.get σ % 2 ^ 64))).toNat).set (j + 2)
          (b0 || decide (r0 = ptrObj (a.get σ % 2 ^ 64))).toNat) % 2 ^ 64)) =
        As.any (fun x => r0 == ptrObj (x.get σ % 2 ^ 64)) := by
      apply any_congr_mem; intro x hx
      have hx' := hA x (List.mem_cons_of_mem _ hx)
      rw [get_set3 hx' _ (by omega), get_set3 hx' _ (by omega), get_set3 hx' _ (by omega)]
    have hv1 : (Arg.v (j + 1) 1).get ((σ.set j (ptrObj (a.get σ % 2 ^ 64))).set (j + 1)
        (decide (r0 = ptrObj (a.get σ % 2 ^ 64))).toNat) = (decide (r0 = ptrObj (a.get σ % 2 ^ 64))).toNat := by
      simp [set_apply]
    have hh1 : h0.get ((σ.set j (ptrObj (a.get σ % 2 ^ 64))).set (j + 1)
        (decide (r0 = ptrObj (a.get σ % 2 ^ 64))).toNat) = b0.toNat := by
      rw [get_set3 hb _ (by omega), get_set3 hb _ (Nat.le_refl j), hg]
    generalize escLoop ro (j + 3) (some (.v (j + 2) 1)) As = L at hT e1 hh hn ⊢
    refine ⟨σ', ?_, fun i hi => ?_, fun h hs => ?_, fun hs => absurd (hn hs).1 (by simp)⟩
    · rw [List.cons_append, List.cons_append, List.cons_append, List.nil_append, xs_lshr48 P ω σ t j a _ w0,
        xs_eq64 P ω _ t (j + 1) ro _ _ w1 hrw, get_set3 hro _ (Nat.le_refl j), hrg, Arg.get_v',
        show σ.set j (ptrObj (a.get σ % 2 ^ 64)) j = ptrObj (a.get σ % 2 ^ 64) by simp [set_apply],
        Nat.mod_eq_of_lt hr0, ptrObj_mod]
      rw [xs_or1 P ω _ t (j + 2) h0 _ _ w2 hh1 hv1]
      exact e1
    · rw [ag i (by omega)]
      simp only [set_apply, show i ≠ j by omega, show i ≠ j + 1 by omega, show i ≠ j + 2 by omega, ite_false]
    · obtain ⟨h1, h2⟩ := hh h hs
      refine ⟨Arg.below_mono (by simp only [List.length_append, List.length_cons, List.length_nil]; omega) h1, ?_⟩
      rw [h2, hany, List.any_cons, Bool.or_assoc]
      congr 2

/-- The loop always produces a hit variable for a non-empty list. -/
theorem escLoop_some (ro : Arg) : ∀ (As : List Arg) (j : Nat) (hit : Option Arg),
    hit.isSome ∨ As ≠ [] → ∃ h, (escLoop ro j hit As).2.2 = some h
  | [], _, some h, _ => ⟨h, rfl⟩
  | [], _, none, hs => by simp at hs
  | _ :: As, j, none, _ => escLoop_some ro As (j + 2) _ (.inl rfl)
  | _ :: As, j, some _, _ => escLoop_some ro As (j + 3) _ (.inl rfl)

/-- **The stack-escape check**: the statements of `escChk` fail exactly when
the returned pointer is not null and its object is one of the function's
own stack objects; otherwise they only write temporaries. -/
theorem escChk_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (j : Nat) (RET : Arg)
    (As : List Arg) (hAs : ∀ a ∈ As, a.below j) (hT : TempsOK P j (escChk j RET As).2) :
    ((As.any (fun a => ptrObj (RET.get σ % 2 ^ 64) == ptrObj (a.get σ % 2 ^ 64)) &&
        ptrObj (RET.get σ % 2 ^ 64) != 0) = true → xStmts P ω σ t (escChk j RET As).1 = .fail) ∧
    ((As.any (fun a => ptrObj (RET.get σ % 2 ^ 64) == ptrObj (a.get σ % 2 ^ 64)) &&
        ptrObj (RET.get σ % 2 ^ 64) != 0) = false →
      ∃ σ', xStmts P ω σ t (escChk j RET As).1 = .ok σ' t ∧ ∀ i, i < j → σ' i = σ i) := by
  cases As with
  | nil => exact ⟨fun h => by simp at h, fun _ => ⟨σ, rfl, fun _ _ => rfl⟩⟩
  | cons a As =>
    let r0 := ptrObj (RET.get σ % 2 ^ 64)
    have hr0 : r0 < 2 ^ 64 := Nat.lt_trans (ptrObj_lt _) (by decide)
    let L := escLoop (.v j 64) (j + 1) none (a :: As)
    obtain ⟨h, hsome⟩ := escLoop_some (.v j 64) (a :: As) (j + 1) none (.inr (by simp))
    let m := j + 1 + L.2.1.length
    have hE : escChk j RET (a :: As) =
        ([.assign j (.bin .lshr) [RET, c64 48]] ++ L.1 ++
          [.assign m (.cmp .ne) [.v j 64, c64 0], .assign (m + 1) (.bin .and) [h, .v m 1],
           .check (.v (m + 1) 1) "stack-escape" "MEM-STACK-ESCAPE"],
         [64] ++ L.2.1 ++ [1, 1]) := by
      simp only [escChk, L, m, hsome, Option.getD_some]
    rw [hE] at hT ⊢
    have w0 : P.wd j = 64 := by simpa using hT.left.left 0 (by decide)
    have hTL : TempsOK P (j + 1) L.2.1 := hT.left.right
    have hTm : TempsOK P m [1, 1] := hT.right.cast (by simp [m]; omega)
    have wm : P.wd m = 1 := by simpa using hTm 0 (by decide)
    have wm1 : P.wd (m + 1) = 1 := by simpa using hTm 1 (by decide)
    let σ0 := σ.set j r0
    obtain ⟨σ', e1, ag, hh, _⟩ := escLoop_run P ω t (.v j 64) r0 hr0 (a :: As) (j + 1) none false σ0
      (by simp [Arg.below]) rfl (by simp [σ0, set_apply])
      (fun x hx => Arg.below_mono (by omega) (hAs x hx)) (fun h e => by cases e) (fun _ => rfl) hTL
    obtain ⟨hhb, hhg⟩ := hh h hsome
    have hag : ∀ x ∈ a :: As, x.get σ0 = x.get σ := fun x hx => get_set3 (hAs x hx) _ (Nat.le_refl j) r0
    have hany : (a :: As).any (fun x => r0 == ptrObj (x.get σ0 % 2 ^ 64)) =
        (a :: As).any (fun x => r0 == ptrObj (x.get σ % 2 ^ 64)) := by
      apply any_congr_mem; intro x hx; rw [hag x hx]
    let hitB := (a :: As).any (fun x => r0 == ptrObj (x.get σ % 2 ^ 64))
    have hσ'j : σ' j = r0 := by rw [ag j (by omega)]; simp [σ0, set_apply]
    have hhit : h.get σ' = hitB.toNat := by rw [hhg, hany]; simp [hitB]
    let nz : Bool := !decide (r0 % 2 ^ 64 = 0 % 2 ^ 64)
    have hnz : nz = (r0 != 0) := by
      simp only [nz]; rw [Nat.mod_eq_of_lt hr0, Nat.zero_mod]; cases r0 <;> rfl
    let σ2 := σ'.set m nz.toNat
    have hh2 : h.get σ2 = hitB.toNat := by rw [get_set3 hhb _ (by exact Nat.le_refl _)]; exact hhit
    have hm2 : (Arg.v m 1).get σ2 = nz.toNat := by simp [σ2, set_apply]
    let σ3 := σ2.set (m + 1) (hitB && nz).toNat
    have hrun : xStmts P ω σ t ([.assign j (.bin .lshr) [RET, c64 48]] ++ L.1 ++
          [.assign m (.cmp .ne) [.v j 64, c64 0], .assign (m + 1) (.bin .and) [h, .v m 1],
           .check (.v (m + 1) 1) "stack-escape" "MEM-STACK-ESCAPE"]) =
        if (hitB && nz) = true then .fail else .ok σ3 t := by
      rw [List.append_assoc, List.singleton_append, xs_lshr48 P ω σ t j RET _ w0]
      rw [xStmts_append, e1]
      simp only [XPRes.then]
      rw [xs_ne64 P ω σ' t m (.v j 64) (c64 0) _ wm rfl, Arg.get_v', hσ'j, c64_get]
      rw [xs_and1 P ω σ2 t (m + 1) h (.v m 1) _ wm1 hh2 hm2, xStmts_check]
      simp [σ3, set_apply]
    rw [hrun, hnz]
    have hb : (hitB && (r0 != 0)) = ((a :: As).any (fun x => ptrObj (RET.get σ % 2 ^ 64) == ptrObj (x.get σ % 2 ^ 64)) &&
        ptrObj (RET.get σ % 2 ^ 64) != 0) := rfl
    rw [hb]
    refine ⟨fun hc => by simp only [hc, ite_true], fun hc => ⟨σ3, by simp only [hc, Bool.false_eq_true, ite_false], fun i hi => ?_⟩⟩
    simp only [σ3, σ2, set_apply, show i ≠ m + 1 by omega, show i ≠ m by omega, ite_false]
    rw [ag i (by omega)]; simp [σ0, set_apply, show i ≠ j by omega]

theorem escArgsX_below {c : Ctx} : ∀ {ns : List String} {As : List Arg}, escArgsX c ns = .ok As →
    ∀ a ∈ As, a.below c.hi
  | [], As, h, a, ha => by simp only [escArgsX, Except.ok.injEq] at h; subst h; simp at ha
  | n :: ns, As, h, a, ha => by
    simp only [escArgsX] at h
    split at h
    · rename_i x _
      obtain ⟨u1, hu1, h⟩ := Except.bind_ok h
      obtain ⟨u2, _, h⟩ := Except.bind_ok h
      obtain ⟨As', hAs', h⟩ := Except.bind_ok h
      simp only [pure, Except.pure, Except.ok.injEq] at h; subst h
      rcases List.mem_cons.mp ha with rfl | ha
      · exact Arg.lt_below (need_ok hu1)
      · exact escArgsX_below hAs' a ha
    · simp at h

/-- The LLVM side: reading the objects' registers either gets stuck or
gives the condition on their variables (they have no shadow, so a related
register holds a number). -/
theorem escHit_ok {c : Ctx} {R : SRegs} {σ : Store} (hR : RelX c R σ) (v : Nat) :
    ∀ (ns : List String) (As : List Arg), escArgsX c ns = .ok As →
      escHit R v ns = .stuck ∨
      escHit R v ns = .ok (As.any (fun a => ptrObj (v % 2 ^ 64) == ptrObj (a.get σ % 2 ^ 64)))
  | [], As, h => by
    simp only [escArgsX, Except.ok.injEq] at h; subst h; exact .inr rfl
  | n :: ns, As, h => by
    simp only [escArgsX] at h
    split at h
    · rename_i x hx
      obtain ⟨u1, _, h⟩ := Except.bind_ok h
      obtain ⟨u2, hu2, h⟩ := Except.bind_ok h
      have hsh := isNone_eq (need_ok hu2)
      obtain ⟨As', hAs', h⟩ := Except.bind_ok h
      simp only [pure, Except.pure, Except.ok.injEq] at h; subst h
      have ih := escHit_ok hR v ns As' hAs'
      simp only [escHit]
      cases hn : R n with
      | none => simp [sOpnd, hn, Res.bind]
      | some xv =>
        obtain ⟨a, hl, hxv⟩ := hR n xv hn
        rw [hx] at hl; cases hl
        have hsh' : c.shOf n = none := by
          unfold Ctx.shOf; split <;> simp_all
        cases xv with
        | ind => obtain ⟨s, hs, _⟩ := hxv; rw [hsh'] at hs; cases hs
        | val pv =>
          obtain ⟨hpv, _⟩ := hxv
          simp only [sOpnd, hn, Res.bind]
          rcases ih with ih | ih
          · rw [ih]; exact .inl rfl
          · rw [ih]; right; simp only [List.any_cons, hpv]
    · simp at h

end PrismRefine
