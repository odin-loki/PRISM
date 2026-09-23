/-
Axiom audit.  `lake env lean Audit.lean` prints the axioms every headline
theorem depends on.  CI (`.github/workflows/proofs-semantics.yml`) fails if
any line mentions an axiom other than Lean's standard three
(`propext`, `Classical.choice`, `Quot.sound`) or `sorryAx`.
-/
import PrismSem

open PrismSem

-- Part 5.2: expression semantics, UB conditions
#print axioms ubExpr_correct
#print axioms ubE_ubExpr
-- Part 5.2: operational semantics
#print axioms BigStep.det
#print axioms BigStep.not_unwind
#print axioms run_sound
#print axioms run_mono
#print axioms run_adequate
#print axioms bigStep_iff_run
-- Part 8.2: property instrumentation
#print axioms run_instr
#print axioms instr_fail_ub_iff
#print axioms instr_no_ub
#print axioms instr_fail_user_iff
#print axioms bigStep_instr
#print axioms bigStep_instr_fail_ub_iff
-- Part 5.3 / 8.2: bounded encoder
#print axioms enc_spec
#print axioms encode_fail_iff
#print axioms encode_ub_iff
#print axioms encode_unwind_iff
#print axioms bmc_sound
#print axioms bmc_complete
#print axioms bmc_sound_unbounded
#print axioms bmc_complete_full
#print axioms bmc_complete_limit
#print axioms straightLine_encode_exact
#print axioms loopFree_encode_exact
#print axioms instr_encode_sound
-- Part 2.5 / 5.3 / 8.2: memory model, memory instrumentation, memory encoder
#print axioms Mem.MBigStep.det
#print axioms Mem.mrun_sound
#print axioms Mem.mrun_adequate
#print axioms Mem.mbigStep_iff_mrun
#print axioms Mem.mrun_minstr
#print axioms Mem.minstr_fail_ub_iff
#print axioms Mem.mbigStep_minstr_fail_ub_iff
#print axioms Mem.uaf_is_ub
#print axioms Mem.double_free_is_ub
#print axioms Mem.oob_is_ub
#print axioms Mem.null_deref_is_ub
#print axioms Mem.store_load_ok
#print axioms Mem.menc_spec
#print axioms Mem.mencode_fail_iff
#print axioms Mem.mencode_ub_iff
#print axioms Mem.mencode_unwind_iff
#print axioms Mem.mbmc_sound
#print axioms Mem.mbmc_complete
#print axioms Mem.mbmc_sound_unbounded
#print axioms Mem.mloopFree_encode_exact
#print axioms Mem.minstr_encode_sound
