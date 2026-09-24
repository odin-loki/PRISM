/-
PRISM refinement, extended fragment — the memory statements the translator
emits compute what the LLVM semantics computes (`XLlvm.lean`): the access
checks fail exactly on `accessBad`, the pointer arithmetic of
`getelementptr` fails exactly where `gepVal` is undefined.
-/
import PrismRefine.Refine
import PrismRefine.XPir
import PrismRefine.XTranslate

namespace PrismRefine

open PrismSem

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
  | σ, t, .alloc .. :: r, ys => xStmts_append P ω _ _ r ys
  | σ, t, .load .. :: r, ys => xStmts_append P ω _ _ r ys
  | σ, t, .store .. :: r, ys => xStmts_append P ω _ _ r ys
  | σ, t, .free .. :: r, ys => xStmts_append P ω _ _ r ys
  | σ, t, .memcpy .. :: r, ys => xStmts_append P ω _ _ r ys
  | σ, t, .memset .. :: r, ys => xStmts_append P ω _ _ r ys

theorem bv_mod (w x : Nat) : bv w (x % 2 ^ w) = bv w x := by
  apply BitVec.eq_of_toNat_eq; simp

@[simp] theorem xStmts_nil (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) :
    xStmts P ω σ t [] = .ok σ t := rfl

theorem xStmts_assign (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (d : Nat) (op : POp)
    (args : List Arg) (r : List PStmt) :
    xStmts P ω σ t (.assign d op args :: r) =
      xStmts P ω (σ.set d (evalOpM t.mem σ op (P.wd d) args % 2 ^ P.wd d)) t r := rfl

theorem xStmts_check (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (a : Arg) (p c : String)
    (r : List PStmt) :
    xStmts P ω σ t (.check a p c :: r) = if truthN (a.get σ) then .fail else xStmts P ω σ t r := rfl

@[simp] theorem Arg.get_v' (σ : Store) (i w : Nat) : (Arg.v i w).get σ = σ i := rfl
@[simp] theorem Arg.get_c' (σ : Store) (w b : Nat) : (Arg.c w b).get σ = b := rfl
@[simp] theorem Arg.width_v' (i w : Nat) : (Arg.v i w).width = w := rfl
@[simp] theorem Arg.width_c' (w b : Nat) : (Arg.c w b).width = w := rfl
@[simp] theorem c64_get (σ : Store) (v : Nat) : (c64 v).get σ = v % 2 ^ 64 := rfl
@[simp] theorem c64_width (v : Nat) : (c64 v).width = 64 := rfl

theorem set_apply (σ : Store) (i v j : Nat) : σ.set i v j = if j = i then v else σ j := rfl

theorem Arg.get_set_ge {k : Nat} {A : Arg} (hA : A.below k) (τ : Store) {j : Nat} (v : Nat) (hj : k ≤ j) :
    A.get (τ.set j v) = A.get τ := by
  cases A with
  | c _ _ => rfl
  | v i _ => simp only [Arg.below] at hA; simp [set_apply, show i ≠ j by omega]

/-! ### Normal forms of the values the check sequences compute -/

theorem icmpVal_eq' (p : Pred) (w x y : Nat) : icmpVal p w x y = (evalPred p (bv w x) (bv w y)).toNat := by
  simp [icmpVal]

@[simp] theorem boolToNat_mod1 (b : Bool) : b.toNat % 2 ^ 1 = b.toNat := by cases b <;> rfl
@[simp] theorem ite_toNat (b : Bool) : (if b = true then 1 else 0) = b.toNat := by cases b <;> rfl

theorem pred_eq (w x y : Nat) : evalPred .eq (bv w x) (bv w y) = decide (x % 2 ^ w = y % 2 ^ w) := by
  unfold evalPred; apply Bool.eq_iff_iff.mpr; simp [BitVec.toNat_eq]
theorem pred_ne (w x y : Nat) : evalPred .ne (bv w x) (bv w y) = !decide (x % 2 ^ w = y % 2 ^ w) := by
  unfold evalPred; apply Bool.eq_iff_iff.mpr; simp [BitVec.toNat_eq]

@[simp] theorem toNat_eq0 (b : Bool) : decide (b.toNat = 0) = !b := by cases b <;> rfl
theorem pred_ugt (w x y : Nat) : evalPred .ugt (bv w x) (bv w y) = decide (y % 2 ^ w < x % 2 ^ w) := by
  simp [evalPred, BitVec.ult]
theorem pred_uge (w x y : Nat) : evalPred .uge (bv w x) (bv w y) = decide (y % 2 ^ w ≤ x % 2 ^ w) := by
  simp [evalPred, BitVec.ule]
theorem pred_ult (w x y : Nat) : evalPred .ult (bv w x) (bv w y) = decide (x % 2 ^ w < y % 2 ^ w) := by
  simp [evalPred, BitVec.ult]

theorem and1 (a b : Bool) : binVal .and 1 a.toNat b.toNat = (a && b).toNat := by
  cases a <;> cases b <;> rfl
theorem or1 (a b : Bool) : binVal .or 1 a.toNat b.toNat = (a || b).toNat := by
  cases a <;> cases b <;> rfl

theorem add64 (x y : Nat) : binVal .add 64 x y = (x + y) % 2 ^ 64 := by
  simp [binVal, evalBin, BitVec.toNat_add]

theorem lshr48 (x : Nat) : binVal .lshr 64 x (48 % 2 ^ 64) = ptrObj (x % 2 ^ 64) := by
  simp [binVal, evalBin, ptrObj, Nat.shiftRight_eq_div_pow]

theorem mask48 (x : Nat) : binVal .and 64 x ((2 ^ 48 - 1) % 2 ^ 64) = ptrOff (x % 2 ^ 64) := by
  have : (2 : Nat) ^ 48 - 1 < 2 ^ 64 := by decide
  simp only [binVal, evalBin, BitVec.toNat_and, BitVec.toNat_ofNat, Nat.mod_mod, ptrOff]
  rw [Nat.mod_eq_of_lt this, show (2 : Nat) ^ 48 - 1 = 2 ^ 48 - 1 from rfl, Nat.and_two_pow_sub_one_eq_mod]

theorem ptrObj_lt (x : Nat) : ptrObj (x % 2 ^ 64) < 2 ^ 16 := by
  unfold ptrObj
  have := Nat.mod_lt x (Nat.two_pow_pos 64)
  omega
theorem ptrOff_lt (x : Nat) : ptrOff x < 2 ^ 48 := Nat.mod_lt _ (Nat.two_pow_pos 48)

@[simp] theorem ptrObj_mod (x : Nat) : ptrObj (x % 2 ^ 64) % 2 ^ 64 = ptrObj (x % 2 ^ 64) :=
  Nat.mod_eq_of_lt (by have := ptrObj_lt x; omega)
@[simp] theorem ptrOff_mod (x : Nat) : ptrOff x % 2 ^ 64 = ptrOff x :=
  Nat.mod_eq_of_lt (by have := ptrOff_lt x; omega)
@[simp] theorem kind_mod (m : Mem) (p : Nat) : m.kind p % 2 ^ 8 = m.kind p := by
  simp [Mem.kind]
@[simp] theorem size_mod (m : Mem) (p : Nat) : m.size p % 2 ^ 64 = m.size p := by
  simp [Mem.size]
@[simp] theorem align_mod (m : Mem) (p : Nat) : m.align p % 2 ^ 64 = m.align p := by
  simp [Mem.align]

theorem uadd_nov (f n : Nat) (hf : f < 2 ^ 48) (hn : n ≤ 8) :
    testVal (.ovf .uadd) 64 f (n % 2 ^ 64) = false := by
  simp only [testVal, ovfTest, BitVec.toNat_add, BitVec.toNat_ofNat, decide_eq_false_iff_not, Nat.not_lt]
  rw [Nat.mod_eq_of_lt (a := n) (by omega), Nat.mod_eq_of_lt (a := f) (by omega),
    Nat.mod_eq_of_lt (a := n) (by omega), Nat.mod_eq_of_lt (by omega)]
  omega

/-- The first part of `accessBad` (`accChk0`). -/
def bad0 (m : Mem) (q n : Nat) : Bool :=
  ptrObj q == 0 || (ptrObj q != 0 && m.kind q == 0) || (m.kind q != 0 && !m.live q) ||
  (m.live q && decide (m.size q < ptrOff q + n))

theorem wd_of {P : PFunc} {k : Nat} {ts : List Nat} (hT : TempsOK P k ts) (j : Nat) (h : j < ts.length) :
    P.wd (k + j) = ts[j] := hT j h

theorem accChk0_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k : Nat) (A : Arg)
    (hA : A.below k) (n : Nat) (hn : n ≤ 8) (write : Bool) (hT : TempsOK P k accT0) :
    (bad0 t.mem (A.get σ % 2 ^ 64) n = true → xStmts P ω σ t (accChk0 k A n write) = .fail) ∧
    (bad0 t.mem (A.get σ % 2 ^ 64) n = false → ∃ σ', xStmts P ω σ t (accChk0 k A n write) = .ok σ' t ∧
      (∀ i, i < k → σ' i = σ i) ∧ σ' (k + 1) = ptrOff (A.get σ % 2 ^ 64) ∧
      σ' (k + 2) = t.mem.kind (A.get σ % 2 ^ 64) ∧ σ' (k + 3) = (t.mem.live (A.get σ % 2 ^ 64)).toNat) := by
  have w0 : P.wd k = 64 := by simpa [accT0] using wd_of hT 0 (by decide)
  have hkk : ∀ a b : Nat, (k + a = k + b) = (a = b) := fun a b => by simp
  have hk0 : ∀ a : Nat, (k + a = k) = (a = 0) := fun a => by simp
  have hk1 : ∀ a : Nat, (k = k + a) = (a = 0) := fun a => by simp [eq_comm]
  have hAs := fun τ j v (hj : k ≤ j) => Arg.get_set_ge hA τ (j := j) v hj
  generalize hq : A.get σ % 2 ^ 64 = q
  have hoff := ptrOff_lt q
  have hn64 : n % 2 ^ 64 = n := Nat.mod_eq_of_lt (by omega)
  have hsum : (ptrOff q + n) % 2 ^ 64 = ptrOff q + n := Nat.mod_eq_of_lt (by omega)
  have hobj : ptrObj q % 2 ^ 64 = ptrObj q := by rw [← hq]; exact ptrObj_mod _
  have hov : testVal (.ovf .uadd) 64 (ptrOff q) n = false := by
    have := uadd_nov _ n hoff hn; rwa [hn64] at this
  simp (config := { decide := true }) only [accChk0, xStmts_assign, xStmts_check, w0,
    wd_of hT 1 (by decide), wd_of hT 2 (by decide),
    wd_of hT 3 (by decide), wd_of hT 4 (by decide), wd_of hT 5 (by decide), wd_of hT 6 (by decide),
    wd_of hT 7 (by decide), wd_of hT 8 (by decide),
    wd_of hT 9 (by decide), wd_of hT 10 (by decide), wd_of hT 11 (by decide), wd_of hT 12 (by decide),
    wd_of hT 13 (by decide), wd_of hT 14 (by decide), wd_of hT 15 (by decide), wd_of hT 16 (by decide),
    accT0, List.getElem_cons_succ,
    List.getElem_cons_zero, evalOpM, evalOp, Arg.get_v', Arg.get_c', c64_get, set_apply, hkk, hk0, hk1,
    Arg.width_v', Arg.width_c', c64_width, hAs, Nat.le_add_right, Nat.le_refl, ite_true, ite_false,
    lshr48, mask48, hq, ptrObj_mod, ptrOff_mod, kind_mod, size_mod, icmpVal_eq', pred_eq, pred_ne, pred_ugt,
    boolToNat_mod1, ite_toNat, and1, or1, add64, hn64, uadd_nov _ n hoff hn, BitVec.toNat_ofBool,
    truthN_boolToNat, Nat.zero_mod, Nat.mod_mod, hsum, toNat_eq0, Bool.or_false, xStmts_nil, hobj, hov]
  constructor
  · intro hb
    simp only [bad0] at hb
    by_cases h1 : ptrObj q = 0
    · simp [h1]
    by_cases h2 : t.mem.kind q = 0
    · simp [h1, h2]
    cases h3 : t.mem.live q
    · simp [h1, h2, h3]
    · simp [h1, h2, h3] at hb
      simp [h1, h2, h3, hb]
  · intro hb
    simp only [bad0, Bool.or_eq_false_iff, Bool.and_eq_false_iff] at hb
    have h1 : ptrObj q ≠ 0 := by simpa using hb.1.1.1
    have h2 : t.mem.kind q ≠ 0 := by have := hb.1.1.2; simp_all
    have h3 : t.mem.live q = true := by have := hb.1.2; simp_all
    have h4 : ¬ t.mem.size q < ptrOff q + n := by have := hb.2; simp_all
    simp only [h1, h2, h3, h4, decide_false, decide_true, Bool.not_false, Bool.not_true, Bool.false_and,
      Bool.and_false, Bool.true_and, Bool.false_eq_true, ite_false]
    refine ⟨_, rfl, ?_, ?_, ?_, ?_⟩
    · intro i hi
      have hne : ∀ c, i ≠ k + c := fun c => by omega
      simp [set_apply, hne, show i ≠ k by omega]
    · simp [set_apply]
    · simp [set_apply]
    · simp [set_apply]

theorem maskPow (x e : Nat) (he : e ≤ 64) : binVal .and 64 x ((2 ^ e - 1) % 2 ^ 64) = x % 2 ^ 64 % 2 ^ e := by
  have : (2 : Nat) ^ e - 1 < 2 ^ 64 := by
    have := Nat.pow_le_pow_right (by decide : 0 < 2) he
    have := Nat.two_pow_pos e
    omega
  simp only [binVal, evalBin, BitVec.toNat_and, BitVec.toNat_ofNat, Nat.mod_mod]
  rw [Nat.mod_eq_of_lt this, Nat.and_two_pow_sub_one_eq_mod]

theorem accAlign_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k a : Nat) (A : Arg)
    (hA : A.below k) (hka : k + 4 < a) (hT : TempsOK P a [64, 64, 1, 1, 1, 1]) {q : Nat}
    (hq : A.get σ % 2 ^ 64 = q) (h1 : σ (k + 1) = ptrOff q) (h3 : σ (k + 3) = (t.mem.live q).toNat)
    (e : Nat) (he : e < 32) :
    ((t.mem.live q && (ptrOff q % 2 ^ e != 0 || decide (t.mem.align q < 2 ^ e))) = true →
      xStmts P ω σ t (accAlign k a A (2 ^ e)) = .fail) ∧
    ((t.mem.live q && (ptrOff q % 2 ^ e != 0 || decide (t.mem.align q < 2 ^ e))) = false →
      ∃ σ', xStmts P ω σ t (accAlign k a A (2 ^ e)) = .ok σ' t ∧ ∀ i, i < a → σ' i = σ i) := by
  have w0 : P.wd a = 64 := by simpa using wd_of hT 0 (by decide)
  have hkk : ∀ x y : Nat, (a + x = a + y) = (x = y) := fun x y => by simp
  have hk0 : ∀ x : Nat, (a + x = a) = (x = 0) := fun x => by simp
  have hk1 : ∀ x : Nat, (a = a + x) = (x = 0) := fun x => by simp [eq_comm]
  have hk3 : ∀ x : Nat, (k + 3 = a + x) = False := fun x => by simp; omega
  have hk3' : (k + 3 = a) = False := by simp; omega
  have hk1a : ∀ x : Nat, (k + 1 = a + x) = False := fun x => by simp; omega
  have hk1a' : (k + 1 = a) = False := by simp; omega
  have hAs := fun τ j v (hj : a ≤ j) => Arg.get_set_ge (Arg.below_mono (by omega) hA) τ (j := j) v hj
  have hoff := ptrOff_lt q
  have hpe : (2 : Nat) ^ e < 2 ^ 32 := Nat.pow_lt_pow_right (by decide) he
  have hpe64 : (2 : Nat) ^ e % 2 ^ 64 = 2 ^ e := Nat.mod_eq_of_lt (by omega)
  have hm : binVal .and 64 (ptrOff q) ((2 ^ e - 1) % 2 ^ 64) % 2 ^ 64 = ptrOff q % 2 ^ e := by
    rw [maskPow _ _ (by omega), Nat.mod_eq_of_lt (a := ptrOff q) (by omega)]
    exact Nat.mod_eq_of_lt (by have := Nat.mod_lt (ptrOff q) (Nat.two_pow_pos e); omega)
  have hm2 : ptrOff q % 2 ^ e % 2 ^ 64 = ptrOff q % 2 ^ e :=
    Nat.mod_eq_of_lt (by have := Nat.mod_lt (ptrOff q) (Nat.two_pow_pos e); omega)
  simp (config := { decide := true }) only [accAlign, xStmts_assign, xStmts_check, w0,
    wd_of hT 1 (by decide), wd_of hT 2 (by decide), wd_of hT 3 (by decide), wd_of hT 4 (by decide),
    wd_of hT 5 (by decide), List.getElem_cons_succ, List.getElem_cons_zero, evalOpM, evalOp, Arg.get_v',
    Arg.get_c', c64_get, set_apply, hkk, hk0, hk1, hk3, hk3', hk1a, hk1a', h1, h3, hm, hm2,
    Arg.width_v', Arg.width_c', c64_width, hAs, Nat.le_add_right, Nat.le_refl, ite_true, ite_false,
    hq, align_mod, icmpVal_eq', pred_ne, pred_ult, boolToNat_mod1, and1, or1, BitVec.toNat_ofBool,
    truthN_boolToNat, Nat.zero_mod, hpe64, xStmts_nil, show (2 : Nat) ^ e - 1 = 2 ^ e - 1 from rfl]
  rw [show (ptrOff q % 2 ^ e != 0) = !decide (ptrOff q % 2 ^ e = 0) from Bool.eq_iff_iff.mpr (by simp)]
  constructor
  · intro hb; simp only [hb, ite_true]
  · intro hb; simp only [hb, Bool.false_eq_true, ite_false]
    refine ⟨_, rfl, fun i hi => ?_⟩
    have hne : ∀ c, i ≠ a + c := fun c => by omega
    simp [set_apply, hne, show i ≠ a by omega]

theorem accConst_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k j : Nat)
    (hkj : k + 3 < j) (hT : TempsOK P j [1, 1]) {q : Nat}
    (h2 : σ (k + 2) = t.mem.kind q) (h3 : σ (k + 3) = (t.mem.live q).toNat) :
    ((t.mem.live q && t.mem.kind q == 4) = true → xStmts P ω σ t (accConst k j) = .fail) ∧
    ((t.mem.live q && t.mem.kind q == 4) = false →
      ∃ σ', xStmts P ω σ t (accConst k j) = .ok σ' t ∧ ∀ i, i < j → σ' i = σ i) := by
  have w0 : P.wd j = 1 := by simpa using wd_of hT 0 (by decide)
  have w1 : P.wd (j + 1) = 1 := by simpa using wd_of hT 1 (by decide)
  have e1 : (k + 3 = j + 1) = False := by simp; omega
  have e2 : (k + 3 = j) = False := by simp; omega
  have e3 : (k + 2 = j) = False := by simp; omega
  have e4 : (j = j + 1) = False := by simp
  simp (config := { decide := true }) only [accConst, xStmts_assign, xStmts_check, w0, w1, evalOpM, evalOp,
    Arg.get_v', Arg.get_c', set_apply, e1, e2, e3, e4, ite_true, ite_false, h2, h3, kind_mod,
    Arg.width_v', Arg.width_c', icmpVal_eq', pred_eq, boolToNat_mod1, and1, BitVec.toNat_ofBool,
    truthN_boolToNat, xStmts_nil]
  have hb4 : (decide (t.mem.kind q = 4 % 2 ^ 8)) = (t.mem.kind q == 4) := Bool.eq_iff_iff.mpr (by simp)
  rw [hb4]
  constructor
  · intro hb; simp only [hb, ite_true]
  · intro hb; simp only [hb, Bool.false_eq_true, ite_false]
    refine ⟨_, rfl, fun i hi => ?_⟩
    simp [set_apply, show i ≠ j by omega, show i ≠ j + 1 by omega]

theorem accessBad_eq (m : Mem) (q n : Nat) (write : Bool) (al : Nat) :
    accessBad m q n write al = (bad0 m q n ||
      (decide (1 < al) && (m.live q && (ptrOff q % al != 0 || decide (m.align q < al)))) ||
      (write && (m.live q && m.kind q == 4))) := by
  simp only [accessBad, bad0, Bool.or_assoc, Bool.and_assoc]

theorem alignOK_pow {al : Nat} (h : alignOK al = true) : al = 0 ∨ ∃ e, e < 32 ∧ al = 2 ^ e := by
  simp only [alignOK, Bool.or_eq_true, beq_iff_eq, List.any_eq_true, List.mem_range] at h
  rcases h with h | ⟨e, he, h⟩
  · exact .inl h
  · exact .inr ⟨e, he, h⟩

/-- `MemTr::access_checks` fails exactly on `accessBad`. -/
theorem accessChecks_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (k : Nat) (A : Arg)
    (hA : A.below k) (n : Nat) (hn : n ≤ 8) (write : Bool) (al : Nat) (hal : alignOK al = true)
    (hT : TempsOK P k (accessChecks k A n write al).2) :
    (accessBad t.mem (A.get σ % 2 ^ 64) n write al = true →
      xStmts P ω σ t (accessChecks k A n write al).1 = .fail) ∧
    (accessBad t.mem (A.get σ % 2 ^ 64) n write al = false →
      ∃ σ', xStmts P ω σ t (accessChecks k A n write al).1 = .ok σ' t ∧ ∀ i, i < k → σ' i = σ i) := by
  simp only [accessChecks] at hT ⊢
  rw [accessBad_eq, xStmts_append, xStmts_append]
  generalize hq : A.get σ % 2 ^ 64 = q
  have h0 := accChk0_run P ω σ t k A hA n hn write hT.left.left
  rw [hq] at h0
  cases hb0 : bad0 t.mem q n
  · obtain ⟨σ1, e1, ag1, h1, h2, h3⟩ := h0.2 hb0
    rw [e1]
    simp only [XPRes.then, Bool.false_or]
    have hT1 : TempsOK P (k + 17) (accT1 al) := hT.left.right
    have hq1 : A.get σ1 % 2 ^ 64 = q := by rw [Arg.get_agree ag1 hA, hq]
    -- the alignment part
    have hal' : ∃ σ2, (((decide (1 < al) && (t.mem.live q && (ptrOff q % al != 0 ||
        decide (t.mem.align q < al)))) = true → xStmts P ω σ1 t (if 1 < al then accAlign k (k + 17) A al else [])
          = .fail) ∧
        ((decide (1 < al) && (t.mem.live q && (ptrOff q % al != 0 || decide (t.mem.align q < al)))) = false →
          xStmts P ω σ1 t (if 1 < al then accAlign k (k + 17) A al else []) = .ok σ2 t ∧
          ∀ i, i < k + 17 → σ2 i = σ1 i)) := by
      by_cases h1al : 1 < al
      · rcases alignOK_pow hal with h | ⟨e, he, rfl⟩
        · omega
        have hTa : TempsOK P (k + 17) [64, 64, 1, 1, 1, 1] := by
          simpa [accT1, h1al] using hT1
        have ha := accAlign_run P ω σ1 t k (k + 17) A hA (by omega) hTa hq1 h1 h3 e he
        cases hba : (t.mem.live q && (ptrOff q % 2 ^ e != 0 || decide (t.mem.align q < 2 ^ e)))
        · obtain ⟨σ2, e2, ag2⟩ := ha.2 hba
          exact ⟨σ2, fun h => by simp [h1al, hba] at h, fun _ => ⟨by simp only [h1al, ite_true]; exact e2, ag2⟩⟩
        · exact ⟨σ1, fun _ => by simp only [h1al, ite_true]; exact ha.1 hba,
            fun h => by simp [h1al, hba] at h⟩
      · exact ⟨σ1, fun h => by simp [h1al] at h, fun _ => ⟨by simp [h1al], fun _ _ => rfl⟩⟩
    obtain ⟨σ2, ha1, ha2⟩ := hal'
    cases hba : (decide (1 < al) && (t.mem.live q && (ptrOff q % al != 0 || decide (t.mem.align q < al))))
    · rw [(ha2 hba).1]
      obtain ag2 := (ha2 hba).2
      simp only [XPRes.then, Bool.false_or]
      have hj : TempsOK P (k + 17 + (accT1 al).length) (accT2 write) := by
        have := hT.right
        have e : k + (accT0 ++ accT1 al).length = k + 17 + (accT1 al).length := by
          simp only [List.length_append, accT0, List.length_cons, List.length_nil]; omega
        rwa [e] at this
      cases write
      · refine ⟨fun h => by simp at h, fun _ => ⟨σ2, by simp, fun i hi => by rw [ag2 i (by omega), ag1 i hi]⟩⟩
      · have hTc : TempsOK P (k + 17 + (accT1 al).length) [1, 1] := by simpa [accT2] using hj
        have hc := accConst_run P ω σ2 t k (k + 17 + (accT1 al).length) (by omega) hTc
          (q := q) (by rw [ag2 _ (by omega), h2]) (by rw [ag2 _ (by omega), h3])
        simp only [ite_true, Bool.true_and]
        refine ⟨hc.1, fun h => ?_⟩
        obtain ⟨σ3, e3, ag3⟩ := hc.2 h
        exact ⟨σ3, e3, fun i hi => by rw [ag3 i (by omega), ag2 i (by omega), ag1 i hi]⟩
    · rw [ha1 hba]
      simp only [XPRes.then, Bool.true_or, forall_const, Bool.true_eq_false, false_implies, and_true]
  · simp only [XPRes.then, h0.1 hb0, Bool.true_or, forall_const, Bool.true_eq_false, false_implies, and_true]

/-! ### `getelementptr`: `MemTr::gep`'s statements compute `gepVal` -/

/-- The variable part of the offset on both sides. -/
def VarRel (σ : Store) (m av : Nat) : Option Arg → Option Nat → Prop
  | none, none => True
  | some A, some y => A.get σ = y ∧ A.width = 64 ∧ A.below m ∧ A.var? ≠ some av
  | _, _ => False

/-- The translator's running state `g` (its statements run from `σ0` to
`σ`, temporaries from `k`) and the semantics' accumulator `acc` agree. -/
structure GRel (P : PFunc) (ω : Nat → Nat) (t : World) (k av : Nat) (σ0 : Store) (g : GSt) (acc : GAcc)
    (σ : Store) : Prop where
  run : xStmts P ω σ0 t g.s = .ok σ t
  agree : ∀ j, j < k → σ j = σ0 j
  cst : g.cst = acc.cst
  var : VarRel σ (k + g.ts.length) av g.var acc.var
  avk : av < k

def GSim (P : PFunc) (ω : Nat → Nat) (t : World) (k av : Nat) (σ0 : Store) : Res GAcc → GSt → Prop
  | .ok acc, g => ∃ σ, GRel P ω t k av σ0 g acc σ
  | .ub, g => xStmts P ω σ0 t g.s = .fail
  | .stuck, _ => True

theorem VarRel.mono {σ : Store} {m m' av : Nat} (h : m ≤ m') : ∀ {a : Option Arg} {y : Option Nat},
    VarRel σ m av a y → VarRel σ m' av a y
  | none, none, _ => trivial
  | some _, some _, ⟨h1, h2, h3, h4⟩ => ⟨h1, h2, Arg.below_mono h h3, h4⟩
  | none, some _, h => h.elim
  | some _, none, h => h.elim

theorem VarRel.agree {σ σ' : Store} {m av : Nat} (ha : ∀ j, j < m → σ' j = σ j) :
    ∀ {a : Option Arg} {y : Option Nat}, VarRel σ m av a y → VarRel σ' m av a y
  | none, none, _ => trivial
  | some _, some _, ⟨h1, h2, h3, h4⟩ => ⟨by rw [Arg.get_agree ha h3, h1], h2, h3, h4⟩
  | none, some _, h => h.elim
  | some _, none, h => h.elim

theorem xStmts_run_append {P : PFunc} {ω : Nat → Nat} {t : World} {σ0 σ : Store} {s : List PStmt}
    (h : xStmts P ω σ0 t s = .ok σ t) (Δ : List PStmt) :
    xStmts P ω σ0 t (s ++ Δ) = xStmts P ω σ t Δ := by
  rw [xStmts_append, h]; rfl

theorem ovf1 (o : OvfOp) (w x y : Nat) :
    (BitVec.ofBool (ovfTest o w x y)).toNat % 2 ^ 1 = (ovfTest o w x y).toNat := by
  cases ovfTest o w x y <;> rfl

theorem gAddVar_sim {P : PFunc} {ω : Nat → Nat} {t : World} {k av : Nat} {σ0 σ : Store} {g : GSt} {acc : GAcc}
    (hG : GRel P ω t k av σ0 g acc σ) {x : Arg} {xv : Nat} (hx : x.get σ = xv) (hxw : x.width = 64)
    (hxb : x.below (k + g.ts.length)) (hxi : x.var? ≠ some av) (hT : TempsOK P k (gAddVarT k g x).ts) :
    GSim P ω t k av σ0 (gAddVar acc xv) (gAddVarT k g x) := by
  have hak := hG.avk
  have hv := hG.var
  cases hgv : g.var with
  | none =>
    rw [hgv] at hv
    cases hav : acc.var with
    | none =>
      simp only [gAddVar, hav, gAddVarT, hgv, GSim]
      exact ⟨σ, hG.run, hG.agree, hG.cst, ⟨hx, hxw, hxb, hxi⟩, hak⟩
    | some _ => rw [hav] at hv; exact hv.elim
  | some y =>
    rw [hgv] at hv
    cases hav : acc.var with
    | none => rw [hav] at hv; exact hv.elim
    | some yv =>
      rw [hav] at hv
      obtain ⟨hy, hyw, hyb, _⟩ := hv
      simp only [gAddVarT, hgv] at hT ⊢
      have w0 : P.wd (k + g.ts.length) = 1 := by
        have := hT (g.ts.length) (by simp); simpa using this
      have w1 : P.wd (k + g.ts.length + 1) = 64 := by
        have := hT (g.ts.length + 1) (by simp); simpa [Nat.add_assoc] using this
      have hyj : ∀ τ v c, y.get (τ.set (k + g.ts.length + c) v) = y.get τ := fun τ v c =>
        Arg.get_agree (fun i hi => Store.set_other _ _ (by omega)) hyb
      have hxj : ∀ τ v c, x.get (τ.set (k + g.ts.length + c) v) = x.get τ := fun τ v c =>
        Arg.get_agree (fun i hi => Store.set_other _ _ (by omega)) hxb
      have hyj0 : ∀ τ v, y.get (τ.set (k + g.ts.length) v) = y.get τ := fun τ v => hyj τ v 0
      have hxj0 : ∀ τ v, x.get (τ.set (k + g.ts.length) v) = x.get τ := fun τ v => hxj τ v 0
      simp only [gAddVar, hav]
      have hrun := xStmts_run_append (Δ := [.assign (k + g.ts.length) (.ovf .sadd) [y, x],
        .check (.v (k + g.ts.length) 1) "ptr-arith" "MEM-PTR-ARITH",
        .assign (k + g.ts.length + 1) (.bin .add) [y, x]]) hG.run
      cases hov : ovfTest .sadd 64 yv xv
      · simp only [Bool.false_eq_true, ite_false, GSim]
        have e : xStmts P ω σ t [.assign (k + g.ts.length) (.ovf .sadd) [y, x],
            .check (.v (k + g.ts.length) 1) "ptr-arith" "MEM-PTR-ARITH",
            .assign (k + g.ts.length + 1) (.bin .add) [y, x]] =
            .ok ((σ.set (k + g.ts.length) 0).set (k + g.ts.length + 1) ((yv + xv) % 2 ^ 64)) t := by
          simp only [xStmts_assign, xStmts_check, xStmts_nil, evalOpM, evalOp, testVal, w0, w1, hyw,
            Arg.get_v', Store.set_same, hyj0, hxj0, hy, hx, ovf1, hov, truthN_boolToNat, Bool.false_eq_true,
            ite_false, add64, Nat.mod_mod]
          rfl
        refine ⟨_, by rw [hrun, e], ?_, hG.cst, ?_, hak⟩
        · intro i hi
          rw [Store.set_other _ _ (by omega), Store.set_other _ _ (by omega)]
          exact hG.agree i hi
        · simp only [List.length_append, List.length_cons, List.length_nil, VarRel]
          refine ⟨by simp, rfl, by simp only [Arg.below]; omega, by simp [Arg.var?]; omega⟩
      · simp only [ite_true, GSim]
        rw [hrun]
        simp only [xStmts_assign, xStmts_check, evalOpM, evalOp, testVal, w0, hyw,
          Arg.get_v', Store.set_same, hy, hx, ovf1, hov, truthN_boolToNat, ite_true]
        rfl

theorem xStmts_fail_append {P : PFunc} {ω : Nat → Nat} {t : World} {σ0 : Store} {s : List PStmt}
    (h : xStmts P ω σ0 t s = .fail) (Δ : List PStmt) : xStmts P ω σ0 t (s ++ Δ) = .fail := by
  rw [xStmts_append, h]; rfl

theorem gAddVarT_ext (k : Nat) (g : GSt) (x : Arg) : ∃ Δ, (gAddVarT k g x).s = g.s ++ Δ := by
  unfold gAddVarT; split
  · exact ⟨[], by simp⟩
  · exact ⟨_, rfl⟩

theorem sext64_lt (w v : Nat) : sext64 w v < 2 ^ 64 := BitVec.isLt _

theorem castSext (w v : Nat) : castVal .sext w 64 v = sext64 w v := rfl

/-- An index operand: its value `v` in the store `σ0` the loop starts from. -/
def IArg (σ0 : Store) (k av : Nat) (o : Opnd) (w : Nat) (A : Arg) (v : Nat) : Prop :=
  A.get σ0 = v ∧ A.width = w ∧ w ≤ 64 ∧ A.below k ∧
  (match o with
   | .const _ => ∃ b, A = .c w b
   | _ => ∃ j, A = .v j w) ∧ A.var? ≠ some av

theorem GRel.get {P : PFunc} {ω : Nat → Nat} {t : World} {k av : Nat} {σ0 σ : Store} {g : GSt} {acc : GAcc}
    (hG : GRel P ω t k av σ0 g acc σ) {A : Arg} (hA : A.below k) : A.get σ = A.get σ0 :=
  Arg.get_agree hG.agree hA

/-- One statement assigning temporary `k + |ts|` (width `wd`) appended to `g`. -/
theorem GRel.push {P : PFunc} {ω : Nat → Nat} {t : World} {k av : Nat} {σ0 σ : Store} {g : GSt} {acc : GAcc}
    (hG : GRel P ω t k av σ0 g acc σ) (op : POp) (args : List Arg) (wd : Nat)
    (hT : P.wd (k + g.ts.length) = wd) :
    GRel P ω t k av σ0 { g with s := g.s ++ [.assign (k + g.ts.length) op args], ts := g.ts ++ [wd] } acc
      (σ.set (k + g.ts.length) (evalOpM t.mem σ op wd args % 2 ^ wd)) := by
  refine ⟨?_, fun j hj => ?_, hG.cst, ?_, hG.avk⟩
  · rw [xStmts_run_append hG.run]; simp [xStmts_assign, hT]
  · rw [Store.set_other _ _ (by omega)]; exact hG.agree j hj
  · simp only [List.length_append, List.length_cons, List.length_nil]
    exact VarRel.mono (by omega) (VarRel.agree (fun j hj => Store.set_other _ _ (by omega)) hG.var)

theorem gSext_sim {P : PFunc} {ω : Nat → Nat} {t : World} {k av : Nat} {σ0 σ : Store} {g : GSt} {acc : GAcc}
    (hG : GRel P ω t k av σ0 g acc σ) {j w : Nat} (hw : w ≤ 64) (hAb : (Arg.v j w).below k)
    (hAi : (Arg.v j w).var? ≠ some av) (hT : TempsOK P k (gSext k g (.v j w)).1.ts) :
    ∃ σ', GRel P ω t k av σ0 (gSext k g (.v j w)).1 acc σ' ∧
      (gSext k g (.v j w)).2.get σ' = (if w < 64 then sext64 w (σ0 j) else σ0 j) ∧
      (gSext k g (.v j w)).2.width = 64 ∧
      (gSext k g (.v j w)).2.below (k + (gSext k g (.v j w)).1.ts.length) ∧
      (gSext k g (.v j w)).2.var? ≠ some av := by
  have hj : σ j = σ0 j := hG.agree j hAb
  have hak := hG.avk
  by_cases h64 : w < 64
  · simp only [gSext, h64, ite_true] at hT ⊢
    have w0 : P.wd (k + g.ts.length) = 64 := by have := hT g.ts.length (by simp); simpa using this
    refine ⟨_, hG.push (.cast .sext) [.v j w] 64 w0, ?_, rfl, by simp [Arg.below], by simp [Arg.var?]; omega⟩
    simp only [Arg.get_v', Store.set_same, evalOpM, evalOp, Arg.width_v', castSext, hj]
    exact Nat.mod_eq_of_lt (sext64_lt _ _)
  · simp only [gSext, h64, ite_false]
    have : w = 64 := by omega
    subst this
    exact ⟨σ, hG, hj, rfl, Arg.below_mono (by omega) hAb, hAi⟩

theorem bv64_mod (x : Nat) : bv 64 (x % 2 ^ 64) = bv 64 x := bv_mod 64 x

theorem gAddVarT_ts (k : Nat) (g : GSt) (x : Arg) : ∃ l, (gAddVarT k g x).ts = g.ts ++ l := by
  unfold gAddVarT; split
  · exact ⟨[], by simp⟩
  · exact ⟨_, rfl⟩

theorem TempsOK.pre {P : PFunc} {k : Nat} {a b : List Nat} (h : TempsOK P k b) (hp : ∃ l, b = a ++ l) :
    TempsOK P k a := by
  obtain ⟨l, rfl⟩ := hp; exact h.left

theorem ovf_mod64 (o : OvfOp) (x y : Nat) : ovfTest o 64 x (y % 2 ^ 64) = ovfTest o 64 x y := by
  simp only [ovfTest, bv64_mod]

theorem mul_mod64 (x y : Nat) : binVal .mul 64 x (y % 2 ^ 64) % 2 ^ 64 = (bv 64 x * bv 64 y).toNat := by
  simp only [binVal, evalBin, bv64_mod]
  exact Nat.mod_eq_of_lt (BitVec.isLt _)

/-- A variable index term (`gTermT` on a variable, `gTerm` on a register). -/
theorem gTermT_var {P : PFunc} {ω : Nat → Nat} {t : World} {k av : Nat} {σ0 σ : Store} {g : GSt} {acc : GAcc}
    (hG : GRel P ω t k av σ0 g acc σ) {w : Nat} {A : Arg} {v : Nat}
    (hAv : A.get σ0 = v) (hAw : A.width = w) (hw : w ≤ 64) (hAb : A.below k) (hsh : ∃ j, A = .v j w)
    (hAi : A.var? ≠ some av) {scale : Nat} {g' : GSt} (h : gTermT k g A scale = .ok g') (hT : TempsOK P k g'.ts) :
    GSim P ω t k av σ0
      (if scale = 0 then .ok acc
       else if scale = 1 then gAddVar acc (if w < 64 then sext64 w v else v)
       else if ovfTest .smul 64 (if w < 64 then sext64 w v else v) scale then .ub
       else gAddVar acc ((bv 64 (if w < 64 then sext64 w v else v) * bv 64 scale).toNat)) g' := by
  obtain ⟨j, rfl⟩ := hsh
  simp only [Arg.get_v'] at hAv; subst hAv
  simp only [gTermT] at h
  generalize hgs : gSext k g (.v j w) = gs at h
  obtain ⟨g1, s1⟩ := gs
  simp only at h
  have hpre : ∃ l, g'.ts = g1.ts ++ l := by
    split at h
    · simp only [Except.ok.injEq] at h; subst h; exact ⟨[], by simp⟩
    split at h
    · simp only [Except.ok.injEq] at h; subst h; exact gAddVarT_ts k g1 s1
    · simp only [Except.ok.injEq] at h; subst h
      obtain ⟨l, hl⟩ := gAddVarT_ts k { g1 with s := g1.s ++ _, ts := g1.ts ++ [1, 64] } (.v (k + g1.ts.length + 1) 64)
      exact ⟨[1, 64] ++ l, by rw [hl]; simp⟩
  have hT1 : TempsOK P k (gSext k g (.v j w)).1.ts := by rw [hgs]; exact hT.pre hpre
  obtain ⟨σ1, hG1, hs1, hs1w, hs1b, hs1i⟩ := gSext_sim hG hw hAb hAi hT1
  rw [hgs] at hG1 hs1 hs1w hs1b hs1i
  simp only at hG1 hs1 hs1w hs1b hs1i
  generalize hsv : (if w < 64 then sext64 w (σ0 j) else σ0 j) = sv at hs1 ⊢
  split at h
  · rename_i h0
    simp only [Except.ok.injEq] at h; subst h
    simp only [h0, ite_true, GSim]; exact ⟨σ1, hG1⟩
  rename_i h0
  split at h
  · rename_i h1
    simp only [Except.ok.injEq] at h; subst h
    simp only [h0, h1, ite_true, ite_false]
    exact gAddVar_sim hG1 hs1 hs1w hs1b hs1i hT
  · rename_i h1
    simp only [Except.ok.injEq] at h; subst h
    simp only [h0, h1, ite_false]
    have hT2 := hT.pre (gAddVarT_ts _ _ _)
    simp only at hT2
    have w0 : P.wd (k + g1.ts.length) = 1 := by have := hT2 g1.ts.length (by simp); simpa using this
    have w1 : P.wd (k + g1.ts.length + 1) = 64 := by
      have := hT2 (g1.ts.length + 1) (by simp); simpa [Nat.add_assoc] using this
    have hsj : ∀ τ v c, s1.get (τ.set (k + g1.ts.length + c) v) = s1.get τ := fun τ v c =>
      Arg.get_agree (fun i hi => Store.set_other _ _ (by omega)) hs1b
    have hsj0 : ∀ τ v, s1.get (τ.set (k + g1.ts.length) v) = s1.get τ := fun τ v => hsj τ v 0
    have hrun := xStmts_run_append (Δ := [.assign (k + g1.ts.length) (.ovf .smul) [s1, c64 scale],
      .check (.v (k + g1.ts.length) 1) "ptr-arith" "MEM-PTR-ARITH",
      .assign (k + g1.ts.length + 1) (.bin .mul) [s1, c64 scale]]) hG1.run
    cases hov : ovfTest .smul 64 sv scale
    · simp only [Bool.false_eq_true, ite_false]
      have e : xStmts P ω σ1 t [.assign (k + g1.ts.length) (.ovf .smul) [s1, c64 scale],
          .check (.v (k + g1.ts.length) 1) "ptr-arith" "MEM-PTR-ARITH",
          .assign (k + g1.ts.length + 1) (.bin .mul) [s1, c64 scale]] =
          .ok ((σ1.set (k + g1.ts.length) 0).set (k + g1.ts.length + 1) (bv 64 sv * bv 64 scale).toNat) t := by
        simp only [xStmts_assign, xStmts_check, xStmts_nil, evalOpM, evalOp, testVal, w0, w1, hs1w,
          Arg.get_v', Store.set_same, hsj0, hs1, ovf1, c64_get, ovf_mod64, hov, truthN_boolToNat,
          Bool.false_eq_true, ite_false, mul_mod64]
        rfl
      have hG2 : GRel P ω t k av σ0 { g1 with s := g1.s ++ [.assign (k + g1.ts.length) (.ovf .smul) [s1, c64 scale],
          .check (.v (k + g1.ts.length) 1) "ptr-arith" "MEM-PTR-ARITH",
          .assign (k + g1.ts.length + 1) (.bin .mul) [s1, c64 scale]], ts := g1.ts ++ [1, 64] } acc
          ((σ1.set (k + g1.ts.length) 0).set (k + g1.ts.length + 1) (bv 64 sv * bv 64 scale).toNat) := by
        refine ⟨by rw [hrun, e], fun i hi => ?_, hG1.cst, ?_, hG1.avk⟩
        · rw [Store.set_other _ _ (by omega), Store.set_other _ _ (by omega)]; exact hG1.agree i hi
        · simp only [List.length_append, List.length_cons, List.length_nil]
          exact VarRel.mono (by omega) (VarRel.agree (fun i hi => by
            rw [Store.set_other _ _ (by omega), Store.set_other _ _ (by omega)]) hG1.var)
      exact gAddVar_sim hG2 (by simp) rfl (by simp only [Arg.below, List.length_append]; simp; omega)
        (by have := hG1.avk; simp [Arg.var?]; omega) hT
    · simp only [ite_true, GSim]
      obtain ⟨Δ, hΔ⟩ := gAddVarT_ext k { g1 with s := g1.s ++ [.assign (k + g1.ts.length) (.ovf .smul) [s1, c64 scale],
          .check (.v (k + g1.ts.length) 1) "ptr-arith" "MEM-PTR-ARITH",
          .assign (k + g1.ts.length + 1) (.bin .mul) [s1, c64 scale]], ts := g1.ts ++ [1, 64] }
          (.v (k + g1.ts.length + 1) 64)
      rw [hΔ]
      simp only
      rw [List.append_assoc, xStmts_append, hG1.run]
      show (xStmts P ω σ1 t ([_, _, _] ++ Δ)) = _
      rw [xStmts_append]
      simp only [xStmts_assign, xStmts_check, evalOpM, evalOp, testVal, w0, hs1w,
        Arg.get_v', Store.set_same, hs1, ovf1, c64_get, ovf_mod64, hov, truthN_boolToNat, ite_true]
      rfl

theorem gTermT_sim {P : PFunc} {ω : Nat → Nat} {t : World} {k av : Nat} {σ0 σ : Store} {g : GSt} {acc : GAcc}
    (hG : GRel P ω t k av σ0 g acc σ) {o : Opnd} {w : Nat} {A : Arg} {v : Nat} (hA : IArg σ0 k av o w A v)
    {scale : Nat} {g' : GSt} (h : gTermT k g A scale = .ok g') (hT : TempsOK P k g'.ts) :
    GSim P ω t k av σ0 (gTerm acc o w scale v) g' := by
  obtain ⟨hAv, hAw, hw, hAb, hsh, hAi⟩ := hA
  cases o with
  | const b =>
    obtain ⟨b', rfl⟩ := hsh
    simp only [Arg.get_c'] at hAv; subst hAv
    simp only [gTermT] at h
    split at h
    · simp only [Except.ok.injEq] at h; subst h
      simp only [gTerm, GSim]
      exact ⟨σ, hG.run, hG.agree, by simp [hG.cst], hG.var, hG.avk⟩
    · cases h
  | reg n => simp only [gTerm]; exact gTermT_var hG hAv hAw hw hAb hsh hAi h hT
  | poison => simp only [gTerm]; exact gTermT_var hG hAv hAw hw hAb hsh hAi h hT

theorem bound_tail {P : PFunc} {ω : Nat → Nat} {t : World} {k av : Nat} {σ0 σ1 : Store} {g1 : GSt} {acc : GAcc}
    (hG1 : GRel P ω t k av σ0 g1 acc σ1) {s64 : Arg} {sv : Nat} (hs : s64.get σ1 % 2 ^ 64 = sv) (hsw : s64.width = 64)
    {n : Nat} (hn : n < 2 ^ 63) (use : Nat) (prop cls : String) (hT : TempsOK P k (g1.ts ++ [1])) :
    ((if use = 0 then decide (n < sv) else decide (n ≤ sv)) = true →
      xStmts P ω σ0 t (g1.s ++ [.assign (k + g1.ts.length) (.cmp (if use = 0 then .ugt else .uge)) [s64, c64 n],
        .check (.v (k + g1.ts.length) 1) prop cls]) = .fail) ∧
    ((if use = 0 then decide (n < sv) else decide (n ≤ sv)) = false →
      ∃ σ', GRel P ω t k av σ0 { g1 with s := g1.s ++ [.assign (k + g1.ts.length) (.cmp (if use = 0 then .ugt else .uge))
        [s64, c64 n], .check (.v (k + g1.ts.length) 1) prop cls], ts := g1.ts ++ [1] } acc σ') := by
  have w0 : P.wd (k + g1.ts.length) = 1 := by have := hT g1.ts.length (by simp); simpa using this
  have hn64 : n % 2 ^ 64 = n := Nat.mod_eq_of_lt (by omega)
  have hc : truthN (evalOpM t.mem σ1 (.cmp (if use = 0 then .ugt else .uge)) 1 [s64, c64 n] % 2 ^ 1) =
      (if use = 0 then decide (n < sv) else decide (n ≤ sv)) := by
    by_cases hu : use = 0
    · simp only [hu, ite_true, evalOpM, evalOp, icmpVal_eq', hsw, pred_ugt, c64_get, Nat.mod_mod, hn64, hs,
        boolToNat_mod1, truthN_boolToNat]
    · simp only [hu, ite_false, evalOpM, evalOp, icmpVal_eq', hsw, pred_uge, c64_get, Nat.mod_mod, hn64, hs,
        boolToNat_mod1, truthN_boolToNat]
  have hrun := xStmts_run_append (Δ := [.assign (k + g1.ts.length) (.cmp (if use = 0 then .ugt else .uge))
    [s64, c64 n], .check (.v (k + g1.ts.length) 1) prop cls]) hG1.run
  refine ⟨fun h => ?_, fun h => ?_⟩
  · rw [hrun]
    simp only [xStmts_assign, xStmts_check, w0, Arg.get_v', Store.set_same, hc, h, ite_true]
  refine ⟨σ1.set (k + g1.ts.length) (evalOpM t.mem σ1 (.cmp (if use = 0 then .ugt else .uge)) 1 [s64, c64 n] % 2 ^ 1),
    ?_, fun j hj => ?_, hG1.cst, ?_, hG1.avk⟩
  · show xStmts P ω σ0 t (g1.s ++ _) = _
    rw [hrun]
    simp only [xStmts_assign, xStmts_check, w0, Arg.get_v', Store.set_same, hc, h, Bool.false_eq_true,
      ite_false, xStmts_nil]
  · rw [Store.set_other _ _ (by omega)]; exact hG1.agree j hj
  · simp only [List.length_append, List.length_cons, List.length_nil]
    exact VarRel.mono (by omega) (VarRel.agree (fun j hj => Store.set_other _ _ (by omega)) hG1.var)

theorem gBoundT_sim {P : PFunc} {ω : Nat → Nat} {t : World} {k av : Nat} {σ0 σ : Store} {g : GSt} {acc : GAcc}
    (hG : GRel P ω t k av σ0 g acc σ) {o : Opnd} {w : Nat} {A : Arg} {v : Nat} (hA : IArg σ0 k av o w A v)
    {n use : Nat} (hn : n < 2 ^ 63) (hT : TempsOK P k (gBoundT k g A n use).ts) :
    (gBoundBad w n use v = true → xStmts P ω σ0 t (gBoundT k g A n use).s = .fail) ∧
    (gBoundBad w n use v = false → ∃ σ', GRel P ω t k av σ0 (gBoundT k g A n use) acc σ') := by
  obtain ⟨hAv, hAw, hw, hAb, hsh, hAi⟩ := hA
  by_cases hn0 : n = 0
  · subst hn0
    simp only [gBoundT, ite_true, gBoundBad]
    exact ⟨fun h => by simp at h, fun _ => ⟨σ, hG⟩⟩
  have hbad : gBoundBad w n use v =
      (if use = 0 then decide (n < (if w < 64 then sext64 w v % 2 ^ 64 else v % 2 ^ 64))
       else decide (n ≤ (if w < 64 then sext64 w v % 2 ^ 64 else v % 2 ^ 64))) := by
    simp only [gBoundBad]
    have : decide (0 < n) = true := by simp; omega
    rw [this, Bool.true_and]
  rw [hbad]
  cases o
  case const b =>
    obtain ⟨b', rfl⟩ := hsh
    simp only [Arg.get_c'] at hAv; subst hAv
    by_cases h64 : w < 64
    · simp only [gBoundT, hn0, h64, ite_true, ite_false] at hT ⊢
      exact bound_tail hG (s64 := c64 (sext64 w b')) (by simp [h64]) rfl hn use _ _ hT
    · simp only [gBoundT, hn0, h64, ite_false] at hT ⊢
      exact bound_tail hG (s64 := .c w b') (by simp [h64]) (by simp; omega) hn use _ _ hT
  all_goals
    obtain ⟨j, rfl⟩ := hsh
    simp only [Arg.get_v'] at hAv; subst hAv
    simp only [gBoundT, hn0, ite_false] at hT ⊢
    generalize hgs : gSext k g (.v j w) = gs at hT ⊢
    obtain ⟨g1, s1⟩ := gs
    simp only at hT ⊢
    have hT1 : TempsOK P k (gSext k g (.v j w)).1.ts := by rw [hgs]; exact hT.left
    obtain ⟨σ1, hG1, hs1, hs1w, _⟩ := gSext_sim hG hw hAb hAi hT1
    rw [hgs] at hG1 hs1 hs1w
    simp only at hG1 hs1 hs1w
    refine bound_tail hG1 (s64 := s1) ?_ hs1w hn use _ _ hT
    rw [hs1]; split <;> rfl
/-- `g'` continues `g`: more statements and temporaries after its own. -/
def GExt (g g' : GSt) : Prop := (∃ l, g'.s = g.s ++ l) ∧ (∃ l, g'.ts = g.ts ++ l)

theorem GExt.refl (g : GSt) : GExt g g := ⟨⟨[], by simp⟩, ⟨[], by simp⟩⟩

theorem GExt.trans {a b c : GSt} (h1 : GExt a b) (h2 : GExt b c) : GExt a c := by
  obtain ⟨⟨l1, e1⟩, ⟨m1, f1⟩⟩ := h1
  obtain ⟨⟨l2, e2⟩, ⟨m2, f2⟩⟩ := h2
  exact ⟨⟨l1 ++ l2, by rw [e2, e1, List.append_assoc]⟩, ⟨m1 ++ m2, by rw [f2, f1, List.append_assoc]⟩⟩

theorem gAddVarT_gext (k : Nat) (g : GSt) (x : Arg) : GExt g (gAddVarT k g x) := by
  unfold gAddVarT; split
  · exact ⟨⟨[], by simp⟩, ⟨[], by simp⟩⟩
  · exact ⟨⟨_, rfl⟩, ⟨_, rfl⟩⟩

theorem gSext_gext (k : Nat) (g : GSt) (A : Arg) : GExt g (gSext k g A).1 := by
  unfold gSext; split
  · split
    · exact ⟨⟨_, rfl⟩, ⟨_, rfl⟩⟩
    · exact GExt.refl g
  · exact GExt.refl g

theorem gTermT_gext {k : Nat} {g g' : GSt} {A : Arg} {scale : Nat} (h : gTermT k g A scale = .ok g') :
    GExt g g' := by
  cases A with
  | c w b =>
    simp only [gTermT] at h
    split at h
    · simp only [Except.ok.injEq] at h; subst h; exact ⟨⟨[], by simp⟩, ⟨[], by simp⟩⟩
    · cases h
  | v j w =>
    simp only [gTermT] at h
    have e := gSext_gext k g (.v j w)
    generalize gSext k g (.v j w) = gs at h e
    obtain ⟨g1, s1⟩ := gs
    simp only at h e
    split at h
    · simp only [Except.ok.injEq] at h; subst h; exact e
    split at h
    · simp only [Except.ok.injEq] at h; subst h; exact e.trans (gAddVarT_gext _ _ _)
    · simp only [Except.ok.injEq] at h; subst h
      exact e.trans ((show GExt g1 _ from ⟨⟨_, rfl⟩, ⟨_, rfl⟩⟩).trans (gAddVarT_gext _ _ _))

theorem gBoundT_gext (k : Nat) (g : GSt) (A : Arg) (n use : Nat) : GExt g (gBoundT k g A n use) := by
  unfold gBoundT
  split
  · exact GExt.refl g
  · generalize (if use = 1 then ("oob-read", "MEM-OOB-READ")
      else if use = 2 then ("oob-write", "MEM-OOB-WRITE") else ("ptr-arith", "MEM-PTR-ARITH")) = pc
    obtain ⟨p, c⟩ := pc
    cases A with
    | c w b => exact ⟨⟨_, rfl⟩, ⟨_, rfl⟩⟩
    | v j w =>
      have e := gSext_gext k g (.v j w)
      generalize gSext k g (.v j w) = gs at e ⊢
      obtain ⟨g1, s1⟩ := gs
      exact e.trans ⟨⟨_, rfl⟩, ⟨_, rfl⟩⟩

theorem gLoop_gext {k : Nat} : ∀ {g g' : GSt} {L : List (GIdx × Arg)}, gLoop k g L = .ok g' → GExt g g'
  | g, g', [], h => by simp only [gLoop, Except.ok.injEq] at h; subst h; exact GExt.refl g
  | g, g', (.first _ _ scale, A) :: t, h => by
    simp only [gLoop] at h
    obtain ⟨g1, h1, h⟩ := Except.bind_ok h
    exact (gTermT_gext h1).trans (gLoop_gext h)
  | g, g', (.field off, _) :: t, h => by
    simp only [gLoop] at h
    exact (show GExt g { g with cst := g.cst + off } from ⟨⟨[], by simp⟩, ⟨[], by simp⟩⟩).trans (gLoop_gext h)
  | g, g', (.arr _ _ scale n use, A) :: t, h => by
    simp only [gLoop] at h
    split at h
    · simp [throw, throwThe, MonadExceptOf.throw, bind, Except.bind] at h
    · obtain ⟨g1, h1, h⟩ := Except.bind_ok h
      exact (gBoundT_gext k g A n use).trans ((gTermT_gext h1).trans (gLoop_gext h))

theorem GSim.ext_ub {P : PFunc} {ω : Nat → Nat} {t : World} {σ0 : Store} {g g' : GSt}
    (h : xStmts P ω σ0 t g.s = .fail) (e : GExt g g') : xStmts P ω σ0 t g'.s = .fail := by
  obtain ⟨⟨l, hl⟩, _⟩ := e
  rw [hl]; exact xStmts_fail_append h l

theorem TempsOK.ext {P : PFunc} {k : Nat} {g g' : GSt} (h : TempsOK P k g'.ts) (e : GExt g g') :
    TempsOK P k g.ts := h.pre e.2

/-- Index operands and their values, position by position. -/
inductive IdxR (σ0 : Store) (k av : Nat) : List GIdx → List Arg → List Nat → Prop
  | nil : IdxR σ0 k av [] [] []
  | first {o : Opnd} {w sc : Nat} {A : Arg} {v : Nat} {ix : List GIdx} {As : List Arg} {vs : List Nat} :
    IArg σ0 k av o w A v → IdxR σ0 k av ix As vs → IdxR σ0 k av (.first o w sc :: ix) (A :: As) (v :: vs)
  | field {off : Nat} {A : Arg} {v : Nat} {ix : List GIdx} {As : List Arg} {vs : List Nat} :
    IdxR σ0 k av ix As vs → IdxR σ0 k av (.field off :: ix) (A :: As) (v :: vs)
  | arr {o : Opnd} {w sc n use : Nat} {A : Arg} {v : Nat} {ix : List GIdx} {As : List Arg} {vs : List Nat} :
    IArg σ0 k av o w A v → IdxR σ0 k av ix As vs → IdxR σ0 k av (.arr o w sc n use :: ix) (A :: As) (v :: vs)

theorem gLoop_sim {P : PFunc} {ω : Nat → Nat} {t : World} {k av : Nat} {σ0 : Store} :
    ∀ {ix : List GIdx} {As : List Arg} {vs : List Nat}, IdxR σ0 k av ix As vs →
    ∀ {g g' : GSt} {acc : GAcc} {σ : Store}, GRel P ω t k av σ0 g acc σ → gLoop k g (ix.zip As) = .ok g' →
    TempsOK P k g'.ts → GSim P ω t k av σ0 (gFold acc (ix.zip vs)) g'
  | _, _, _, .nil, g, g', acc, σ, hG, h, _ => by
    simp only [List.zip_nil_left, gLoop, Except.ok.injEq] at h; subst h
    exact ⟨σ, hG⟩
  | _, _, _, .first hA hI, g, g', acc, σ, hG, h, hT => by
    simp only [List.zip_cons_cons, gLoop] at h
    obtain ⟨g1, h1, h⟩ := Except.bind_ok h
    have e2 := gLoop_gext h
    have hs := gTermT_sim hG hA h1 (hT.ext e2)
    simp only [List.zip_cons_cons, gFold]
    generalize gTerm acc _ _ _ _ = r at hs
    cases r with
    | ok acc1 =>
      obtain ⟨σ1, hG1⟩ := hs
      exact gLoop_sim hI hG1 h hT
    | ub => exact GSim.ext_ub hs e2
    | stuck => trivial
  | _, _, _, .field hI, g, g', acc, σ, hG, h, hT => by
    simp only [List.zip_cons_cons, gLoop] at h
    simp only [List.zip_cons_cons, gFold]
    exact gLoop_sim hI (g := { g with cst := g.cst + _ }) (acc := { acc with cst := acc.cst + _ }) (σ := σ)
      ⟨hG.run, hG.agree, by simp [hG.cst], hG.var, hG.avk⟩ h hT
  | _, _, _, .arr (n := n) (use := use) hA hI, g, g', acc, σ, hG, h, hT => by
    simp only [List.zip_cons_cons, gLoop] at h
    split at h
    · simp [throw, throwThe, MonadExceptOf.throw, bind, Except.bind] at h
    rename_i hu
    obtain ⟨g1, h1, h⟩ := Except.bind_ok h
    have hn : n < 2 ^ 63 := by simpa using hu
    have e1 := gTermT_gext h1
    have e2 := gLoop_gext h
    have hb := gBoundT_sim hG hA (use := use) hn (hT.ext (e1.trans e2))
    simp only [List.zip_cons_cons, gFold]
    cases hbad : gBoundBad _ n use _
    · obtain ⟨σb, hGb⟩ := hb.2 hbad
      have hs := gTermT_sim hGb hA h1 (hT.ext e2)
      simp only [Bool.false_eq_true, ite_false]
      generalize gTerm acc _ _ _ _ = r at hs
      cases r with
      | ok acc1 =>
        obtain ⟨σ1, hG1⟩ := hs
        exact gLoop_sim hI hG1 h hT
      | ub => exact GSim.ext_ub hs e2
      | stuck => trivial
    · simp only [ite_true, GSim]
      exact GSim.ext_ub (hb.1 hbad) (e1.trans e2)

theorem Arg.get_set_ne' {i : Nat} (σ : Store) (v : Nat) : ∀ {a : Arg}, a.var? ≠ some i →
    a.get (σ.set i v) = a.get σ
  | .c _ _, _ => rfl
  | .v j _, h => by
    simp only [Arg.var?, ne_eq, Option.some.injEq] at h
    exact Store.set_other _ _ h

/-- When the pointer addition of `getelementptr` is undefined (`gFinish`'s `fin`). -/
def finBad (m : Mem) (inb : Bool) (b' dv : Nat) : Bool :=
  if inb then
    (decide (ptrObj b' = 0) && !decide (dv % 2 ^ 64 = 0)) ||
    (!decide (m.kind b' = 0) && (!decide (ptrObj ((b' + dv) % 2 ^ 64) = ptrObj b') ||
      decide (m.size b' < ptrOff ((b' + dv) % 2 ^ 64))))
  else !decide (ptrObj b' = 0) && !decide (ptrObj ((b' + dv) % 2 ^ 64) = ptrObj b')

theorem gFin_run (P : PFunc) (ω : Nat → Nat) (σ : Store) (t : World) (i j : Nat) (hij : i < j) (B D : Arg)
    (hBj : B.below j) (hBi : B.var? ≠ some i) (hDj : D.below j) (hDi : D.var? ≠ some i) (hDw : D.width = 64)
    (hwi : P.wd i = 64) (inb : Bool) (hT : TempsOK P j (gFinT inb)) :
    (finBad t.mem inb (B.get σ % 2 ^ 64) (D.get σ) = true → xStmts P ω σ t (gFinS i j B D inb) = .fail) ∧
    (finBad t.mem inb (B.get σ % 2 ^ 64) (D.get σ) = false → ∃ σ', xStmts P ω σ t (gFinS i j B D inb) = .ok σ' t ∧
      σ' i = (B.get σ % 2 ^ 64 + D.get σ) % 2 ^ 64 ∧ ∀ x, x < j → x ≠ i → σ' x = σ x) := by
  have hkk : ∀ a b : Nat, (j + a = j + b) = (a = b) := fun a b => by simp
  have hk0 : ∀ a : Nat, (j + a = j) = (a = 0) := fun a => by simp
  have hk1 : ∀ a : Nat, (j = j + a) = (a = 0) := fun a => by simp [eq_comm]
  have hi1 : ∀ a : Nat, (i = j + a) = False := fun a => by simp; omega
  have hi2 : ∀ a : Nat, (j + a = i) = False := fun a => by simp; omega
  have hi3 : (i = j) = False := by simp; omega
  have hi4 : (j = i) = False := by simp; omega
  have hBs : ∀ τ x c, B.get (τ.set (j + c) x) = B.get τ := fun τ x c =>
    Arg.get_agree (fun y hy => Store.set_other _ _ (by omega)) hBj
  have hBs0 : ∀ τ x, B.get (τ.set j x) = B.get τ := fun τ x => by simpa using hBs τ x 0
  have hBsi : ∀ τ x, B.get (τ.set i x) = B.get τ := fun τ x => Arg.get_set_ne' τ x hBi
  have hDs : ∀ τ x c, D.get (τ.set (j + c) x) = D.get τ := fun τ x c =>
    Arg.get_agree (fun y hy => Store.set_other _ _ (by omega)) hDj
  have hDs0 : ∀ τ x, D.get (τ.set j x) = D.get τ := fun τ x => by simpa using hDs τ x 0
  have hDsi : ∀ τ x, D.get (τ.set i x) = D.get τ := fun τ x => Arg.get_set_ne' τ x hDi
  generalize hb : B.get σ % 2 ^ 64 = b'
  have hb64 : b' < 2 ^ 64 := by rw [← hb]; exact Nat.mod_lt _ (Nat.two_pow_pos 64)
  have hr64 : ((b' + D.get σ) % 2 ^ 64) % 2 ^ 64 = (b' + D.get σ) % 2 ^ 64 := Nat.mod_mod _ _
  have hadd : binVal .add 64 (B.get σ) (D.get σ) = (b' + D.get σ) % 2 ^ 64 := by
    rw [add64, ← hb, Nat.mod_add_mod]
  have hobj : ∀ x : Nat, ptrObj (x % 2 ^ 64) % 2 ^ 64 = ptrObj (x % 2 ^ 64) := ptrObj_mod
  have hbo : ptrObj (B.get σ % 2 ^ 64) = ptrObj b' := by rw [hb]
  have hbo' : ptrObj b' % 2 ^ 64 = ptrObj b' := by rw [← hb]; exact ptrObj_mod _
  have hro : ∀ x, ptrObj (x % 2 ^ 64) % 2 ^ 64 = ptrObj (x % 2 ^ 64) := ptrObj_mod
  cases inb
  · have w0 : P.wd j = 64 := by simpa [gFinT] using wd_of hT 0 (by decide)
    simp (config := { decide := true }) only [gFinS, gFinT, List.cons_append, List.nil_append, xStmts_assign,
      xStmts_check, xStmts_nil, w0, wd_of hT 1 (by decide), wd_of hT 2 (by decide), wd_of hT 3 (by decide),
      wd_of hT 4 (by decide), hwi, Bool.false_eq_true, ite_false, List.getElem_cons_succ, List.getElem_cons_zero,
      evalOpM, evalOp, Arg.get_v', Arg.get_c', c64_get, set_apply, hkk, hk0, hk1, hi1, hi2, hi3, hi4,
      Arg.width_v', Arg.width_c', c64_width, hBs, hBs0, hBsi, hDs, hDs0, hDsi, ite_true,
      lshr48, hadd, hr64, hobj, hbo, icmpVal_eq', pred_eq, pred_ne, boolToNat_mod1, and1, BitVec.toNat_ofBool,
      truthN_boolToNat, Nat.zero_mod, finBad, hbo', hro]
    refine ⟨fun h => by simp only [h, ite_true], fun h => ?_⟩
    simp only [h, Bool.false_eq_true, ite_false]
    refine ⟨_, rfl, by simp [set_apply, hi1, hi3], fun x hx hxi => ?_⟩
    have hne : ∀ c, x ≠ j + c := fun c => by omega
    simp [set_apply, hne, show x ≠ j by omega, hxi]
  · have w0 : P.wd j = 64 := by simpa [gFinT] using wd_of hT 0 (by decide)
    have hro' : ptrOff ((b' + D.get σ) % 2 ^ 64 % 2 ^ 64) = ptrOff ((b' + D.get σ) % 2 ^ 64) := by
      rw [Nat.mod_mod]
    simp (config := { decide := true }) only [gFinS, gFinT, List.cons_append, List.nil_append, xStmts_assign,
      xStmts_check, xStmts_nil, w0, wd_of hT 1 (by decide), wd_of hT 2 (by decide), wd_of hT 3 (by decide),
      wd_of hT 4 (by decide), wd_of hT 5 (by decide), wd_of hT 6 (by decide), wd_of hT 7 (by decide),
      wd_of hT 8 (by decide), wd_of hT 9 (by decide), wd_of hT 10 (by decide), wd_of hT 11 (by decide),
      wd_of hT 12 (by decide), hwi, ite_true, List.getElem_cons_succ, List.getElem_cons_zero,
      evalOpM, evalOp, Arg.get_v', Arg.get_c', c64_get, set_apply, hkk, hk0, hk1, hi1, hi2, hi3, hi4,
      Arg.width_v', Arg.width_c', c64_width, hBs, hBs0, hBsi, hDs, hDs0, hDsi, ite_false, Bool.false_eq_true,
      lshr48, mask48, hadd, hr64, hobj, hbo, icmpVal_eq', pred_eq, pred_ne, pred_ugt, boolToNat_mod1, and1, or1,
      BitVec.toNat_ofBool, truthN_boolToNat, Nat.zero_mod, finBad, hbo', hro, hb, kind_mod, size_mod, ptrOff_mod,
      hro', hDw]
    refine ⟨fun h => ?_, fun h => ?_⟩
    · cases h1 : (decide (ptrObj b' = 0) && !decide (D.get σ % 2 ^ 64 = 0))
      · rw [h1, Bool.false_or] at h; simp only [h1, h, Bool.false_eq_true, ite_true, ite_false]
      · simp only [ite_true]
    · rw [Bool.or_eq_false_iff] at h
      simp only [h.1, h.2, Bool.false_eq_true, ite_false]
      refine ⟨_, rfl, by simp [set_apply, hi1, hi3], fun x hx hxi => ?_⟩
      have hne : ∀ c, x ≠ j + c := fun c => by omega
      simp [set_apply, hne, show x ≠ j by omega, hxi]

theorem gFin_eq (m : Mem) (inb : Bool) (b d : Nat) :
    gFin m inb b d = if finBad m inb (b % 2 ^ 64) d then .ub else .ok ((b % 2 ^ 64 + d) % 2 ^ 64) := by
  have e1 : ∀ x : Nat, (x == 0) = decide (x = 0) := fun x => Bool.eq_iff_iff.mpr (by simp)
  have e2 : ∀ x y : Nat, (x != y) = !decide (x = y) := fun x y => Bool.eq_iff_iff.mpr (by simp)
  cases inb <;> simp only [gFin, finBad, e1, e2, Bool.false_eq_true, ite_false, ite_true]

/-- What the end of `getelementptr` leaves: the result in `i`, nothing else
below `k` changed. -/
def GEndSim (t : World) (k i : Nat) (σ0 : Store) : Res Nat → XPRes → Prop
  | .ok r, p => ∃ σ', p = .ok σ' t ∧ σ' i = r ∧ ∀ x, x < k → x ≠ i → σ' x = σ0 x
  | .ub, p => p = .fail
  | .stuck, _ => True

theorem cst_toNat (c : Int) : (c % 2 ^ 64).toNat % 2 ^ 64 = (c % 2 ^ 64).toNat := by
  apply Nat.mod_eq_of_lt
  have h1 : c % 2 ^ 64 < 2 ^ 64 := Int.emod_lt_of_pos _ (by decide)
  have h2 : 0 ≤ c % 2 ^ 64 := Int.emod_nonneg _ (by decide)
  omega

/-- The pointer addition and object checks, from the loop's end state. -/
theorem gFin_sim {P : PFunc} {ω : Nat → Nat} {t : World} {k i : Nat} {σ0 σ : Store} {g : GSt} {acc : GAcc}
    (hG : GRel P ω t k i σ0 g acc σ) {B D : Arg} (hBb : B.below k) (hBi : B.var? ≠ some i)
    (hDb : D.below (k + g.ts.length)) (hDi : D.var? ≠ some i) (hDw : D.width = 64) (hwi : P.wd i = 64)
    (inb : Bool) (hT : TempsOK P k (g.ts ++ gFinT inb)) :
    GEndSim t k i σ0 (gFin t.mem inb (B.get σ0) (D.get σ)) (xStmts P ω σ0 t (g.s ++ gFinS i (k + g.ts.length) B D inb)) := by
  have hik := hG.avk
  have hr := gFin_run P ω σ t i (k + g.ts.length) (by omega) B D (Arg.below_mono (by omega) hBb) hBi hDb hDi hDw
    hwi inb hT.right
  rw [hG.get hBb] at hr
  rw [xStmts_run_append hG.run, gFin_eq]
  cases hb : finBad t.mem inb (B.get σ0 % 2 ^ 64) (D.get σ)
  · obtain ⟨σ', e, h1, h2⟩ := hr.2 hb
    simp only [Bool.false_eq_true, ite_false, GEndSim]
    exact ⟨σ', e, h1, fun x hx hxi => by rw [h2 x (by omega) hxi]; exact hG.agree x hx⟩
  · simp only [ite_true, GEndSim]; exact hr.1 hb

theorem gEnd_sim {P : PFunc} {ω : Nat → Nat} {t : World} {k i : Nat} {σ0 σ : Store} {g : GSt} {acc : GAcc}
    (hG : GRel P ω t k i σ0 g acc σ) {B : Arg} (hBb : B.below k) (hBi : B.var? ≠ some i) (hwi : P.wd i = 64)
    (inb : Bool) (hT : TempsOK P k (gEnd k i B inb g).2) :
    GEndSim t k i σ0 (gFinish t.mem inb (B.get σ0) acc) (xStmts P ω σ0 t (gEnd k i B inb g).1) := by
  have hik := hG.avk
  have hv := hG.var
  have hc := hG.cst
  cases hgv : g.var with
  | none =>
    rw [hgv] at hv
    cases hav : acc.var with
    | some _ => rw [hav] at hv; exact hv.elim
    | none =>
      by_cases h0 : acc.cst = 0
      · have hg0 : g.cst = 0 := by rw [hc, h0]
        simp only [gEnd, hgv, gFinish, hav, h0, hg0, ite_true, Option.isNone_none, Bool.true_and,
          beq_self_eq_true, GEndSim] at hT ⊢
        rw [xStmts_run_append hG.run]
        simp only [xStmts_assign, xStmts_nil, evalOpM, evalOp, hwi, hG.get hBb]
        refine ⟨_, rfl, by simp, fun x hx hxi => ?_⟩
        rw [Store.set_other _ _ hxi]; exact hG.agree x hx
      · have hg0 : g.cst ≠ 0 := by rw [hc]; exact h0
        simp only [gEnd, hgv, gFinish, hav, h0, hg0, ite_false, Option.isNone_none, Bool.true_and,
          beq_iff_eq, GEndSim] at hT ⊢
        have := gFin_sim hG (D := c64 (g.cst % 2 ^ 64).toNat) hBb hBi trivial (by simp [c64, Arg.var?]) rfl hwi inb hT
        simp only [c64_get, cst_toNat] at this
        rw [← hc]
        exact this
  | some A =>
    rw [hgv] at hv
    cases hav : acc.var with
    | none => rw [hav] at hv; exact hv.elim
    | some y =>
      rw [hav] at hv
      obtain ⟨hA, hAw, hAb, hAi⟩ := hv
      by_cases h0 : acc.cst = 0
      · have hg0 : g.cst = 0 := by rw [hc, h0]
        simp only [gEnd, hgv, gFinish, hav, h0, hg0, ite_true, ne_eq, not_true_eq_false, ite_false,
          Option.isNone_some, Bool.false_and, Bool.false_eq_true] at hT ⊢
        have := gFin_sim hG hBb hBi hAb hAi hAw hwi inb hT
        rw [hA] at this
        exact this
      · have hg0 : g.cst ≠ 0 := by rw [hc]; exact h0
        simp only [gEnd, hgv, gFinish, hav, h0, hg0, ite_false, ne_eq, not_false_eq_true, ite_true] at hT ⊢
        generalize hg2 : gAddVarT k g (c64 (g.cst % 2 ^ 64).toNat) = g2 at hT ⊢
        obtain ⟨D2, hD2⟩ : ∃ D2, g2.var = some D2 := by rw [← hg2]; simp [gAddVarT, hgv]
        simp only [hD2, Option.isNone_some, Bool.false_and, Bool.false_eq_true, ite_false, Option.getD_some] at hT ⊢
        have hT2 : TempsOK P k (gAddVarT k g (c64 (g.cst % 2 ^ 64).toNat)).ts := by rw [hg2]; exact hT.left
        have hs := gAddVar_sim hG (x := c64 (g.cst % 2 ^ 64).toNat) (xv := (acc.cst % 2 ^ 64).toNat)
          (by rw [c64_get, cst_toNat, hc]) rfl trivial (by simp [c64, Arg.var?]) hT2
        rw [hg2] at hs
        cases hr : gAddVar acc (acc.cst % 2 ^ 64).toNat with
        | ok a =>
          rw [hr] at hs
          obtain ⟨σ2, hG2⟩ := hs
          have hv2 := hG2.var
          rw [hD2] at hv2
          cases ha : a.var with
          | none => rw [ha] at hv2; exact hv2.elim
          | some y2 =>
            rw [ha] at hv2
            obtain ⟨h1, h2, h3, h4⟩ := hv2
            have := gFin_sim hG2 hBb hBi h3 h4 h2 hwi inb hT
            rw [h1] at this
            simp only [Res.bind, ha, Option.getD_some]
            exact this
        | ub =>
          rw [hr] at hs
          simp only [Res.bind, GEndSim]
          exact xStmts_fail_append hs _
        | stuck => trivial

end PrismRefine
