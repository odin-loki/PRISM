/-
PRISM verdict lattice: the model of `src/prism/verdict/verdict.cpp` and
`prism/laws.py`, and the laws of `docs/VERDICTS.md` proved as theorems.

Everything here is a total function over a finite domain. `Main.lean`
prints every function over its whole domain as JSON
(`tests/data/verdict_tables.json`), and the C++ and Python engines are
tested entry by entry against that table, so the running code is shown
equal to this model (see docs/VERDICTS.md, "Connecting the proof to the
code").

Core Lean only (no Mathlib). No `sorry`; `Prism/Axioms.lean` prints the
axioms of every main theorem and CI rejects anything outside
{propext, Classical.choice, Quot.sound}.
-/

namespace Prism

/-! ## The verdict set -/

/-- Every status string PRISM may write (include/prism/laws.hpp). -/
inductive Verdict where
  | provedCertified  -- PROVED-CERTIFIED: UNSAT checked by a verified checker
  | provedUnbounded  -- PROVED-UNBOUNDED: k-induction closed
  | proved           -- PROVED: all properties hold, loops closed in k
  | provedAssuming   -- PROVED-ASSUMING: proved under explicit requires
  | bounded          -- BOUNDED: nothing found within k; that is all
  | failed           -- FAILED: counterexample
  | unknown          -- UNKNOWN: solver ran, did not conclude
  | timeout
  | error
  | nofunc
  | notrun           -- NOTRUN: missing tool / not executed
  | needsHarness     -- NEEDS-HARNESS: pointer params, no precondition
  | crash            -- fuzz
  | clean            -- fuzz: CLEAN is not a proof
  | noseed
  | sanfail
  | hypothesis       -- LLM
  | reads            -- LLM
  deriving DecidableEq, Repr, Inhabited

namespace Verdict

/-- All verdicts in table order (the C++ enum order). -/
def all : List Verdict :=
  [provedCertified, provedUnbounded, proved, provedAssuming, bounded, failed,
   unknown, timeout, error, nofunc, notrun, needsHarness,
   crash, clean, noseed, sanfail, hypothesis, reads]

theorem mem_all (v : Verdict) : v ∈ all := by cases v <;> decide

/-- Quantifiers over the finite verdict set are decidable, so every law
below that ranges over verdicts is checked by `decide` over the whole domain. -/
instance decForall (P : Verdict → Prop) [DecidablePred P] : Decidable (∀ v, P v) :=
  decidable_of_iff (∀ v ∈ all, P v) ⟨fun h v => h v (mem_all v), fun h v _ => h v⟩

/-- The exact status string written into reports. -/
def name : Verdict → String
  | provedCertified => "PROVED-CERTIFIED"
  | provedUnbounded => "PROVED-UNBOUNDED"
  | proved => "PROVED"
  | provedAssuming => "PROVED-ASSUMING"
  | bounded => "BOUNDED"
  | failed => "FAILED"
  | unknown => "UNKNOWN"
  | timeout => "TIMEOUT"
  | error => "ERROR"
  | nofunc => "NOFUNC"
  | notrun => "NOTRUN"
  | needsHarness => "NEEDS-HARNESS"
  | crash => "CRASH"
  | clean => "CLEAN"
  | noseed => "NOSEED"
  | sanfail => "SANFAIL"
  | hypothesis => "HYPOTHESIS"
  | reads => "READS"

theorem name_injective (a b : Verdict) (h : a.name = b.name) : a = b := by
  cases a <;> cases b <;> first | rfl | (simp [name] at h)

/-- A proof: PROVED-CERTIFIED, PROVED-UNBOUNDED, PROVED, PROVED-ASSUMING. -/
def isProof : Verdict → Bool
  | provedCertified | provedUnbounded | proved | provedAssuming => true
  | _ => false

/-- A formal claim: a proof or BOUNDED. No two distinct ones ever merge (Law 2). -/
def isFormal : Verdict → Bool
  | provedCertified | provedUnbounded | proved | provedAssuming | bounded => true
  | _ => false

/-- The instrument gave an answer (a formal claim or a counterexample). -/
def isAnswered : Verdict → Bool
  | provedCertified | provedUnbounded | proved | provedAssuming | bounded | failed => true
  | _ => false

/-- No answer: the instrument did not conclude (Law 7: written down, never clean). -/
def isNoAnswer : Verdict → Bool
  | unknown | timeout | error | nofunc | notrun | needsHarness => true
  | _ => false

/-- A defect result (becomes a SARIF result). -/
def isDefect : Verdict → Bool
  | failed | crash | sanfail => true
  | _ => false

/-- Model (LLM) output (Law 4). -/
def isModel : Verdict → Bool
  | hypothesis | reads => true
  | _ => false

/-- Strength of a formal claim; 0 for everything that is not one. -/
def rank : Verdict → Nat
  | provedCertified => 5
  | provedUnbounded => 4
  | proved => 3
  | provedAssuming => 2
  | bounded => 1
  | _ => 0

theorem isProof_isFormal (v : Verdict) (h : v.isProof = true) : v.isFormal = true := by
  cases v <;> simp_all [isProof, isFormal]

theorem isFormal_iff_rank_pos (v : Verdict) : v.isFormal = true ↔ 0 < v.rank := by
  cases v <;> simp [isFormal, rank]

theorem answered_noAnswer_disjoint (v : Verdict) :
    ¬ (v.isAnswered = true ∧ v.isNoAnswer = true) := by
  cases v <;> simp [isAnswered, isNoAnswer]

end Verdict

open Verdict

/-! ## Merge law -/

/-- Why two claims about one function stay two claims. -/
inductive Refusal where
  | none          -- may merge
  | formal        -- two distinct formal claims (Law 2)
  | promoteFuzz   -- CLEAN with a formal claim (Law 3)
  | promoteModel  -- HYPOTHESIS/READS with a formal claim (Law 4)
  | notrunClean   -- NOTRUN with CLEAN or an answer (Law 1)
  deriving DecidableEq, Repr

def Refusal.name : Refusal → String
  | .none => "none"
  | .formal => "formal"
  | .promoteFuzz => "promote-fuzz"
  | .promoteModel => "promote-model"
  | .notrunClean => "notrun-clean"

def mergeRefusal (a b : Verdict) : Refusal :=
  if a = b then .none
  else if a.isFormal && b.isFormal then .formal
  else if (a = clean && b.isFormal) || (b = clean && a.isFormal) then .promoteFuzz
  else if (a.isModel && b.isFormal) || (b.isModel && a.isFormal) then .promoteModel
  else if (a = notrun && (b.isAnswered || b = clean)) ||
          (b = notrun && (a.isAnswered || a = clean)) then .notrunClean
  else .none

/-- Law 2: PROVED and BOUNDED never merge. -/
theorem proved_bounded_never_merge :
    mergeRefusal proved bounded = .formal ∧ mergeRefusal bounded proved = .formal := by
  decide

/-- Law 2 (extended): no two distinct formal claims ever merge, including
PROVED-CERTIFIED with any weaker proof. -/
theorem formal_never_merge (a b : Verdict) (ha : a.isFormal = true) (hb : b.isFormal = true)
    (hne : a ≠ b) : mergeRefusal a b = .formal := by
  simp [mergeRefusal, hne, ha, hb]

theorem certified_never_merges_weaker (b : Verdict) (hb : b.isFormal = true)
    (hne : b ≠ provedCertified) :
    mergeRefusal provedCertified b ≠ .none ∧ mergeRefusal b provedCertified ≠ .none := by
  revert b; decide

/-- Law 3: CLEAN is never merged into (promoted to) any formal claim. -/
theorem clean_never_promoted (b : Verdict) (hb : b.isFormal = true) :
    mergeRefusal clean b ≠ .none ∧ mergeRefusal b clean ≠ .none := by
  revert b; decide

/-- Law 4: model output is never merged into a formal claim. -/
theorem model_never_promoted (a b : Verdict) (ha : a.isModel = true) (hb : b.isFormal = true) :
    mergeRefusal a b ≠ .none ∧ mergeRefusal b a ≠ .none := by
  revert a b; decide

/-- Law 1: NOTRUN never merges with CLEAN or with an answer. -/
theorem notrun_never_merges_clean (b : Verdict) (hb : b.isAnswered = true ∨ b = clean) :
    mergeRefusal notrun b ≠ .none ∧ mergeRefusal b notrun ≠ .none := by
  revert b; decide

theorem mergeRefusal_symm (a b : Verdict) : mergeRefusal a b = mergeRefusal b a := by
  revert a b; decide

theorem mergeRefusal_refl (a : Verdict) : mergeRefusal a a = .none := by
  simp [mergeRefusal]

/-! ## Rewrites: how a recorded status may change later -/

/-- May a finding recorded as `a` later be rewritten to `b`? Formal claims
only weaken; a formal claim may be withdrawn to UNKNOWN/ERROR; every status
without an answer (NOTRUN, UNKNOWN, ...) stays without one; nothing becomes
a formal claim by rewriting. -/
def mayRewrite (a b : Verdict) : Bool :=
  if a = b then true
  else if b.isFormal then a.isFormal && decide (b.rank < a.rank)
  else if a.isFormal then b = unknown || b = error
  else if a.isNoAnswer then b.isNoAnswer
  else true

/-- Reachability through any chain of rewrites. -/
inductive Reach : Verdict → Verdict → Prop where
  | refl (a : Verdict) : Reach a a
  | step {a b c : Verdict} : mayRewrite a b = true → Reach b c → Reach a c

theorem rewrite_never_creates_formal (a b : Verdict) (h : mayRewrite a b = true)
    (hb : b.isFormal = true) : a.isFormal = true ∧ b.rank ≤ a.rank := by
  revert a b; decide

theorem rewrite_to_certified (a : Verdict) (h : mayRewrite a provedCertified = true) :
    a = provedCertified := by
  revert a; decide

/-- Law 3 as a rewrite: CLEAN is never rewritten to a formal claim. -/
theorem clean_never_rewritten_to_proof (b : Verdict) (hb : b.isFormal = true) :
    mayRewrite clean b = false := by
  revert b; decide

/-- Law 1 as a rewrite: NOTRUN never becomes CLEAN or an answer. -/
theorem notrun_never_becomes_clean (b : Verdict) (h : mayRewrite notrun b = true) :
    b ≠ clean ∧ b.isAnswered = false := by
  revert b; decide

/-- A status without an answer is only ever rewritten to another one. -/
theorem rewrite_keeps_noAnswer (a b : Verdict) (h : mayRewrite a b = true)
    (ha : a.isNoAnswer = true) : b.isNoAnswer = true := by
  revert a b; decide

theorem rewrite_keeps_nonformal (a b : Verdict) (h : mayRewrite a b = true)
    (ha : a.isFormal = false) : b.isFormal = false := by
  revert a b; decide

theorem reach_nonformal (a b : Verdict) (h : Reach a b) (ha : a.isFormal = false) :
    b.isFormal = false := by
  induction h with
  | refl => exact ha
  | step hab _ ih => exact ih (rewrite_keeps_nonformal _ _ hab ha)

theorem reach_certified (a b : Verdict) (h : Reach a b) (hb : b = provedCertified) :
    a = provedCertified := by
  induction h with
  | refl => exact hb
  | step hab _ ih =>
    have := ih hb
    subst this
    exact rewrite_to_certified _ hab

theorem reach_noAnswer (a b : Verdict) (h : Reach a b) (ha : a.isNoAnswer = true) :
    b.isNoAnswer = true := by
  induction h with
  | refl => exact ha
  | step hab _ ih => exact ih (rewrite_keeps_noAnswer _ _ hab ha)

theorem reach_notrun (b : Verdict) (h : Reach notrun b) : b ≠ clean ∧ b.isAnswered = false := by
  have hb := reach_noAnswer _ _ h rfl
  clear h; revert b; decide

/-! ## Origins and admission -/

/-- Where a result came from. Only a solver or an external prover may prove. -/
inductive Origin where
  | solver          -- PRISM's own BMC / k-induction / WP / LTL checker
  | externalProver  -- ESBMC, CBMC, Dafny
  | fuzzer          -- fuzzers, property-based testing
  | model           -- LLM output
  | lint            -- pattern and dataflow checkers, compilers, linters
  | execution       -- sanitizers, differential and concrete execution
  | pipeline        -- inventory, classify, unify, unknown stages
  deriving DecidableEq, Repr

namespace Origin

def all : List Origin := [solver, externalProver, fuzzer, model, lint, execution, pipeline]

theorem mem_all (o : Origin) : o ∈ all := by cases o <;> decide

instance decForall (P : Origin → Prop) [DecidablePred P] : Decidable (∀ o, P o) :=
  decidable_of_iff (∀ o ∈ all, P o) ⟨fun h o => h o (mem_all o), fun h o _ => h o⟩

def name : Origin → String
  | solver => "solver"
  | externalProver => "external-prover"
  | fuzzer => "fuzzer"
  | model => "model"
  | lint => "lint"
  | execution => "execution"
  | pipeline => "pipeline"

def mayProve : Origin → Bool
  | solver | externalProver => true
  | _ => false

end Origin

/-- The gate every formal claim passes. Non-formal statuses pass unchanged
(so NOTRUN stays NOTRUN). A formal claim from an origin that may not prove
becomes UNKNOWN. PROVED-CERTIFIED needs PRISM's own solver and a checked
certificate; otherwise it falls back to plain PROVED, never upward. -/
def admit (o : Origin) (v : Verdict) (cert : Bool) : Verdict :=
  if v.isFormal = false then v
  else if o.mayProve = false then unknown
  else if v = provedCertified then
    (if o = .solver && cert then provedCertified else proved)
  else v

theorem admit_notrun (o : Origin) (c : Bool) : admit o notrun c = notrun := by
  simp [admit, isFormal]

theorem admit_nonformal (o : Origin) (v : Verdict) (c : Bool) (h : v.isFormal = false) :
    admit o v c = v := by
  simp [admit, h]

/-- No origin that may not prove (fuzzer, model, lint, execution, pipeline)
ever gets a formal verdict out of `admit`. -/
theorem admit_nonproving (o : Origin) (ho : o.mayProve = false) (v : Verdict) (c : Bool) :
    (admit o v c).isFormal = false := by
  revert o v c; decide

theorem admit_fuzzer_never_proof (v : Verdict) (c : Bool) :
    (admit .fuzzer v c).isProof = false := by
  revert v c; decide

theorem admit_model_never_proof (v : Verdict) (c : Bool) :
    (admit .model v c).isProof = false := by
  revert v c; decide

/-- PROVED-CERTIFIED arises exactly when the solver asked for it with a checked certificate. -/
theorem admit_certified_iff (o : Origin) (v : Verdict) (c : Bool) :
    admit o v c = provedCertified ↔ (o = .solver ∧ v = provedCertified ∧ c = true) := by
  revert o v c; decide

/-- `admit` never strengthens. -/
theorem admit_rank_le (o : Origin) (v : Verdict) (c : Bool) : (admit o v c).rank ≤ v.rank := by
  revert o v c; decide

/-- A proof out of `admit` was asked for as a proof. -/
theorem admit_proof_requires_proof (o : Origin) (v : Verdict) (c : Bool)
    (h : (admit o v c).isProof = true) : v.isProof = true := by
  revert o v c; decide

theorem admit_idem (o : Origin) (v : Verdict) (c : Bool) :
    admit o (admit o v c) c = admit o v c := by
  revert o v c; decide

/-- No path from a fuzzer-only or model-only result reaches any formal
verdict: whatever `admit` returns, no chain of rewrites leads to one. -/
theorem no_path_fuzzer_or_model_to_proof (o : Origin) (ho : o = .fuzzer ∨ o = .model)
    (v : Verdict) (c : Bool) (w : Verdict) (h : Reach (admit o v c) w) : w.isFormal = false := by
  apply reach_nonformal _ _ h
  apply admit_nonproving
  cases ho <;> subst_vars <;> rfl

/-- PROVED-CERTIFIED only arises with a checked certificate, through any path. -/
theorem certified_only_with_certificate (o : Origin) (v : Verdict) (c : Bool)
    (h : Reach (admit o v c) provedCertified) : c = true ∧ o = .solver := by
  have := reach_certified _ _ h rfl
  have := (admit_certified_iff o v c).1 this
  exact ⟨this.2.2, this.1⟩

/-- NOTRUN never becomes clean: admission keeps it, and no rewrite chain
takes it to CLEAN or to an answer. -/
theorem notrun_never_clean (o : Origin) (c : Bool) (w : Verdict)
    (h : Reach (admit o notrun c) w) : w ≠ clean ∧ w.isAnswered = false := by
  rw [admit_notrun] at h
  exact reach_notrun w h

/-! ## Stages and the pipeline audit -/

/-- The pipeline stages (include/prism/pipeline.hpp STAGE_ORDER) plus `other`. -/
inductive Stage where
  | inventory | classify | lints | taint | thread | interval
  | warnings | cppcheck | pbsd | sanitize | optional | polyglot | esbmc
  | dafny | contracts | wp | bmc | pir | harness | review | concolic | fuzz | diff
  | rapid | muttest | ltl | llm | execute | repair | unify
  | other
  deriving DecidableEq, Repr

namespace Stage

def all : List Stage :=
  [inventory, classify, lints, taint, thread, interval,
   warnings, cppcheck, pbsd, sanitize, optional, polyglot, esbmc,
   dafny, contracts, wp, bmc, pir, harness, review, concolic, fuzz, diff,
   rapid, muttest, ltl, llm, execute, repair, unify, other]

theorem mem_all (s : Stage) : s ∈ all := by cases s <;> decide

instance decForall (P : Stage → Prop) [DecidablePred P] : Decidable (∀ s, P s) :=
  decidable_of_iff (∀ s ∈ all, P s) ⟨fun h s => h s (mem_all s), fun h s _ => h s⟩

def name : Stage → String
  | inventory => "inventory" | classify => "classify" | lints => "lints"
  | taint => "taint" | thread => "thread" | interval => "interval"
  | warnings => "warnings" | cppcheck => "cppcheck" | pbsd => "pbsd"
  | sanitize => "sanitize" | optional => "optional" | polyglot => "polyglot"
  | esbmc => "esbmc" | dafny => "dafny" | contracts => "contracts" | wp => "wp"
  | bmc => "bmc" | pir => "pir" | harness => "harness" | review => "review"
  | concolic => "concolic" | fuzz => "fuzz"
  | diff => "diff" | rapid => "rapid" | muttest => "muttest" | ltl => "ltl"
  | llm => "llm" | execute => "execute" | repair => "repair" | unify => "unify"
  | other => "other"

/-- Which origin a stage's findings have. This is the audit table. -/
def origin : Stage → Origin
  | inventory | classify | unify | other => .pipeline
  | lints | taint | thread | interval | warnings | cppcheck | pbsd | polyglot => .lint
  | sanitize | concolic | diff | muttest | execute => .execution
  | fuzz | rapid => .fuzzer
  | llm | repair => .model
  | optional | esbmc | dafny => .externalProver
  | contracts | wp | bmc | pir | harness | review | ltl => .solver

def mayProve (s : Stage) : Bool := s.origin.mayProve

end Stage

structure AuditResult where
  status : Verdict
  violation : Bool
  deriving DecidableEq, Repr

/-- Final pipeline pass over every finding: the status after `admit`, and
whether that differs from what the stage wrote (an audit violation). -/
def audit (s : Stage) (v : Verdict) (cert : Bool) : AuditResult :=
  let r := admit s.origin v cert
  ⟨r, decide (r ≠ v)⟩

/-- Exactly these stages may emit a formal claim. -/
theorem proving_stages (s : Stage) :
    s.mayProve = true ↔ s ∈ [Stage.optional, .esbmc, .dafny, .contracts, .wp, .bmc, .pir, .harness, .review, .ltl] := by
  revert s; decide

/-- The audit never lets a non-proving stage output a formal verdict. -/
theorem audit_nonproving (s : Stage) (hs : s.mayProve = false) (v : Verdict) (c : Bool) :
    (audit s v c).status.isFormal = false :=
  admit_nonproving _ hs v c

/-- A non-proving stage that wrote a formal claim is flagged and demoted to UNKNOWN. -/
theorem audit_flags_nonproving (s : Stage) (hs : s.mayProve = false) (v : Verdict)
    (hv : v.isFormal = true) (c : Bool) :
    audit s v c = ⟨unknown, true⟩ := by
  revert s v c; decide

/-- The audit output never carries PROVED-CERTIFIED without a checked certificate. -/
theorem audit_certified (s : Stage) (v : Verdict) (c : Bool)
    (h : (audit s v c).status = provedCertified) : c = true ∧ s.origin = .solver := by
  have := (admit_certified_iff _ v c).1 h
  exact ⟨this.2.2, this.1⟩

theorem audit_violation_iff (s : Stage) (v : Verdict) (c : Bool) :
    (audit s v c).violation = true ↔ (audit s v c).status ≠ v := by
  simp [audit]

theorem audit_nonformal_untouched (s : Stage) (v : Verdict) (c : Bool) (h : v.isFormal = false) :
    audit s v c = ⟨v, false⟩ := by
  simp [audit, admit_nonformal _ _ _ h]

theorem audit_notrun (s : Stage) (c : Bool) : audit s notrun c = ⟨notrun, false⟩ :=
  audit_nonformal_untouched s notrun c rfl

theorem audit_idem (s : Stage) (v : Verdict) (c : Bool) :
    audit s (audit s v c).status c = ⟨(audit s v c).status, false⟩ := by
  simp [audit, admit_idem]

/-! ## Confidence = visibility × answer × resolution (Law 5) -/

/-- A non-negative fraction. `den = 0` never occurs from `ratio`. -/
structure Frac where
  num : Nat
  den : Nat
  deriving DecidableEq, Repr

def Frac.mul (a b : Frac) : Frac := ⟨a.num * b.num, a.den * b.den⟩

/-- n/d, and 0 when there is no data (d = 0): no data scores 0. -/
def ratio (n d : Nat) : Frac := if d = 0 then ⟨0, 1⟩ else ⟨n, d⟩

def confidence (vis ans res : Frac) : Frac := vis.mul (ans.mul res)

structure Score where
  vis : Frac
  ans : Frac
  res : Frac
  conf : Frac
  deriving DecidableEq, Repr

/-- The pipeline's score from its counts (prism/confidence.py, apply_confidence). -/
def score (nFun classified attempted answered resolved : Nat) : Score :=
  let v := ratio classified nFun
  let a := ratio answered attempted
  let r := ratio resolved answered
  ⟨v, a, r, confidence v a r⟩

theorem ratio_den_pos (n d : Nat) : 0 < (ratio n d).den := by
  unfold ratio; split <;> simp <;> omega

/-- Law 5: visibility zero means confidence zero. -/
theorem confidence_zero_of_visibility_zero (v a r : Frac) (h : v.num = 0) :
    (confidence v a r).num = 0 := by
  simp [confidence, Frac.mul, h]

theorem confidence_zero_of_answer_zero (v a r : Frac) (h : a.num = 0) :
    (confidence v a r).num = 0 := by
  simp [confidence, Frac.mul, h]

theorem confidence_zero_of_resolution_zero (v a r : Frac) (h : r.num = 0) :
    (confidence v a r).num = 0 := by
  simp [confidence, Frac.mul, h]

/-- No functions parsed: confidence is 0, not n/a. -/
theorem score_no_functions (c t a r : Nat) : (score 0 c t a r).conf.num = 0 := by
  simp [score, ratio, confidence, Frac.mul]

theorem score_no_data (n c t a r : Nat) (h : c = 0 ∨ n = 0) : (score n c t a r).conf.num = 0 := by
  rcases h with h | h <;> subst h <;> simp [score, ratio, confidence, Frac.mul] <;> split <;> simp

theorem ratio_le_one (n d : Nat) (h : n ≤ d) : (ratio n d).num ≤ (ratio n d).den := by
  unfold ratio; split <;> simp_all

/-- Each factor at most 1 means confidence at most 1. -/
theorem confidence_le_one (v a r : Frac) (hv : v.num ≤ v.den) (ha : a.num ≤ a.den)
    (hr : r.num ≤ r.den) : (confidence v a r).num ≤ (confidence v a r).den := by
  simp only [confidence, Frac.mul]
  exact Nat.mul_le_mul hv (Nat.mul_le_mul ha hr)

theorem score_le_one (n c t a r : Nat) (hc : c ≤ n) (ha : a ≤ t) (hr : r ≤ a) :
    (score n c t a r).conf.num ≤ (score n c t a r).conf.den :=
  confidence_le_one _ _ _ (ratio_le_one _ _ hc) (ratio_le_one _ _ ha) (ratio_le_one _ _ hr)

/-- Confidence never exceeds visibility (cross-multiplied): answering and
resolving can only lose confidence, never add visibility. -/
theorem confidence_le_visibility (v a r : Frac) (ha : a.num ≤ a.den) (hr : r.num ≤ r.den) :
    (confidence v a r).num * v.den ≤ v.num * (confidence v a r).den := by
  simp only [confidence, Frac.mul]
  have h := Nat.mul_le_mul ha hr
  have := Nat.mul_le_mul_left (v.num * v.den) h
  calc v.num * (a.num * r.num) * v.den
      = v.num * v.den * (a.num * r.num) := by
        rw [Nat.mul_assoc, Nat.mul_comm (a.num * r.num) v.den, ← Nat.mul_assoc]
    _ ≤ v.num * v.den * (a.den * r.den) := this
    _ = v.num * (v.den * (a.den * r.den)) := by rw [Nat.mul_assoc]

/-- Monotone in visibility: seeing more (same denominator) never lowers confidence. -/
theorem confidence_mono_visibility (v v' a r : Frac) (hd : v.den = v'.den) (h : v.num ≤ v'.num) :
    (confidence v a r).num ≤ (confidence v' a r).num ∧
    (confidence v a r).den = (confidence v' a r).den := by
  simp only [confidence, Frac.mul, hd]
  exact ⟨Nat.mul_le_mul_right _ h, trivial⟩

/-- Monotone in resolution: resolving more (same denominator) never lowers confidence. -/
theorem confidence_mono_resolution (v a r r' : Frac) (hd : r.den = r'.den) (h : r.num ≤ r'.num) :
    (confidence v a r).num ≤ (confidence v a r').num ∧
    (confidence v a r).den = (confidence v a r').den := by
  simp only [confidence, Frac.mul, hd]
  exact ⟨Nat.mul_le_mul_left _ (Nat.mul_le_mul_left _ h), trivial⟩

end Prism
