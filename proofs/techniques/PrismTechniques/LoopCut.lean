/-
PRISM techniques: the pir loop-cut encoding with Houdini invariants
(roadmap 8.2, row "Loop invariants"; M8; `src/prism/pir/houdini.inc`,
docs/PIR.md "Loop invariants"), and the sequential guard of `assume`
(soundness bug S9, docs/CONFORMANCE.md).

**Programs.**  A structured program over an abstract state `S`: blocks of
statements (`upd` — an assignment or a havoc, any relation; `check` — a
property instance; `assume`), sequencing, branches, and loops with any
number of exits (`brk`) and latches (falling off the body, or `cont`), and
`ret`.  Loops nest and follow each other freely; each carries an id `L`
(invariants and havoc relations are per loop).

**Sequential semantics** (`BExec`, `Exec`): the uncut program.  A failed
check ends the run with `err`; a false `assume` blocks the run (no outcome).
`Exec P s .err` is "some execution of `P` from `s` violates a property"; it
is inductive, so it covers every execution that reaches a violation after
any finite number of loop iterations, however deep the nesting.

**Part 1 — the per-statement guard (S9).**  encode.cpp `encode_node` gives
every statement of a block the guard `r` = the node's reach strengthened by
every `assume` already passed (`r := r && cond`), and every value is
defined whatever the guard.  `encViol_iff` proves: some property instance
of a block is violated under its guard iff the sequential semantics fails a
check (`encExit_iff`: the exit guard `exit_reach` with no violated instance
iff the block completes in that state).  The encoding before the fix made
each `assume` an axiom of the whole node (`OldViol`); `old_viol_imp`
shows it only lost alarms, and `s9_old_encoding_misses` is the S9 program
(`100 / x; assume(x != 0)` with `x = 0`) that it missed.

**Part 2 — the loop cut.**  PRISM encodes every loop once (cut mode,
unwind 1): at the header the entry state `e` is recorded, the loop state is
havocked to any `s` with `Hav L e s` (the header phis and the write
footprint), the invariants `Inv L e s` of the loop are assumed there, the
body runs once, and every back edge is a cut that records the next state
`t`.  Houdini's queries at its fixpoint are

* base:  at every header, `Inv L e e`;
* step:  at every back edge, `Inv L e t`;
* final: no property instance of the cut program is violated;

each asked with the invariants of the loops before the point assumed at
their havocked headers, guarded by "no property violated before this
header".  `GRun` is that encoding as a path semantics: the path does not
stop at a violation (`v` records one), a base/step goal that fails where no
property was violated before sets `q`, and a havoc assumption is active only
while neither flag is set (so each query assumes exactly the loops before
its point on the path; an earlier violation switches the assumptions off,
as the guard `noviol(props_before)` does).  A complete `GRun` path from
the start with `v ∨ q` at the end is a model of one of Houdini's queries
(the first flag set says which), so "every query UNSAT" gives the premise of
`loopcut_sound`:

  **`loopcut_sound`**: if no complete path of the cut encoding sets `v` or
  `q`, then no execution of the uncut program violates a property —
  provided every loop's havoc covers its header states (`FrameOK`: `Hav L e
  e`, and a body iteration that ends at a latch without violation from a
  state in `Hav L e` stays in `Hav L e`; for PIR: phis are havocked and
  every byte the loop writes is in its footprint, the loop allocates and
  frees nothing) and the encoding's values always exist (`Total`: every
  `upd` is total, as Z3 assignments and fresh havoc constants are).

The proof goes through the sequential cut semantics `CExec` (first failure
stops; `exec_cut`: a violating execution of the uncut program gives a
failing run of the cut program, by induction over the iterations with the
entry state fixed) and `cexec_grun` (a failing cut run is a flagged
complete encoding path; the guard of the havoc assumptions is what lets the
path be completed after a violation).

What is modelled, not proved of the C++: the encoder's DAG and Z3 formula
are represented by this path semantics (on the one path a model makes
reachable, topological order is execution order); PIR is a CFG, here a
structured program with multi-exit, multi-latch loops; the templates,
`SymMem`, the footprint analysis and the Z3 answers are the premises
(`FrameOK`, and that the queries are UNSAT).
-/

namespace PrismTechniques.LoopCut

/-! ## Programs and the sequential semantics -/

inductive Stmt (S : Type) where
  | upd (R : S → S → Prop)
  | check (c : S → Prop)
  | assume (c : S → Prop)

inductive Prog (S : Type) where
  | blk (b : List (Stmt S))
  | seq (p q : Prog S)
  | ite (c : S → Prop) (p q : Prog S)
  | loop (L : Nat) (body : Prog S)
  | brk
  | cont
  | ret

/-- Outcomes.  `err`: a property is violated.  `cut`: the path ended at a back
edge of the cut program.  `dead`: the encoding path ended at a false
`assume`.  `Exec` produces neither `cut` nor `dead`. -/
inductive Out (S : Type) where
  | norm (s : S)
  | brk (s : S)
  | cont (s : S)
  | ret (s : S)
  | err
  | cut
  | dead

variable {S : Type}

/-- A body outcome that goes back to the header (a latch), with the state. -/
def latchState : Out S → Option S
  | .norm s => some s
  | .cont s => some s
  | _ => none

/-- A body outcome that leaves the loop, with the loop's outcome. -/
def exitOut : Out S → Option (Out S)
  | .norm _ => none
  | .cont _ => none
  | .brk s => some (.norm s)
  | o => some o

/-- Sequential semantics of a block: `none` is a failed check. -/
inductive BExec : List (Stmt S) → S → Option S → Prop
  | nil {s} : BExec [] s (some s)
  | upd {R rest s s' o} : R s s' → BExec rest s' o → BExec (.upd R :: rest) s o
  | checkOk {c rest s o} : c s → BExec rest s o → BExec (.check c :: rest) s o
  | checkFail {c rest s} : ¬ c s → BExec (.check c :: rest) s none
  | assume {c rest s o} : c s → BExec rest s o → BExec (.assume c :: rest) s o

/-- Sequential semantics of the uncut program. -/
inductive Exec : Prog S → S → Out S → Prop
  | blkOk {b s t} : BExec b s (some t) → Exec (.blk b) s (.norm t)
  | blkErr {b s} : BExec b s none → Exec (.blk b) s .err
  | seqN {p q s t o} : Exec p s (.norm t) → Exec q t o → Exec (.seq p q) s o
  | seqX {p q s o} : Exec p s o → (∀ t, o ≠ .norm t) → Exec (.seq p q) s o
  | iteT {c p q s o} : c s → Exec p s o → Exec (.ite c p q) s o
  | iteF {c p q s o} : ¬ c s → Exec q s o → Exec (.ite c p q) s o
  | brk {s} : Exec .brk s (.brk s)
  | cont {s} : Exec .cont s (.cont s)
  | ret {s} : Exec .ret s (.ret s)
  | loopNext {L b s o t o'} :
      Exec b s o → latchState o = some t → Exec (.loop L b) t o' → Exec (.loop L b) s o'
  | loopExit {L b s o o'} : Exec b s o → exitOut o = some o' → Exec (.loop L b) s o'

/-! ## Part 1: the per-statement guard of encode.cpp (S9) -/

/-- What statement `st` requires of the state before it and after it. -/
def stepOK : Stmt S → S → S → Prop
  | .upd R, s, s' => R s s'
  | _, s, s' => s' = s

/-- `ss 0, …, ss n` are the states before the first `n + 1` statements (Z3
defines every value, whatever the guard). -/
def Chain : List (Stmt S) → (Nat → S) → Nat → Prop
  | _, _, 0 => True
  | [], _, _ + 1 => True
  | st :: rest, ss, n + 1 => stepOK st (ss 0) (ss 1) ∧ Chain rest (fun k => ss (k + 1)) n

/-- The contribution of one statement to the running guard. -/
def assumeOK : Stmt S → S → Prop
  | .assume c, s => c s
  | _, _ => True

/-- The running guard before statement `n` (`r := r && cond` at every
`assume`, encode.cpp `encode_node`), relative to the node's reach. -/
def guard : List (Stmt S) → (Nat → S) → Nat → Prop
  | _, _, 0 => True
  | [], _, _ + 1 => True
  | st :: rest, ss, n + 1 => assumeOK st (ss 0) ∧ guard rest (fun k => ss (k + 1)) n

/-- Statement `n` is a check whose condition is false. -/
def instViol : List (Stmt S) → (Nat → S) → Nat → Prop
  | [], _, _ => False
  | .check c :: _, ss, 0 => ¬ c (ss 0)
  | .upd _ :: _, _, 0 => False
  | .assume _ :: _, _, 0 => False
  | _ :: rest, ss, n + 1 => instViol rest (fun k => ss (k + 1)) n

/-- The encoding (after the S9 fix) reports a violation in block `b`: some
property instance is violated under its own guard. -/
def EncViol (b : List (Stmt S)) (s0 : S) : Prop :=
  ∃ (ss : Nat → S) (n : Nat), ss 0 = s0 ∧ Chain b ss n ∧ guard b ss n ∧ instViol b ss n

/-- The encoding before the fix: every `assume` of the node is an axiom
(`reach(node) => cond`), so the whole block's assumptions constrain every
instance in it. -/
def OldViol (b : List (Stmt S)) (s0 : S) : Prop :=
  ∃ (ss : Nat → S) (n : Nat), ss 0 = s0 ∧ Chain b ss b.length ∧ guard b ss b.length ∧
    instViol b ss n

/-- `cons s ss` is the state sequence `s, ss 0, ss 1, …`. -/
def cons (s : S) (ss : Nat → S) : Nat → S
  | 0 => s
  | k + 1 => ss k

theorem encViol_seq {b : List (Stmt S)} :
    ∀ (ss : Nat → S) (n : Nat), Chain b ss n → guard b ss n → instViol b ss n →
      BExec b (ss 0) none := by
  induction b with
  | nil => intro ss n _ _ hv; cases n <;> exact absurd hv id
  | cons st rest ih =>
    intro ss n hc hg hv
    cases n with
    | zero =>
      cases st with
      | check c => exact BExec.checkFail hv
      | upd R => exact absurd hv id
      | assume c => exact absurd hv id
    | succ n =>
      obtain ⟨hst, hc'⟩ := hc
      obtain ⟨ha, hg'⟩ := hg
      have hr := ih (fun k => ss (k + 1)) n hc' hg' (by cases st <;> exact hv)
      cases st with
      | upd R => exact BExec.upd hst hr
      | check c =>
        have h1 : ss 1 = ss 0 := hst
        by_cases hcs : c (ss 0)
        · exact BExec.checkOk hcs (h1 ▸ hr)
        · exact BExec.checkFail hcs
      | assume c =>
        have h1 : ss 1 = ss 0 := hst
        exact BExec.assume ha (h1 ▸ hr)

theorem seq_encViol {b : List (Stmt S)} {s0 : S} {o : Option S} (h : BExec b s0 o) :
    o = none → ∃ (ss : Nat → S) (n : Nat), ss 0 = s0 ∧ Chain b ss n ∧ guard b ss n ∧
      instViol b ss n := by
  induction h with
  | nil => intro h; cases h
  | @upd R rest s s' o hR _ ih =>
    intro ho
    obtain ⟨ss, n, h0, hc, hg, hv⟩ := ih ho
    refine ⟨cons s ss, n + 1, rfl, ⟨?_, hc⟩, ⟨trivial, hg⟩, hv⟩
    show R s (ss 0); rw [h0]; exact hR
  | @checkOk c rest s o hcs _ ih =>
    intro ho
    obtain ⟨ss, n, h0, hc, hg, hv⟩ := ih ho
    exact ⟨cons s ss, n + 1, rfl, ⟨h0, hc⟩, ⟨trivial, hg⟩, hv⟩
  | @checkFail c rest s hcs =>
    intro _
    exact ⟨fun _ => s, 0, rfl, trivial, trivial, hcs⟩
  | @assume c rest s o hcs _ ih =>
    intro ho
    obtain ⟨ss, n, h0, hc, hg, hv⟩ := ih ho
    exact ⟨cons s ss, n + 1, rfl, ⟨h0, hc⟩, ⟨hcs, hg⟩, hv⟩

/-- **S9: the per-statement guard is the sequential semantics.**  Some
property instance of a block is violated under its running guard iff the
block, run statement by statement, fails a check. -/
theorem encViol_iff (b : List (Stmt S)) (s0 : S) : EncViol b s0 ↔ BExec b s0 none := by
  constructor
  · rintro ⟨ss, n, rfl, hc, hg, hv⟩
    exact encViol_seq ss n hc hg hv
  · intro h
    exact seq_encViol h rfl

/-- The exit guard: the block's statements all encoded under a true guard
and no instance violated, ending in `t`. -/
def EncExit (b : List (Stmt S)) (s0 t : S) : Prop :=
  ∃ ss : Nat → S, ss 0 = s0 ∧ Chain b ss b.length ∧ guard b ss b.length ∧
    (∀ n, ¬ instViol b ss n) ∧ ss b.length = t

theorem encExit_seq {b : List (Stmt S)} :
    ∀ (ss : Nat → S), Chain b ss b.length → guard b ss b.length → (∀ n, ¬ instViol b ss n) →
      BExec b (ss 0) (some (ss b.length)) := by
  induction b with
  | nil => intro ss _ _ _; exact BExec.nil
  | cons st rest ih =>
    intro ss hc hg hv
    obtain ⟨hst, hc'⟩ := hc
    obtain ⟨ha, hg'⟩ := hg
    have hv' : ∀ n, ¬ instViol rest (fun k => ss (k + 1)) n := by
      intro n h
      apply hv (n + 1)
      cases st <;> exact h
    have hr := ih (fun k => ss (k + 1)) hc' hg' hv'
    cases st with
    | upd R => exact BExec.upd hst hr
    | check c =>
      have h1 : ss 1 = ss 0 := hst
      have hcs : c (ss 0) := Classical.byContradiction fun h => hv 0 h
      exact BExec.checkOk hcs (h1 ▸ hr)
    | assume c =>
      have h1 : ss 1 = ss 0 := hst
      exact BExec.assume ha (h1 ▸ hr)

theorem seq_encExit {b : List (Stmt S)} {s0 : S} {o : Option S} (h : BExec b s0 o) :
    ∀ t, o = some t → EncExit b s0 t := by
  induction h with
  | nil => intro t ht; cases ht; exact ⟨fun _ => _, rfl, trivial, trivial, fun n h => by cases n <;> exact h, rfl⟩
  | @upd R rest s s' o hR _ ih =>
    intro t ht
    obtain ⟨ss, h0, hc, hg, hv, hn⟩ := ih t ht
    refine ⟨cons s ss, rfl, ⟨?_, hc⟩, ⟨trivial, hg⟩, ?_, hn⟩
    · show R s (ss 0); rw [h0]; exact hR
    · intro n h; cases n with
      | zero => exact h
      | succ n => exact hv n h
  | @checkOk c rest s o hcs _ ih =>
    intro t ht
    obtain ⟨ss, h0, hc, hg, hv, hn⟩ := ih t ht
    refine ⟨cons s ss, rfl, ⟨h0, hc⟩, ⟨trivial, hg⟩, ?_, hn⟩
    intro n h; cases n with
    | zero => exact h hcs
    | succ n => exact hv n h
  | checkFail _ => intro t ht; cases ht
  | @assume c rest s o hcs _ ih =>
    intro t ht
    obtain ⟨ss, h0, hc, hg, hv, hn⟩ := ih t ht
    refine ⟨cons s ss, rfl, ⟨h0, hc⟩, ⟨hcs, hg⟩, ?_, hn⟩
    intro n h; cases n with
    | zero => exact h
    | succ n => exact hv n h

/-- **S9, exit guard.**  The block's exit guard (`exit_reach`: the reach
strengthened by all its assumes) holds with no violated instance, ending in
`t`, iff the block runs sequentially to `t`. -/
theorem encExit_iff (b : List (Stmt S)) (s0 t : S) : EncExit b s0 t ↔ BExec b s0 (some t) := by
  constructor
  · rintro ⟨ss, rfl, hc, hg, hv, rfl⟩
    exact encExit_seq ss hc hg hv
  · intro h
    exact seq_encExit h t rfl

theorem chain_le {b : List (Stmt S)} :
    ∀ (ss : Nat → S) (m n : Nat), n ≤ m → Chain b ss m → Chain b ss n := by
  induction b with
  | nil => intro ss m n _ _; cases n <;> trivial
  | cons st rest ih =>
    intro ss m n hnm hc
    cases n with
    | zero => trivial
    | succ n =>
      cases m with
      | zero => omega
      | succ m => exact ⟨hc.1, ih _ m n (by omega) hc.2⟩

theorem guard_le {b : List (Stmt S)} :
    ∀ (ss : Nat → S) (m n : Nat), n ≤ m → guard b ss m → guard b ss n := by
  induction b with
  | nil => intro ss m n _ _; cases n <;> trivial
  | cons st rest ih =>
    intro ss m n hnm hc
    cases n with
    | zero => trivial
    | succ n =>
      cases m with
      | zero => omega
      | succ m => exact ⟨hc.1, ih _ m n (by omega) hc.2⟩

theorem instViol_lt {b : List (Stmt S)} :
    ∀ (ss : Nat → S) (n : Nat), instViol b ss n → n < b.length := by
  induction b with
  | nil => intro ss n h; exact absurd h id
  | cons st rest ih =>
    intro ss n h
    cases n with
    | zero => simp
    | succ n =>
      have := ih _ n (by cases st <;> exact h)
      simp; omega

/-- The old encoding only lost alarms: every violation it reported is one the
fixed encoding reports. -/
theorem old_viol_imp (b : List (Stmt S)) (s0 : S) : OldViol b s0 → EncViol b s0 := by
  rintro ⟨ss, n, h0, hc, hg, hv⟩
  have hn : n ≤ b.length := Nat.le_of_lt (instViol_lt ss n hv)
  exact ⟨ss, n, h0, chain_le ss _ n hn hc, guard_le ss _ n hn hg, hv⟩

/-- **The S9 program.**  `q = 100 / x; __VERIFIER_assume(x != 0);` with
`x = 0`: the division check fails first (sequentially, and in the fixed
encoding), but the old encoding, whose `assume` constrained the whole node,
reports nothing. -/
theorem s9_old_encoding_misses :
    let b : List (Stmt Int) := [.check (fun x => x ≠ 0), .assume (fun x => x ≠ 0)]
    BExec b 0 none ∧ EncViol b 0 ∧ ¬ OldViol b 0 := by
  intro b
  have h1 : BExec b 0 none := BExec.checkFail (by decide)
  refine ⟨h1, (encViol_iff b 0).2 h1, ?_⟩
  rintro ⟨ss, n, h0, hc, hg, _⟩
  obtain ⟨hs1, -⟩ := hc
  obtain ⟨-, ha, -⟩ := hg
  have e1 : ss 1 = ss 0 := hs1
  have : ss 1 ≠ 0 := ha
  exact this (e1.trans h0)

/-! ## Part 2: the loop cut -/

section Cut

variable (Inv : Nat → S → S → Prop) (Hav : Nat → S → S → Prop)

/-- The cut program, run sequentially (the first failure ends the run).  At
the header of loop `L` with entry state `e`: the base goal `Inv L e e`
(failing it is an `err`), then the havoc to `s` with `Hav L e s` and the
assumption `Inv L e s`, then the body once; a latch is a cut whose step goal
`Inv L e t` must hold. -/
inductive CExec : Prog S → S → Out S → Prop
  | blkOk {b s t} : BExec b s (some t) → CExec (.blk b) s (.norm t)
  | blkErr {b s} : BExec b s none → CExec (.blk b) s .err
  | seqN {p q s t o} : CExec p s (.norm t) → CExec q t o → CExec (.seq p q) s o
  | seqX {p q s o} : CExec p s o → (∀ t, o ≠ .norm t) → CExec (.seq p q) s o
  | iteT {c p q s o} : c s → CExec p s o → CExec (.ite c p q) s o
  | iteF {c p q s o} : ¬ c s → CExec q s o → CExec (.ite c p q) s o
  | brk {s} : CExec .brk s (.brk s)
  | cont {s} : CExec .cont s (.cont s)
  | ret {s} : CExec .ret s (.ret s)
  | loopBase {L b e} : ¬ Inv L e e → CExec (.loop L b) e .err
  | loopStep {L b e s o t} :
      Inv L e e → Hav L e s → Inv L e s → CExec b s o → latchState o = some t →
      ¬ Inv L e t → CExec (.loop L b) e .err
  | loopCut {L b e s o t} :
      Inv L e e → Hav L e s → Inv L e s → CExec b s o → latchState o = some t →
      Inv L e t → CExec (.loop L b) e .cut
  | loopExit {L b e s o o'} :
      Inv L e e → Hav L e s → Inv L e s → CExec b s o → exitOut o = some o' →
      CExec (.loop L b) e o'

/-- The havoc of every loop covers its header states: the entry state is one
of them, and a body iteration that returns to the header without a
violation (on the uncut program) keeps a covered state covered. -/
def FrameOK : Prog S → Prop
  | .blk _ => True
  | .seq p q => FrameOK p ∧ FrameOK q
  | .ite _ p q => FrameOK p ∧ FrameOK q
  | .loop L b =>
      (∀ e, Hav L e e) ∧
      (∀ e s o t, Exec b s o → latchState o = some t → Hav L e s → Hav L e t) ∧ FrameOK b
  | .brk => True
  | .cont => True
  | .ret => True

/-- **The loop-cut argument.**  Every execution of the uncut program either
is matched by a run of the cut program with the same outcome, or the cut
program fails (a property, or a base or step goal); and from a covered
header state with the invariant holding, the rest of a loop's execution is
matched by the cut loop entered at `e`.  (Induction over the execution,
i.e. over the header visits, with the entry state fixed.) -/
theorem exec_cut {P : Prog S} {s : S} {o : Out S} (h : Exec P s o) :
    FrameOK Hav P →
    (CExec Inv Hav P s .err ∨ CExec Inv Hav P s o) ∧
    (∀ L b, P = .loop L b → ∀ e, Hav L e s → Inv L e e → Inv L e s →
      CExec Inv Hav (.loop L b) e .err ∨ CExec Inv Hav (.loop L b) e o) := by
  induction h with
  | blkOk hb => exact fun _ => ⟨Or.inr (CExec.blkOk hb), fun _ _ h => by cases h⟩
  | blkErr hb => exact fun _ => ⟨Or.inr (CExec.blkErr hb), fun _ _ h => by cases h⟩
  | seqN _ _ ihp ihq =>
    intro hF
    refine ⟨?_, fun _ _ h => by cases h⟩
    rcases (ihp hF.1).1 with h1 | h1
    · exact Or.inl (CExec.seqX h1 (fun _ h => by cases h))
    · rcases (ihq hF.2).1 with h2 | h2
      · exact Or.inl (CExec.seqN h1 h2)
      · exact Or.inr (CExec.seqN h1 h2)
  | seqX _ hne ihp =>
    intro hF
    refine ⟨?_, fun _ _ h => by cases h⟩
    rcases (ihp hF.1).1 with h1 | h1
    · exact Or.inl (CExec.seqX h1 (fun _ h => by cases h))
    · exact Or.inr (CExec.seqX h1 hne)
  | iteT hc _ ihp =>
    intro hF
    refine ⟨?_, fun _ _ h => by cases h⟩
    rcases (ihp hF.1).1 with h1 | h1
    · exact Or.inl (CExec.iteT hc h1)
    · exact Or.inr (CExec.iteT hc h1)
  | iteF hc _ ihq =>
    intro hF
    refine ⟨?_, fun _ _ h => by cases h⟩
    rcases (ihq hF.2).1 with h1 | h1
    · exact Or.inl (CExec.iteF hc h1)
    · exact Or.inr (CExec.iteF hc h1)
  | brk => exact fun _ => ⟨Or.inr CExec.brk, fun _ _ h => by cases h⟩
  | cont => exact fun _ => ⟨Or.inr CExec.cont, fun _ _ h => by cases h⟩
  | ret => exact fun _ => ⟨Or.inr CExec.ret, fun _ _ h => by cases h⟩
  | @loopNext L b s o t o' hb hl _ ihb ihL =>
    intro hF
    have hF' := hF
    obtain ⟨hrefl, hpres, hFb⟩ := hF
    have part2 : ∀ e, Hav L e s → Inv L e e → Inv L e s →
        CExec Inv Hav (.loop L b) e .err ∨ CExec Inv Hav (.loop L b) e o' := by
      intro e he hee hes
      rcases (ihb hFb).1 with h1 | h1
      · exact Or.inl (CExec.loopExit hee he hes h1 rfl)
      · by_cases hit : Inv L e t
        · exact (ihL hF').2 L b rfl e (hpres e s o t hb hl he) hee hit
        · exact Or.inl (CExec.loopStep hee he hes h1 hl hit)
    refine ⟨?_, fun L' b' h => ?_⟩
    · by_cases hss : Inv L s s
      · exact part2 s (hrefl s) hss hss
      · exact Or.inl (CExec.loopBase hss)
    · cases h; exact part2
  | @loopExit L b s o o' _ hx ihb =>
    intro hF
    obtain ⟨hrefl, _, hFb⟩ := hF
    have part2 : ∀ e, Hav L e s → Inv L e e → Inv L e s →
        CExec Inv Hav (.loop L b) e .err ∨ CExec Inv Hav (.loop L b) e o' := by
      intro e he hee hes
      rcases (ihb hFb).1 with h1 | h1
      · exact Or.inl (CExec.loopExit hee he hes h1 rfl)
      · exact Or.inr (CExec.loopExit hee he hes h1 hx)
    refine ⟨?_, fun L' b' h => ?_⟩
    · by_cases hss : Inv L s s
      · exact part2 s (hrefl s) hss hss
      · exact Or.inl (CExec.loopBase hss)
    · cases h; exact part2

/-- A violation of the uncut program is a failure of the cut program. -/
theorem exec_err_cut {P : Prog S} {s : S} (hF : FrameOK Hav P) (h : Exec P s .err) :
    CExec Inv Hav P s .err := by
  rcases (exec_cut Inv Hav h hF).1 with h1 | h1 <;> exact h1

/-! ### The encoding as a path semantics -/

/-- A block in the encoding: the path goes on after a violated instance
(`v` records it) and ends (`dead`) at a false `assume`; an `assume`
constrains only what follows it (Part 1). -/
inductive GBlk : List (Stmt S) → S → Prop → Out S → Prop → Prop
  | nil {s v} : GBlk [] s v (.norm s) v
  | upd {R rest s s' v o v'} : R s s' → GBlk rest s' v o v' → GBlk (.upd R :: rest) s v o v'
  | check {c rest s v v1 o v'} :
      (v1 ↔ v ∨ ¬ c s) → GBlk rest s v1 o v' → GBlk (.check c :: rest) s v o v'
  | assumeT {c rest s v o v'} : c s → GBlk rest s v o v' → GBlk (.assume c :: rest) s v o v'
  | assumeF {c rest s v} : ¬ c s → GBlk (.assume c :: rest) s v .dead v

/-- The cut encoding as a path semantics: `v` — a property instance is
violated on the path so far; `q` — a base or step goal failed at a point
with no violation before it.  The havoc assumption `Inv L e s` of a header
is guarded by "no violation before this header" (and is dropped once a goal
failed: the query of that goal does not assume the loops after it). -/
inductive GRun : Prog S → S → Prop → Prop → Out S → Prop → Prop → Prop
  | blk {b s v q o v'} : GBlk b s v o v' → GRun (.blk b) s v q o v' q
  | seqN {p p' s t v q v1 q1 o v2 q2} :
      GRun p s v q (.norm t) v1 q1 → GRun p' t v1 q1 o v2 q2 → GRun (.seq p p') s v q o v2 q2
  | seqX {p p' s v q o v' q'} :
      GRun p s v q o v' q' → (∀ t, o ≠ .norm t) → GRun (.seq p p') s v q o v' q'
  | iteT {c p p' s v q o v' q'} : c s → GRun p s v q o v' q' → GRun (.ite c p p') s v q o v' q'
  | iteF {c p p' s v q o v' q'} : ¬ c s → GRun p' s v q o v' q' → GRun (.ite c p p') s v q o v' q'
  | brk {s v q} : GRun .brk s v q (.brk s) v q
  | cont {s v q} : GRun .cont s v q (.cont s) v q
  | ret {s v q} : GRun .ret s v q (.ret s) v q
  | loopLatch {L b e v q q1 s o t v2 q2 q3} :
      (q1 ↔ q ∨ (¬ v ∧ ¬ Inv L e e)) → Hav L e s → (¬ v ∧ ¬ q1 → Inv L e s) →
      GRun b s v q1 o v2 q2 → latchState o = some t →
      (q3 ↔ q2 ∨ (¬ v2 ∧ ¬ Inv L e t)) → GRun (.loop L b) e v q .cut v2 q3
  | loopExit {L b e v q q1 s o o' v2 q2} :
      (q1 ↔ q ∨ (¬ v ∧ ¬ Inv L e e)) → Hav L e s → (¬ v ∧ ¬ q1 → Inv L e s) →
      GRun b s v q1 o v2 q2 → exitOut o = some o' → GRun (.loop L b) e v q o' v2 q2

/-- Every assignment and havoc of the program is total (a Z3 assignment
defines its value, a havoc is a fresh constant). -/
def TotalB : List (Stmt S) → Prop
  | [] => True
  | .upd R :: rest => (∀ s, ∃ s', R s s') ∧ TotalB rest
  | _ :: rest => TotalB rest

def Total : Prog S → Prop
  | .blk b => TotalB b
  | .seq p q => Total p ∧ Total q
  | .ite _ p q => Total p ∧ Total q
  | .loop _ b => Total b
  | .brk => True
  | .cont => True
  | .ret => True

theorem gblk_complete {b : List (Stmt S)} (hT : TotalB b) :
    ∀ s v, ∃ o v', GBlk b s v o v' ∧ (v → v') := by
  induction b with
  | nil => intro s v; exact ⟨_, _, GBlk.nil, id⟩
  | cons st rest ih =>
    intro s v
    cases st with
    | upd R =>
      obtain ⟨hR, hT'⟩ := hT
      obtain ⟨s', hs'⟩ := hR s
      obtain ⟨o, v', h, hv⟩ := ih hT' s' v
      exact ⟨o, v', GBlk.upd hs' h, hv⟩
    | check c =>
      obtain ⟨o, v', h, hv⟩ := ih hT s (v ∨ ¬ c s)
      exact ⟨o, v', GBlk.check Iff.rfl h, fun h' => hv (Or.inl h')⟩
    | assume c =>
      by_cases hc : c s
      · obtain ⟨o, v', h, hv⟩ := ih hT s v
        exact ⟨o, v', GBlk.assumeT hc h, hv⟩
      · exact ⟨_, _, GBlk.assumeF hc, id⟩

/-- Wrapping one body path into the cut loop, whatever the body outcome. -/
theorem grun_loop_wrap {L : Nat} {b : Prog S} {e s : S} {v q q1 v2 q2 : Prop} {o : Out S}
    (hq1 : q1 ↔ q ∨ (¬ v ∧ ¬ Inv L e e)) (hs : Hav L e s) (ha : ¬ v ∧ ¬ q1 → Inv L e s)
    (hb : GRun Inv Hav b s v q1 o v2 q2) :
    ∃ o' q3, GRun Inv Hav (.loop L b) e v q o' v2 q3 ∧ (q2 → q3) := by
  cases o with
  | norm t => exact ⟨_, _, GRun.loopLatch hq1 hs ha hb rfl Iff.rfl, Or.inl⟩
  | cont t => exact ⟨_, _, GRun.loopLatch hq1 hs ha hb rfl Iff.rfl, Or.inl⟩
  | brk t => exact ⟨_, _, GRun.loopExit hq1 hs ha hb rfl, id⟩
  | ret t => exact ⟨_, _, GRun.loopExit hq1 hs ha hb rfl, id⟩
  | err => exact ⟨_, _, GRun.loopExit hq1 hs ha hb rfl, id⟩
  | cut => exact ⟨_, _, GRun.loopExit hq1 hs ha hb rfl, id⟩
  | dead => exact ⟨_, _, GRun.loopExit hq1 hs ha hb rfl, id⟩

/-- After a violation or a failed goal, the encoding path can always be
completed: nothing after it is assumed (this is what the guard "no
violation before this header" is for). -/
theorem grun_complete {P : Prog S} (hT : Total P) (hF : FrameOK Hav P) :
    ∀ s (v q : Prop), (v ∨ q) → ∃ o v' q', GRun Inv Hav P s v q o v' q' ∧ (v' ∨ q') := by
  induction P with
  | blk b =>
    intro s v q hvq
    obtain ⟨o, v', h, hv⟩ := gblk_complete hT s v
    exact ⟨o, v', q, GRun.blk h, hvq.imp hv id⟩
  | seq p p' ihp ihp' =>
    intro s v q hvq
    obtain ⟨o, v1, q1, h1, hvq1⟩ := ihp hT.1 hF.1 s v q hvq
    cases o with
    | norm t =>
      obtain ⟨o2, v2, q2, h2, hvq2⟩ := ihp' hT.2 hF.2 t v1 q1 hvq1
      exact ⟨o2, v2, q2, GRun.seqN h1 h2, hvq2⟩
    | brk t => exact ⟨_, _, _, GRun.seqX h1 (fun _ h => by cases h), hvq1⟩
    | cont t => exact ⟨_, _, _, GRun.seqX h1 (fun _ h => by cases h), hvq1⟩
    | ret t => exact ⟨_, _, _, GRun.seqX h1 (fun _ h => by cases h), hvq1⟩
    | err => exact ⟨_, _, _, GRun.seqX h1 (fun _ h => by cases h), hvq1⟩
    | cut => exact ⟨_, _, _, GRun.seqX h1 (fun _ h => by cases h), hvq1⟩
    | dead => exact ⟨_, _, _, GRun.seqX h1 (fun _ h => by cases h), hvq1⟩
  | ite c p p' ihp ihp' =>
    intro s v q hvq
    by_cases hc : c s
    · obtain ⟨o, v', q', h, hh⟩ := ihp hT.1 hF.1 s v q hvq
      exact ⟨o, v', q', GRun.iteT hc h, hh⟩
    · obtain ⟨o, v', q', h, hh⟩ := ihp' hT.2 hF.2 s v q hvq
      exact ⟨o, v', q', GRun.iteF hc h, hh⟩
  | loop L b ihb =>
    intro e v q hvq
    obtain ⟨hrefl, _, hFb⟩ := hF
    have hq1 : (q ∨ (¬ v ∧ ¬ Inv L e e)) ↔ q ∨ (¬ v ∧ ¬ Inv L e e) := Iff.rfl
    have ha : ¬ v ∧ ¬ (q ∨ (¬ v ∧ ¬ Inv L e e)) → Inv L e e := by
      rintro ⟨hv, hq⟩
      rcases hvq with h | h
      · exact absurd h hv
      · exact absurd (Or.inl h) hq
    obtain ⟨o, v2, q2, hb, hvq2⟩ := ihb hT hFb e v _ (hvq.imp id Or.inl)
    obtain ⟨o', q3, h, hq3⟩ := grun_loop_wrap Inv Hav hq1 (hrefl e) ha hb
    exact ⟨o', v2, q3, h, hvq2.imp id hq3⟩
  | brk => intro s v q hvq; exact ⟨_, _, _, GRun.brk, hvq⟩
  | cont => intro s v q hvq; exact ⟨_, _, _, GRun.cont, hvq⟩
  | ret => intro s v q hvq; exact ⟨_, _, _, GRun.ret, hvq⟩

theorem bexec_gblk {b : List (Stmt S)} {s : S} {o : Option S} (h : BExec b s o) :
    TotalB b → ∀ v : Prop,
      (o = none → ∃ o' v', GBlk b s v o' v' ∧ v') ∧
      (∀ t, o = some t → GBlk b s v (.norm t) v) := by
  induction h with
  | nil => intro _ v; exact ⟨fun h => (by cases h), fun t ht => (by cases ht; exact GBlk.nil)⟩
  | upd hR _ ih =>
    intro hT v
    obtain ⟨h1, h2⟩ := ih hT.2 v
    refine ⟨fun ho => ?_, fun t ht => GBlk.upd hR (h2 t ht)⟩
    obtain ⟨o', v', hg, hv⟩ := h1 ho
    exact ⟨o', v', GBlk.upd hR hg, hv⟩
  | @checkOk c rest s o hcs _ ih =>
    intro hT v
    have hiff : v ↔ v ∨ ¬ c s := ⟨Or.inl, fun h => h.elim id (fun h' => absurd hcs h')⟩
    obtain ⟨h1, h2⟩ := ih hT v
    refine ⟨fun ho => ?_, fun t ht => GBlk.check hiff (h2 t ht)⟩
    obtain ⟨o', v', hg, hv⟩ := h1 ho
    exact ⟨o', v', GBlk.check hiff hg, hv⟩
  | @checkFail c rest s hcs =>
    intro hT v
    refine ⟨fun _ => ?_, fun t ht => (by cases ht)⟩
    obtain ⟨o', v', hg, hv⟩ := gblk_complete (b := rest) hT s (v ∨ ¬ c s)
    exact ⟨o', v', GBlk.check Iff.rfl hg, hv (Or.inr hcs)⟩
  | assume hcs _ ih =>
    intro hT v
    obtain ⟨h1, h2⟩ := ih hT v
    refine ⟨fun ho => ?_, fun t ht => GBlk.assumeT hcs (h2 t ht)⟩
    obtain ⟨o', v', hg, hv⟩ := h1 ho
    exact ⟨o', v', GBlk.assumeT hcs hg, hv⟩

/-- A failing run of the cut program is a flagged complete path of the
encoding; any other run is an unflagged path with the same outcome. -/
theorem cexec_grun {P : Prog S} {s : S} {o : Out S} (h : CExec Inv Hav P s o) :
    Total P → FrameOK Hav P →
    (o = .err → ∃ o' v' q', GRun Inv Hav P s False False o' v' q' ∧ (v' ∨ q')) ∧
    (o ≠ .err → GRun Inv Hav P s False False o False False) := by
  induction h with
  | blkOk hb =>
    intro hT _
    exact ⟨fun h => (by cases h), fun _ => GRun.blk ((bexec_gblk hb hT False).2 _ rfl)⟩
  | blkErr hb =>
    intro hT _
    refine ⟨fun _ => ?_, fun h => absurd rfl h⟩
    obtain ⟨o', v', hg, hv⟩ := (bexec_gblk hb hT False).1 rfl
    exact ⟨o', v', False, GRun.blk hg, Or.inl hv⟩
  | @seqN p p' s t o _ _ ihp ihq =>
    intro hT hF
    have hp := (ihp hT.1 hF.1).2 (fun h => by cases h)
    obtain ⟨h1, h2⟩ := ihq hT.2 hF.2
    refine ⟨fun ho => ?_, fun ho => GRun.seqN hp (h2 ho)⟩
    obtain ⟨o', v', q', hg, hvq⟩ := h1 ho
    exact ⟨o', v', q', GRun.seqN hp hg, hvq⟩
  | @seqX p p' s o _ hne ihp =>
    intro hT hF
    obtain ⟨h1, h2⟩ := ihp hT.1 hF.1
    refine ⟨fun ho => ?_, fun ho => GRun.seqX (h2 ho) hne⟩
    obtain ⟨o', v', q', hg, hvq⟩ := h1 ho
    cases o' with
    | norm t =>
      obtain ⟨o2, v2, q2, h2', hvq2⟩ := grun_complete Inv Hav hT.2 hF.2 t v' q' hvq
      exact ⟨o2, v2, q2, GRun.seqN hg h2', hvq2⟩
    | brk t => exact ⟨_, _, _, GRun.seqX hg (fun _ h => by cases h), hvq⟩
    | cont t => exact ⟨_, _, _, GRun.seqX hg (fun _ h => by cases h), hvq⟩
    | ret t => exact ⟨_, _, _, GRun.seqX hg (fun _ h => by cases h), hvq⟩
    | err => exact ⟨_, _, _, GRun.seqX hg (fun _ h => by cases h), hvq⟩
    | cut => exact ⟨_, _, _, GRun.seqX hg (fun _ h => by cases h), hvq⟩
    | dead => exact ⟨_, _, _, GRun.seqX hg (fun _ h => by cases h), hvq⟩
  | iteT hc _ ih =>
    intro hT hF
    obtain ⟨h1, h2⟩ := ih hT.1 hF.1
    refine ⟨fun ho => ?_, fun ho => GRun.iteT hc (h2 ho)⟩
    obtain ⟨o', v', q', hg, hvq⟩ := h1 ho
    exact ⟨o', v', q', GRun.iteT hc hg, hvq⟩
  | iteF hc _ ih =>
    intro hT hF
    obtain ⟨h1, h2⟩ := ih hT.2 hF.2
    refine ⟨fun ho => ?_, fun ho => GRun.iteF hc (h2 ho)⟩
    obtain ⟨o', v', q', hg, hvq⟩ := h1 ho
    exact ⟨o', v', q', GRun.iteF hc hg, hvq⟩
  | brk => exact fun _ _ => ⟨fun h => (by cases h), fun _ => GRun.brk⟩
  | cont => exact fun _ _ => ⟨fun h => (by cases h), fun _ => GRun.cont⟩
  | ret => exact fun _ _ => ⟨fun h => (by cases h), fun _ => GRun.ret⟩
  | @loopBase L b e hne =>
    intro hT hF
    refine ⟨fun _ => ?_, fun h => absurd rfl h⟩
    obtain ⟨hrefl, _, hFb⟩ := hF
    have hq1 : (False ∨ (¬ False ∧ ¬ Inv L e e)) := Or.inr ⟨id, hne⟩
    have ha : ¬ False ∧ ¬ (False ∨ (¬ False ∧ ¬ Inv L e e)) → Inv L e e :=
      fun h => absurd hq1 h.2
    obtain ⟨o, v2, q2, hb, hvq2⟩ := grun_complete Inv Hav (P := b) hT hFb e False _ (Or.inr hq1)
    obtain ⟨o', q3, hg, hq3⟩ := grun_loop_wrap Inv Hav Iff.rfl (hrefl e) ha hb
    exact ⟨o', v2, q3, hg, hvq2.imp id hq3⟩
  | @loopStep L b e s o t hee hs hes _ hl hnt ih =>
    intro hT hF
    refine ⟨fun _ => ?_, fun h => absurd rfl h⟩
    have hb := (ih hT hF.2.2).2 (by intro h; subst h; cases hl)
    have hq1 : False ↔ False ∨ (¬ False ∧ ¬ Inv L e e) :=
      ⟨False.elim, fun h => h.elim id (fun h' => h'.2 hee)⟩
    exact ⟨_, _, _, GRun.loopLatch hq1 hs (fun _ => hes) hb hl Iff.rfl, Or.inr (Or.inr ⟨id, hnt⟩)⟩
  | @loopCut L b e s o t hee hs hes _ hl ht ih =>
    intro hT hF
    refine ⟨fun h => (by cases h), fun _ => ?_⟩
    have hb := (ih hT hF.2.2).2 (by intro h; subst h; cases hl)
    have hq1 : False ↔ False ∨ (¬ False ∧ ¬ Inv L e e) :=
      ⟨False.elim, fun h => h.elim id (fun h' => h'.2 hee)⟩
    have hq3 : False ↔ False ∨ (¬ False ∧ ¬ Inv L e t) :=
      ⟨False.elim, fun h => h.elim id (fun h' => h'.2 ht)⟩
    exact GRun.loopLatch hq1 hs (fun _ => hes) hb hl hq3
  | @loopExit L b e s o o' hee hs hes _ hx ih =>
    intro hT hF
    have hq1 : False ↔ False ∨ (¬ False ∧ ¬ Inv L e e) :=
      ⟨False.elim, fun h => h.elim id (fun h' => h'.2 hee)⟩
    obtain ⟨h1, h2⟩ := ih hT hF.2.2
    by_cases ho : o = .err
    · subst ho
      have ho' : o' = .err := by cases hx; rfl
      subst ho'
      refine ⟨fun _ => ?_, fun h => absurd rfl h⟩
      obtain ⟨o2, v2, q2, hb, hvq2⟩ := h1 rfl
      obtain ⟨o3, q3, hg, hq3⟩ := grun_loop_wrap Inv Hav hq1 hs (fun _ => hes) hb
      exact ⟨o3, v2, q3, hg, hvq2.imp id hq3⟩
    · have hb := h2 ho
      have hne : o' ≠ .err := by
        intro h; subst h
        cases o <;> simp [exitOut] at hx
        exact ho rfl
      exact ⟨fun h => absurd h hne, fun _ => GRun.loopExit hq1 hs (fun _ => hes) hb hx⟩

/-- **Loop-cut soundness.**  If no complete path of the cut encoding (from
the function entry, nothing violated yet) violates a property instance or
fails a base or step goal — i.e. every Houdini query at the fixpoint and the
final query are UNSAT — then no execution of the uncut program, through any
number of iterations of any of its loops (nested or sequential), violates a
property. -/
theorem loopcut_sound (P : Prog S) (s0 : S) (hF : FrameOK Hav P) (hT : Total P)
    (hcut : ∀ o v q, GRun Inv Hav P s0 False False o v q → ¬ v ∧ ¬ q) :
    ¬ Exec P s0 .err := by
  intro h
  obtain ⟨o, v, q, hg, hvq⟩ := (cexec_grun Inv Hav (exec_err_cut Inv Hav hF h) hT hF).1 rfl
  have := hcut o v q hg
  rcases hvq with hv | hq
  · exact this.1 hv
  · exact this.2 hq

/-- The sequential cut program alone: if it cannot fail, the uncut program
cannot fail (no totality needed). -/
theorem loopcut_sound_seq (P : Prog S) (s0 : S) (hF : FrameOK Hav P)
    (hcut : ¬ CExec Inv Hav P s0 .err) : ¬ Exec P s0 .err :=
  fun h => hcut (exec_err_cut Inv Hav hF h)

end Cut

/-- Houdini's invariant of a loop: the conjunction of its surviving
candidates (`holds c e s`: candidate `c` in header state `s` of a visit whose
loop entry state was `e`). -/
def survivorsInv {C : Type} (holds : C → S → S → Prop) (surv : Nat → List C) :
    Nat → S → S → Prop :=
  fun L e s => ∀ c ∈ surv L, holds c e s

/-- Loop-cut soundness with the invariants given as Houdini's survivor
lists. -/
theorem houdini_loopcut_sound {C : Type} (holds : C → S → S → Prop) (surv : Nat → List C)
    (Hav : Nat → S → S → Prop) (P : Prog S) (s0 : S) (hF : FrameOK Hav P) (hT : Total P)
    (hcut : ∀ o v q, GRun (survivorsInv holds surv) Hav P s0 False False o v q → ¬ v ∧ ¬ q) :
    ¬ Exec P s0 .err :=
  loopcut_sound _ Hav P s0 hF hT hcut

end PrismTechniques.LoopCut
