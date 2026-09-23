import Prism.Verdict

/-!
Truth tables of every function in `Prism.Verdict` over its whole finite
domain, as JSON. `lake exe verdict_tables` prints this; the output is
committed as `tests/data/verdict_tables.json` and the C++ doctest and the
Python test compare the running code with it entry by entry.

Confidence ranges over the naturals, so its table is a sample grid (every
count up to 3); its laws are proved for all inputs in `Prism.Verdict`.
-/

namespace Prism.Export

open Prism

def q (s : String) : String := "\"" ++ s ++ "\""

def b (x : Bool) : String := if x then "true" else "false"

def obj (fields : List (String × String)) : String :=
  "{" ++ ", ".intercalate (fields.map fun (k, v) => q k ++ ": " ++ v) ++ "}"

def arr (rows : List String) : String :=
  "[\n    " ++ ",\n    ".intercalate rows ++ "\n  ]"

def frac (f : Frac) : String := "[" ++ toString f.num ++ ", " ++ toString f.den ++ "]"

def verdictRows : List String :=
  Verdict.all.map fun v => obj [
    ("name", q v.name), ("is_proof", b v.isProof), ("is_formal", b v.isFormal),
    ("answered", b v.isAnswered), ("no_answer", b v.isNoAnswer),
    ("defect", b v.isDefect), ("model", b v.isModel), ("rank", toString v.rank)]

def originRows : List String :=
  Origin.all.map fun o => obj [("name", q o.name), ("may_prove", b o.mayProve)]

def stageRows : List String :=
  Stage.all.map fun s => obj [("name", q s.name), ("origin", q s.origin.name),
    ("may_prove", b s.mayProve)]

def mergeRows : List String :=
  Verdict.all.flatMap fun a => Verdict.all.map fun c =>
    obj [("a", q a.name), ("b", q c.name), ("refusal", q (mergeRefusal a c).name)]

def rewriteRows : List String :=
  Verdict.all.flatMap fun a => Verdict.all.map fun c =>
    obj [("from", q a.name), ("to", q c.name), ("allowed", b (mayRewrite a c))]

def admitRows : List String :=
  Origin.all.flatMap fun o => Verdict.all.flatMap fun v => [false, true].map fun c =>
    obj [("origin", q o.name), ("status", q v.name), ("certificate", b c),
         ("result", q (admit o v c).name)]

def auditRows : List String :=
  Stage.all.flatMap fun s => Verdict.all.flatMap fun v => [false, true].map fun c =>
    let r := audit s v c
    obj [("stage", q s.name), ("status", q v.name), ("certificate", b c),
         ("result", q r.status.name), ("violation", b r.violation)]

def upTo (n : Nat) : List Nat := List.range (n + 1)

def confidenceRows : List String :=
  (upTo 3).flatMap fun n => (upTo n).flatMap fun cl => (upTo n).flatMap fun t =>
    (upTo t).flatMap fun a => (upTo a).map fun r =>
      let s := score n cl t a r
      obj [("n_fun", toString n), ("classified", toString cl), ("attempted", toString t),
           ("answered", toString a), ("resolved", toString r),
           ("vis", frac s.vis), ("ans", frac s.ans), ("res", frac s.res), ("conf", frac s.conf)]

def tables : String :=
  "{\n" ++ ",\n".intercalate [
    "  " ++ q "source" ++ ": " ++ q "proofs/Prism/Verdict.lean (lake exe verdict_tables)",
    "  " ++ q "verdicts" ++ ": " ++ arr verdictRows,
    "  " ++ q "origins" ++ ": " ++ arr originRows,
    "  " ++ q "stages" ++ ": " ++ arr stageRows,
    "  " ++ q "merge" ++ ": " ++ arr mergeRows,
    "  " ++ q "rewrite" ++ ": " ++ arr rewriteRows,
    "  " ++ q "admit" ++ ": " ++ arr admitRows,
    "  " ++ q "audit" ++ ": " ++ arr auditRows,
    "  " ++ q "confidence" ++ ": " ++ arr confidenceRows] ++ "\n}\n"

end Prism.Export
