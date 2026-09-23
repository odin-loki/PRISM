import Prism.Verdict

/-!
Axiom audit of the main theorems. `proofs/check.sh` runs this file and
fails if any theorem depends on `sorryAx` or on an axiom outside
{propext, Classical.choice, Quot.sound}.
-/

open Prism

-- Law 2 (merge)
#print axioms proved_bounded_never_merge
#print axioms formal_never_merge
#print axioms certified_never_merges_weaker
#print axioms mergeRefusal_symm
-- Law 3 / Law 4 (CLEAN and model output never promoted)
#print axioms clean_never_promoted
#print axioms clean_never_rewritten_to_proof
#print axioms model_never_promoted
-- Law 1 (NOTRUN never clean)
#print axioms notrun_never_merges_clean
#print axioms notrun_never_becomes_clean
#print axioms notrun_never_clean
-- Admission
#print axioms admit_nonproving
#print axioms admit_fuzzer_never_proof
#print axioms admit_model_never_proof
#print axioms admit_certified_iff
#print axioms admit_rank_le
#print axioms admit_proof_requires_proof
#print axioms no_path_fuzzer_or_model_to_proof
#print axioms certified_only_with_certificate
-- Audit
#print axioms proving_stages
#print axioms audit_nonproving
#print axioms audit_flags_nonproving
#print axioms audit_certified
#print axioms audit_notrun
#print axioms audit_idem
-- Law 5 (confidence)
#print axioms confidence_zero_of_visibility_zero
#print axioms score_no_functions
#print axioms score_no_data
#print axioms confidence_le_one
#print axioms score_le_one
#print axioms confidence_le_visibility
#print axioms confidence_mono_visibility
#print axioms confidence_mono_resolution
