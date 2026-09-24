/-
PRISM techniques: lazy sequentialisation for `N` threads and `K` rounds
(roadmap 2.6 "Threads and atomics", 8.2 row "Concurrency (lazy
sequentialisation)", milestone M10).  This generalises the two-thread
round-robin proof of `LazySeq.lean` to the schedule the `conc` stage
(`src/prism/conc/lazy.cpp`) actually uses.

Model (sequential consistency).  A system is `N` threads over one shared
state `G` (globals, mutex owners, active flags) and a per-thread local state
`L` (program counter and locals).  Thread `t` takes atomic steps
`S t l g l' g'`; a step is any relation, so nondeterminism, blocking
`assume`s (locks, joins), and any control flow are allowed.  In particular a
thread may be straight-line code (`L` = remaining code, as in `LazySeq`),
a loop-free DAG, or a loop unrolled to a bound — `lazy.cpp`'s unrolled
thread graph with its node labels as `pc` is an instance — and also code
with unbounded loops.  Every interleaving of atomic steps is an execution
(`Step`/`Star`: run the current thread, or switch to any thread `t < N`,
counting switches).

The sequentialised program (`Slots π`).  Lazy-CSeq runs a fixed pattern `π`
of slots; in slot `t` thread `t` resumes from its saved state, runs zero or
more steps (up to a nondeterministic context-switch point) and saves its
state.  `lazy.cpp` uses `prismSched N K` = `K` rounds of threads
`0, 1, …, N-1` in order, then one final slot for thread 0 (the harness):
`K·N + 1` slots, `K·N` context switches (`context_switch_bound`).

Proved (no assumption on `S`):
* `slots_of_star` / `star_of_slots` (normal form): an interleaving with `sw`
  switches is exactly a run of `Slots σ` for its schedule `σ` (the thread of
  each of its `sw + 1` segments);
* `slots_mono`: if `σ` is a subsequence of `π`, every run along `σ` is a run
  along `π` (the other slots run zero steps);
* `lazy_sound`: every run of the sequentialised program is a real
  interleaving (from thread 0, with `K·N` switches) — no spurious
  counterexamples;
* `rr_covers_runs` / `lazy_covers_runs`: an interleaving whose schedule
  splits into at most `K` strictly increasing runs of thread ids is covered;
* `lazy_covers` (corollary): every interleaving that starts in thread 0 and
  has at most `K - 1` context switches in total is covered by `K` rounds;
* `lazy_covers_two`: with two threads (the harness and one created thread)
  every interleaving with at most `2K` context switches is covered — the
  full `K·N` bound;
* `per_thread_bound_not_enough` (honest limit): a bound on context switches
  *per thread* is not enough for `N ≥ 3`.  With `N = 3`, `K = 1`, the
  schedule `0, 2, 1` (every thread runs once, nobody is preempted) reaches a
  state no run of `prismSched 3 1` reaches.

Not covered: unbounded rounds (a `BOUNDED` verdict is only for schedules that
fit the pattern), relaxed / weak memory (every step is SC), and the Z3
encoding of the slots in `lazy.cpp` (the `pc`/`cs` formula, the race and
deadlock monitors) — what is proved is the scheduling argument that encoding
relies on.  Loop unrolling inside a thread cuts paths past the bound; that
is the same bounded unwinding as BMC, not part of this proof.
-/

namespace PrismTechniques.LazySeqN

variable {L G : Type}

/-- Thread `t` in local state `l`, shared state `g`, steps to `l'`, `g'`. -/
abbrev Sys (L G : Type) := Nat → L → G → L → G → Prop

/-- Zero or more steps of one thread: a segment without context switch. -/
inductive Seg (S : Sys L G) (t : Nat) : L → G → L → G → Prop
  | refl (l : L) (g : G) : Seg S t l g l g
  | head {l g l1 g1 l2 g2} : S t l g l1 g1 → Seg S t l1 g1 l2 g2 → Seg S t l g l2 g2

def upd (ls : Nat → L) (t : Nat) (l : L) : Nat → L := fun u => if u = t then l else ls u

@[simp] theorem upd_same (ls : Nat → L) (t : Nat) (l : L) : upd ls t l t = l := by simp [upd]

@[simp] theorem upd_self (ls : Nat → L) (t : Nat) : upd ls t (ls t) = ls := by
  funext u; simp only [upd]; split <;> simp_all

@[simp] theorem upd_upd (ls : Nat → L) (t : Nat) (l l' : L) : upd (upd ls t l) t l' = upd ls t l' := by
  funext u; simp only [upd]; split <;> simp_all

/-- Runs of the sequentialised program along the slot pattern `π`. -/
def Slots (S : Sys L G) : List Nat → (Nat → L) → G → (Nat → L) → G → Prop
  | [], ls, g, ls', g' => ls' = ls ∧ g' = g
  | t :: π, ls, g, ls', g' => ∃ l m, Seg S t (ls t) g l m ∧ Slots S π (upd ls t l) m ls' g'

/-! ### Interleaving semantics (sequential consistency) -/

structure Cfg (L G : Type) where
  ls : Nat → L
  g : G
  cur : Nat
  sw : Nat

inductive Step (S : Sys L G) (N : Nat) : Cfg L G → Cfg L G → Prop
  | run {ls g cur sw l g'} : S cur (ls cur) g l g' → Step S N ⟨ls, g, cur, sw⟩ ⟨upd ls cur l, g', cur, sw⟩
  | switch {ls g cur sw} (t : Nat) : t < N → Step S N ⟨ls, g, cur, sw⟩ ⟨ls, g, t, sw + 1⟩

inductive Star (S : Sys L G) (N : Nat) : Cfg L G → Cfg L G → Prop
  | refl (c : Cfg L G) : Star S N c c
  | head {c d e : Cfg L G} : Step S N c d → Star S N d e → Star S N c e

theorem Star.trans {S : Sys L G} {N : Nat} {c d e : Cfg L G}
    (h1 : Star S N c d) (h2 : Star S N d e) : Star S N c e := by
  induction h1 with
  | refl => exact h2
  | head s _ ih => exact Star.head s (ih h2)

/-! ### Normal form: interleavings are runs along their schedule -/

theorem slots_of_star {S : Sys L G} {N : Nat} {c d : Cfg L G} (h : Star S N c d) :
    c.sw ≤ d.sw ∧ ∃ τ, τ.length = d.sw - c.sw ∧ (∀ t ∈ τ, t < N) ∧
      Slots S (c.cur :: τ) c.ls c.g d.ls d.g := by
  induction h with
  | refl c =>
    refine ⟨Nat.le_refl _, [], by simp, by simp, c.ls c.cur, c.g, Seg.refl _ _, ?_⟩
    simp [Slots]
  | @head c d e s _ ih =>
    obtain ⟨hle, τ, hlen, hlt, hsl⟩ := ih
    cases s with
    | @run ls g cur sw l g' hst =>
      obtain ⟨l2, m, hseg, hrest⟩ := hsl
      simp only [upd_same, upd_upd] at hseg hrest
      exact ⟨hle, τ, hlen, hlt, l2, m, Seg.head hst hseg, hrest⟩
    | @switch ls g cur sw t ht =>
      simp only at hle hlen hsl ⊢
      refine ⟨by omega, t :: τ, by simp; omega, ?_, ls cur, g, Seg.refl _ _, by simpa using hsl⟩
      intro u hu
      simp only [List.mem_cons] at hu
      rcases hu with rfl | hu
      · exact ht
      · exact hlt u hu

theorem star_of_seg {S : Sys L G} {N t : Nat} {l l' : L} {g g' : G} (h : Seg S t l g l' g') :
    ∀ (ls : Nat → L) (sw : Nat), ls t = l → Star S N ⟨ls, g, t, sw⟩ ⟨upd ls t l', g', t, sw⟩ := by
  induction h with
  | refl l g =>
    intro ls sw hl; subst hl; simpa using Star.refl (S := S) (N := N) ⟨ls, g, t, sw⟩
  | @head l g l1 g1 l2 g2 hst _ ih =>
    intro ls sw hl
    subst hl
    have := ih (upd ls t l1) sw (by simp)
    simp only [upd_upd] at this
    exact Star.head (Step.run hst) this

theorem star_of_slots {S : Sys L G} {N : Nat} :
    ∀ (τ : List Nat) (t0 : Nat) (ls ls' : Nat → L) (g g' : G) (sw : Nat), (∀ t ∈ τ, t < N) →
      Slots S (t0 :: τ) ls g ls' g' → ∃ c, Star S N ⟨ls, g, t0, sw⟩ ⟨ls', g', c, sw + τ.length⟩
  | [], t0, ls, ls', g, g', sw, _, ⟨l, m, hseg, hl, hg⟩ => by
    subst hl hg
    exact ⟨t0, by simpa using star_of_seg hseg ls sw rfl⟩
  | t1 :: τ, t0, ls, ls', g, g', sw, hlt, ⟨l, m, hseg, hrest⟩ => by
    obtain ⟨c, hst⟩ := star_of_slots τ t1 (upd ls t0 l) ls' m g' (sw + 1)
      (fun u hu => hlt u (List.mem_cons_of_mem _ hu)) hrest
    refine ⟨c, ?_⟩
    have h1 := star_of_seg (N := N) hseg ls sw rfl
    have h2 : Step S N ⟨upd ls t0 l, m, t0, sw⟩ ⟨upd ls t0 l, m, t1, sw + 1⟩ :=
      Step.switch t1 (hlt t1 List.mem_cons_self)
    have := h1.trans (Star.head h2 hst)
    simpa [Nat.add_assoc, Nat.add_comm 1] using this

/-! ### Extra slots only add runs -/

theorem slots_mono {S : Sys L G} {σ π : List Nat} (h : List.Sublist σ π) :
    ∀ {ls ls' : Nat → L} {g g' : G}, Slots S σ ls g ls' g' → Slots S π ls g ls' g' := by
  induction h with
  | slnil => exact id
  | cons a _ ih =>
    intro ls ls' g g' hs
    exact ⟨ls a, g, Seg.refl _ _, by simpa using ih hs⟩
  | cons_cons a _ ih =>
    intro ls ls' g g' hs
    obtain ⟨l, m, hseg, hrest⟩ := hs
    exact ⟨l, m, hseg, ih hrest⟩

/-! ### The round-robin pattern of `lazy.cpp` -/

/-- `K` rounds of threads `0, 1, …, N-1`. -/
def rr (N K : Nat) : List Nat := (List.replicate K (List.range N)).flatten

/-- `lazy.cpp`: `for r < rounds: for t < N: run_slot(r, t)`, then
`run_slot(rounds, 0)` (the harness's final slot). -/
def prismSched (N K : Nat) : List Nat := rr N K ++ [0]

theorem length_rr (N K : Nat) : (rr N K).length = K * N := by
  induction K with
  | zero => simp [rr]
  | succ K ih =>
    simp only [rr, List.replicate_succ, List.flatten_cons, List.length_append, List.length_range] at ih ⊢
    rw [ih, Nat.succ_mul, Nat.add_comm]

/-- The sequentialised program has `K·N + 1` slots, i.e. `K·N` context
switches (`lazy.cpp`: `context_switch_bound = rounds * N`). -/
theorem length_prismSched (N K : Nat) : (prismSched N K).length = K * N + 1 := by
  simp [prismSched, length_rr]

theorem mem_rr {N K t : Nat} (h : t ∈ rr N K) : t < N := by
  simp only [rr, List.mem_flatten, List.mem_replicate] at h
  obtain ⟨l, ⟨_, rfl⟩, ht⟩ := h
  simpa using ht

theorem prismSched_cons {N : Nat} (hN : 0 < N) (K : Nat) :
    ∃ τ, prismSched N K = 0 :: τ ∧ ∀ t ∈ τ, t < N := by
  cases K with
  | zero => exact ⟨[], by simp [prismSched, rr], by simp⟩
  | succ K =>
    obtain ⟨n, rfl⟩ : ∃ n, N = n + 1 := ⟨N - 1, by omega⟩
    refine ⟨List.range' 1 n ++ rr (n + 1) K ++ [0], ?_, ?_⟩
    · simp [prismSched, rr, List.replicate_succ, List.range_eq_range', List.range'_succ]
    · intro t ht
      simp only [List.mem_append, List.mem_range', List.mem_singleton] at ht
      rcases ht with (⟨i, hi, rfl⟩ | ht) | rfl
      · omega
      · exact mem_rr ht
      · omega

/-- A strictly increasing list of thread ids in `[a, a + n)` is a
subsequence of `a, a+1, …, a+n-1`. -/
theorem sublist_range' : ∀ (n a : Nat) (l : List Nat), l.Pairwise (· < ·) →
    (∀ x ∈ l, a ≤ x ∧ x < a + n) → List.Sublist l (List.range' a n)
  | 0, a, [], _, _ => List.Sublist.slnil
  | 0, a, x :: l, _, hb => by have := hb x List.mem_cons_self; omega
  | n + 1, a, [], _, _ => List.nil_sublist _
  | n + 1, a, x :: l, hp, hb => by
    rw [List.range'_succ]
    have hx := hb x List.mem_cons_self
    rcases Nat.eq_or_lt_of_le hx.1 with h | h
    · subst h
      refine List.Sublist.cons_cons _ (sublist_range' n (a + 1) l (List.pairwise_cons.1 hp).2 ?_)
      intro y hy
      have := (List.pairwise_cons.1 hp).1 y hy
      have := hb y (List.mem_cons_of_mem _ hy)
      omega
    · refine List.Sublist.cons _ (sublist_range' n (a + 1) (x :: l) hp ?_)
      intro y hy
      have := hb y hy
      rcases List.mem_cons.1 hy with rfl | hy
      · omega
      · have := (List.pairwise_cons.1 hp).1 y hy; omega

/-- **Round-robin coverage.**  A schedule made of at most `K` strictly
increasing runs of thread ids below `N` embeds in `K` round-robin rounds. -/
theorem rr_covers_runs (N : Nat) : ∀ (K : Nat) (runs : List (List Nat)), runs.length ≤ K →
    (∀ r ∈ runs, r.Pairwise (· < ·) ∧ ∀ x ∈ r, x < N) → List.Sublist runs.flatten (rr N K)
  | _, [], _, _ => List.nil_sublist _
  | 0, _ :: _, hl, _ => by simp at hl
  | K + 1, r :: rs, hl, hr => by
    simp only [rr, List.replicate_succ, List.flatten_cons]
    have h1 := hr r List.mem_cons_self
    refine List.Sublist.append ?_ (rr_covers_runs N K rs (by simp at hl; omega)
      (fun r' h' => hr r' (List.mem_cons_of_mem _ h')))
    rw [List.range_eq_range']
    exact sublist_range' N 0 r h1.1 (fun x hx => ⟨Nat.zero_le _, by simpa using h1.2 x hx⟩)

/-! ### Main theorems -/

/-- **No spurious runs.**  Every run of the sequentialised program is a real
interleaving from thread 0 with `K·N` context switches. -/
theorem lazy_sound {S : Sys L G} {N : Nat} (hN : 0 < N) (K : Nat) (ls ls' : Nat → L) (g g' : G)
    (h : Slots S (prismSched N K) ls g ls' g') :
    ∃ c, Star S N ⟨ls, g, 0, 0⟩ ⟨ls', g', c, K * N⟩ := by
  obtain ⟨τ, hτ, hlt⟩ := prismSched_cons hN K
  have hlen : τ.length = K * N := by
    have := length_prismSched N K; rw [hτ] at this; simpa using this
  rw [hτ] at h
  obtain ⟨c, hst⟩ := star_of_slots τ 0 ls ls' g g' 0 hlt h
  exact ⟨c, by simpa [hlen] using hst⟩

/-- **Coverage (schedule shape).**  An interleaving from thread 0 whose
schedule `0 :: τ` splits into at most `K` strictly increasing runs is
matched by a run of the sequentialised program, reaching the same state. -/
theorem lazy_covers_runs {S : Sys L G} {N : Nat} (K : Nat) (τ : List Nat) (runs : List (List Nat))
    (hruns : runs.flatten = 0 :: τ) (hlen : runs.length ≤ K)
    (hinc : ∀ r ∈ runs, r.Pairwise (· < ·) ∧ ∀ x ∈ r, x < N)
    (ls ls' : Nat → L) (g g' : G) (h : Slots S (0 :: τ) ls g ls' g') :
    Slots S (prismSched N K) ls g ls' g' := by
  have hsub := rr_covers_runs N K runs hlen hinc
  rw [hruns] at hsub
  exact slots_mono (hsub.trans (List.sublist_append_left _ _)) h

/-- **Coverage (switch count).**  Every interleaving that starts in thread 0
and makes at most `K - 1` context switches, reaching any state, is matched by
a run of the sequentialised program with `K` rounds. -/
theorem lazy_covers {S : Sys L G} {N : Nat} (hN : 0 < N) (K : Nat) (ls ls' : Nat → L) (g g' : G)
    (c : Nat) (sw : Nat) (hrun : Star S N ⟨ls, g, 0, 0⟩ ⟨ls', g', c, sw⟩) (hbound : sw + 1 ≤ K) :
    Slots S (prismSched N K) ls g ls' g' := by
  obtain ⟨_, τ, hlen, hlt, hs⟩ := slots_of_star hrun
  simp only [Nat.sub_zero] at hlen
  refine lazy_covers_runs K τ ((0 :: τ).map (fun x => [x]))
    (by simp only [List.map_cons, List.flatten_cons]; congr 1; clear hs hlt hlen; induction τ <;> simp_all)
    (by simp; omega) ?_ ls ls' g g' hs
  intro r hr
  simp only [List.mem_map, List.mem_cons] at hr
  obtain ⟨x, hx, rfl⟩ := hr
  refine ⟨by simp, ?_⟩
  intro y hy
  simp only [List.mem_singleton] at hy
  subst hy
  rcases hx with rfl | hx
  · exact hN
  · exact hlt _ hx

/-- The whole picture in one statement: for `K ≥ 1` rounds the states the
sequentialised program reaches lie between the interleavings with at most
`K - 1` switches and the interleavings with at most `K·N` switches. -/
theorem lazy_between {S : Sys L G} {N : Nat} (hN : 0 < N) (K : Nat) (ls : Nat → L) (g : G) :
    (∀ ls' g' c sw, Star S N ⟨ls, g, 0, 0⟩ ⟨ls', g', c, sw⟩ → sw + 1 ≤ K →
      Slots S (prismSched N K) ls g ls' g') ∧
    (∀ ls' g', Slots S (prismSched N K) ls g ls' g' →
      ∃ c, Star S N ⟨ls, g, 0, 0⟩ ⟨ls', g', c, K * N⟩) :=
  ⟨fun ls' g' c sw h hb => lazy_covers hN K ls ls' g g' c sw h hb,
   fun ls' g' h => lazy_sound hN K ls ls' g g' h⟩

/-! ### Two threads (the harness and one created thread) -/

theorem seg_trans {S : Sys L G} {t : Nat} {l l1 l2 : L} {g g1 g2 : G}
    (h1 : Seg S t l g l1 g1) (h2 : Seg S t l1 g1 l2 g2) : Seg S t l g l2 g2 := by
  induction h1 with
  | refl => exact h2
  | head s _ ih => exact Seg.head s (ih h2)

/-- Two consecutive slots of one thread are one slot. -/
theorem slots_merge {S : Sys L G} {t : Nat} {π : List Nat} {ls ls' : Nat → L} {g g' : G}
    (h : Slots S (t :: t :: π) ls g ls' g') : Slots S (t :: π) ls g ls' g' := by
  obtain ⟨l, m, h1, l2, m2, h2, hr⟩ := h
  simp only [upd_same, upd_upd] at h2 hr
  exact ⟨l2, m2, seg_trans h1 h2, hr⟩

theorem prismSched_two_succ (K : Nat) : prismSched 2 (K + 1) = 0 :: 1 :: prismSched 2 K := by
  simp [prismSched, rr, List.replicate_succ, List.range_succ]

theorem two_aux {S : Sys L G} : ∀ (τ : List Nat) (a K : Nat) (ls ls' : Nat → L) (g g' : G),
    a < 2 → (∀ t ∈ τ, t < 2) → τ.length ≤ 2 * K + a → Slots S (a :: τ) ls g ls' g' →
    Slots S (if a = 0 then prismSched 2 K else 1 :: prismSched 2 K) ls g ls' g'
  | [], a, K, ls, ls', g, g', ha, _, _, h => by
    obtain ⟨τ, hτ, _⟩ := prismSched_cons (N := 2) (by decide) K
    refine slots_mono ?_ h
    rcases (show a = 0 ∨ a = 1 by omega) with rfl | rfl
    · simp only [hτ]; exact List.Sublist.cons_cons _ (List.nil_sublist _)
    · simp only [show (1 : Nat) ≠ 0 by decide, ite_false]
      exact List.Sublist.cons_cons _ (List.nil_sublist _)
  | b :: τ, a, K, ls, ls', g, g', ha, hlt, hlen, h => by
    have hb := hlt b List.mem_cons_self
    have hlt' : ∀ t ∈ τ, t < 2 := fun t ht => hlt t (List.mem_cons_of_mem _ ht)
    simp only [List.length_cons] at hlen
    by_cases hab : b = a
    · subst hab
      exact two_aux τ b K ls ls' g g' ha hlt' (by omega) (slots_merge h)
    · obtain ⟨l, m, hseg, hrest⟩ := h
      rcases (show a = 0 ∨ a = 1 by omega) with rfl | rfl
      · have hb1 : b = 1 := by omega
        subst hb1
        cases K with
        | zero => omega
        | succ K =>
          have := two_aux τ 1 K (upd ls 0 l) ls' m g' (by decide) hlt' (by omega) hrest
          simp only [show (1 : Nat) ≠ 0 by decide, ite_false] at this
          simp only [prismSched_two_succ]
          exact ⟨l, m, hseg, this⟩
      · have hb0 : b = 0 := by omega
        subst hb0
        have := two_aux τ 0 K (upd ls 1 l) ls' m g' (by decide) hlt' (by omega) hrest
        simp only at this
        simp only [show (1 : Nat) ≠ 0 by decide, ite_false]
        exact ⟨l, m, hseg, this⟩

/-- **Two threads** (the harness `T0` and one created thread, `N = 2`):
every interleaving with at most `2K` context switches is covered by `K`
rounds plus the harness's final slot — the bound `K·N = 2K` is reached. -/
theorem lazy_covers_two {S : Sys L G} (K : Nat) (ls ls' : Nat → L) (g g' : G) (c sw : Nat)
    (hrun : Star S 2 ⟨ls, g, 0, 0⟩ ⟨ls', g', c, sw⟩) (hbound : sw ≤ 2 * K) :
    Slots S (prismSched 2 K) ls g ls' g' := by
  obtain ⟨_, τ, hlen, hlt, hs⟩ := slots_of_star hrun
  simp only [Nat.sub_zero] at hlen
  have := two_aux τ 0 K ls ls' g g' (by decide) hlt (by omega) hs
  simpa using this

/-! ### A bound per thread is not enough (N = 3, K = 1) -/

/-- Thread 1 doubles `g` once, thread 2 increments it once, thread 0 (the
harness) does nothing; the local state records "done". -/
def demo : Sys Bool Nat := fun t l g l' g' =>
  l = false ∧ l' = true ∧ ((t = 1 ∧ g' = 2 * g) ∨ (t = 2 ∧ g' = g + 1))

theorem demo_done {t : Nat} {l l' : Bool} {g g' : Nat} (h : Seg demo t l g l' g') :
    l = true → l' = true ∧ g' = g := by
  induction h with
  | refl => intro h; exact ⟨h, rfl⟩
  | head hs _ _ => intro h; exact absurd h (by simp [hs.1])

theorem demo_seg {t : Nat} {l l' : Bool} {g g' : Nat} (h : Seg demo t l g l' g') :
    g' = g ∨ (t = 1 ∧ g' = 2 * g) ∨ (t = 2 ∧ g' = g + 1) := by
  cases h with
  | refl => exact Or.inl rfl
  | head hs hr =>
    obtain ⟨_, h1, h2⟩ := hs
    obtain ⟨_, rfl⟩ := demo_done hr h1
    exact Or.inr h2

/-- The schedule `0, 2, 1` — each thread runs one segment, no thread is
preempted — reaches `g = 4` from `g = 1`. -/
theorem demo_reaches : ∃ ls', Slots demo [0, 2, 1] (fun _ => false) 1 ls' 4 :=
  ⟨_, false, 1, Seg.refl _ _, true, 2, Seg.head (t := 2) ⟨rfl, rfl, Or.inr ⟨rfl, rfl⟩⟩ (Seg.refl _ _),
    true, 4, Seg.head (t := 1) ⟨by simp [upd], rfl, Or.inl ⟨rfl, rfl⟩⟩ (Seg.refl _ _), rfl, rfl⟩

/-- **Limit.**  One round of three threads (`prismSched 3 1` = `0 1 2 0`)
never reaches `g = 4`: a bound on context switches per thread does not
imply coverage for `N ≥ 3`; the schedule must fit the round-robin pattern
(`lazy_covers_runs`). -/
theorem per_thread_bound_not_enough :
    (∃ ls', Slots demo [0, 2, 1] (fun _ => false) 1 ls' 4) ∧
    ¬ ∃ ls', Slots demo (prismSched 3 1) (fun _ => false) 1 ls' 4 := by
  refine ⟨demo_reaches, ?_⟩
  rw [show prismSched 3 1 = [0, 1, 2, 0] by decide]
  rintro ⟨ls', l0, m0, h0, l1, m1, h1, l2, m2, h2, l3, m3, h3, _, hg⟩
  have e0 := demo_seg h0
  have e1 := demo_seg h1
  have e2 := demo_seg h2
  have e3 := demo_seg h3
  omega

end PrismTechniques.LazySeqN
