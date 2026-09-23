/-
PRISM techniques: the certified-mode bit-blaster, circuit layer (roadmap 5.4
and 8.2, row "Bit-blaster"; roadmap 3.2 "Certified mode").

This file holds the bit-level semantic facts (reused from core Lean's
`bv_decide` development in `Init.Data.BitVec.Bitblast`), the and/or/xor gate
circuits, their Tseitin clauses, and the specification (`Spec`, `Rep`) every
circuit builder is proved against.  `PrismTechniques/BitblastOps.lean` builds
the operator circuits, `PrismTechniques/BitblastDiv.lean` the divider, and
`PrismTechniques/BitblastEncode.lean` the expression language, the encoder,
`toCNF` and the main theorems (`toCNF_equisat`, `certified_unsat`,
`certified_dag_unsat`).

Reuse of core Lean: `BitVec.carry`, `BitVec.carry_succ`, `BitVec.getLsbD_add`,
`BitVec.getLsbD_add_add_bool` (ripple-carry adder / subtractor),
`BitVec.ult_eq_not_carry`, `BitVec.ule_eq_carry`, `BitVec.slt_eq_ult`
(comparators), `BitVec.mulRec`, `BitVec.getLsbD_mul` (multiplier), the shift
recurrences `shiftLeftRec` / `ushiftRightRec` / `sshiftRightRec` (barrel
shifters), the overflow characterisations `saddOverflow_eq`, `ssubOverflow_eq`,
`umulOverflow_eq`, `two_pow_le_toInt_mul_toInt_iff`,
`toInt_mul_toInt_lt_neg_two_pow_iff`, the division recurrence `divRec`
(`udiv_eq_divRec`, `umod_eq_divRec`) and the soundness theorem of the LRAT
checker, `Std.Tactic.BVDecide.LRAT.check_sound`.  The circuits, the Tseitin
encoding and the equisatisfiability proof are PRISM's own.

Variable layout: formula input bit `j` is CNF variable `2*j`; CNF variable
`1` is the constant `true`; gate `k` (0-based, in creation order) is CNF
variable `2*k+3`.  A circuit caches its gate count (`Circuit.len`) so the
executable encoder is linear, not quadratic, in the number of gates; the
well-formedness invariant `Circuit.WF` says the cache is right.
-/
import Std.Tactic.BVDecide.LRAT.Checker

namespace PrismTechniques.Bitblast

open Std.Sat

/-! ## Semantic helpers on bit lists -/

/-- Little-endian bit list of a bitvector. -/
def toBits {w : Nat} (x : BitVec w) : List Bool := (List.range w).map x.getLsbD

theorem zipWith_map_same {α β γ δ : Type} (f : β → γ → δ) (g : α → β) (h : α → γ) :
    ∀ l : List α, List.zipWith f (l.map g) (l.map h) = l.map (fun i => f (g i) (h i))
  | [] => rfl
  | a :: l => by simp [zipWith_map_same f g h l]

theorem toBits_and {w : Nat} (x y : BitVec w) :
    toBits (x &&& y) = List.zipWith (· && ·) (toBits x) (toBits y) := by
  simp [toBits]

theorem toBits_or {w : Nat} (x y : BitVec w) :
    toBits (x ||| y) = List.zipWith (· || ·) (toBits x) (toBits y) := by
  simp [toBits]

theorem toBits_xor {w : Nat} (x y : BitVec w) :
    toBits (x ^^^ y) = List.zipWith (· ^^ ·) (toBits x) (toBits y) := by
  simp [toBits]

theorem toBits_not {w : Nat} (x : BitVec w) : toBits (~~~x) = (toBits x).map (!·) := by
  simp only [toBits, List.map_map]
  apply List.map_congr_left
  intro i hi
  simp [List.mem_range.1 hi]

theorem toBits_ite {w : Nat} (b : Bool) (x y : BitVec w) :
    toBits (if b then x else y) =
      List.zipWith (fun p q => if b then p else q) (toBits x) (toBits y) := by
  cases b <;> simp [toBits]

theorem toBits_ofBool (b : Bool) : toBits (BitVec.ofBool b) = [b] := by
  simp [toBits, List.range_succ]

/-- Ripple-carry adder on bit lists, written with the gates the encoder uses;
the final carry is appended as the last element. -/
def rippleSem : List Bool → List Bool → Bool → List Bool
  | a :: as, b :: bs, c => ((a ^^ b) ^^ c) :: rippleSem as bs ((a && b) || ((a ^^ b) && c))
  | _, _, c => [c]

theorem ripple_carry_gate (a b c : Bool) :
    ((a && b) || ((a ^^ b) && c)) = Bool.atLeastTwo a b c := by
  cases a <;> cases b <;> cases c <;> rfl

/-- The list adder computes core Lean's `BitVec.carry` chain (reused from
`Init.Data.BitVec.Bitblast`). -/
theorem rippleSem_range {w : Nat} (x y : BitVec w) (c : Bool) : ∀ n s : Nat,
    rippleSem ((List.range' s n).map x.getLsbD) ((List.range' s n).map y.getLsbD)
        (BitVec.carry s x y c) =
      (List.range' s n).map (fun i => x.getLsbD i ^^ (y.getLsbD i ^^ BitVec.carry i x y c)) ++
        [BitVec.carry (s + n) x y c]
  | 0, s => by simp [rippleSem]
  | n + 1, s => by
    simp only [List.range'_succ, List.map_cons, rippleSem, List.cons_append]
    rw [ripple_carry_gate, ← BitVec.carry_succ, rippleSem_range x y c n (s + 1)]
    simp [Nat.add_assoc, Nat.add_comm 1 n]

theorem toBits_add {w : Nat} (x y : BitVec w) :
    toBits (x + y) = (rippleSem (toBits x) (toBits y) false).dropLast := by
  have h := rippleSem_range x y false w 0
  rw [BitVec.carry_zero] at h
  simp only [toBits, List.range_eq_range']
  rw [h, List.dropLast_concat]
  apply List.map_congr_left
  intro i hi
  rw [BitVec.getLsbD_add (by simpa using hi)]

theorem ult_bits {w : Nat} (x y : BitVec w) :
    x.ult y = !(rippleSem (toBits x) (toBits (~~~y)) true).getLastD false := by
  have h := rippleSem_range x (~~~y) true w 0
  rw [BitVec.carry_zero] at h
  simp only [toBits, List.range_eq_range']
  rw [h, List.getLastD_concat, BitVec.ult_eq_not_carry]
  simp

theorem msb_bits {w : Nat} (x : BitVec w) : x.msb = (toBits x).getLastD false := by
  rw [BitVec.msb_eq_getLsbD_last]
  cases w with
  | zero => simp [toBits]
  | succ n => simp [toBits, List.range_succ]

theorem slt_bits {w : Nat} (x y : BitVec w) :
    x.slt y = ((((toBits x).getLastD false) ^^ ((toBits y).getLastD false)) ^^
      !(rippleSem (toBits x) (toBits (~~~y)) true).getLastD false) := by
  rw [BitVec.slt_eq_ult, ← msb_bits, ← msb_bits, ← ult_bits]

/-- Conjunction accumulator, as the encoder computes it. -/
def andSem : List Bool → Bool → Bool
  | [], acc => acc
  | b :: bs, acc => andSem bs (acc && b)

theorem andSem_eq : ∀ (bs : List Bool) (acc : Bool), andSem bs acc = (acc && bs.all id)
  | [], acc => by simp [andSem]
  | b :: bs, acc => by simp [andSem, andSem_eq bs, Bool.and_assoc]

theorem beq_bits {w : Nat} (x y : BitVec w) :
    (x == y) = andSem ((List.zipWith (· ^^ ·) (toBits x) (toBits y)).map (!·)) true := by
  rw [andSem_eq, Bool.true_and, Bool.eq_iff_iff, beq_iff_eq, List.all_eq_true]
  simp only [toBits, zipWith_map_same, List.map_map, List.mem_map, List.mem_range]
  constructor
  · rintro rfl
    rintro _ ⟨i, _, rfl⟩
    simp
  · intro h
    apply BitVec.eq_of_getLsbD_eq
    intro i hi
    have := h _ ⟨i, hi, rfl⟩
    simpa using this

/-- A bitvector from a bit function. -/
def bvOf : (w : Nat) → (Nat → Bool) → BitVec w
  | 0, _ => 0#0
  | w + 1, f => BitVec.cons (f w) (bvOf w f)

theorem getLsbD_bvOf : ∀ (w : Nat) (f : Nat → Bool) (i : Nat),
    (bvOf w f).getLsbD i = (decide (i < w) && f i)
  | 0, f, i => by simp [bvOf]
  | w + 1, f, i => by
    rw [bvOf, BitVec.getLsbD_cons, getLsbD_bvOf w f i]
    by_cases h : i = w
    · subst h; simp
    · by_cases hi : i < w
      · simp [h, hi]; omega
      · have : ¬ i < w + 1 := by omega
        simp [h, hi, this]

theorem toBits_bvOf (w : Nat) (f : Nat → Bool) :
    toBits (bvOf w f) = (List.range w).map f := by
  simp only [toBits]
  apply List.map_congr_left
  intro i hi
  rw [getLsbD_bvOf]
  simp [List.mem_range.1 hi]

/-! ### Multiplication (shift-and-add, core Lean's `BitVec.mulRec`) -/

theorem toBits_getD {w : Nat} (x : BitVec w) (j : Nat) : (toBits x).getD j false = x.getLsbD j := by
  rw [List.getD_eq_getElem?_getD]
  by_cases h : j < w
  · simp [toBits, h]
  · simp [toBits, h, BitVec.getLsbD_of_ge x j (by omega)]

/-- The bits of one partial product `if b then x <<< s else 0`. -/
theorem toBits_pp {w : Nat} (b : Bool) (x : BitVec w) (s : Nat) :
    toBits (if b then x <<< s else 0) =
      (List.range w).map (fun i => (if s ≤ i then x.getLsbD (i - s) else false) && b) := by
  cases b
  · simp [toBits]
  · simp only [toBits, ite_true, Bool.and_true]
    apply List.map_congr_left
    intro i hi
    have hi' := List.mem_range.1 hi
    rw [BitVec.getLsbD_shiftLeft]
    by_cases hs : s ≤ i
    · simp only [decide_eq_true hi', decide_eq_false (Nat.not_lt.2 hs), hs, ite_true,
        Bool.true_and, Bool.not_false]
    · simp only [decide_eq_true hi', decide_eq_true (Nat.lt_of_not_le hs), hs, ite_false,
        Bool.true_and, Bool.not_true, Bool.false_and]

theorem mulRec_zero' {w : Nat} (x y : BitVec w) :
    BitVec.mulRec x y 0 = if y.getLsbD 0 then x <<< 0 else 0 := rfl

/-- Multiplication is the shift-and-add recurrence (core `BitVec.getLsbD_mul`). -/
theorem toBits_mul {w : Nat} (x y : BitVec w) : toBits (x * y) = toBits (BitVec.mulRec x y w) := by
  simp only [toBits]
  congr 1
  funext i
  exact BitVec.getLsbD_mul x y i


/-! ### More adder facts: carry out, subtraction, comparisons -/

theorem rippleSem_eq {w : Nat} (x y : BitVec w) (c : Bool) :
    rippleSem (toBits x) (toBits y) c =
      (List.range w).map (fun i => x.getLsbD i ^^ (y.getLsbD i ^^ BitVec.carry i x y c)) ++
        [BitVec.carry w x y c] := by
  have h := rippleSem_range x y c w 0
  rw [BitVec.carry_zero, Nat.zero_add] at h
  simpa only [toBits, List.range_eq_range'] using h

/-- The carry out of the list adder is core Lean's `BitVec.carry w`. -/
theorem rippleSem_last {w : Nat} (x y : BitVec w) (c : Bool) :
    (rippleSem (toBits x) (toBits y) c).getLastD false = BitVec.carry w x y c := by
  rw [rippleSem_eq, List.getLastD_concat]

theorem ult_bits' {w : Nat} (x y : BitVec w) :
    x.ult y = !(rippleSem (toBits x) (toBits (~~~y)) true).getLastD false := by
  rw [rippleSem_last, BitVec.ult_eq_not_carry]

theorem ule_bits {w : Nat} (x y : BitVec w) :
    x.ule y = (rippleSem (toBits y) (toBits (~~~x)) true).getLastD false := by
  rw [rippleSem_last, BitVec.ule_eq_carry]

theorem sub_eq_add_not_add_one {w : Nat} (x y : BitVec w) :
    x - y = x + ~~~y + BitVec.setWidth w (BitVec.ofBool true) := by
  rw [BitVec.sub_eq_add_neg, BitVec.neg_eq_not_add, ← BitVec.add_assoc]
  congr 1
  apply BitVec.eq_of_toNat_eq
  simp

/-- Subtraction is the adder on `x`, `~~~y` with carry-in `true`. -/
theorem toBits_sub {w : Nat} (x y : BitVec w) :
    toBits (x - y) = (rippleSem (toBits x) (toBits (~~~y)) true).dropLast := by
  rw [rippleSem_eq, List.dropLast_concat]
  simp only [toBits]
  apply List.map_congr_left
  intro i hi
  rw [sub_eq_add_not_add_one, BitVec.getLsbD_add_add_bool (List.mem_range.1 hi)]

theorem uaddOverflow_carry {w : Nat} (x y : BitVec w) :
    x.uaddOverflow y = BitVec.carry w x y false := by
  simp [BitVec.uaddOverflow, BitVec.carry, Nat.mod_eq_of_lt x.isLt, Nat.mod_eq_of_lt y.isLt]

/-- A 1-bit value is `1` exactly when its bit 0 is set. -/
theorem bv1_eq_one (x : BitVec 1) : x = 1#1 ↔ x.getLsbD 0 = true := by
  constructor
  · rintro rfl; rfl
  · intro h
    apply BitVec.eq_of_getLsbD_eq
    intro i hi
    have : i = 0 := by omega
    subst this
    simpa using h

theorem toBits_length {w : Nat} (x : BitVec w) : (toBits x).length = w := by simp [toBits]

/-- Two bit lists of the same bitvector width agree when they agree bitwise. -/
theorem toBits_eq_map {w : Nat} (x : BitVec w) (f : Nat → Bool)
    (h : ∀ i, i < w → f i = x.getLsbD i) : (List.range w).map f = toBits x := by
  simp only [toBits]
  apply List.map_congr_left
  intro i hi
  exact h i (List.mem_range.1 hi)

/-! ## Literals, gates and circuits -/

/-- A literal `(v, p)` is true under `α` when `α v = p` (the convention of
`Std.Sat.CNF`). -/
abbrev Lit := Nat × Bool

def Lit.val (α : Nat → Bool) (l : Lit) : Bool := α l.1 == l.2

def Lit.neg (l : Lit) : Lit := (l.1, !l.2)

@[simp] theorem Lit.val_neg (α : Nat → Bool) (l : Lit) : l.neg.val α = !(l.val α) := by
  obtain ⟨v, p⟩ := l
  cases h : α v <;> cases p <;> simp [Lit.val, Lit.neg, h]

/-- The literal of the constant `true` (CNF variable 1) and of `false`. -/
def TT : Lit := (1, true)
def FF : Lit := (1, false)

inductive Gate
  | and (a b : Lit)
  | or (a b : Lit)
  | xor (a b : Lit)

def Gate.eval (α : Nat → Bool) : Gate → Bool
  | .and a b => a.val α && b.val α
  | .or a b => a.val α || b.val α
  | .xor a b => a.val α ^^ b.val α

/-- Tseitin clauses defining output variable `o` as the gate. -/
def Gate.clauses (o : Nat) : Gate → List (CNF.Clause Nat)
  | .and a b => [[(o, false), a], [(o, false), b], [(o, true), a.neg, b.neg]]
  | .or a b => [[(o, true), a.neg], [(o, true), b.neg], [(o, false), a, b]]
  | .xor a b =>
    [[(o, false), a, b], [(o, false), a.neg, b.neg],
     [(o, true), a.neg, b], [(o, true), a, b.neg]]

theorem Gate.clauses_all (α : Nat → Bool) (o : Nat) (g : Gate) :
    (g.clauses o).all (CNF.Clause.eval α) = (α o == g.eval α) := by
  cases g with
  | and a b | or a b | xor a b =>
    obtain ⟨av, ap⟩ := a
    obtain ⟨bv, bp⟩ := b
    cases h0 : α o <;> cases h1 : α av <;> cases h2 : α bv <;> cases ap <;> cases bp <;>
      simp [Gate.clauses, Gate.eval, Lit.val, Lit.neg, h0, h1, h2]

def outVar (n : Nat) : Nat := 2 * n + 3

/-- A circuit: its gates, newest first (the head of `g :: gs` is gate number
`gs.length`, defining CNF variable `outVar gs.length`), and the cached gate
count. -/
structure Circuit where
  gates : List Gate
  len : Nat

def Circuit.empty : Circuit := ⟨[], 0⟩

/-- `α` respects every gate definition (and `α 1 = true`). -/
def ConsL (α : Nat → Bool) : List Gate → Prop
  | [] => α 1 = true
  | g :: gs => α (outVar gs.length) = g.eval α ∧ ConsL α gs

def Consistent (α : Nat → Bool) (c : Circuit) : Prop := ConsL α c.gates

/-- The Tseitin clauses of all gates (specification form). -/
def clausesL : List Gate → List (CNF.Clause Nat)
  | [] => [[(1, true)]]
  | g :: gs => g.clauses (outVar gs.length) ++ clausesL gs

/-- The same clauses, computed tail-recursively from the cached count (the
executable form). -/
def clausesAcc : Nat → List Gate → Array (CNF.Clause Nat) → Array (CNF.Clause Nat)
  | _, [], acc => acc.push [(1, true)]
  | n, g :: gs, acc => clausesAcc (n - 1) gs (acc ++ (g.clauses (outVar (n - 1))).toArray)

theorem clausesAcc_toList : ∀ (gs : List Gate) (acc : Array (CNF.Clause Nat)),
    (clausesAcc gs.length gs acc).toList = acc.toList ++ clausesL gs
  | [], acc => by simp [clausesAcc, clausesL]
  | g :: gs, acc => by
    simp only [clausesAcc, List.length_cons, Nat.add_sub_cancel, clausesL]
    rw [clausesAcc_toList gs]
    simp

theorem clausesL_all_iff (α : Nat → Bool) :
    ∀ gs : List Gate, (clausesL gs).all (CNF.Clause.eval α) = true ↔ ConsL α gs
  | [] => by simp [clausesL, ConsL]
  | g :: gs => by
    rw [clausesL, List.all_append, Bool.and_eq_true, Gate.clauses_all,
      clausesL_all_iff α gs, ConsL, beq_iff_eq]

theorem consL_of_suffix {α : Nat → Bool} {gs gs' : List Gate} (h : gs <:+ gs')
    (hc : ConsL α gs') : ConsL α gs := by
  obtain ⟨l, rfl⟩ := h
  induction l with
  | nil => simpa using hc
  | cons g l ih => exact ih hc.2

theorem consistent_of_suffix {α : Nat → Bool} {c c' : Circuit} (h : c.gates <:+ c'.gates)
    (hc : Consistent α c') : Consistent α c := consL_of_suffix h hc

theorem consistent_true {α : Nat → Bool} {c : Circuit} (hc : Consistent α c) : α 1 = true :=
  consL_of_suffix (gs := []) (List.nil_suffix) hc

theorem val_TT {α : Nat → Bool} {c : Circuit} (hc : Consistent α c) : TT.val α = true := by
  simp [TT, Lit.val, consistent_true hc]

theorem val_FF {α : Nat → Bool} {c : Circuit} (hc : Consistent α c) : FF.val α = false := by
  simp [FF, Lit.val, consistent_true hc]

/-! ### Well-formedness and the canonical extension of an input assignment -/

/-- A literal usable when `n` gates exist: an input bit (even variable), the
constant (variable 1), or an existing gate. -/
def Scoped (n : Nat) (l : Lit) : Prop := l.1 % 2 = 0 ∨ l.1 < 2 * n + 2

theorem Scoped.mono {n m : Nat} {l : Lit} (h : Scoped n l) (hnm : n ≤ m) : Scoped m l := by
  unfold Scoped at *; omega

@[simp] theorem scoped_neg {n : Nat} {l : Lit} : Scoped n l.neg ↔ Scoped n l := Iff.rfl

theorem scoped_TT (n : Nat) : Scoped n TT := by simp [Scoped, TT]
theorem scoped_FF (n : Nat) : Scoped n FF := by simp [Scoped, FF]

def Gate.Scoped (n : Nat) : Gate → Prop
  | .and a b | .or a b | .xor a b => PrismTechniques.Bitblast.Scoped n a ∧
      PrismTechniques.Bitblast.Scoped n b

/-- Gates only read input bits, the constant, and earlier gates. -/
def WFL : List Gate → Prop
  | [] => True
  | g :: gs => g.Scoped gs.length ∧ WFL gs

/-- A well-formed circuit: gates read only earlier gates, and the cached count
is the number of gates. -/
def Circuit.WF (c : Circuit) : Prop := c.len = c.gates.length ∧ WFL c.gates

theorem Circuit.wf_empty : Circuit.empty.WF := ⟨rfl, trivial⟩

theorem Circuit.len_le {c c' : Circuit} (h : c.gates <:+ c'.gates) (hc : c.WF) (hc' : c'.WF) :
    c.len ≤ c'.len := by
  rw [hc.1, hc'.1]; exact h.length_le

/-- Two assignments agree on everything a circuit with `n` gates can read. -/
def Agree (n : Nat) (α β : Nat → Bool) : Prop := ∀ v, (v % 2 = 0 ∨ v < 2 * n + 2) → α v = β v

theorem Gate.eval_congr {n : Nat} {α β : Nat → Bool} {g : Gate} (hs : g.Scoped n)
    (h : Agree n α β) : g.eval α = g.eval β := by
  cases g with
  | and a b | or a b | xor a b =>
    obtain ⟨ha, hb⟩ := hs
    simp [Gate.eval, Lit.val, h a.1 ha, h b.1 hb]

theorem consL_congr {α β : Nat → Bool} :
    ∀ gs : List Gate, WFL gs → Agree gs.length α β → (ConsL α gs ↔ ConsL β gs)
  | [], _, h => by simp [ConsL, h 1 (by omega)]
  | g :: gs, ⟨hg, hwf⟩, h => by
    have h' : Agree gs.length α β := fun v hv => h v (by simp; omega)
    simp only [ConsL]
    rw [h (outVar gs.length) (by simp [outVar]; omega), Gate.eval_congr hg h',
      consL_congr gs hwf h']

/-- Extend an input assignment `ρ` (input bit `j` ↦ variable `2*j`) to all
gate variables by evaluating the gates in order. -/
def extendL (ρ : Nat → Bool) : List Gate → Nat → Bool
  | [] => fun v => if v % 2 = 0 then ρ (v / 2) else v == 1
  | g :: gs => fun v => if v = outVar gs.length then g.eval (extendL ρ gs) else extendL ρ gs v

theorem extendL_input (ρ : Nat → Bool) (j : Nat) : ∀ gs : List Gate, extendL ρ gs (2 * j) = ρ j
  | [] => by simp [extendL]
  | g :: gs => by
    have : 2 * j ≠ outVar gs.length := by simp [outVar]; omega
    simp [extendL, this, extendL_input ρ j gs]

theorem extendL_agree (ρ : Nat → Bool) (g : Gate) (gs : List Gate) :
    Agree gs.length (extendL ρ (g :: gs)) (extendL ρ gs) := by
  intro v hv
  have : v ≠ outVar gs.length := by simp [outVar]; omega
  simp [extendL, this]

theorem extendL_consistent (ρ : Nat → Bool) :
    ∀ gs : List Gate, WFL gs → ConsL (extendL ρ gs) gs
  | [], _ => by simp [extendL, ConsL]
  | g :: gs, ⟨hg, hwf⟩ => by
    refine ⟨?_, ?_⟩
    · simp only [extendL, ite_true]
      exact (Gate.eval_congr hg (extendL_agree ρ g gs)).symm
    · exact (consL_congr gs hwf (extendL_agree ρ g gs)).2 (extendL_consistent ρ gs hwf)

/-! ## The builder specification -/

def mkGate (g : Gate) (c : Circuit) : Lit × Circuit :=
  ((outVar c.len, true), ⟨g :: c.gates, c.len + 1⟩)

def Good (n : Nat) (ls : List Lit) : Prop := ∀ l ∈ ls, Scoped n l

theorem Good.mono {n m : Nat} {ls : List Lit} (h : Good n ls) (hnm : n ≤ m) : Good m ls :=
  fun l hl => (h l hl).mono hnm

theorem good_nil (n : Nat) : Good n [] := fun _ h => absurd h List.not_mem_nil

theorem good_cons {n : Nat} {l : Lit} {ls : List Lit} (h1 : Scoped n l) (h2 : Good n ls) :
    Good n (l :: ls) := by
  intro x hx
  rcases List.mem_cons.1 hx with rfl | hx
  · exact h1
  · exact h2 x hx

theorem good_append {n : Nat} {as bs : List Lit} (h1 : Good n as) (h2 : Good n bs) :
    Good n (as ++ bs) := by
  intro x hx
  rcases List.mem_append.1 hx with hx | hx
  · exact h1 x hx
  · exact h2 x hx

theorem good_map_neg {n : Nat} {ls : List Lit} (h : Good n ls) : Good n (ls.map Lit.neg) := by
  intro l hl
  obtain ⟨l', hl', rfl⟩ := List.mem_map.1 hl
  exact h l' hl'

theorem good_range {n w : Nat} (f : Nat → Lit) (h : ∀ i, i < w → Scoped n (f i)) :
    Good n ((List.range w).map f) := by
  intro l hl
  obtain ⟨i, hi, rfl⟩ := List.mem_map.1 hl
  exact h i (List.mem_range.1 hi)

theorem good_dropLast {n : Nat} {ls : List Lit} (h : Good n ls) : Good n ls.dropLast :=
  fun l hl => h l (List.dropLast_subset _ hl)

theorem good_getD {n : Nat} {ls : List Lit} (h : Good n ls) (j : Nat) : Scoped n (ls.getD j FF) := by
  rw [List.getD_eq_getElem?_getD]
  cases hj : ls[j]? with
  | none => exact scoped_FF n
  | some l => exact h l (List.mem_of_getElem? hj)

theorem good_headD {n : Nat} {ls : List Lit} (h : Good n ls) : Scoped n (ls.headD FF) := by
  cases ls with
  | nil => exact scoped_FF n
  | cons l ls => exact h l (List.mem_cons_self ..)

theorem good_getLastD {n : Nat} {ls : List Lit} (h : Good n ls) : Scoped n (ls.getLastD FF) := by
  rw [List.getLastD_eq_getLast?]
  cases hl : ls.getLast? with
  | none => exact scoped_FF n
  | some l => exact h l (List.mem_of_getLast? hl)

theorem val_getD {α : Nat → Bool} {c : Circuit} (hc : Consistent α c) (ls : List Lit) (j : Nat) :
    (ls.getD j FF).val α = (ls.map (Lit.val α)).getD j false := by
  rw [List.getD_eq_getElem?_getD, List.getD_eq_getElem?_getD, List.getElem?_map]
  cases ls[j]? with
  | none => exact val_FF hc
  | some l => rfl

theorem val_headD {α : Nat → Bool} {c : Circuit} (hc : Consistent α c) (ls : List Lit) :
    (ls.headD FF).val α = (ls.map (Lit.val α)).headD false := by
  rw [← val_FF hc, List.headD_map]

theorem val_getLastD {α : Nat → Bool} {c : Circuit} (hc : Consistent α c) (ls : List Lit) :
    (ls.getLastD FF).val α = (ls.map (Lit.val α)).getLastD false := by
  rw [← val_FF hc, List.getLastD_map]

theorem map_neg_val (α : Nat → Bool) (ls : List Lit) :
    (ls.map Lit.neg).map (Lit.val α) = (ls.map (Lit.val α)).map (!·) := by
  simp [List.map_map, Function.comp_def]

/-- Builder specification: the new circuit extends the old one and is
well-formed, the output literals are in scope, and under every consistent
assignment the output literals evaluate to `sem α`. -/
structure Spec (c : Circuit) (r : List Lit × Circuit) (sem : (Nat → Bool) → List Bool) :
    Prop where
  suffix : c.gates <:+ r.2.gates
  wf : r.2.WF
  good : Good r.2.len r.1
  sem : ∀ α, Consistent α r.2 → r.1.map (Lit.val α) = sem α

theorem Spec.trans {c : Circuit} {r r' : List Lit × Circuit} {s s' : (Nat → Bool) → List Bool}
    (h : Spec c r s) (h' : Spec r.2 r' s') : Spec c r' s' :=
  ⟨h.suffix.trans h'.suffix, h'.wf, h'.good, h'.sem⟩

theorem Spec.congr {c : Circuit} {r : List Lit × Circuit} {s s' : (Nat → Bool) → List Bool}
    (h : Spec c r s) (hs : ∀ α, Consistent α r.2 → s α = s' α) : Spec c r s' :=
  ⟨h.suffix, h.wf, h.good, fun α hc => (h.sem α hc).trans (hs α hc)⟩

theorem Spec.refl {c : Circuit} (hwf : c.WF) : Spec c ([], c) (fun _ => []) :=
  ⟨List.suffix_refl _, hwf, good_nil _, fun _ _ => rfl⟩

/-- `ls` represents the bitvector `X α` in every assignment consistent with `c`. -/
structure Rep (c : Circuit) (ls : List Lit) {w : Nat} (X : (Nat → Bool) → BitVec w) : Prop where
  wf : c.WF
  good : Good c.len ls
  sem : ∀ α, Consistent α c → ls.map (Lit.val α) = toBits (X α)

theorem Rep.mono {c c' : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) (hs : c.gates <:+ c'.gates) (hwf : c'.WF) : Rep c' ls X :=
  ⟨hwf, h.good.mono (Circuit.len_le hs h.wf hwf), fun α hc => h.sem α (consistent_of_suffix hs hc)⟩

theorem Spec.rep {c : Circuit} {r : List Lit × Circuit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Spec c r (fun α => toBits (X α))) : Rep r.2 r.1 X :=
  ⟨h.wf, h.good, h.sem⟩

theorem Rep.spec {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) : Spec c (ls, c) (fun α => toBits (X α)) :=
  ⟨List.suffix_refl _, h.wf, h.good, h.sem⟩

theorem Rep.length {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) : ls.length = w := by
  have := congrArg List.length (h.sem (extendL (fun _ => false) c.gates)
    (extendL_consistent _ _ h.wf.2))
  simpa [toBits_length] using this

theorem Rep.bit {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) {α : Nat → Bool} (hc : Consistent α c) (j : Nat) :
    (ls.getD j FF).val α = (X α).getLsbD j := by
  rw [val_getD hc, h.sem α hc, toBits_getD]

theorem Rep.msb {c : Circuit} {ls : List Lit} {w : Nat} {X : (Nat → Bool) → BitVec w}
    (h : Rep c ls X) {α : Nat → Bool} (hc : Consistent α c) :
    (ls.getLastD FF).val α = (X α).msb := by
  rw [val_getLastD hc, h.sem α hc, msb_bits]

/-- A one-literal representation of a Boolean. -/
theorem rep_single {c : Circuit} {l : Lit} {B : (Nat → Bool) → Bool} (hwf : c.WF)
    (hl : Scoped c.len l) (hv : ∀ α, Consistent α c → l.val α = B α) :
    Rep c [l] (fun α => BitVec.ofBool (B α)) :=
  ⟨hwf, good_cons hl (good_nil _), fun α hc => by simp [toBits_ofBool, hv α hc]⟩

/-- Rewiring: a list of literals picked from existing ones, with no new
gates, is correct when it is correct bit by bit. -/
theorem wire_spec {c : Circuit} {w : Nat} (f : Nat → Lit) (Z : (Nat → Bool) → BitVec w)
    (hwf : c.WF) (hs : ∀ i, i < w → Scoped c.len (f i))
    (hv : ∀ α, Consistent α c → ∀ i, i < w → (f i).val α = (Z α).getLsbD i) :
    Spec c ((List.range w).map f, c) (fun α => toBits (Z α)) :=
  ⟨List.suffix_refl _, hwf, good_range f hs, fun α hc => by
    rw [List.map_map]
    exact toBits_eq_map _ _ (fun i hi => hv α hc i hi)⟩

/-! ### Gates -/

theorem mkGate_wf {g : Gate} {c : Circuit} (hwf : c.WF) (hg : g.Scoped c.len) :
    (mkGate g c).2.WF := by
  obtain ⟨h1, h2⟩ := hwf
  refine ⟨by simp [mkGate, h1], ?_, h2⟩
  rw [← h1]; exact hg

theorem mkGate_scoped (g : Gate) (c : Circuit) : Scoped (mkGate g c).2.len (mkGate g c).1 := by
  simp only [mkGate, Scoped, outVar]
  omega

theorem mkGate_val {α : Nat → Bool} {g : Gate} {c : Circuit} (hwf : c.WF)
    (hc : Consistent α (mkGate g c).2) : (mkGate g c).1.val α = g.eval α := by
  have h := hc.1
  simp only [mkGate, Lit.val, beq_true]
  rw [hwf.1]
  exact h

@[simp] theorem mkGate_len (g : Gate) (c : Circuit) : (mkGate g c).2.len = c.len + 1 := rfl

theorem mkGate_suffix (g : Gate) (c : Circuit) : c.gates <:+ (mkGate g c).2.gates :=
  List.suffix_cons g c.gates

/-- One gate as a builder step. -/
theorem mkGate_spec {g : Gate} {c : Circuit} (hwf : c.WF) (hg : g.Scoped c.len) :
    Spec c ([(mkGate g c).1], (mkGate g c).2) (fun α => [g.eval α]) :=
  ⟨mkGate_suffix g c, mkGate_wf hwf hg, good_cons (mkGate_scoped g c) (good_nil _),
    fun α hc => by simp [mkGate_val hwf hc]⟩

/-- A one-gate Boolean: the output literal represents `B` when the gate
evaluates to `B`. -/
theorem mkGate_rep {g : Gate} {c : Circuit} {B : (Nat → Bool) → Bool} (hwf : c.WF)
    (hg : g.Scoped c.len) (hv : ∀ α, Consistent α (mkGate g c).2 → g.eval α = B α) :
    Rep (mkGate g c).2 [(mkGate g c).1] (fun α => BitVec.ofBool (B α)) :=
  rep_single (mkGate_wf hwf hg) (mkGate_scoped g c)
    (fun α hc => (mkGate_val hwf hc).trans (hv α hc))

end PrismTechniques.Bitblast
