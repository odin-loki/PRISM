/-
PRISM refinement — IEEE 754 binary formats and correctly rounded addition
(roadmap 8.2 "Floating point", extending the round-to-nearest-even kernel
`PrismTechniques.FloatRound.rne` of proofs/techniques, imported, not copied).

Formats.  `Fmt` is a binary interchange format with precision `p` (bits of
significand, hidden bit included) and exponent field width `ew`:
`binary32 = ⟨24, 8⟩`, `binary64 = ⟨53, 11⟩`.  A datum is the bit-level triple
(sign, biased exponent, fraction) and `decode` gives its value (IEEE 754-2019
§3.4): exponent all ones = infinity / NaN, zero = subnormal, else normal.

Exact arithmetic.  Every finite datum is an integer multiple of the smallest
subnormal `2^(emin - p + 1)`; `decode` returns the magnitude as a natural
number in that unit.  The exact rational sum of two finite data is therefore
an exact integer `S` in the same unit (no rounding happens anywhere before
`round`), and distances between rationals compare exactly as distances
between these integers.

Proved (for every format with `p ≥ 2`, `ew ≥ 2`):
* `roundU_repr`, `roundU_nearest`, `roundU_tie_even`: rounding to nearest,
  ties to even, with unbounded exponent, returns a value with a `p`-bit
  significand that is at least as close to the exact value as every such
  value, and in a tie has an even significand;
* `round_correct`: with the format's exponent range, the result is the
  nearest *finite* datum when it is at most the largest finite magnitude,
  and otherwise (IEEE 754 §7.4: overflow in round-to-nearest) infinity — which
  happens only when the exact magnitude exceeds the largest finite one;
* `decode_encode`: every representable magnitude is encoded exactly;
* `add_correct`: bit-level addition of two finite data (`add`: decode,
  exact integer sum, round, encode) decodes to the correctly rounded exact
  sum, with IEEE's zero-sign rule and overflow to infinity.

`add` also defines IEEE's special cases (NaN operands, infinities, `∞ - ∞`
invalid).  Subtraction, multiplication, division, the exception flags and
PRISM's floating-point checks are in `FloatOps.lean`.  Not covered: the
other rounding modes and NaN payloads.
-/
import PrismTechniques.FloatRound

namespace PrismRefine.Float

open PrismTechniques.FloatRound

structure Fmt where
  p : Nat
  ew : Nat
  hp : 2 ≤ p
  hew : 2 ≤ ew

def binary32 : Fmt := ⟨24, 8, by decide, by decide⟩
def binary64 : Fmt := ⟨53, 11, by decide, by decide⟩

/-- Largest exponent step of a finite datum, in units of the smallest
subnormal: the largest finite magnitude is `(2^p - 1) * 2^qmax`. -/
def Fmt.qmax (f : Fmt) : Nat := 2 ^ f.ew - 3

def Fmt.maxN (f : Fmt) : Nat := (2 ^ f.p - 1) * 2 ^ f.qmax

/-- A datum: sign, biased exponent field, fraction field. -/
structure FP (f : Fmt) where
  sign : Bool
  exp : Nat
  frac : Nat
  deriving DecidableEq, Repr

inductive Val where
  | fin (s : Bool) (mag : Nat)
  | inf (s : Bool)
  | nan
  deriving DecidableEq, Repr

def decode {f : Fmt} (x : FP f) : Val :=
  if x.exp = 2 ^ f.ew - 1 then (if x.frac = 0 then .inf x.sign else .nan)
  else if x.exp = 0 then .fin x.sign x.frac
  else .fin x.sign ((2 ^ (f.p - 1) + x.frac) * 2 ^ (x.exp - 1))

/-- Magnitudes with a `p`-bit significand and unbounded exponent. -/
def ReprU (p N : Nat) : Prop := ∃ m q, m < 2 ^ p ∧ N = m * 2 ^ q

/-- Finite magnitudes of the format. -/
def Repr (f : Fmt) (N : Nat) : Prop := ∃ m q, m < 2 ^ f.p ∧ q ≤ f.qmax ∧ N = m * 2 ^ q

/-- Round to nearest, ties to even, unbounded exponent. -/
def roundU (p N : Nat) : Nat :=
  if N < 2 ^ p then N else rne N (2 ^ (N.log2 + 1 - p)) * 2 ^ (N.log2 + 1 - p)

/-! ### Rounding -/

theorem log2_bounds {N : Nat} (h : N ≠ 0) : 2 ^ N.log2 ≤ N ∧ N < 2 ^ (N.log2 + 1) :=
  ⟨Nat.log2_self_le h, Nat.lt_log2_self⟩

theorem pow_split' (a b : Nat) (h : b ≤ a) : 2 ^ a = 2 ^ (a - b) * 2 ^ b := by
  rw [← Nat.pow_add]; congr 1; omega

theorem rne_le (n P : Nat) : rne n P ≤ n / P + 1 := by
  unfold rne; split <;> omega

theorem rne_ge (n P : Nat) : n / P ≤ rne n P := by
  unfold rne; split <;> omega

theorem roundU_repr (p N : Nat) (hp : 1 ≤ p) : ReprU p (roundU p N) := by
  unfold roundU
  split
  · exact ⟨N, 0, by assumption, by simp⟩
  · rename_i hN
    have hN0 : N ≠ 0 := by intro h; subst h; exact hN (Nat.two_pow_pos p)
    obtain ⟨_, hhi⟩ := log2_bounds hN0
    have hL : p ≤ N.log2 := (Nat.le_log2 hN0).mpr (by omega)
    generalize hs : N.log2 + 1 - p = s
    have hsplit : 2 ^ (N.log2 + 1) = 2 ^ p * 2 ^ s := by
      rw [← Nat.pow_add]; congr 1; omega
    have hq : N / 2 ^ s < 2 ^ p := by
      rw [Nat.div_lt_iff_lt_mul (Nat.two_pow_pos s)]; omega
    have hr := rne_le N (2 ^ s)
    by_cases he : rne N (2 ^ s) < 2 ^ p
    · exact ⟨_, s, he, rfl⟩
    · have : rne N (2 ^ s) = 2 ^ p := by omega
      refine ⟨1, p + s, by have := Nat.one_lt_two_pow_iff.mpr (show p ≠ 0 by omega); omega, ?_⟩
      rw [this, Nat.pow_add]; simp

/-- Every `p`-bit magnitude at or above `2^(L-1)` is a multiple of
`2^(L - p)`. -/
theorem repr_dvd {p L g : Nat} (hg : ReprU p g) (hge : 2 ^ (L - 1) ≤ g) (hL : p + 1 ≤ L) :
    2 ^ (L - p) ∣ g := by
  obtain ⟨m, q, hm, rfl⟩ := hg
  have hq : L - p ≤ q := by
    refine Nat.le_of_not_lt (fun hc => ?_)
    have : q + 1 ≤ L - p := by omega
    have h1 : m * 2 ^ q < 2 ^ p * 2 ^ q := Nat.mul_lt_mul_of_pos_right hm (Nat.two_pow_pos q)
    have h2 : 2 ^ p * 2 ^ q ≤ 2 ^ (L - 1) := by
      rw [← Nat.pow_add]; exact Nat.pow_le_pow_right (by decide) (by omega)
    omega
  exact Nat.dvd_trans (Nat.pow_dvd_pow 2 hq) (Nat.dvd_mul_left _ _)

theorem roundU_nearest (p N : Nat) (hp : 1 ≤ p) (g : Nat) (hg : ReprU p g) :
    dist (roundU p N) N ≤ dist g N := by
  unfold roundU
  split
  · simp [dist]
  · rename_i hN
    have hN0 : N ≠ 0 := by intro h; subst h; exact hN (Nat.two_pow_pos p)
    obtain ⟨hlo, hhi⟩ := log2_bounds hN0
    have hL : p ≤ N.log2 := (Nat.le_log2 hN0).mpr (by omega)
    generalize hL' : N.log2 + 1 = L at hhi
    have hlo' : 2 ^ (L - 1) ≤ N := by rw [← hL']; simpa using hlo
    have hP : 0 < 2 ^ (L - p) := Nat.two_pow_pos _
    by_cases hgl : g < 2 ^ (L - 1)
    · -- compare with 2^(L-1), a multiple of the ulp
      have hm := rne_nearest N (2 ^ (L - p)) hP (2 ^ (L - 1 - (L - p)))
      have he : 2 ^ (L - 1 - (L - p)) * 2 ^ (L - p) = 2 ^ (L - 1) := by
        rw [← Nat.pow_add]; congr 1; omega
      rw [he] at hm
      refine Nat.le_trans hm ?_
      simp only [dist]; omega
    · obtain ⟨k, hk⟩ := repr_dvd hg (L := L) (by omega) (by omega)
      have hm := rne_nearest N (2 ^ (L - p)) hP k
      rw [hk, Nat.mul_comm (2 ^ (L - p)) k]; exact hm

theorem roundU_tie_even (p N : Nat) (hN : 2 ^ p ≤ N)
    (htie : 2 * (N % 2 ^ (N.log2 + 1 - p)) = 2 ^ (N.log2 + 1 - p)) :
    (roundU p N / 2 ^ (N.log2 + 1 - p)) % 2 = 0 := by
  unfold roundU
  rw [ite_eq_right (by omega), Nat.mul_div_cancel _ (Nat.two_pow_pos _)]
  exact rne_tie_even N _ htie

theorem maxN_reprU (f : Fmt) : ReprU f.p f.maxN :=
  ⟨2 ^ f.p - 1, f.qmax, by have := Nat.two_pow_pos f.p; omega, rfl⟩

theorem repr_of_reprU (f : Fmt) {N : Nat} (h : ReprU f.p N) (hle : N ≤ f.maxN) : Repr f N := by
  obtain ⟨m, q, hm, rfl⟩ := h
  by_cases hq : q ≤ f.qmax
  · exact ⟨m, q, hm, hq, rfl⟩
  · refine ⟨m * 2 ^ (q - f.qmax), f.qmax, ?_, Nat.le_refl _, ?_⟩
    · have e : m * 2 ^ q = m * 2 ^ (q - f.qmax) * 2 ^ f.qmax := by
        rw [Nat.mul_assoc, ← Nat.pow_add]; congr 2; omega
      rw [e] at hle
      unfold Fmt.maxN at hle
      have := Nat.le_of_mul_le_mul_right hle (Nat.two_pow_pos f.qmax)
      have := Nat.two_pow_pos f.p
      omega
    · rw [Nat.mul_assoc, ← Nat.pow_add]; congr 2; omega

theorem reprU_of_repr (f : Fmt) {N : Nat} (h : Repr f N) : ReprU f.p N := by
  obtain ⟨m, q, hm, _, rfl⟩ := h
  exact ⟨m, q, hm, rfl⟩

/-- Rounding into the format (magnitude): `none` is overflow to infinity. -/
def round (f : Fmt) (N : Nat) : Option Nat :=
  if roundU f.p N ≤ f.maxN then some (roundU f.p N) else none

/-- **Correct rounding** (round to nearest, ties to even, IEEE 754 §4.3.1,
§7.4). -/
theorem round_correct (f : Fmt) (N : Nat) :
    (∀ r, round f N = some r →
      Repr f r ∧ ∀ g, Repr f g → dist r N ≤ dist g N) ∧
    (round f N = none → f.maxN < N) := by
  have hp : 1 ≤ f.p := by have := f.hp; omega
  constructor
  · intro r hr
    unfold round at hr
    split at hr
    · rename_i hle
      simp at hr; subst hr
      exact ⟨repr_of_reprU f (roundU_repr f.p N hp) hle,
        fun g hg => roundU_nearest f.p N hp g (reprU_of_repr f hg)⟩
    · simp at hr
  · intro hr
    unfold round at hr
    split at hr
    · simp at hr
    · rename_i hgt
      have hn := roundU_nearest f.p N hp f.maxN (maxN_reprU f)
      simp only [dist] at hn
      omega

/-! ### Encoding -/

/-- Encode a finite magnitude (assumed representable). -/
def encode (f : Fmt) (s : Bool) (r : Nat) : FP f :=
  if r < 2 ^ (f.p - 1) then ⟨s, 0, r⟩
  else
    let q := r.log2 + 1 - f.p
    ⟨s, q + 1, r / 2 ^ q - 2 ^ (f.p - 1)⟩

theorem qmax_lt (f : Fmt) : f.qmax + 2 < 2 ^ f.ew := by
  unfold Fmt.qmax
  have : 4 ≤ 2 ^ f.ew := by
    have := Nat.pow_le_pow_right (n := 2) (by decide) f.hew; simpa using this
  omega

theorem decode_encode (f : Fmt) (s : Bool) (r : Nat) (hr : Repr f r) (hle : r ≤ f.maxN) :
    decode (encode f s r) = .fin s r := by
  have hq2 := qmax_lt f
  have hp := f.hp
  unfold encode
  split
  · have h0 : (0 : Nat) ≠ 2 ^ f.ew - 1 := by omega
    simp [decode, h0]
  · rename_i hge
    dsimp only
    have hr0 : r ≠ 0 := by intro h; subst h; have := Nat.two_pow_pos (f.p - 1); omega
    obtain ⟨hlo, hhi⟩ := log2_bounds hr0
    have hL : f.p - 1 ≤ r.log2 := (Nat.le_log2 hr0).mpr (by omega)
    generalize hq : r.log2 + 1 - f.p = q
    -- r is a multiple of 2^q
    have hdvd : 2 ^ q ∣ r := by
      by_cases hLp : f.p + 1 ≤ r.log2 + 1
      · have := repr_dvd (reprU_of_repr f hr) (L := r.log2 + 1) (by simpa using hlo) hLp
        rwa [hq] at this
      · have : q = 0 := by omega
        subst this; simp
    have hm_lo : 2 ^ (f.p - 1) ≤ r / 2 ^ q := by
      rw [Nat.le_div_iff_mul_le (Nat.two_pow_pos q), ← Nat.pow_add]
      exact Nat.le_trans (Nat.pow_le_pow_right (by decide) (by omega)) hlo
    -- q stays below the top exponent
    have hqmax : q ≤ f.qmax := by
      refine Nat.le_of_not_lt (fun hc => ?_)
      have h1 : 2 ^ (f.p + f.qmax) ≤ r := by
        refine Nat.le_trans (Nat.pow_le_pow_right (by decide) ?_) hlo; omega
      have h2 : f.maxN < 2 ^ (f.p + f.qmax) := by
        unfold Fmt.maxN; rw [Nat.pow_add]
        have := Nat.two_pow_pos f.p; have := Nat.two_pow_pos f.qmax
        exact Nat.mul_lt_mul_of_pos_right (by omega) (by assumption)
      omega
    simp only [decode]
    rw [ite_eq_right (by omega), ite_eq_right (by omega)]
    congr 1
    rw [Nat.add_sub_cancel' hm_lo, Nat.add_sub_cancel, Nat.div_mul_cancel hdvd]

/-! ### Special data -/

/-- Infinity with sign `s`. -/
def infD (f : Fmt) (s : Bool) : FP f := ⟨s, 2 ^ f.ew - 1, 0⟩
/-- The default quiet NaN (IEEE 754 §6.2.1: the leading fraction bit set). -/
def nanD (f : Fmt) : FP f := ⟨false, 2 ^ f.ew - 1, 2 ^ (f.p - 2)⟩
/-- Zero with sign `s`. -/
def zeroD (f : Fmt) (s : Bool) : FP f := ⟨s, 0, 0⟩

/-- The datum for a rounded magnitude: `none` (overflow) is infinity. -/
def fromRound (f : Fmt) (s : Bool) : Option Nat → FP f
  | some r => encode f s r
  | none => infD f s

theorem decode_infD (f : Fmt) (s : Bool) : decode (infD f s) = .inf s := by
  simp [decode, infD]

theorem decode_nanD (f : Fmt) : decode (nanD f) = .nan := by
  have := Nat.two_pow_pos (f.p - 2)
  simp [decode, nanD]

theorem decode_zeroD (f : Fmt) (s : Bool) : decode (zeroD f s) = .fin s 0 := by
  have := qmax_lt f
  have h0 : (0 : Nat) ≠ 2 ^ f.ew - 1 := by omega
  simp [decode, zeroD, h0]

theorem decode_fromRound_some (f : Fmt) (s : Bool) {r : Nat} (h : Repr f r ∧ r ≤ f.maxN) :
    decode (fromRound f s (some r)) = .fin s r := decode_encode f s r h.1 h.2

theorem decode_fromRound_none (f : Fmt) (s : Bool) : decode (fromRound f s none) = .inf s :=
  decode_infD f s

theorem round_some (f : Fmt) {N r : Nat} (h : round f N = some r) : Repr f r ∧ r ≤ f.maxN := by
  refine ⟨((round_correct f N).1 r h).1, ?_⟩
  unfold round at h; split at h
  · simp at h; subst h; assumption
  · simp at h

/-! ### Addition -/

/-- Signed value of a finite magnitude, in units of the smallest subnormal. -/
def sval (s : Bool) (n : Nat) : Int := if s then -(n : Int) else n

/-- Bit-level addition (round to nearest, ties to even), IEEE 754 §5.4.1,
§6: a NaN operand gives NaN; `∞ + (-∞)` is invalid (NaN); `±∞` plus
anything finite, or two infinities of one sign, give that infinity; finite
operands: exact integer sum, one rounding, encoding, overflow to infinity. -/
def add (f : Fmt) (x y : FP f) : FP f :=
  match decode x, decode y with
  | .nan, _ => nanD f
  | _, .nan => nanD f
  | .inf sx, .inf sy => if sx = sy then infD f sx else nanD f
  | .inf sx, .fin _ _ => infD f sx
  | .fin _ _, .inf sy => infD f sy
  | .fin sx nx, .fin sy ny =>
    let S := sval sx nx + sval sy ny
    if S = 0 then zeroD f (sx && sy) else fromRound f (decide (S < 0)) (round f S.natAbs)

/-- **Correctly rounded addition.**  For finite `x`, `y` with exact sum `S`
(in units of the smallest subnormal): the result is `±0` with IEEE's sign
rule when `S = 0`; otherwise it is the finite datum nearest to `S` (with the
sign of `S`) when that rounding does not overflow, and infinity with the
sign of `S` when it does — which requires `|S|` to exceed the largest finite
magnitude. -/
theorem add_correct (f : Fmt) (x y : FP f) {sx sy : Bool} {nx ny : Nat}
    (hx : decode x = .fin sx nx) (hy : decode y = .fin sy ny) :
    let S := sval sx nx + sval sy ny
    (S = 0 → decode (add f x y) = .fin (sx && sy) 0) ∧
    (∀ r, S ≠ 0 → round f S.natAbs = some r →
      decode (add f x y) = .fin (decide (S < 0)) r ∧ Repr f r ∧
      ∀ g, Repr f g → dist r S.natAbs ≤ dist g S.natAbs) ∧
    (S ≠ 0 → round f S.natAbs = none →
      decode (add f x y) = .inf (decide (S < 0)) ∧ f.maxN < S.natAbs) := by
  intro S
  have hrc := round_correct f S.natAbs
  refine ⟨?_, ?_, ?_⟩
  · intro h0
    simp only [add, hx, hy]
    rw [ite_eq_left h0]
    exact decode_zeroD f _
  · intro r h0 hr
    obtain ⟨hrep, hnear⟩ := hrc.1 r hr
    refine ⟨?_, hrep, hnear⟩
    simp only [add, hx, hy]
    rw [ite_eq_right h0, hr]
    exact decode_fromRound_some f _ (round_some f hr)
  · intro h0 hr
    refine ⟨?_, hrc.2 hr⟩
    simp only [add, hx, hy]
    rw [ite_eq_right h0, hr]
    exact decode_fromRound_none f _

/-- Sanity checks on binary32 (evaluated by the kernel). -/
example : binary32.qmax = 253 := by decide
example : roundU 24 (2 ^ 24 + 1) = 2 ^ 24 := by decide
example : roundU 24 (2 ^ 24 + 3) = 2 ^ 24 + 4 := by decide

end PrismRefine.Float
