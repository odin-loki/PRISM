/-
`pir_lean_check FILE.pirl ...` — compare the C++ translator's PIR with the
proved Lean translator (see `PrismRefine/Check.lean`).  One line per
function, then a summary line
  `summary agree=N agree-reject=N outside=N mismatch=N`.
Exit status 1 when any function is a MISMATCH, 2 on usage errors.
-/
import PrismRefine.Check

open PrismRefine.Check

def main (args : List String) : IO UInt32 := do
  if args.isEmpty then
    IO.eprintln "usage: pir_lean_check FILE.pirl ..."
    return 2
  let mut counts : Array Nat := #[0, 0, 0, 0]
  for path in args do
    let text ← IO.FS.readFile path
    for r in parseFile text do
      let (v, msg) := check r
      let k := match v with
        | .agree => 0 | .agreeReject => 1 | .outside => 2 | .mismatch => 3
      counts := counts.modify k (· + 1)
      IO.println s!"{v.tag}\t{path}\t{r.name}\t{msg}"
  IO.println s!"summary agree={counts[0]!} agree-reject={counts[1]!} outside={counts[2]!} mismatch={counts[3]!}"
  return if counts[3]! > 0 then 1 else 0
