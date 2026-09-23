/-
Axiom audit.  `lake env lean Audit.lean` prints the axioms of every headline
theorem; `check.sh` fails unless each list is within Lean's standard three
(`propext`, `Classical.choice`, `Quot.sound`).
-/
import PrismRefine

open PrismRefine

-- Parts 8.2 / 2.4: the translation LLVM fragment -> PIR is correct
#print axioms translate_exact
#print axioms strict_lazy
#print axioms pir_sound
#print axioms pir_sound_all
#print axioms pir_faithful_ret
#print axioms pir_faithful_fail
#print axioms pir_faithful_fuel
#print axioms pir_no_stop
-- Part 8.2 "Property instrumentation": the C++ checks test the LangRef conditions
#print axioms checks_bad
#print axioms nneg_bad
#print axioms checks_eq_ubBin
#print axioms checks_cover_ubBin
#print axioms test_sadd
#print axioms test_ssub
#print axioms test_smul
#print axioms test_uadd
#print axioms test_usub
#print axioms test_umul
#print axioms test_shlNuw
#print axioms test_shlNsw
#print axioms test_shlS
#print axioms test_lostL
#print axioms test_lostA
#print axioms test_inexactU
#print axioms test_inexactS
#print axioms shlNuw_core
#print axioms shlNsw_core
#print axioms shlS_core
#print axioms lostL_core
#print axioms ashr_shl_eq
-- simulation lemmas
#print axioms inst_sim
#print axioms insts_sim
#print axioms phis_sim
#print axioms term_sim
#print axioms run_sim
#print axioms emit_all
#print axioms inst_lift
#print axioms run_lift
-- M9 extended fragment: freeze, undef under freeze, direct calls (inlined),
-- for every certificate that passes the executable check `validB`
#print axioms translateX_exact
#print axioms strict_lazyX
#print axioms pir_sound_x
#print axioms pir_sound_all_x
#print axioms pir_faithful_ret_x
#print axioms pir_faithful_fail_x
#print axioms pir_faithful_fuel_x
#print axioms pir_no_stop_x
#print axioms sinstX_sim
#print axioms sinstsX_sim
#print axioms phisX_sim
#print axioms enter_sim
#print axioms argsX_osim
#print axioms step_sim
#print axioms run_simX
#print axioms step_lift
#print axioms run_liftX
#print axioms validB_facts
#print axioms accessChecks_run
#print axioms gLoop_sim
#print axioms gEnd_sim
#print axioms gFin_run
#print axioms storeVal_sim
#print axioms idxOps_sim
-- Part 8.2 "Floating point" (stretch): IEEE binary formats, correctly rounded addition
#print axioms PrismRefine.Float.roundU_repr
#print axioms PrismRefine.Float.roundU_nearest
#print axioms PrismRefine.Float.roundU_tie_even
#print axioms PrismRefine.Float.round_correct
#print axioms PrismRefine.Float.decode_encode
#print axioms PrismRefine.Float.add_correct
