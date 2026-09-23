/-
PRISM refinement — correctly rounded subtraction, multiplication and
division, IEEE special values and exception flags, and the floating-point
checks PRISM encodes (roadmap 8.2 "Floating point", milestone M8).

Builds on `Float.lean` (formats, `decode`/`encode`, correct rounding of an
integer magnitude, correctly rounded addition).  Everything is for an
arbitrary binary format `Fmt` (precision `p`, exponent width `ew`), round to
nearest, ties to even.

Units.  A finite datum's magnitude is a natural number `n` in units of the
smallest subnormal `2^-D` (`Fmt.D`: `1.0` is `2^D` units).  The exact product
of two such magnitudes is `nx·ny / 2^D` units and the exact quotient
`nx·2^D / ny` units: rationals.  `roundQ p a b` rounds `a / b` to a `p`-bit
significand (unbounded exponent above, grid of one unit below), and
`roundF f a b` adds the format's range (`none` = overflow).  Nearness is
proved on distances scaled by `b` — `dist (r·b) a = b·|r - a/b|` — which is
the real-number distance times a positive constant.

Proved:
* `roundQ_repr`, `roundQ_nearest`, `roundQ_tie_even`, `roundF_correct`:
  correct rounding of a rational, ties to even, overflow only above the
  largest finite magnitude (IEEE 754 §4.3.1, §7.4);
* `sub_correct`, `mul_correct`, `div_correct`: bit-level `sub`, `mul`, `div`
  of finite data decode to the correctly rounded exact result (IEEE's sign
  rules, signed zeros, overflow to infinity, underflow to a signed zero);
  `add`/`sub`/`mul`/`div` also define every special case (NaN operands,
  `∞ - ∞`, `0 × ∞`, `0 / 0`, `∞ / ∞`, `x / 0`, `x / ∞`) as IEEE 754 §6, §7;
* exception flags (IEEE 754 §7.2–7.4), against the conditions
  `src/prism/pir/translate_fp.cpp` (`FpTr::checks`) builds, mirrored
  operator by operator in `prismInvalid`, `prismDivZero`, `prismOverflow`:
  - `prism_overflow_eq`: FLOAT-OVERFLOW is exactly IEEE overflow, for
    `fadd`, `fsub`, `fmul`, `fdiv`, every pair of operands;
  - `prism_invalid_eq`: FLOAT-INVALID is exactly IEEE invalid operation for
    quiet operands; `ieee_invalid_eq`: IEEE additionally signals invalid for
    a signalling-NaN operand, which PRISM does not flag (Z3's FP theory has
    one NaN and LLVM's default environment does not preserve signalling
    NaNs);
  - `prism_divzero_eq`: FLOAT-DIV-ZERO is IEEE divide-by-zero **or** `0 / 0`
    (IEEE: invalid, also reported as FLOAT-INVALID) **or** `±∞ / 0` (IEEE: no
    exception, the result is an exact infinity); `ieee_divzero_imp_prism`:
    it misses no IEEE divide-by-zero;
* `cast_ovf_iff`: the FLOAT-CAST-OVF condition the encoder builds for
  `fptosi`/`fptoui` (`src/prism/pir/encode.cpp`, `Op::FToSIOvf` /
  `Op::FToUIOvf`: NaN, infinity, or the value truncated toward zero below
  `lo` or at least `hi`) holds exactly when the value is out of the range of
  the `k`-bit integer type (C11 6.3.1.4), for every format and every `k ≥ 1`,
  including bounds that are not representable (`2^16` in binary16 rounds to
  infinity).

Trusted, not proved here: that Z3's floating-point theory implements IEEE
754 (`Z3_mk_fpa_*`), and that `Z3_mk_fpa_numeral_double` rounds a numeral to
nearest, ties to even (it does; `pow2Val` models it by `round`).  Not
covered: other rounding modes, `frem`/`sqrt`/`fma`, NaN payloads, and the
underflow/inexact flags (PRISM does not check them).
-/
import PrismRefine.Float

namespace PrismRefine.Float

open PrismTechniques.FloatRound

/-! ### Rounding a rational `a / b` -/

/-- Round `a / b` to nearest, ties to even, with a `p`-bit significand:
grid `1` below `2^p`, else grid `2^(⌊log₂(a/b)⌋ + 1 - p)`. -/
def roundQ (p a b : Nat) : Nat :=
  if a / b < 2 ^ p then rne a b
  else rne a (b * 2 ^ ((a / b).log2 + 1 - p)) * 2 ^ ((a / b).log2 + 1 - p)

/-- With `b = 1` this is `roundU` (the rounding of `Float.lean`). -/
theorem roundQ_one (p N : Nat) : roundQ p N 1 = roundU p N := by
  simp [roundQ, roundU, rne, Nat.mod_one]

theorem roundQ_repr (p a b : Nat) (hp : 1 ≤ p) : ReprU p (roundQ p a b) := by
  unfold roundQ
  split
  · rename_i hlt
    have := rne_le a b
    by_cases he : rne a b < 2 ^ p
    · exact ⟨_, 0, he, by simp⟩
    · have e : rne a b = 2 ^ p := by omega
      refine ⟨1, p, by have := Nat.one_lt_two_pow_iff.mpr (show p ≠ 0 by omega); omega, ?_⟩
      rw [e]; simp
  · rename_i hN
    generalize hNd : a / b = N at hN ⊢
    have hN0 : N ≠ 0 := by intro h; subst h; exact hN (Nat.two_pow_pos p)
    obtain ⟨_, hhi⟩ := log2_bounds hN0
    have hL : p ≤ N.log2 := (Nat.le_log2 hN0).mpr (by omega)
    generalize hs : N.log2 + 1 - p = s
    have hsplit : 2 ^ (N.log2 + 1) = 2 ^ p * 2 ^ s := by
      rw [← Nat.pow_add]; congr 1; omega
    have hq : a / (b * 2 ^ s) < 2 ^ p := by
      rw [← Nat.div_div_eq_div_mul, hNd, Nat.div_lt_iff_lt_mul (Nat.two_pow_pos s)]; omega
    have hr := rne_le a (b * 2 ^ s)
    by_cases he : rne a (b * 2 ^ s) < 2 ^ p
    · exact ⟨_, s, he, rfl⟩
    · have e : rne a (b * 2 ^ s) = 2 ^ p := by omega
      refine ⟨1, p + s, by have := Nat.one_lt_two_pow_iff.mpr (show p ≠ 0 by omega); omega, ?_⟩
      rw [e, Nat.pow_add]; simp

/-- **Correct rounding of a rational**: no `p`-bit value is nearer to
`a / b` (distances scaled by `b`). -/
theorem roundQ_nearest (p a b : Nat) (hp : 1 ≤ p) (hb : 0 < b) (g : Nat) (hg : ReprU p g) :
    dist (roundQ p a b * b) a ≤ dist (g * b) a := by
  unfold roundQ
  split
  · exact rne_nearest a b hb g
  · rename_i hN
    have hab := Nat.div_mul_le_self a b
    generalize hNd : a / b = N at hN hab
    have hN0 : N ≠ 0 := by intro h; subst h; exact hN (Nat.two_pow_pos p)
    obtain ⟨hlo, hhi⟩ := log2_bounds hN0
    have hL : p ≤ N.log2 := (Nat.le_log2 hN0).mpr (by omega)
    generalize hL' : N.log2 + 1 = L at hhi
    have hlo' : 2 ^ (L - 1) ≤ N := by rw [← hL']; simpa using hlo
    have hP : 0 < b * 2 ^ (L - p) := Nat.mul_pos hb (Nat.two_pow_pos _)
    have hre : rne a (b * 2 ^ (L - p)) * 2 ^ (L - p) * b = rne a (b * 2 ^ (L - p)) * (b * 2 ^ (L - p)) := by
      rw [Nat.mul_assoc, Nat.mul_comm (2 ^ (L - p)) b]
    rw [hre]
    by_cases hgl : g < 2 ^ (L - 1)
    · have hm := rne_nearest a (b * 2 ^ (L - p)) hP (2 ^ (L - 1 - (L - p)))
      have he : 2 ^ (L - 1 - (L - p)) * (b * 2 ^ (L - p)) = 2 ^ (L - 1) * b := by
        rw [Nat.mul_comm b, ← Nat.mul_assoc, ← Nat.pow_add]; congr 2; omega
      rw [he] at hm
      refine Nat.le_trans hm ?_
      have h1 : g * b ≤ 2 ^ (L - 1) * b := Nat.mul_le_mul_right b (Nat.le_of_lt hgl)
      have h2 : 2 ^ (L - 1) * b ≤ N * b := Nat.mul_le_mul_right b hlo'
      simp only [dist]; omega
    · obtain ⟨k, hk⟩ := repr_dvd hg (L := L) (by omega) (by omega)
      have hm := rne_nearest a (b * 2 ^ (L - p)) hP k
      rw [hk, Nat.mul_comm (2 ^ (L - p)) k, Nat.mul_assoc, Nat.mul_comm (2 ^ (L - p)) b]
      exact hm

/-- The grid exponent `roundQ` rounds at. -/
def gridExp (p a b : Nat) : Nat := if a / b < 2 ^ p then 0 else (a / b).log2 + 1 - p

theorem roundQ_eq (p a b : Nat) :
    roundQ p a b = rne a (b * 2 ^ gridExp p a b) * 2 ^ gridExp p a b := by
  unfold roundQ gridExp; split <;> simp

/-- **Ties to even**: in an exact tie the chosen significand is even. -/
theorem roundQ_tie_even (p a b : Nat)
    (htie : 2 * (a % (b * 2 ^ gridExp p a b)) = b * 2 ^ gridExp p a b) :
    (roundQ p a b / 2 ^ gridExp p a b) % 2 = 0 := by
  rw [roundQ_eq, Nat.mul_div_cancel _ (Nat.two_pow_pos _)]
  exact rne_tie_even a _ htie

/-- Rounding `a / b` into the format; `none` is overflow to infinity. -/
def roundF (f : Fmt) (a b : Nat) : Option Nat :=
  if 0 < b ∧ roundQ f.p a b ≤ f.maxN then some (roundQ f.p a b) else none

theorem roundF_correct (f : Fmt) (a b : Nat) (hb : 0 < b) :
    (∀ r, roundF f a b = some r →
      Repr f r ∧ r ≤ f.maxN ∧ ∀ g, Repr f g → dist (r * b) a ≤ dist (g * b) a) ∧
    (roundF f a b = none → f.maxN * b < a) := by
  have hp : 1 ≤ f.p := by have := f.hp; omega
  constructor
  · intro r hr
    unfold roundF at hr
    split at hr
    · rename_i hle
      simp at hr; subst hr
      exact ⟨repr_of_reprU f (roundQ_repr f.p a b hp) hle.2, hle.2,
        fun g hg => roundQ_nearest f.p a b hp hb g (reprU_of_repr f hg)⟩
    · simp at hr
  · intro hr
    unfold roundF at hr
    split at hr
    · simp at hr
    · rename_i hgt
      have hgt' : f.maxN < roundQ f.p a b := by omega
      have hn := roundQ_nearest f.p a b hp hb f.maxN (maxN_reprU f)
      have := Nat.mul_lt_mul_of_pos_right hgt' hb
      simp only [dist] at hn
      generalize roundQ f.p a b * b = A at *
      omega

theorem roundF_some (f : Fmt) {a b r : Nat} (h : roundF f a b = some r) : Repr f r ∧ r ≤ f.maxN := by
  have hb : 0 < b := by
    unfold roundF at h; split at h
    · rename_i hc; exact hc.1
    · simp at h
  obtain ⟨h1, h2, _⟩ := (roundF_correct f a b hb).1 r h
  exact ⟨h1, h2⟩

/-! ### Negation, subtraction, multiplication, division -/

/-- `1.0` is `2^D` units of the smallest subnormal:
`D = p - 1 - emin = p + 2^(ew-1) - 3` (binary32: 149, binary64: 1074). -/
def Fmt.D (f : Fmt) : Nat := f.p + 2 ^ (f.ew - 1) - 3

example : binary32.D = 149 := by decide
example : binary64.D = 1074 := by decide

def neg {f : Fmt} (x : FP f) : FP f := ⟨!x.sign, x.exp, x.frac⟩

def Val.neg : Val → Val
  | .fin s n => .fin (!s) n
  | .inf s => .inf (!s)
  | .nan => .nan

theorem decode_neg {f : Fmt} (x : FP f) : decode (neg x) = (decode x).neg := by
  unfold decode neg
  simp only
  split
  · split <;> rfl
  · split <;> rfl

/-- `x - y` is `x + (-y)` (IEEE 754 §5.4.1). -/
def sub (f : Fmt) (x y : FP f) : FP f := add f x (neg y)

/-- Multiplication (IEEE 754 §5.4.1, §7.2): the sign is the exclusive or of
the signs, `0 × ∞` is invalid. -/
def mul (f : Fmt) (x y : FP f) : FP f :=
  match decode x, decode y with
  | .nan, _ => nanD f
  | _, .nan => nanD f
  | .inf sx, .inf sy => infD f (sx ^^ sy)
  | .inf sx, .fin sy ny => if ny = 0 then nanD f else infD f (sx ^^ sy)
  | .fin sx nx, .inf sy => if nx = 0 then nanD f else infD f (sx ^^ sy)
  | .fin sx nx, .fin sy ny => fromRound f (sx ^^ sy) (roundF f (nx * ny) (2 ^ f.D))

/-- Division (IEEE 754 §5.4.1, §7.2, §7.3): `0 / 0` and `∞ / ∞` are
invalid, a finite non-zero `x / 0` is an exact infinity (divide by zero),
`∞ / y` is infinite, `x / ∞` is zero. -/
def div (f : Fmt) (x y : FP f) : FP f :=
  match decode x, decode y with
  | .nan, _ => nanD f
  | _, .nan => nanD f
  | .inf _, .inf _ => nanD f
  | .inf sx, .fin sy _ => infD f (sx ^^ sy)
  | .fin sx _, .inf sy => zeroD f (sx ^^ sy)
  | .fin sx nx, .fin sy ny =>
    if ny = 0 then (if nx = 0 then nanD f else infD f (sx ^^ sy))
    else fromRound f (sx ^^ sy) (roundF f (nx * 2 ^ f.D) ny)

theorem sval_not (s : Bool) (n : Nat) : sval (!s) n = -sval s n := by
  cases s <;> simp [sval]

/-- **Correctly rounded subtraction** (from `add_correct`): exact difference
`S`, `+0` for an exact zero unless both are `-0 - (+0)`-like
(`sx && !sy`), the nearest finite datum, or infinity on overflow. -/
theorem sub_correct (f : Fmt) (x y : FP f) {sx sy : Bool} {nx ny : Nat}
    (hx : decode x = .fin sx nx) (hy : decode y = .fin sy ny) :
    (sval sx nx - sval sy ny = 0 → decode (sub f x y) = .fin (sx && !sy) 0) ∧
    (∀ r, sval sx nx - sval sy ny ≠ 0 → round f (sval sx nx - sval sy ny).natAbs = some r →
      decode (sub f x y) = .fin (decide (sval sx nx - sval sy ny < 0)) r ∧ Repr f r ∧
      ∀ g, Repr f g → dist r (sval sx nx - sval sy ny).natAbs ≤ dist g (sval sx nx - sval sy ny).natAbs) ∧
    (sval sx nx - sval sy ny ≠ 0 → round f (sval sx nx - sval sy ny).natAbs = none →
      decode (sub f x y) = .inf (decide (sval sx nx - sval sy ny < 0)) ∧
        f.maxN < (sval sx nx - sval sy ny).natAbs) := by
  have hny : decode (neg y) = .fin (!sy) ny := by rw [decode_neg, hy]; rfl
  have h := add_correct f x (neg y) hx hny
  simp only [sval_not, ← Int.sub_eq_add_neg] at h
  exact h

/-- **Correctly rounded multiplication** of finite data: the exact product
is `nx·ny / 2^D` units; the result is the nearest finite datum with sign
`sx xor sy` (a signed zero when the product rounds to zero), or infinity on
overflow, which requires the exact product to exceed the largest finite
magnitude. -/
theorem mul_correct (f : Fmt) (x y : FP f) {sx sy : Bool} {nx ny : Nat}
    (hx : decode x = .fin sx nx) (hy : decode y = .fin sy ny) :
    (∀ r, roundF f (nx * ny) (2 ^ f.D) = some r →
      decode (mul f x y) = .fin (sx ^^ sy) r ∧ Repr f r ∧
      ∀ g, Repr f g → dist (r * 2 ^ f.D) (nx * ny) ≤ dist (g * 2 ^ f.D) (nx * ny)) ∧
    (roundF f (nx * ny) (2 ^ f.D) = none →
      decode (mul f x y) = .inf (sx ^^ sy) ∧ f.maxN * 2 ^ f.D < nx * ny) := by
  have hc := roundF_correct f (nx * ny) (2 ^ f.D) (Nat.two_pow_pos _)
  constructor
  · intro r hr
    obtain ⟨h1, h2, h3⟩ := hc.1 r hr
    refine ⟨?_, h1, h3⟩
    simp only [mul, hx, hy, hr]
    exact decode_fromRound_some f _ ⟨h1, h2⟩
  · intro hr
    refine ⟨?_, hc.2 hr⟩
    simp only [mul, hx, hy, hr]
    exact decode_fromRound_none f _

/-- **Correctly rounded division** of finite data by a non-zero finite
divisor: the exact quotient is `nx·2^D / ny` units. -/
theorem div_correct (f : Fmt) (x y : FP f) {sx sy : Bool} {nx ny : Nat}
    (hx : decode x = .fin sx nx) (hy : decode y = .fin sy ny) (hny : ny ≠ 0) :
    (∀ r, roundF f (nx * 2 ^ f.D) ny = some r →
      decode (div f x y) = .fin (sx ^^ sy) r ∧ Repr f r ∧
      ∀ g, Repr f g → dist (r * ny) (nx * 2 ^ f.D) ≤ dist (g * ny) (nx * 2 ^ f.D)) ∧
    (roundF f (nx * 2 ^ f.D) ny = none →
      decode (div f x y) = .inf (sx ^^ sy) ∧ f.maxN * ny < nx * 2 ^ f.D) := by
  have hc := roundF_correct f (nx * 2 ^ f.D) ny (Nat.pos_of_ne_zero hny)
  constructor
  · intro r hr
    obtain ⟨h1, h2, h3⟩ := hc.1 r hr
    refine ⟨?_, h1, h3⟩
    simp only [div, hx, hy, hr, hny, ite_false]
    exact decode_fromRound_some f _ ⟨h1, h2⟩
  · intro hr
    refine ⟨?_, hc.2 hr⟩
    simp only [div, hx, hy, hr, hny, ite_false]
    exact decode_fromRound_none f _

/-! ### Exception flags: IEEE 754 §7 and the conditions PRISM encodes -/

def Val.isNaN : Val → Bool
  | .nan => true
  | _ => false

def Val.isInf : Val → Bool
  | .inf _ => true
  | _ => false

def Val.isZero : Val → Bool
  | .fin _ n => n == 0
  | _ => false

@[simp] theorem isNaN_fin (s : Bool) (n : Nat) : (Val.fin s n).isNaN = false := rfl
@[simp] theorem isNaN_inf (s : Bool) : (Val.inf s).isNaN = false := rfl
@[simp] theorem isNaN_nan : Val.nan.isNaN = true := rfl
@[simp] theorem isInf_fin (s : Bool) (n : Nat) : (Val.fin s n).isInf = false := rfl
@[simp] theorem isInf_inf (s : Bool) : (Val.inf s).isInf = true := rfl
@[simp] theorem isInf_nan : Val.nan.isInf = false := rfl
@[simp] theorem isZero_fin (s : Bool) (n : Nat) : (Val.fin s n).isZero = (n == 0) := rfl
@[simp] theorem isZero_inf (s : Bool) : (Val.inf s).isZero = false := rfl
@[simp] theorem isZero_nan : Val.nan.isZero = false := rfl

inductive BinOp where
  | add | sub | mul | div
  deriving DecidableEq

/-- LLVM `fadd`/`fsub`/`fmul`/`fdiv` (`Op::FAdd` … in PIR; Z3 `fpa_add` … with RNE). -/
def eval (f : Fmt) : BinOp → FP f → FP f → FP f
  | .add => add f
  | .sub => sub f
  | .mul => mul f
  | .div => div f

/-! #### PRISM's conditions (`FpTr::checks`, `src/prism/pir/translate_fp.cpp`)

Mirrored operator by operator; `tests/test_proofs_float_conc.py` locks each C++
expression to its definition below.  `Xor(e, 1)` is `e ^^ true`.

    Arg n = p(b, Op::FIsNaN, {a});
    Arg fin = p(b, Op::Xor, {p(b, Op::Or, {n, p(b, Op::FIsInf, {a})}), Arg::c(1, 1)});
    divz = p(b, Op::FIsZero, {args[1]});
    FLOAT-DIV-ZERO:  p(b, Op::And, {*divz, p(b, Op::Xor, {p(b, Op::FIsNaN, {args[0]}), Arg::c(1, 1)})})
    FLOAT-INVALID:   p(b, Op::And, {p(b, Op::FIsNaN, {r}), p(b, Op::Xor, {*any_nan, Arg::c(1, 1)})})
    FLOAT-OVERFLOW:  Arg ovf = p(b, Op::And, {p(b, Op::FIsInf, {r}), *all_finite});
                     if (divz) ovf = p(b, Op::And, {ovf, p(b, Op::Xor, {*divz, Arg::c(1, 1)})});
-/

/-- `fin` for one operand: `Xor(Or(FIsNaN a, FIsInf a), 1)`. -/
def pFin (a : Val) : Bool := (a.isNaN || a.isInf) ^^ true

/-- FLOAT-DIV-ZERO (`fdiv` only): `And(FIsZero(y), Xor(FIsNaN(x), 1))`. -/
def prismDivZero (x y : Val) : Bool := y.isZero && (x.isNaN ^^ true)

/-- FLOAT-INVALID: `And(FIsNaN(r), Xor(Or(FIsNaN x, FIsNaN y), 1))`. -/
def prismInvalid (x y r : Val) : Bool := r.isNaN && ((x.isNaN || y.isNaN) ^^ true)

/-- FLOAT-OVERFLOW: `And(FIsInf(r), And(fin x, fin y))`, and for `fdiv`
also `Xor(divz, 1)`. -/
def prismOverflow (op : BinOp) (x y r : Val) : Bool :=
  if op = .div then (r.isInf && (pFin x && pFin y)) && (y.isZero ^^ true)
  else r.isInf && (pFin x && pFin y)

/-! #### IEEE 754-2019 §7 (written from the standard, not from the code) -/

/-- §7.2 invalid operation, quiet operands: (d) `∞ - ∞` magnitude
subtraction of infinities, (c) `0 × ∞`, (e) `0 / 0` and `∞ / ∞`. -/
def ieeeInvalidQ : BinOp → Val → Val → Bool
  | .add, .inf a, .inf b => a != b
  | .sub, .inf a, .inf b => a == b
  | .mul, .inf _, .fin _ n => n == 0
  | .mul, .fin _ n, .inf _ => n == 0
  | .div, .fin _ n, .fin _ m => n == 0 && m == 0
  | .div, .inf _, .inf _ => true
  | _, _, _ => false

/-- A signalling NaN: exponent all ones, fraction non-zero with its leading
bit clear (IEEE 754 §6.2.1). -/
def isSNaN {f : Fmt} (x : FP f) : Bool :=
  x.exp == 2 ^ f.ew - 1 && x.frac != 0 && decide (x.frac < 2 ^ (f.p - 2))

/-- §7.2 in full: (a) any signalling-NaN operand, or the quiet cases. -/
def ieeeInvalid {f : Fmt} (op : BinOp) (x y : FP f) : Bool :=
  ieeeInvalidQ op (decode x) (decode y) || isSNaN x || isSNaN y

/-- §7.3 division by zero: a finite non-zero dividend and a zero divisor. -/
def ieeeDivByZero : Val → Val → Bool
  | .fin _ n, .fin _ m => n != 0 && m == 0
  | _, _ => false

/-- §7.4 overflow: finite operands whose exact result, rounded with an
unbounded exponent range, exceeds the largest finite magnitude. -/
def ieeeOverflow (f : Fmt) : BinOp → Val → Val → Bool
  | .add, .fin sx nx, .fin sy ny => (round f (sval sx nx + sval sy ny).natAbs).isNone
  | .sub, .fin sx nx, .fin sy ny => (round f (sval sx nx - sval sy ny).natAbs).isNone
  | .mul, .fin _ nx, .fin _ ny => (roundF f (nx * ny) (2 ^ f.D)).isNone
  | .div, .fin _ nx, .fin _ ny => ny != 0 && (roundF f (nx * 2 ^ f.D) ny).isNone
  | _, _, _ => false

/-! #### The equivalences -/

theorem isNaN_fromRound (f : Fmt) (s : Bool) (o : Option Nat)
    (h : ∀ r, o = some r → Repr f r ∧ r ≤ f.maxN) : (decode (fromRound f s o)).isNaN = false := by
  cases o with
  | none => rw [decode_fromRound_none]; rfl
  | some r => rw [decode_fromRound_some f s (h r rfl)]; rfl

theorem isInf_fromRound (f : Fmt) (s : Bool) (o : Option Nat)
    (h : ∀ r, o = some r → Repr f r ∧ r ≤ f.maxN) : (decode (fromRound f s o)).isInf = o.isNone := by
  cases o with
  | none => rw [decode_fromRound_none]; rfl
  | some r => rw [decode_fromRound_some f s (h r rfl)]; rfl

theorem nat_beq_zero (m : Nat) : (m == 0) = decide (m = 0) := by cases m <;> rfl

theorem isNaN_ite_nan_inf (f : Fmt) (c : Prop) [Decidable c] (s : Bool) :
    (decode (if c then nanD f else infD f s)).isNaN = decide c := by
  split <;> simp_all [decode_nanD, decode_infD]

theorem isInf_ite_nan_inf (f : Fmt) (c : Prop) [Decidable c] (s : Bool) :
    (decode (if c then nanD f else infD f s)).isInf = !decide c := by
  split <;> simp_all [decode_nanD, decode_infD]

theorem round_zero_isSome (f : Fmt) : (round f 0).isNone = false := by
  have := Nat.two_pow_pos f.p
  simp [round, roundU, this]

theorem add_fin_flags (f : Fmt) (x y : FP f) {sx sy : Bool} {nx ny : Nat}
    (hx : decode x = .fin sx nx) (hy : decode y = .fin sy ny) :
    (decode (add f x y)).isNaN = false ∧
    (decode (add f x y)).isInf = (round f (sval sx nx + sval sy ny).natAbs).isNone := by
  simp only [add, hx, hy]
  split
  · rename_i h0
    rw [decode_zeroD, h0]
    exact ⟨rfl, (round_zero_isSome f).symm⟩
  · exact ⟨isNaN_fromRound f _ _ (fun r h => round_some f h),
      isInf_fromRound f _ _ (fun r h => round_some f h)⟩


theorem sub_fin_flags (f : Fmt) (x y : FP f) {sx sy : Bool} {nx ny : Nat}
    (hx : decode x = .fin sx nx) (hy : decode y = .fin sy ny) :
    (decode (sub f x y)).isNaN = false ∧
    (decode (sub f x y)).isInf = (round f (sval sx nx - sval sy ny).natAbs).isNone := by
  have hny : decode (neg y) = .fin (!sy) ny := by rw [decode_neg, hy]; rfl
  have h := add_fin_flags f x (neg y) hx hny
  simp only [sval_not, ← Int.sub_eq_add_neg] at h
  exact h

theorem mul_fin_flags (f : Fmt) (x y : FP f) {sx sy : Bool} {nx ny : Nat}
    (hx : decode x = .fin sx nx) (hy : decode y = .fin sy ny) :
    (decode (mul f x y)).isNaN = false ∧
    (decode (mul f x y)).isInf = (roundF f (nx * ny) (2 ^ f.D)).isNone := by
  simp only [mul, hx, hy]
  exact ⟨isNaN_fromRound f _ _ (fun r h => roundF_some f h),
    isInf_fromRound f _ _ (fun r h => roundF_some f h)⟩

theorem div_fin_flags (f : Fmt) (x y : FP f) {sx sy : Bool} {nx ny : Nat}
    (hx : decode x = .fin sx nx) (hy : decode y = .fin sy ny) :
    (decode (div f x y)).isNaN = (nx == 0 && ny == 0) ∧
    (decode (div f x y)).isInf =
      ((ny != 0 && (roundF f (nx * 2 ^ f.D) ny).isNone) || (nx != 0 && ny == 0)) := by
  simp only [div, hx, hy]
  by_cases hy0 : ny = 0
  · by_cases hx0 : nx = 0
    · simp [hy0, hx0, decode_nanD, Val.isNaN, Val.isInf]
    · simp [hy0, hx0, decode_infD, Val.isNaN, Val.isInf]
  · rw [ite_eq_right_iff.mpr (fun h => absurd h hy0)]
    rw [isNaN_fromRound f _ _ (fun r h => roundF_some f h),
      isInf_fromRound f _ _ (fun r h => roundF_some f h)]
    have h1 : (ny != 0) = true := by simpa using hy0
    have h2 : (ny == 0) = false := by simpa using hy0
    simp [h1, h2]

/-- **FLOAT-OVERFLOW is exactly IEEE overflow** for `fadd`, `fsub`, `fmul`,
`fdiv`, for every pair of operands (special values included). -/
theorem prism_overflow_eq (f : Fmt) (op : BinOp) (x y : FP f) :
    prismOverflow op (decode x) (decode y) (decode (eval f op x y)) =
      ieeeOverflow f op (decode x) (decode y) := by
  have hnan := decode_nanD f
  have hinf := decode_infD f
  have hzero := decode_zeroD f
  cases op <;> cases hx : decode x <;> cases hy : decode y
  all_goals first
    | (simp [eval, prismOverflow, ieeeOverflow, pFin, (add_fin_flags f x y hx hy).2]; done)
    | (simp [eval, prismOverflow, ieeeOverflow, pFin, (sub_fin_flags f x y hx hy).2]; done)
    | (simp [eval, prismOverflow, ieeeOverflow, pFin, (mul_fin_flags f x y hx hy).2]; done)
    | (simp [eval, prismOverflow, ieeeOverflow, pFin, (div_fin_flags f x y hx hy).2]
       cases ‹Nat› <;> cases ‹Nat› <;> simp; done)
    | (simp [eval, prismOverflow, ieeeOverflow, pFin, add, sub, mul, div, decode_neg, Val.neg, hx, hy,
        hnan, hinf, hzero]
       try (split <;> simp_all))

/-- **FLOAT-INVALID is exactly IEEE invalid operation on quiet operands**
for `fadd`, `fsub`, `fmul`, `fdiv`, every pair of operands. -/
theorem prism_invalid_eq (f : Fmt) (op : BinOp) (x y : FP f) :
    prismInvalid (decode x) (decode y) (decode (eval f op x y)) =
      ieeeInvalidQ op (decode x) (decode y) := by
  have hnan := decode_nanD f
  have hinf := decode_infD f
  have hzero := decode_zeroD f
  cases op <;> cases hx : decode x <;> cases hy : decode y
  all_goals first
    | (simp [eval, prismInvalid, ieeeInvalidQ, (add_fin_flags f x y hx hy).1]; done)
    | (simp [eval, prismInvalid, ieeeInvalidQ, (sub_fin_flags f x y hx hy).1]; done)
    | (simp [eval, prismInvalid, ieeeInvalidQ, (mul_fin_flags f x y hx hy).1]; done)
    | (simp [eval, prismInvalid, ieeeInvalidQ, (div_fin_flags f x y hx hy).1]; done)
    | (simp [eval, prismInvalid, ieeeInvalidQ, add, sub, mul, div, decode_neg, Val.neg, hx, hy,
        hnan, hinf, hzero]
       try (split <;> simp_all)
       done)
    | (simp [eval, prismInvalid, ieeeInvalidQ, add, sub, mul, div, decode_neg, Val.neg, hx, hy,
        hnan, hinf, hzero, isNaN_ite_nan_inf, nat_beq_zero]
       done)
    | (simp [eval, prismInvalid, ieeeInvalidQ, add, sub, mul, div, decode_neg, Val.neg, hx, hy,
        hnan, hinf, hzero]
       cases ‹Bool› <;> cases ‹Bool› <;> simp_all
       done)

/-- A signalling NaN is a NaN. -/
theorem decode_snan {f : Fmt} (x : FP f) (h : isSNaN x = true) : decode x = .nan := by
  simp only [isSNaN, Bool.and_eq_true, beq_iff_eq, bne_iff_ne, ne_eq, decide_eq_true_eq] at h
  obtain ⟨⟨he, hf⟩, _⟩ := h
  simp [decode, he, hf]

/-- IEEE's invalid flag is PRISM's FLOAT-INVALID plus a signalling-NaN
operand — the one case PRISM does not report (both operands are NaN then
excluded by `Xor(any_nan, 1)`; Z3's FP theory and LLVM's default
floating-point environment do not distinguish signalling NaNs). -/
theorem ieee_invalid_eq (f : Fmt) (op : BinOp) (x y : FP f) :
    ieeeInvalid op x y =
      (prismInvalid (decode x) (decode y) (decode (eval f op x y)) || isSNaN x || isSNaN y) := by
  rw [ieeeInvalid, prism_invalid_eq]

/-- **FLOAT-DIV-ZERO** is IEEE divide-by-zero, or `0 / 0` (IEEE invalid,
reported as FLOAT-INVALID too), or `±∞ / 0` (no IEEE exception). -/
theorem prism_divzero_eq (x y : Val) :
    prismDivZero x y = (ieeeDivByZero x y || (x.isZero && y.isZero) || (x.isInf && y.isZero)) := by
  cases x <;> cases y <;> simp [prismDivZero, ieeeDivByZero]
  rename_i n _ m
  simp only [bne]
  cases n == 0 <;> cases m == 0 <;> rfl

theorem ieee_divzero_imp_prism (x y : Val) (h : ieeeDivByZero x y = true) :
    prismDivZero x y = true := by
  rw [prism_divzero_eq, h]; rfl

/-- The two extra cases really are extra: `0 / 0` and `∞ / 0` are flagged,
IEEE raises no divide-by-zero for them. -/
theorem prism_divzero_extra (s t u : Bool) :
    prismDivZero (.fin s 0) (.fin t 0) = true ∧ ieeeDivByZero (.fin s 0) (.fin t 0) = false ∧
    prismDivZero (.inf u) (.fin t 0) = true ∧ ieeeDivByZero (.inf u) (.fin t 0) = false := by
  simp [prismDivZero, ieeeDivByZero]

/-! ### Float-to-integer conversion: FLOAT-CAST-OVF -/

/-- A well-formed datum: the fields fit their bit widths (every datum PRISM
decodes comes from a `1 + ew + (p-1)`-bit vector). -/
def FP.wf {f : Fmt} (x : FP f) : Prop := x.exp < 2 ^ f.ew ∧ x.frac < 2 ^ (f.p - 1)

theorem decode_le_maxN {f : Fmt} (x : FP f) (hw : x.wf) {s : Bool} {n : Nat}
    (h : decode x = .fin s n) : n ≤ f.maxN := by
  have hq2 := qmax_lt f
  have hp := f.hp
  have hm : 2 ^ (f.p - 1) + x.frac ≤ 2 ^ f.p - 1 := by
    have : 2 ^ f.p = 2 * 2 ^ (f.p - 1) := by
      rw [← Nat.pow_succ']; congr 1; omega
    have := hw.2; omega
  unfold decode at h
  split at h
  · split at h <;> simp at h
  · split at h
    · simp at h; obtain ⟨_, rfl⟩ := h
      unfold Fmt.maxN
      have := Nat.two_pow_pos (f.p - 1)
      exact Nat.le_trans (by omega : x.frac ≤ 2 ^ f.p - 1) (Nat.le_mul_of_pos_right _ (Nat.two_pow_pos _))
    · simp at h; obtain ⟨_, rfl⟩ := h
      unfold Fmt.maxN
      have he : x.exp - 1 ≤ f.qmax := by unfold Fmt.qmax; have := hw.1; omega
      exact Nat.mul_le_mul hm (Nat.pow_le_pow_right (by decide) he)

theorem roundU_exact (p N : Nat) (hp : 1 ≤ p) (h : ReprU p N) : roundU p N = N := by
  have := roundU_nearest p N hp N h
  simp only [dist, Nat.sub_self, Nat.add_zero] at this
  omega

/-- IEEE `roundToIntegralTowardZero` (Z3 `fpa_round_to_integral` with RTZ):
the magnitude truncated to a multiple of `2^D` units (a whole number). -/
def rtz (f : Fmt) : Val → Val
  | .fin s n => .fin s (n / 2 ^ f.D * 2 ^ f.D)
  | v => v

/-- IEEE `compareQuietLess` (Z3 `fpa_lt`): false on NaN, `-0 = +0`. -/
def Val.lt : Val → Val → Bool
  | .fin sa na, .fin sb nb => decide (sval sa na < sval sb nb)
  | .fin _ _, .inf b => !b
  | .inf a, .fin _ _ => a
  | .inf a, .inf b => a && !b
  | _, _ => false

/-- IEEE `compareQuietLessEqual` (Z3 `fpa_leq`; `fpa_geq t hi` is `hi ≤ t`). -/
def Val.le : Val → Val → Bool
  | .fin sa na, .fin sb nb => decide (sval sa na ≤ sval sb nb)
  | .fin _ _, .inf b => !b
  | .inf a, .fin _ _ => a
  | .inf a, .inf b => a || !b
  | _, _ => false

/-- `fnum(std::ldexp(1.0, j), fw)`: the double `2^j` (exact for `j ≤ 64`)
as a numeral of the format, rounded to nearest even — infinity when `2^j`
is above the format's range. -/
def pow2Val (f : Fmt) (j : Nat) : Val :=
  match round f (2 ^ (j + f.D)) with
  | some r => .fin false r
  | none => .inf false

/-- The FLOAT-CAST-OVF condition of `src/prism/pir/encode.cpp`
(`Op::FToSIOvf` for `fptosi`, `Op::FToUIOvf` for `fptoui` to `iK`):

    t  = round_to_integral(RTZ, x)
    lo = FToSIOvf ? fnum(-2^(k-1)) : +0          hi = fnum(2^(FToSIOvf ? k-1 : k))
    is_nan(x) || is_inf(x) || flt(t, lo) || fpa_geq(t, hi)
-/
def prismCastOvf (f : Fmt) (signed : Bool) (k : Nat) (x : Val) : Bool :=
  let t := rtz f x
  let lo := if signed then (pow2Val f (k - 1)).neg else .fin false 0
  let hi := pow2Val f (if signed then k - 1 else k)
  x.isNaN || x.isInf || Val.lt t lo || Val.le hi t

/-- C11 6.3.1.4p1: converting a real floating value to an integer type
truncates toward zero; the behaviour is defined iff the integral part is
representable (signed `iK`: `[-2^(k-1), 2^(k-1))`, unsigned: `[0, 2^k)`).
NaN and infinities are never representable. -/
def inRange (f : Fmt) (signed : Bool) (k : Nat) : Val → Prop
  | .fin s n =>
    if signed then -((2 ^ (k - 1) : Nat) : Int) ≤ sval s (n / 2 ^ f.D) ∧
      sval s (n / 2 ^ f.D) < ((2 ^ (k - 1) : Nat) : Int)
    else 0 ≤ sval s (n / 2 ^ f.D) ∧ sval s (n / 2 ^ f.D) < ((2 ^ k : Nat) : Int)
  | _ => False

theorem pow2Val_eq (f : Fmt) (j : Nat) :
    pow2Val f j = if 2 ^ (j + f.D) ≤ f.maxN then .fin false (2 ^ (j + f.D)) else .inf false := by
  have hp : 1 ≤ f.p := by have := f.hp; omega
  have hr : roundU f.p (2 ^ (j + f.D)) = 2 ^ (j + f.D) :=
    roundU_exact _ _ hp ⟨1, j + f.D, Nat.one_lt_two_pow (by omega), by simp⟩
  unfold pow2Val round
  rw [hr]
  by_cases h : 2 ^ (j + f.D) ≤ f.maxN <;> simp [h]

/-- Magnitude comparisons at scale `P = 2^D`. -/
theorem scaled_cases (q P H n M : Nat) (hP : 0 < P) (hqn : q * P ≤ n) (hnM : n ≤ M) :
    (H * P ≤ q * P ↔ H ≤ q) ∧ (H * P < q * P ↔ H < q) ∧ (0 < q * P ↔ 0 < q) ∧
    (M < H * P → q < H) := by
  refine ⟨Nat.mul_le_mul_right_iff hP, Nat.mul_lt_mul_right hP, ?_, ?_⟩
  · constructor
    · intro h; exact Nat.pos_of_ne_zero (by rintro rfl; simp at h)
    · intro h; exact Nat.mul_pos h hP
  · intro h
    exact (Nat.mul_lt_mul_right hP).1 (by omega)

/-- **FLOAT-CAST-OVF is exactly C's out-of-range condition** for
`fptosi`/`fptoui` from any binary format to any `iK` (`K ≥ 1`), including
formats where `2^(K-1)` or `2^K` is not representable (the bound becomes
infinity and only NaN / infinity are out of range). -/
theorem cast_ovf_iff (f : Fmt) (signed : Bool) (k : Nat) (x : FP f) (hw : x.wf) :
    prismCastOvf f signed k (decode x) = true ↔ ¬ inRange f signed k (decode x) := by
  cases hx : decode x with
  | nan => simp [prismCastOvf, inRange]
  | inf s => simp [prismCastOvf, inRange]
  | fin s n =>
    have hn := decode_le_maxN x hw hx
    have hP : 0 < 2 ^ f.D := Nat.two_pow_pos _
    have hqn := Nat.div_mul_le_self n (2 ^ f.D)
    simp only [prismCastOvf, inRange, rtz, isNaN_fin, isInf_fin, Bool.false_or]
    obtain ⟨a1, a2, a3, a4⟩ := scaled_cases (n / 2 ^ f.D) (2 ^ f.D) (2 ^ k) n f.maxN hP hqn hn
    obtain ⟨b1, b2, b3, b4⟩ := scaled_cases (n / 2 ^ f.D) (2 ^ f.D) (2 ^ (k - 1)) n f.maxN hP hqn hn
    have hH : 0 < 2 ^ (k - 1) := Nat.two_pow_pos _
    simp only [pow2Val_eq, Nat.pow_add]
    cases signed <;> simp only [Bool.false_eq_true, ite_true, ite_false] <;> split <;>
      (generalize n / 2 ^ f.D * 2 ^ f.D = A at *
       generalize n / 2 ^ f.D = q at *
       generalize 2 ^ k * 2 ^ f.D = B at *
       generalize 2 ^ (k - 1) * 2 ^ f.D = C at *
       generalize 2 ^ k = H1 at *
       generalize 2 ^ (k - 1) = H0 at *
       cases s <;> simp [Val.lt, Val.le, Val.neg, sval] <;> omega)

/-- binary16 (`half`): `2^15` is representable, `2^16` is not — `fptosi half
to i17` has `hi = +∞`, and only NaN and infinities are out of range. -/
def binary16 : Fmt := ⟨11, 5, by decide, by decide⟩

example : binary16.D = 24 := by decide
example : pow2Val binary16 15 = .fin false (2 ^ 39) := by decide
example : pow2Val binary16 16 = .inf false := by decide

end PrismRefine.Float
