/-
`axiom_report ROOT...` — the axioms every declaration of a Lean project uses
(roadmap 8.5, independent proof rechecking).

Run it through `lake env` of the project to audit, so the project's `.olean`
files are on the search path:

    (cd proofs/semantics && lake env ../refinement/.lake/build/bin/axiom_report PrismSem)

It imports the given root modules, walks every constant defined in a module
whose name starts with one of the roots (not only the curated theorems of an
`Audit.lean`), computes its axioms with `Lean.collectAxioms`, prints the
union with a count per axiom, and exits 1 if any declaration depends on an
axiom other than `propext`, `Classical.choice` and `Quot.sound` (so `sorryAx`,
`Lean.ofReduceBool` from `native_decide` / `bv_decide`, `Lean.trustCompiler`
and user axioms all fail), listing the offenders.
-/
import Lean

open Lean

def allowed : List Name := [``propext, ``Classical.choice, ``Quot.sound]

def main (args : List String) : IO UInt32 := do
  if args.isEmpty then
    IO.eprintln "usage: axiom_report ROOT_MODULE..."
    return 2
  initSearchPath (← findSysroot)
  let roots := args.map String.toName
  let env ← importModules (roots.toArray.map fun m => { module := m }) {} (trustLevel := 0)
  let inRoots (m : Name) : Bool := roots.any (·.isPrefixOf m)
  let mut decls := 0
  let mut counts : Std.HashMap Name Nat := {}
  let mut bad : Array (Name × Array Name) := #[]
  for (n, ci) in env.constants.map₁.toList do
    let some idx := env.getModuleIdxFor? n | continue
    let some mod := env.header.moduleNames[idx.toNat]? | continue
    unless inRoots mod do continue
    match ci with
    | .thmInfo _ | .defnInfo _ | .axiomInfo _ | .opaqueInfo _ => pure ()
    | _ => continue
    decls := decls + 1
    let ctx : Core.Context := { fileName := "<axiom_report>", fileMap := default }
    let (axs, _) ← (collectAxioms n : CoreM (Array Name)).toIO ctx { env := env }
    for a in axs do
      counts := counts.insert a (counts.getD a 0 + 1)
    let extra := axs.filter (fun a => !allowed.contains a)
    unless extra.isEmpty do
      bad := bad.push (n, extra)
  IO.println s!"modules: {roots}"
  IO.println s!"declarations audited: {decls}"
  let sorted := counts.toList.toArray.qsort (fun a b => a.1.toString < b.1.toString)
  for (a, c) in sorted do
    IO.println s!"axiom {a}: used by {c} declarations"
  if bad.isEmpty then
    IO.println "ok: every declaration uses only propext / Classical.choice / Quot.sound"
    return 0
  for (n, extra) in bad do
    IO.println s!"FAIL {n}: {extra}"
  return 1
