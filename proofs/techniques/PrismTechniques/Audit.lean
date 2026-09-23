/-
Axiom audit.  `#assert_axioms` fails the build if a theorem depends on any
axiom other than Lean's three standard ones (`propext`, `Classical.choice`,
`Quot.sound`) — in particular `sorryAx` (an unfinished proof) or
`Lean.ofReduceBool` (trusting compiled code, as `native_decide` and
`bv_decide` do).  It also prints the axioms each theorem uses, which is what
`docs/PROOFS_TECHNIQUES.md` records.
-/
import Lean
import PrismTechniques.KInduction
import PrismTechniques.Houdini
import PrismTechniques.Contracts
import PrismTechniques.BitblastEncode
import PrismTechniques.LazySeq
import PrismTechniques.FloatRound

open Lean Elab Command in
elab "#assert_axioms " id:ident : command => do
  let n ← liftCoreM <| realizeGlobalConstNoOverloadWithInfo id
  let axs ← liftCoreM <| collectAxioms n
  let allowed := [``propext, ``Classical.choice, ``Quot.sound]
  for a in axs do
    unless allowed.contains a do
      throwError "{n} depends on the non-standard axiom {a}"
  logInfo m!"{n} axioms: {axs.toList}"

namespace PrismTechniques

-- k-induction (roadmap 8.2)
#assert_axioms KInduction.reach_iff_path
#assert_axioms KInduction.kinduction_sound
#assert_axioms KInduction.step_iff_havoc_query_unsat
#assert_axioms KInduction.step_iff_havoc_split
#assert_axioms KInduction.step_one_iff
#assert_axioms KInduction.step_mono
#assert_axioms KInduction.kinduction_strengthened
#assert_axioms KInduction.kinduction_rel_sound

-- Houdini (roadmap 8.2)
#assert_axioms Houdini.houdini_rounds_le
#assert_axioms Houdini.houdini_inductive
#assert_axioms Houdini.houdini_sound
#assert_axioms Houdini.houdini_maximal
#assert_axioms Houdini.houdini_then_kinduction

-- Contracts and PROVED-ASSUMING (roadmap 8.2)
#assert_axioms Contracts.modular_sound
#assert_axioms Contracts.compose_layer
#assert_axioms Contracts.contract_violation_breaks_modularity
#assert_axioms Contracts.proved_assuming_is_implication
#assert_axioms Contracts.proved_assuming_not_proved
#assert_axioms Contracts.discharge
#assert_axioms Contracts.harness_assumption_discharged
#assert_axioms Contracts.caller_safe

-- Bit-blaster (roadmap 5.4 / 8.2)
#assert_axioms Bitblast.encode_spec
#assert_axioms Bitblast.sat_of_cnf_sat
#assert_axioms Bitblast.cnf_sat_of_sat
#assert_axioms Bitblast.toCNF_equisat
#assert_axioms Bitblast.toCNF_unsat_imp
#assert_axioms Bitblast.certified_unsat

-- Lazy sequentialisation (roadmap 8.2, stretch)
#assert_axioms LazySeq.lazy_seq_covers
#assert_axioms LazySeq.lazy_seq_sound
#assert_axioms LazySeq.lazy_seq_reach_iff

-- Floating point (roadmap 8.2, stretch, small piece)
#assert_axioms FloatRound.rne_exact
#assert_axioms FloatRound.rne_half_ulp
#assert_axioms FloatRound.rne_nearest
#assert_axioms FloatRound.rne_tie_even
#assert_axioms FloatRound.binary16_sig_bound

end PrismTechniques
