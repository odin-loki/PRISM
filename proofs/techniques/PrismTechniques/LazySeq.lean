/-
PRISM techniques: sequentialisation of bounded context-switch concurrency
(roadmap 2.6 "Threads and atomics", 8.2 row "Concurrency (lazy
sequentialisation)").

Scope, stated honestly.  Two threads, each a straight-line list of atomic
actions over a shared state `G` (an action is any relation `G → G → Prop`,
so nondeterminism and blocking `assume`s are allowed; thread-local variables
live in `G`).  This covers the *round-robin, guess-and-check* reduction of
Lal and Reps (the eager scheme underlying Lazy-CSeq); the "lazy" variant's
re-execution of prefixes, loops, dynamic thread creation, more than two
threads, and weak memory are not modelled here (`LazySeqN.lean` covers `N` threads,
`K` rounds and threads with any control flow, the schedule of `lazy.cpp`).

* `Step`/`Star`: the real interleaving semantics, counting context switches.
* `SeqLR K`: runs of the sequentialised program with `K` rounds — thread 1
  runs all its rounds first, on round-indexed copies of the shared state whose
  initial values (except round 0) are *guessed*; then thread 2 runs its
  rounds starting where thread 1 left each copy; the run is kept only if the
  state thread 2 reaches at the end of round `r` equals the guess for round
  `r+1`.

Proved:
* `lazy_seq_covers` (coverage): every interleaving with at most `2K-1`
  context switches, reaching any state (finished or not), is matched by a run
  of the sequentialised program on the executed prefixes;
* `lazy_seq_sound` (no spurious runs): every run of the sequentialised program
  is a real interleaving with at most `2K-1` context switches.
Together: a bad state is reachable within the switch bound iff the
sequentialised program reaches it (`lazy_seq_reach_iff`).
-/

namespace PrismTechniques.LazySeq

variable {G : Type}

abbrev Act (G : Type) := G → G → Prop

/-- Sequential execution of a list of actions. -/
def Exec : List (Act G) → G → G → Prop
  | [], g, g' => g = g'
  | a :: as, g, g' => ∃ m, a g m ∧ Exec as m g'

theorem exec_append : ∀ (s r : List (Act G)) (g g' : G),
    Exec (s ++ r) g g' ↔ ∃ m, Exec s g m ∧ Exec r m g'
  | [], r, g, g' => by simp [Exec]
  | a :: s, r, g, g' => by
    simp only [List.cons_append, Exec]
    constructor
    · rintro ⟨m, ha, h⟩
      obtain ⟨m', h1, h2⟩ := (exec_append s r m g').1 h
      exact ⟨m', ⟨m, ha, h1⟩, h2⟩
    · rintro ⟨m', ⟨m, ha, h1⟩, h2⟩
      exact ⟨m, ha, (exec_append s r m g').2 ⟨m', h1, h2⟩⟩

/-- A configuration: remaining code of thread 1 and 2, shared state, which
thread runs (`true` = thread 1), and context switches so far. -/
structure Config (G : Type) where
  p1 : List (Act G)
  p2 : List (Act G)
  g : G
  cur : Bool
  sw : Nat

/-- Interleaving semantics. -/
inductive Step : Config G → Config G → Prop
  | run1 {a p1 p2 g g' sw} : a g g' → Step ⟨a :: p1, p2, g, true, sw⟩ ⟨p1, p2, g', true, sw⟩
  | run2 {a p1 p2 g g' sw} : a g g' → Step ⟨p1, a :: p2, g, false, sw⟩ ⟨p1, p2, g', false, sw⟩
  | switch {p1 p2 g c sw} : Step ⟨p1, p2, g, c, sw⟩ ⟨p1, p2, g, !c, sw + 1⟩

inductive Star : Config G → Config G → Prop
  | refl (c : Config G) : Star c c
  | head {c d e : Config G} : Step c d → Star d e → Star c e

theorem Star.trans {c d e : Config G} (h1 : Star c d) (h2 : Star d e) : Star c e := by
  induction h1 with
  | refl => exact h2
  | head s _ ih => exact Star.head s (ih h2)

theorem star_sw_le {c d : Config G} (h : Star c d) : c.sw ≤ d.sw := by
  induction h with
  | refl => exact Nat.le_refl _
  | head s _ ih =>
    cases s with
    | run1 => exact ih
    | run2 => exact ih
    | switch => simp at ih ⊢; omega

/-- Dropping the unexecuted suffixes: a run that ends with code `q1`, `q2`
left is a complete run of the executed prefixes. -/
theorem star_prefix {c d : Config G} (h : Star c d) :
    ∃ e1 e2, c.p1 = e1 ++ d.p1 ∧ c.p2 = e2 ++ d.p2 ∧
      Star ⟨e1, e2, c.g, c.cur, c.sw⟩ ⟨[], [], d.g, d.cur, d.sw⟩ := by
  induction h with
  | refl c => exact ⟨[], [], rfl, rfl, Star.refl _⟩
  | head s _ ih =>
    obtain ⟨e1, e2, h1, h2, hs⟩ := ih
    cases s with
    | @run1 a p1 p2 g g' sw ha =>
      simp only at h1 h2
      exact ⟨a :: e1, e2, by simp [h1], h2, Star.head (Step.run1 ha) hs⟩
    | @run2 a p1 p2 g g' sw ha =>
      simp only at h1 h2
      exact ⟨e1, a :: e2, h1, by simp [h2], Star.head (Step.run2 ha) hs⟩
    | switch => exact ⟨e1, e2, h1, h2, Star.head Step.switch hs⟩

/-- Executing a whole segment of the current thread. -/
theorem star_exec1 : ∀ (s r p2 : List (Act G)) (g m : G) (sw : Nat), Exec s g m →
    Star ⟨s ++ r, p2, g, true, sw⟩ ⟨r, p2, m, true, sw⟩
  | [], r, p2, g, m, sw, h => by cases h; exact Star.refl _
  | a :: s, r, p2, g, m, sw, ⟨x, ha, h⟩ => Star.head (Step.run1 ha) (star_exec1 s r p2 x m sw h)

theorem star_exec2 : ∀ (s r p1 : List (Act G)) (g m : G) (sw : Nat), Exec s g m →
    Star ⟨p1, s ++ r, g, false, sw⟩ ⟨p1, r, m, false, sw⟩
  | [], r, p1, g, m, sw, h => by cases h; exact Star.refl _
  | a :: s, r, p1, g, m, sw, ⟨x, ha, h⟩ => Star.head (Step.run2 ha) (star_exec2 s r p1 x m sw h)

/-! ### Segment normal form -/

/-- `n` alternating segments, starting with thread `c`, consume both
programs and lead from `g` to `g'`. -/
def Seg : Nat → Bool → List (Act G) → List (Act G) → G → G → Prop
  | 0, _, p1, p2, g, g' => p1 = [] ∧ p2 = [] ∧ g = g'
  | n + 1, true, p1, p2, g, g' => ∃ s r m, p1 = s ++ r ∧ Exec s g m ∧ Seg n false r p2 m g'
  | n + 1, false, p1, p2, g, g' => ∃ s r m, p2 = s ++ r ∧ Exec s g m ∧ Seg n true p1 r m g'

theorem seg_succ : ∀ (n : Nat) (c : Bool) (p1 p2 : List (Act G)) (g g' : G),
    Seg n c p1 p2 g g' → Seg (n + 1) c p1 p2 g g'
  | 0, true, _, _, _, _, ⟨h1, h2, h3⟩ => ⟨[], [], _, by simp [h1], rfl, rfl, h2, h3⟩
  | 0, false, _, _, _, _, ⟨h1, h2, h3⟩ => ⟨[], [], _, by simp [h2], rfl, h1, rfl, h3⟩
  | n + 1, true, _, _, _, _, ⟨s, r, m, hp, he, hs⟩ =>
    ⟨s, r, m, hp, he, seg_succ n false _ _ _ _ hs⟩
  | n + 1, false, _, _, _, _, ⟨s, r, m, hp, he, hs⟩ =>
    ⟨s, r, m, hp, he, seg_succ n true _ _ _ _ hs⟩

theorem seg_mono {n m : Nat} (h : n ≤ m) {c : Bool} {p1 p2 : List (Act G)} {g g' : G}
    (hs : Seg n c p1 p2 g g') : Seg m c p1 p2 g g' := by
  induction h with
  | refl => exact hs
  | step _ ih => exact seg_succ _ _ _ _ _ _ ih

/-- Every complete interleaving with `sw' - sw` switches has a normal form
with `sw' - sw + 1` segments. -/
theorem seg_of_star {c d : Config G} (h : Star c d) (hd1 : d.p1 = []) (hd2 : d.p2 = []) :
    Seg (d.sw - c.sw + 1) c.cur c.p1 c.p2 c.g d.g := by
  induction h with
  | refl c =>
    rw [Nat.sub_self]
    cases hc : c.cur
    · exact ⟨[], [], c.g, by simp [hd2], rfl, hd1, rfl, rfl⟩
    · exact ⟨[], [], c.g, by simp [hd1], rfl, rfl, hd2, rfl⟩
  | @head c d e s hst ih =>
    specialize ih hd1 hd2
    cases s with
    | @run1 a p1 p2 g g' sw ha =>
      obtain ⟨s, r, m, hp, he, hs⟩ := ih
      simp only at hp he hs
      exact ⟨a :: s, r, m, by simp [hp], ⟨g', ha, he⟩, hs⟩
    | @run2 a p1 p2 g g' sw ha =>
      obtain ⟨s, r, m, hp, he, hs⟩ := ih
      simp only at hp he hs
      exact ⟨a :: s, r, m, by simp [hp], ⟨g', ha, he⟩, hs⟩
    | @switch p1 p2 g c0 sw =>
      have hle := star_sw_le hst
      simp only at hle ih ⊢
      have heq : e.sw - sw + 1 = (e.sw - (sw + 1) + 1) + 1 := by omega
      rw [heq]
      cases c0
      · exact ⟨[], p2, g, rfl, rfl, by simpa using ih⟩
      · exact ⟨[], p1, g, rfl, rfl, by simpa using ih⟩

/-- Conversely every normal form is a real interleaving. -/
theorem star_of_seg : ∀ (n : Nat) (c : Bool) (p1 p2 : List (Act G)) (g g' : G) (sw : Nat),
    Seg (n + 1) c p1 p2 g g' → ∃ c', Star ⟨p1, p2, g, c, sw⟩ ⟨[], [], g', c', sw + n⟩
  | 0, true, p1, p2, g, g', sw, ⟨s, r, m, hp, he, h1, h2, h3⟩ =>
    ⟨true, by subst hp h1 h2 h3; simpa using star_exec1 s [] [] g m sw he⟩
  | 0, false, p1, p2, g, g', sw, ⟨s, r, m, hp, he, h1, h2, h3⟩ =>
    ⟨false, by subst hp h1 h2 h3; simpa using star_exec2 s [] [] g m sw he⟩
  | n + 1, true, p1, p2, g, g', sw, ⟨s, r, m, hp, he, hs⟩ => by
    obtain ⟨c', hst⟩ := star_of_seg n false r p2 m g' (sw + 1) hs
    refine ⟨c', ?_⟩
    subst hp
    have := (star_exec1 s r p2 g m sw he).trans (Star.head Step.switch hst)
    simpa [Nat.add_assoc, Nat.add_comm 1 n] using this
  | n + 1, false, p1, p2, g, g', sw, ⟨s, r, m, hp, he, hs⟩ => by
    obtain ⟨c', hst⟩ := star_of_seg n true p1 r m g' (sw + 1) hs
    refine ⟨c', ?_⟩
    subst hp
    have := (star_exec2 s r p1 g m sw he).trans (Star.head Step.switch hst)
    simpa [Nat.add_assoc, Nat.add_comm 1 n] using this

/-! ### The sequentialised program (round-robin, guess and check) -/

/-- Runs of the sequentialised program with `K` rounds.  `s1 r`, `s2 r` are
the code thread 1/2 executes in round `r`; `x r` is the shared state at the
start of round `r` (`x 0 = g` is the real initial state, `x r` for `r ≥ 1`
is a guess); `y r` is the state thread 1 leaves in round `r`.  Phase 1 runs
thread 1 through all rounds, phase 2 runs thread 2 through all rounds, and
the check `x (r+1)` = end of thread 2's round `r` discards wrong guesses. -/
def SeqLR (K : Nat) (p1 p2 : List (Act G)) (g g' : G) : Prop :=
  ∃ (s1 s2 : Nat → List (Act G)) (x y : Nat → G),
    x 0 = g ∧ x K = g' ∧
    p1 = ((List.range K).map s1).flatten ∧ p2 = ((List.range K).map s2).flatten ∧
    (∀ r, r < K → Exec (s1 r) (x r) (y r)) ∧         -- phase 1: thread 1
    (∀ r, r < K → Exec (s2 r) (y r) (x (r + 1)))     -- phase 2: thread 2 + check

theorem seqLR_zero (p1 p2 : List (Act G)) (g g' : G) :
    SeqLR 0 p1 p2 g g' ↔ p1 = [] ∧ p2 = [] ∧ g = g' := by
  constructor
  · rintro ⟨s1, s2, x, y, hx0, hxK, h1, h2, _, _⟩
    exact ⟨by simpa using h1, by simpa using h2, hx0 ▸ hxK⟩
  · rintro ⟨rfl, rfl, rfl⟩
    exact ⟨fun _ => [], fun _ => [], fun _ => g, fun _ => g, rfl, rfl, by simp, by simp,
      fun r hr => absurd hr (Nat.not_lt_zero _), fun r hr => absurd hr (Nat.not_lt_zero _)⟩

theorem seqLR_succ (K : Nat) (p1 p2 : List (Act G)) (g g' : G) :
    SeqLR (K + 1) p1 p2 g g' ↔
      ∃ s1 r1 s2 r2 m1 m2, p1 = s1 ++ r1 ∧ p2 = s2 ++ r2 ∧ Exec s1 g m1 ∧ Exec s2 m1 m2 ∧
        SeqLR K r1 r2 m2 g' := by
  constructor
  · rintro ⟨s1, s2, x, y, hx0, hxK, h1, h2, e1, e2⟩
    refine ⟨s1 0, ((List.range K).map (fun r => s1 (r + 1))).flatten, s2 0,
      ((List.range K).map (fun r => s2 (r + 1))).flatten, y 0, x 1, ?_, ?_, ?_, ?_, ?_⟩
    · rw [h1, List.range_succ_eq_map]; simp [Function.comp_def]
    · rw [h2, List.range_succ_eq_map]; simp [Function.comp_def]
    · exact hx0 ▸ e1 0 (by omega)
    · exact e2 0 (by omega)
    · refine ⟨fun r => s1 (r + 1), fun r => s2 (r + 1), fun r => x (r + 1), fun r => y (r + 1),
        rfl, hxK, rfl, rfl, fun r hr => e1 (r + 1) (by omega), fun r hr => e2 (r + 1) (by omega)⟩
  · rintro ⟨s1, r1, s2, r2, m1, m2, rfl, rfl, he1, he2, ⟨t1, t2, x, y, hx0, hxK, h1, h2, e1, e2⟩⟩
    refine ⟨fun r => if r = 0 then s1 else t1 (r - 1), fun r => if r = 0 then s2 else t2 (r - 1),
      fun r => if r = 0 then g else x (r - 1), fun r => if r = 0 then m1 else y (r - 1),
      by simp, by simpa using hxK, ?_, ?_, ?_, ?_⟩
    · rw [h1, List.range_succ_eq_map]; simp [Function.comp_def]
    · rw [h2, List.range_succ_eq_map]; simp [Function.comp_def]
    · intro r hr
      cases r with
      | zero => simpa using he1
      | succ r => simpa using e1 r (by omega)
    · intro r hr
      cases r with
      | zero => simpa [hx0] using he2
      | succ r => simpa using e2 r (by omega)

/-- `K` rounds of the sequentialised program are exactly `2K` alternating
segments starting with thread 1. -/
theorem seqLR_iff_seg : ∀ (K : Nat) (p1 p2 : List (Act G)) (g g' : G),
    SeqLR K p1 p2 g g' ↔ Seg (2 * K) true p1 p2 g g'
  | 0, p1, p2, g, g' => by rw [seqLR_zero]; rfl
  | K + 1, p1, p2, g, g' => by
    rw [seqLR_succ, show 2 * (K + 1) = (2 * K + 1) + 1 by omega]
    simp only [Seg]
    constructor
    · rintro ⟨s1, r1, s2, r2, m1, m2, h1, h2, e1, e2, h⟩
      exact ⟨s1, r1, m1, h1, e1, s2, r2, m2, h2, e2, (seqLR_iff_seg K r1 r2 m2 g').1 h⟩
    · rintro ⟨s1, r1, m1, h1, e1, s2, r2, m2, h2, e2, h⟩
      exact ⟨s1, r1, s2, r2, m1, m2, h1, h2, e1, e2, (seqLR_iff_seg K r1 r2 m2 g').2 h⟩

/-! ### Main theorems -/

/-- **Coverage.**  Any interleaving from the initial configuration (thread 1
first, no switches yet) with at most `2K - 1` context switches, reaching
shared state `g'` with code `q1`, `q2` still to run, is matched by a run of
the sequentialised program on the executed prefixes. -/
theorem lazy_seq_covers (P1 P2 q1 q2 : List (Act G)) (g0 g' : G) (c : Bool) (sw K : Nat)
    (hrun : Star ⟨P1, P2, g0, true, 0⟩ ⟨q1, q2, g', c, sw⟩) (hbound : sw + 1 ≤ 2 * K) :
    ∃ e1 e2, P1 = e1 ++ q1 ∧ P2 = e2 ++ q2 ∧ SeqLR K e1 e2 g0 g' := by
  obtain ⟨e1, e2, h1, h2, hs⟩ := star_prefix hrun
  refine ⟨e1, e2, h1, h2, (seqLR_iff_seg K e1 e2 g0 g').2 ?_⟩
  have := seg_of_star hs rfl rfl
  simp only [Nat.sub_zero] at this
  exact seg_mono hbound this

/-- **No spurious runs.**  Every run of the sequentialised program with
`K ≥ 1` rounds is a real interleaving with at most `2K - 1` switches. -/
theorem lazy_seq_sound (K : Nat) (hK : 0 < K) (e1 e2 q1 q2 : List (Act G)) (g0 g' : G)
    (h : SeqLR K e1 e2 g0 g') :
    ∃ c sw, sw + 1 ≤ 2 * K ∧ Star ⟨e1 ++ q1, e2 ++ q2, g0, true, 0⟩ ⟨q1, q2, g', c, sw⟩ := by
  have hs := (seqLR_iff_seg K e1 e2 g0 g').1 h
  obtain ⟨c, hst⟩ := star_of_seg (2 * K - 1) true e1 e2 g0 g' 0
    (by rw [show 2 * K - 1 + 1 = 2 * K by omega]; exact hs)
  refine ⟨c, 2 * K - 1, by omega, ?_⟩
  -- run the same schedule with the unexecuted suffixes appended
  have frame : ∀ {c d : Config G}, Star c d →
      Star ⟨c.p1 ++ q1, c.p2 ++ q2, c.g, c.cur, c.sw⟩ ⟨d.p1 ++ q1, d.p2 ++ q2, d.g, d.cur, d.sw⟩ := by
    intro c d hcd
    induction hcd with
    | refl => exact Star.refl _
    | head s _ ih =>
      cases s with
      | run1 ha => exact Star.head (Step.run1 ha) ih
      | run2 ha => exact Star.head (Step.run2 ha) ih
      | switch => exact Star.head Step.switch ih
  simpa using frame hst

/-- A state is reachable by an interleaving within the context-switch bound
iff the sequentialised program reaches it (for some prefixes of the thread
programs). -/
theorem lazy_seq_reach_iff (K : Nat) (hK : 0 < K) (P1 P2 : List (Act G)) (g0 g' : G) :
    (∃ q1 q2 c sw, sw + 1 ≤ 2 * K ∧ Star ⟨P1, P2, g0, true, 0⟩ ⟨q1, q2, g', c, sw⟩) ↔
    (∃ e1 q1 e2 q2, P1 = e1 ++ q1 ∧ P2 = e2 ++ q2 ∧ SeqLR K e1 e2 g0 g') := by
  constructor
  · rintro ⟨q1, q2, c, sw, hb, hrun⟩
    obtain ⟨e1, e2, h1, h2, h⟩ := lazy_seq_covers P1 P2 q1 q2 g0 g' c sw K hrun hb
    exact ⟨e1, q1, e2, q2, h1, h2, h⟩
  · rintro ⟨e1, q1, e2, q2, rfl, rfl, h⟩
    obtain ⟨c, sw, hb, hst⟩ := lazy_seq_sound K hK e1 e2 q1 q2 g0 g' h
    exact ⟨q1, q2, c, sw, hb, hst⟩

end PrismTechniques.LazySeq
