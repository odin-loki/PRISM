/-
PRISM techniques: floating point — a small, honest piece (roadmap 8.2 row
"Floating point", stretch).

A full proof that PRISM's floating-point encoding matches IEEE 754 needs a
Flocq-scale library (formats with exponent ranges, subnormals, overflow to
infinity, signed zeros, NaNs, all five rounding modes, and the real-number
semantics of every operation).  None of that exists in core Lean, and it is
out of reach here.  What *is* proved is the kernel every IEEE operation shares:
**round-to-nearest, ties-to-even of a non-negative magnitude to a multiple of
the unit in the last place `P`**, and its specialisation to the 11-bit
significand of binary16.

Proved for every `n` and every `P > 0` (`rne n P` is the rounded significand,
`rne n P * P` the rounded value):
* `rne_exact`: representable values are unchanged;
* `rne_half_ulp`: the error is at most half an ulp;
* `rne_nearest`: no multiple of `P` is closer to `n` (correct rounding);
* `rne_tie_even`: exact ties go to the even significand;
* `binary16_sig_bound`: for binary16 (`P = 2^k`, `n < 2^(11+k)`) the rounded
  significand fits in 11 bits or is exactly `2^11` (the carry-out case that
  bumps the exponent).
-/

namespace PrismTechniques.FloatRound

/-- Round `n / P` to the nearest integer, ties to even. -/
def rne (n P : Nat) : Nat :=
  if 2 * (n % P) > P ∨ (2 * (n % P) = P ∧ (n / P) % 2 = 1) then n / P + 1 else n / P

/-- `|a - b|` on naturals. -/
def dist (a b : Nat) : Nat := (a - b) + (b - a)

theorem decomp (n P : Nat) : n / P * P + n % P = n := by
  rw [Nat.mul_comm]; exact Nat.div_add_mod n P

theorem rne_exact (n P : Nat) (hP : 0 < P) (h : n % P = 0) : rne n P * P = n := by
  have hd := decomp n P
  unfold rne
  split <;> omega

theorem rne_half_ulp (n P : Nat) (hP : 0 < P) : 2 * dist (rne n P * P) n ≤ P := by
  have hd := decomp n P
  have hr := Nat.mod_lt n hP
  unfold rne dist
  generalize n / P = q at *
  generalize n % P = r at *
  split
  · rw [Nat.add_mul, Nat.one_mul]
    generalize q * P = A at *
    omega
  · generalize q * P = A at *
    omega

theorem rne_nearest (n P : Nat) (hP : 0 < P) (m : Nat) :
    dist (rne n P * P) n ≤ dist (m * P) n := by
  have hd := decomp n P
  have hr := Nat.mod_lt n hP
  have hm : m * P ≤ n / P * P ∨ n / P * P + P ≤ m * P := by
    rcases Nat.lt_or_ge (n / P) m with h | h
    · right
      have := Nat.mul_le_mul_right P (Nat.succ_le_of_lt h)
      rwa [Nat.succ_mul] at this
    · left; exact Nat.mul_le_mul_right P h
  unfold rne dist
  generalize n / P = q at *
  generalize n % P = r at *
  generalize m * P = B at *
  split
  · rw [Nat.add_mul, Nat.one_mul]
    generalize q * P = A at *
    omega
  · generalize q * P = A at *
    omega

theorem rne_tie_even (n P : Nat) (htie : 2 * (n % P) = P) : rne n P % 2 = 0 := by
  unfold rne
  split <;> omega

/-- binary16 has an 11-bit significand (10 stored bits plus the hidden bit).
Rounding a magnitude below `2^(11+k)` at ulp `2^k` gives at most `2^11`. -/
theorem binary16_sig_bound (n k : Nat) (h : n < 2 ^ (11 + k)) : rne n (2 ^ k) ≤ 2 ^ 11 := by
  have hq : n / 2 ^ k < 2 ^ 11 := by
    rw [Nat.div_lt_iff_lt_mul (Nat.two_pow_pos k)]
    rwa [← Nat.pow_add]
  unfold rne
  split <;> omega

/-- Concrete binary16 check: 2049 = 2^11 + 1 needs 12 bits; at ulp 2 it is
a tie between 2048 and 2050 and rounds to the even significand 1024. -/
example : rne 2049 2 = 1024 := by decide
example : rne 2051 2 = 1026 := by decide

end PrismTechniques.FloatRound
