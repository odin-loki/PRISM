/-
PRISM techniques: a certified-mode bit-blaster (roadmap 5.4 and 8.2, row
"Bit-blaster"; roadmap 3.2 "Certified mode").

A small bitvector expression language (`BVExpr`) with `var`, `const`, `not`,
`and`, `or`, `xor`, `add` (ripple-carry), `ite`, and the 1-bit predicates
`eq`, `ult`, `slt`, is bit-blasted into an and/or/xor gate circuit and
Tseitin-encoded into `Std.Sat.CNF Nat` (core Lean's CNF type, the one the
verified LRAT checker `Std.Tactic.BVDecide.LRAT.check` consumes).

Proved (no `sorry`, standard axioms only):
* `toCNF_equisat`: the CNF is satisfiable iff the formula is satisfiable;
* `toCNF_unsat_imp`: CNF unsatisfiable ⇒ the formula is unsatisfiable (the
  direction certified mode needs);
* `certified_unsat`: a certificate accepted by core Lean's verified LRAT
  checker for `toCNF φ` proves `φ` unsatisfiable.

Reuse of core Lean's `bv_decide` development: the arithmetic facts come from
`Init.Data.BitVec.Bitblast` — `BitVec.carry`, `BitVec.carry_succ`,
`BitVec.getLsbD_add` (ripple-carry adder), `BitVec.ult_eq_not_carry` (unsigned
comparison as a carry chain), `BitVec.slt_eq_ult` and
`BitVec.msb_eq_getLsbD_last` (signed comparison) — and the soundness theorem
of the LRAT checker, `Std.Tactic.BVDecide.LRAT.check_sound`.  The circuit,
Tseitin encoding, and equisatisfiability proof are PRISM's own.

Variable layout: formula input bit `j` is CNF variable `2*j`; CNF variable
`1` is the constant `true`; gate `k` (0-based, in creation order) is CNF
variable `2*k+3`.  A `BVExpr.var base : BVExpr w` names the input bits
`base, …, base+w-1`; the front end gives distinct program variables disjoint
bit ranges.
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

/-! ## Bitvector expressions -/

/-- PRISM's bit-blastable bitvector fragment. -/
inductive BVExpr : Nat → Type
  | var {w : Nat} (base : Nat) : BVExpr w
  | const {w : Nat} (v : BitVec w) : BVExpr w
  | not {w : Nat} (e : BVExpr w) : BVExpr w
  | and {w : Nat} (a b : BVExpr w) : BVExpr w
  | or {w : Nat} (a b : BVExpr w) : BVExpr w
  | xor {w : Nat} (a b : BVExpr w) : BVExpr w
  | add {w : Nat} (a b : BVExpr w) : BVExpr w
  | ite {w : Nat} (c : BVExpr 1) (a b : BVExpr w) : BVExpr w
  | eq {w : Nat} (a b : BVExpr w) : BVExpr 1
  | ult {w : Nat} (a b : BVExpr w) : BVExpr 1
  | slt {w : Nat} (a b : BVExpr w) : BVExpr 1

/-- Semantics under an assignment `ρ` of the input bits. -/
def BVExpr.denote (ρ : Nat → Bool) : {w : Nat} → BVExpr w → BitVec w
  | w, .var base => bvOf w (fun i => ρ (base + i))
  | _, .const v => v
  | _, .not e => ~~~(e.denote ρ)
  | _, .and a b => a.denote ρ &&& b.denote ρ
  | _, .or a b => a.denote ρ ||| b.denote ρ
  | _, .xor a b => a.denote ρ ^^^ b.denote ρ
  | _, .add a b => a.denote ρ + b.denote ρ
  | _, .ite c a b => if (c.denote ρ).getLsbD 0 then a.denote ρ else b.denote ρ
  | _, .eq a b => BitVec.ofBool (a.denote ρ == b.denote ρ)
  | _, .ult a b => BitVec.ofBool ((a.denote ρ).ult (b.denote ρ))
  | _, .slt a b => BitVec.ofBool ((a.denote ρ).slt (b.denote ρ))

/-- A 1-bit formula is satisfiable when some input makes it `1`. -/
def FSat (φ : BVExpr 1) : Prop := ∃ ρ, φ.denote ρ = 1#1

theorem bv1_eq_one (x : BitVec 1) : x = 1#1 ↔ x.getLsbD 0 = true := by
  constructor
  · rintro rfl; rfl
  · intro h
    apply BitVec.eq_of_getLsbD_eq
    intro i hi
    have : i = 0 := by omega
    subst this
    simpa using h

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

/-- A circuit: gates, newest first.  The head of `g :: gs` is gate number
`gs.length`, defining CNF variable `outVar gs.length`. -/
abbrev Circuit := List Gate

def outVar (n : Nat) : Nat := 2 * n + 3

/-- `α` respects every gate definition (and `α 1 = true`). -/
def Consistent (α : Nat → Bool) : Circuit → Prop
  | [] => α 1 = true
  | g :: gs => α (outVar gs.length) = g.eval α ∧ Consistent α gs

def Circuit.clauses : Circuit → List (CNF.Clause Nat)
  | [] => [[(1, true)]]
  | g :: gs => g.clauses (outVar gs.length) ++ Circuit.clauses gs

theorem clauses_all_iff (α : Nat → Bool) :
    ∀ gs : Circuit, (Circuit.clauses gs).all (CNF.Clause.eval α) = true ↔ Consistent α gs
  | [] => by simp [Circuit.clauses, Consistent]
  | g :: gs => by
    rw [Circuit.clauses, List.all_append, Bool.and_eq_true, Gate.clauses_all,
      clauses_all_iff α gs, Consistent, beq_iff_eq]

theorem consistent_of_suffix {α : Nat → Bool} {gs gs' : Circuit} (h : gs <:+ gs')
    (hc : Consistent α gs') : Consistent α gs := by
  obtain ⟨l, rfl⟩ := h
  induction l with
  | nil => simpa using hc
  | cons g l ih => exact ih hc.2

theorem consistent_true {α : Nat → Bool} {gs : Circuit} (hc : Consistent α gs) : α 1 = true :=
  consistent_of_suffix (gs := []) (List.nil_suffix) hc

theorem val_TT {α : Nat → Bool} {gs : Circuit} (hc : Consistent α gs) : TT.val α = true := by
  simp [TT, Lit.val, consistent_true hc]

theorem val_FF {α : Nat → Bool} {gs : Circuit} (hc : Consistent α gs) : FF.val α = false := by
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
def WF : Circuit → Prop
  | [] => True
  | g :: gs => g.Scoped gs.length ∧ WF gs

/-- Two assignments agree on everything a circuit with `n` gates can read. -/
def Agree (n : Nat) (α β : Nat → Bool) : Prop := ∀ v, (v % 2 = 0 ∨ v < 2 * n + 2) → α v = β v

theorem Gate.eval_congr {n : Nat} {α β : Nat → Bool} {g : Gate} (hs : g.Scoped n)
    (h : Agree n α β) : g.eval α = g.eval β := by
  cases g with
  | and a b | or a b | xor a b =>
    obtain ⟨ha, hb⟩ := hs
    simp [Gate.eval, Lit.val, h a.1 ha, h b.1 hb]

theorem consistent_congr {α β : Nat → Bool} :
    ∀ gs : Circuit, WF gs → Agree gs.length α β → (Consistent α gs ↔ Consistent β gs)
  | [], _, h => by simp [Consistent, h 1 (by omega)]
  | g :: gs, ⟨hg, hwf⟩, h => by
    have h' : Agree gs.length α β := fun v hv => h v (by simp; omega)
    simp only [Consistent]
    rw [h (outVar gs.length) (by simp [outVar]; omega), Gate.eval_congr hg h',
      consistent_congr gs hwf h']

/-- Extend an input assignment `ρ` (input bit `j` ↦ variable `2*j`) to all
gate variables by evaluating the gates in order. -/
def extend (ρ : Nat → Bool) : Circuit → Nat → Bool
  | [] => fun v => if v % 2 = 0 then ρ (v / 2) else v == 1
  | g :: gs => fun v => if v = outVar gs.length then g.eval (extend ρ gs) else extend ρ gs v

theorem extend_input (ρ : Nat → Bool) (j : Nat) : ∀ gs : Circuit, extend ρ gs (2 * j) = ρ j
  | [] => by simp [extend]
  | g :: gs => by
    have : 2 * j ≠ outVar gs.length := by simp [outVar]; omega
    simp [extend, this, extend_input ρ j gs]

theorem extend_agree (ρ : Nat → Bool) (g : Gate) (gs : Circuit) :
    Agree gs.length (extend ρ (g :: gs)) (extend ρ gs) := by
  intro v hv
  have : v ≠ outVar gs.length := by simp [outVar]; omega
  simp [extend, this]

theorem extend_consistent (ρ : Nat → Bool) : ∀ gs : Circuit, WF gs → Consistent (extend ρ gs) gs
  | [], _ => by simp [extend, Consistent]
  | g :: gs, ⟨hg, hwf⟩ => by
    refine ⟨?_, ?_⟩
    · simp only [extend, ite_true]
      exact (Gate.eval_congr hg (extend_agree ρ g gs)).symm
    · exact (consistent_congr gs hwf (extend_agree ρ g gs)).2 (extend_consistent ρ gs hwf)

/-! ## The encoder -/

def mkGate (g : Gate) (gs : Circuit) : Lit × Circuit := ((outVar gs.length, true), g :: gs)

/-- Encoder specification: the new circuit extends the old one, stays
well-formed, the output literals are in scope, and under every consistent
assignment the output literals evaluate to `sem α`. -/
structure Spec (gs : Circuit) (r : List Lit × Circuit) (sem : (Nat → Bool) → List Bool) :
    Prop where
  suffix : gs <:+ r.2
  wf : WF r.2
  good : ∀ l ∈ r.1, Scoped r.2.length l
  sem : ∀ α, Consistent α r.2 → r.1.map (Lit.val α) = sem α

def Good (n : Nat) (ls : List Lit) : Prop := ∀ l ∈ ls, Scoped n l

theorem Good.mono {n m : Nat} {ls : List Lit} (h : Good n ls) (hnm : n ≤ m) : Good m ls :=
  fun l hl => (h l hl).mono hnm

theorem mkGate_wf {g : Gate} {gs : Circuit} (hwf : WF gs) (hg : g.Scoped gs.length) :
    WF (mkGate g gs).2 := ⟨hg, hwf⟩

theorem mkGate_scoped (g : Gate) (gs : Circuit) : Scoped (mkGate g gs).2.length (mkGate g gs).1 := by
  simp only [mkGate, Scoped, outVar, List.length_cons]
  omega

theorem mkGate_val {α : Nat → Bool} {g : Gate} {gs : Circuit}
    (hc : Consistent α (mkGate g gs).2) : (mkGate g gs).1.val α = g.eval α := by
  simp [mkGate, Lit.val, hc.1]

@[simp] theorem mkGate_len (g : Gate) (gs : Circuit) : (mkGate g gs).2.length = gs.length + 1 := rfl

theorem mkGate_suffix (g : Gate) (gs : Circuit) : gs <:+ (mkGate g gs).2 := List.suffix_cons g gs

/-- Apply a two-input gate bitwise. -/
def zipGate (mk : Lit → Lit → Gate) : List Lit → List Lit → Circuit → List Lit × Circuit
  | a :: as, b :: bs, gs =>
    let r := mkGate (mk a b) gs
    let rs := zipGate mk as bs r.2
    (r.1 :: rs.1, rs.2)
  | _, _, gs => ([], gs)

theorem zipGate_spec (mk : Lit → Lit → Gate) (f : Bool → Bool → Bool)
    (hmk : ∀ α a b, (mk a b).eval α = f (a.val α) (b.val α))
    (hsc : ∀ n a b, Scoped n a → Scoped n b → (mk a b).Scoped n) :
    ∀ (as bs : List Lit) (gs : Circuit), WF gs → Good gs.length as → Good gs.length bs →
      Spec gs (zipGate mk as bs gs)
        (fun α => List.zipWith f (as.map (Lit.val α)) (bs.map (Lit.val α)))
  | a :: as, b :: bs, gs, hwf, ha, hb => by
    let r := mkGate (mk a b) gs
    have hwf1 : WF r.2 := mkGate_wf hwf
      (hsc _ a b (ha a (List.mem_cons_self ..)) (hb b (List.mem_cons_self ..)))
    have ih := zipGate_spec mk f hmk hsc as bs r.2 hwf1
      ((fun l hl => ha l (List.mem_cons_of_mem _ hl)) |> fun h => Good.mono h (by simp [r]))
      ((fun l hl => hb l (List.mem_cons_of_mem _ hl)) |> fun h => Good.mono h (by simp [r]))
    refine ⟨(mkGate_suffix _ gs).trans ih.suffix, ih.wf, ?_, ?_⟩
    · intro l hl
      simp only [zipGate, List.mem_cons] at hl
      rcases hl with rfl | hl
      · exact (mkGate_scoped _ gs).mono ih.suffix.length_le
      · exact ih.good l hl
    · intro α hc
      simp only [zipGate, List.map_cons, List.zipWith_cons_cons]
      rw [ih.sem α hc, mkGate_val (consistent_of_suffix ih.suffix hc), hmk]
  | [], _, gs, hwf, _, _ => ⟨List.suffix_refl gs, hwf, by simp [zipGate], by simp [zipGate]⟩
  | _ :: _, [], gs, hwf, _, _ => ⟨List.suffix_refl gs, hwf, by simp [zipGate], by simp [zipGate]⟩

end PrismTechniques.Bitblast
