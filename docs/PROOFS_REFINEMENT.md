# LLVM IR to PIR: refinement proof, correspondence checking, independent rechecking

`proofs/refinement/` proves that PRISM's LLVM→PIR translator
(`src/prism/pir/translate*.cpp`) does not lose undefined behaviour, for the
functions in a fragment of LLVM IR. It also proves PRISM's floating-point
checks against IEEE 754 and C11, and holds the tools that recheck every Lean
project in `proofs/`.

* A fragment of LLVM IR is given a formal semantics in Lean, twice: the
  LangRef semantics with lazy poison, and the strict semantics PRISM's
  instrumentation implements.
* The translator is mirrored in Lean, check for check, and proved correct:
  for every function it accepts, every input and every bound on executed
  blocks, **if the LLVM function reaches undefined behaviour or creates
  poison, the PIR program fails a check** (`pir_sound`), and every PIR
  outcome is an LLVM outcome (`pir_faithful_*`). The theorems hold for every
  fuel bound, so loops are covered for any number of iterations.
* An **extended fragment** (`freeze`, calls, stack and heap memory,
  pointers, globals, intrinsics, the libc models, exceptions) is proved per
  function, under a certificate the checker validates (`pir_sound_x`).
* `pir_lean_check` checks that the C++ translator's output **is** the proved
  translator's output, function by function.
* Every Lean project in `proofs/` is rechecked in CI by `leanchecker` and by
  nanoda, an independent Lean kernel, and every declaration's axioms are
  audited.

What is *not* proved is in [What remains](#what-remains); the places where
this work found `translate.cpp` unsound are in
[Findings](#findings-in-translatecpp).

## Project

| | |
|---|---|
| Location | `proofs/refinement/`, a Lake project (`PrismRefine`) |
| Toolchain | `leanprover/lean4:v4.34.0`, core Lean only (no Mathlib) |
| Dependencies | `proofs/semantics` (`PrismSem.evalBin`, `evalPred`, `ubBin`) and `proofs/techniques` (`PrismTechniques.FloatRound.rne`), both as Lake path dependencies |
| Build | `cd proofs/refinement && lake build` |
| Audit | `./check.sh`: build with no warning; no escape hatch in any source (`sorry`, `admit`, `native_decide`, `bv_decide`, `implemented_by`, `extern`, top-level `axiom` or `unsafe`); every theorem in `Audit.lean` within `propext` / `Classical.choice` / `Quot.sound`; every `fixtures/*.pirl` gives exactly its `fixtures/*.expected` line. `proofs/check.sh` runs it as step 5 |
| Executables | `pir_lean_check` (`CheckMain.lean`), `llvm_eval` (`EvalMain.lean`), `axiom_report` (`AxiomReport.lean`) |
| Size (2026-11-05, commit 991c4eafa) | 15 778 lines of Lean; 104 theorems in `Audit.lean`; 55 fixtures |

| File | Contents |
|---|---|
| `PrismRefine/Llvm.lean` | LLVM fragment syntax; LangRef semantics `lRun` (lazy poison); strict semantics `sRun` |
| `PrismRefine/Pir.lean` | PIR as `include/prism/pir.hpp` represents it (CFG, phis, `check`/`assume`, var table) and the semantics of `prism::pir::interpret` |
| `PrismRefine/Translate.lean` | The base translator, mirroring `translate.cpp` |
| `PrismRefine/Ops.lean` | The C++ test operators (`sadd.ovf`, `shl.nsw.ovf`, …) equal the LangRef conditions |
| `PrismRefine/Refine.lean`, `Lazy.lean`, `Sound.lean` | Simulation (`translate_exact`), strict vs LangRef (`strict_lazy`), headline theorems and the link to `PrismSem.ubBin` |
| `PrismRefine/XMem.lean` | Extended fragment: the byte memory and the `World` (oracle counter + memory) both sides share |
| `PrismRefine/XLlvm.lean`, `XPir.lean` | Extended LLVM syntax and semantics on a frame stack; PIR with oracle `havoc` and memory statements |
| `PrismRefine/XTranslate.lean`, `XValid.lean` | The extended translator (inlining, `MemTr`, exceptions), its certificate, the executable check `validB` |
| `PrismRefine/XMemSim.lean`, `XMemOps.lean`, `XHeap.lean`, `XEscape.lean`, `XHavoc.lean` | The memory checks, `getelementptr`, `memcpy`/`memmove`/`memset`, the libc models' allocation and release, the stack-escape check, the stdio model primitives |
| `PrismRefine/XRefine.lean`, `XStep.lean`, `XRun.lean`, `XValidSpec.lean` | Simulation: `translateX_exact` |
| `PrismRefine/XLazy.lean`, `XSound.lean` | Strict vs LangRef side (`strict_lazyX`); headline theorems `pir_sound_x` … |
| `PrismRefine/Check.lean`, `CheckMain.lean` | `pir_lean_check`: the correspondence checker |
| `PrismRefine/Float.lean`, `FloatOps.lean` | IEEE 754 binary formats and operations; PRISM's FLOAT-* conditions |
| `EvalMain.lean` | `llvm_eval`: runs the three semantics on concrete inputs (for `prism-qa llvm-sem-vs-lli`) |
| `AxiomReport.lean` | `axiom_report`: axioms of every declaration of a project |
| `recheck.sh` | Independent recheck of any of the four Lean projects |

## The LLVM fragment and its semantics

The base fragment is what `translate.cpp` handles in a single function
without calls whose values are all integers:

* SSA registers of type `iN`, `1 ≤ N ≤ 64`; integer constants; `poison`;
* `add`/`sub`/`mul` with `nsw`/`nuw`; `udiv`/`sdiv` with `exact`;
  `urem`/`srem`; `shl` with `nsw`/`nuw`; `lshr`/`ashr` with `exact`; `and`;
  `or` with `disjoint`; `xor`;
* `icmp` (ten predicates), `select`, `zext` (with `nneg`), `sext`, `trunc`,
  `phi`;
* `br label`, `br i1`, `ret`, `ret void`, `unreachable`;
* `call iN @__prism.uninit.iN()`, the marker the pir stage inserts before
  mem2reg for every scalar local and parameter slot.

Control state is (previous block, current block, register file); a run is a
deterministic interpreter whose fuel counts executed blocks. Values are
naturals read as `BitVec.ofNat w`; the value of every operation is
`PrismSem.evalBin` / `evalPred` from the PIR semantics.

**LangRef semantics (`lRun`)** — the documented assumptions:

| Construct | Semantics |
|---|---|
| `add/sub/mul nsw` | poison if the mathematical signed result is out of range |
| `add/sub/mul nuw` | poison if the mathematical unsigned result is out of range |
| `udiv/urem`, `sdiv/srem` | UB if the divisor is 0; UB for `INT_MIN / -1` and `INT_MIN % -1`; UB if the divisor is poison; with a poison dividend, UB when the divisor makes some dividend UB |
| `udiv/sdiv exact` | poison if the dividend is not a multiple of the divisor |
| `shl/lshr/ashr` | poison if the amount is `≥ w` |
| `shl nuw` / `shl nsw` | poison if `a·2^b` is out of the unsigned / signed range |
| `lshr/ashr exact` | poison if a set bit is shifted out |
| `or disjoint` | poison if the operands share a set bit |
| `zext nneg` | poison if the operand is negative |
| arithmetic, `icmp`, casts | poison in, poison out |
| `select` | poison condition gives poison; otherwise the chosen operand |
| `phi` | copies the incoming value, poison included |
| `br i1 poison` | UB |
| `ret poison` | UB — PRISM's rule, matching clang's `noundef` on C return values |
| `unreachable` | UB |
| `@__prism.uninit.iN()` | an *indeterminate* value; using it (any operand except a phi input) is UB (C11 6.3.2.1p2); phis copy it |
| `shl` marked `csigned` | UB if the base is negative or `a·2^b ≥ 2^(w-1)` (C11 6.5.7p4). The pir stage learns from clang which `shl` came from a C signed `<<` (`TranslateOptions::signed_shl`) and the exporter passes that fact per instruction |
| undefined register, missing phi entry, branch to a missing block | *stuck* (IR the verifier rejects; no claim is made) |

The run also reports whether poison was ever **created** (a flag violation,
or the literal `poison` read).

**Strict semantics (`sRun`)** — identical, except that creating poison is
itself UB. This is what PRISM checks: `translate.cpp` inserts every check at
the instruction, not at the use.

The approach follows Vellvm (a formal operational semantics of LLVM IR with
poison and UB as explicit outcomes) but is written directly in Lean for this
fragment, without interaction trees.

## The translator and the theorems

`Translate.lean` mirrors `translate.cpp` statement for statement: variable
numbering, block numbering, each operand, check and assignment emitted in
the same order with the same arguments, widths, property name and taxonomy
class; `unreachable` becomes `check 1; stop`; unresolvable phi predecessors
are dropped as `resolve_phis` does. Uninitialised locals are mirrored from
`Tr::enter_frame` / `Tr::operand`: the marker becomes `havoc`, every
register that may carry its value gets a shadow, and every use of a
register with a shadow is preceded by `check shadow` (UNINIT-READ). Where
the C++ translator would throw `UNENCODED`, or the input is outside the
fragment, `translate` returns an error.

| Theorem (`PrismRefine.`) | Statement |
|---|---|
| `translate_exact` | For every accepted `F`, inputs, fuel: strict LLVM returns `v` ⇒ PIR returns `v`; strict LLVM UB ⇒ PIR fails a check; strict out of fuel ⇒ PIR out of fuel (strict *stuck*: no claim) |
| `strict_lazy` | Strict returns / runs out of fuel / is stuck ⇒ LangRef does the same and created no poison; strict UB ⇒ LangRef is UB, creates poison, or is stuck |
| **`pir_sound`** | LangRef run has UB or creates poison within `n` blocks ⇒ PIR fails a check within `n` blocks |
| `pir_sound_all` | If no PIR run fails for any bound, no LangRef run has UB or creates poison, for any bound |
| `pir_faithful_ret` | PIR returns `v` ⇒ LangRef returns `v` with no UB and no poison (or LLVM is stuck) |
| `pir_faithful_fail` | PIR fails a check ⇒ LangRef has UB or creates poison (or is stuck) |
| `pir_faithful_fuel`, `pir_no_stop` | PIR out of fuel ⇒ LangRef out of fuel; PIR never stops or blocks on its own |

Straight-line code, branches with phis (parallel assignment from the
predecessor) and back edges are all handled by `run_sim`, by induction on
fuel.

## The extended fragment

The theorems above hold for every function the base translator accepts.
The extended fragment is proved *per function, under a certificate*:
`pir_lean_check` runs a second Lean translator (`translateX`) that also
returns a certificate, demands that the C++ PIR be exactly that
translator's output, and evaluates the executable check `validB M F P C`.
Every theorem below is stated for every module `M`, function `F`, PIR
function `P` and certificate `C` with `validB M F P C = true`; the verdict
for such a function is `agree-ext`.

| Construct | Accepted | Semantics (strict / LangRef side) |
|---|---|---|
| `freeze` | of any fragment operand, of `poison`, of `undef` | the operand's value; `freeze undef` and `freeze poison` draw one arbitrary value from the oracle |
| `undef` | only as the operand of `freeze` or of `store` | anywhere else it is refused (finding 4) |
| direct `call @f(…)` | functions defined in the module that `Tr::call` inlines, up to the inline depth, non-recursive; not a by-value aggregate parameter of a callee | a new frame; `ret` resumes the caller. An inlined frame's `alloca`s are freed at its `ret` |
| `alloca` | constant size below 2^47 bytes | a new live stack object, every byte uninitialised |
| `load iN` / `store iN`, pointers | `N ≤ 64`; `ptr` values in phis, selects, loads/stores (8 bytes), calls and returns; `null` | UB through null, a pointer to no object, a freed object, out of bounds, misaligned, or (store) into a read-only object; reading an uninitialised byte is UB in the strict semantics (PRISM's rule) and flagged on the LangRef side |
| `getelementptr [inbounds]` | constant struct field indices, integer first and array indices | offset in signed 64 bits, each step overflow-checked (C17 6.5.6p8); an array index stays within its array; arithmetic on null, `inbounds` leaving the object, a move to another object: UB / poison |
| `icmp` on pointers | `eq`/`ne`; relational predicates | `eq`/`ne` compare the 64-bit pointer (distinct objects never compare equal); relational: UB unless both point into the same object (PTR-COMPARE) |
| `ptrtoint`, `sub` of two `ptrtoint`s | | a copy; the difference is UB on different objects (as PTR-COMPARE) |
| `ret ptr` | of the analysed function or an inlined callee | UB if the pointer points into one of that frame's own stack objects (MEM-STACK-ESCAPE) |
| pointer parameters | `byval`/`byref`/`sret`, or a contract with a constant element count | the object the harness provides. An unbound pointer parameter is refused by both translators (`agree-reject`, NEEDS-HARNESS, Law 6) |
| globals | named by the analysed function or an inlined callee; not thread-local | allocated in the prologue (see [Globals](#globals)); read-only data and globals of `main` hold their initialiser, other mutable or external globals arbitrary bytes |
| intrinsics | `llvm.smax`/`smin`/`umax`/`umin`, `abs`, `ctlz`/`cttz`, `ctpop`, `bswap`, `expect`, the six `*.with.overflow` with `extractvalue`, `lifetime.end`, `memcpy`/`memmove`/`memset` | the LangRef values; the poison cases of `abs`/`ctlz`/`cttz` are checked (INT-SIGNED-OVF, INT-CLZ-ZERO); `memcpy`/`memmove`/`memset` with a non-zero length are UB on a bad access or (`memcpy`) overlapping ranges |
| libc model primitives | `__prism_alloc`, `__prism_free`/`__prism_free_check`, `__prism_obj_size`, `__prism_alloc_failed`, `__prism_havoc_bytes`, `__prism_read_range`, `__prism_check`, `__prism_memset`, `__prism_fresh_cstr`, FILE release | the model's allocation (kinds heap, `new`, `new[]`), release checks (MEM-INVALID-FREE, MEM-MISMATCHED-FREE, MEM-DOUBLE-FREE) and stdio primitives |
| nondeterminism | `__VERIFIER_nondet_*()`, `nondet_*()` (undefined in the module); `__VERIFIER_assume`, `__prism_assume` | a value from the oracle (PIR `havoc`); a false assumption makes the LLVM run stuck (no claim), PIR `assume` |
| noreturn failure calls | undefined `__assert_fail` and its variants, `reach_error`/`__VERIFIER_error`, `llvm.trap`/`debugtrap`/`ubsantrap`, `std::__glibcxx_assert_fail`, `abort()` (unless the function can reach `__prism_alloc_failed`) | UB at the block's end; PIR: the failing check, then `stop` |
| scalar pairs | `{ptr\|iN, ptr\|iN}` returned by inlined callees or the analysed function | two values; each pointer field gets the stack-escape check |
| exceptions | `invoke`; landing pads; user `throw` in clang's exact pattern; `std::__throw_*`; `catch (...)` and catch by exact type; `__cxa_end_catch` with destructors; `__cxa_rethrow`; `std::terminate` and `__clang_call_terminate` | the LLVM run makes no claim past a throw (*stuck*); PIR mirrors `Tr`'s dispatch. A catch through a base class is refused |

**Nondeterminism.** Both semantics take an oracle `ω : Nat → Nat` and read it
in execution order; the PIR semantics gives the `t`-th `havoc` the value
`ω t`. The theorems quantify over every `ω`, the same on both sides: for
every choice of the values LLVM leaves open, PIR making the same choices has
the same outcome.

**Calls.** The LLVM machine is a stack of frames; a block is split at its
calls into segments, and one machine step runs one segment. Each inlined
call is an *instance* of the callee with its own names, shadows and
variable range. The certificate records, per instance, the function, the
maps, the variable range, the return target and the PIR block of every
segment; `validB` checks what the proof needs of it.

**Memory** (`XMem.lean`). The object/offset model of `proofs/semantics`
extended to what the C++ engine models (`ConcMem`): each object has a kind
(stack, static, read-only, …) and a base alignment, each byte is
initialised or not, and a pointer is one 64-bit value with the object id in
bits 63..48 and the offset in bits 47..0. Both semantics share this memory
and thread it with the oracle counter as one `World`.

| Theorem (`PrismRefine.`) | Statement |
|---|---|
| `translateX_exact` | strict LLVM returns `v` ⇒ PIR returns `v`; strict UB ⇒ PIR fails a check; out of fuel ⇒ out of fuel (same oracle, every fuel bound) |
| `strict_lazyX` | the strict semantics refines the LangRef-side semantics |
| **`pir_sound_x`** | LangRef-side run has UB, creates poison or reads uninitialised memory within `n` segments ⇒ PIR fails a check within `n` blocks |
| `pir_sound_all_x` | no PIR run fails for any oracle and bound ⇒ no LLVM run is bad for any oracle and bound |
| `pir_faithful_ret_x` / `_fail_x` / `_fuel_x`, `pir_no_stop_x` | every PIR outcome is an LLVM outcome for the same oracle (or LLVM is stuck) |
| `accessChecks_run` | the statements of `MemTr::access_checks` fail exactly when the access is bad (`accessBad`) |
| `gLoop_sim`, `gEnd_sim`, `gFin_run` | the statements of `MemTr::gep` compute the `getelementptr` result and fail exactly where it is undefined |
| `unChecks_bad` | the checks inserted for `llvm.abs` / `ctlz` / `cttz` fail exactly when the LangRef result is poison |
| `accChkG_run`, `overlapChk_run`, `memcpy_run`, `memset_run` | the guarded access checks and the `memcpy` overlap check fail exactly on `cpyBad`, for every 64-bit length; otherwise the statements produce the LLVM memory |
| `pcmp_val`, `pcmp_run` | the relational pointer comparison `MemTr::icmp` emits fails exactly on pointers into different objects and otherwise computes LLVM's predicate |
| `escLoop_run`, `escChk_run`, `escHit_ok` | `MemTr::stack_escape_check` fails exactly when the returned pointer points into one of the frame's own stack objects |
| `freeChk_run`, `freeChkF_run` | `MemTr::dealloc` fails exactly on `freeBad`; the FILE release |
| `hobjszCore_run`, `havoc_run`, `hread_run`, `pred_eq`, `hmemset_run`, `freshCore_run` | the libc model primitives |
| `freeAll_ok` | an inlined frame's stack objects are freed at its `ret` |
| `sinstX_sim`, `phisX_sim`, `enter_sim`, `step_sim`, `run_simX` | one instruction, the phis, a call, one segment, a run |

The memory model is PRISM's, shared by both sides, so the theorem is "PIR
agrees with LLVM *in this memory model*" (a flat 48-bit offset per object,
at most 2^16 objects, provenance only through the object id), not a proof
that the model is LLVM's. The exporter computes allocation sizes, field
offsets and array lengths with the translator's own `pirmem::Layout`, and a
global's size, alignment, kind and initialiser with the functions the
translator uses (`pirmem::entry_global`, `flat_init`); those computations
are trusted, as is the exporter's rendering of LLVM into the fragment
syntax.

### Globals

The translator allocates the variables of the globals the analysed function
and its inlined callees name (`pirmem::entry_globals`, in call order) with
the function's own values, before its results (`Tr::enter_frame`), and
initialises them first in the prologue (`Tr::run_frame`,
`MemTr::emit_entry_globals`). The hidden "an allocation failed" flag object
of the malloc models follows, when the function can reach
`__prism_alloc_failed` (`pirmem::reaches_alloc_failed`), and then the
caught-exception slot when it can reach `__cxa_begin_catch`,
`__cxa_end_catch` or `__cxa_rethrow` (`pirmem::reaches_caught_slot`). The
Lean model treats each as an instruction at the start of the entry block
that allocates and initialises it (`SInst.glob`, exported as `L glob`): the
entry block has no predecessors, so this runs once, before anything reads
it. The exporter passes these objects to inlined callees as extra
parameters. Globals with an `undef` initialiser and thread-local globals
stay outside.

## Property instrumentation: the C++ list, proved

Every check `translate.cpp` inserts in the base fragment, the condition it
tests (the C++ `eval_op`, written in Lean in `Pir.lean`), and the lemma
proving it is the LangRef condition. `checks_bad` combines them: the checks
inserted for one instruction fire exactly when the strict semantics has UB
there.

| Instruction | C++ check (`Op`, prop, class) | Proved equal to | Lemma |
|---|---|---|---|
| `add nsw` | `SAddOvf`, `ovf+`, INT-SIGNED-OVF | signed sum out of range | `test_sadd` |
| `add nuw` | `UAddOvf`, `wrap+`, UB-POISON | unsigned sum `≥ 2^w` | `test_uadd` |
| `sub nsw` / `nuw` | `SSubOvf` `ovf-` / `USubOvf` `wrap-` | signed difference out of range / `a < b` | `test_ssub`, `test_usub` |
| `mul nsw` / `nuw` | `SMulOvf` `ovf*` / `UMulOvf` `wrap*` | signed / unsigned product out of range | `test_smul`, `test_umul` |
| `udiv`, `urem` | `Eq b 0`, `div0`/`mod0`, INT-DIV-ZERO | divisor 0 | `checks_bad` |
| `udiv exact` | `InexactU`, `exact`, UB-POISON | `a mod b ≠ 0` | `test_inexactU` |
| `sdiv`, `srem` | `Eq b 0` `div0`/`mod0`; `SDivOvf` `divovf` INT-SIGNED-OVF | divisor 0; `INT_MIN / -1` | `checks_bad` |
| `sdiv exact` | `InexactS`, `exact`, UB-POISON | `b ∤ a` (integers) | `test_inexactS` |
| `shl`, `lshr`, `ashr` | `ShiftOob`, `shift`, INT-SHIFT-UB | amount `≥ w` | `checks_bad` |
| `shl` (C signed `<<`) | `ShlSOvf`, `shift-base`, INT-SHIFT-UB | base negative or `a·2^b ≥ 2^(w-1)` | `shlS_core`, `test_shlS` |
| `shl nsw` | `ShlNswOvf`, `shl-nsw` | `a·2^b` out of the signed range | `shlNsw_core`, `test_shlNsw` |
| `shl nuw` | `ShlNuwOvf`, `shl-nuw` | `a·2^b ≥ 2^w` | `shlNuw_core`, `test_shlNuw` |
| `lshr exact` | `LostBitsL`, `exact` | `a mod 2^b ≠ 0` | `lostL_core`, `test_lostL` |
| `ashr exact` | `LostBitsA`, `exact` | `a mod 2^b ≠ 0` | `ashr_shl_eq`, `test_lostA` |
| `or disjoint` | `And` then `Ne 0`, `disjoint`, UB-POISON | common set bit | `checks_bad` |
| `zext nneg` | `Slt a 0`, `nneg`, UB-POISON | operand negative | `cast_bad` |
| `poison` operand | `check 1`, `poison`, UB-POISON | use of poison | `opnd_sim` |
| `unreachable` | `check 1`, `unreachable`, CXX-UNREACHABLE | reached | `term_sim` |
| use of a register carrying `@__prism.uninit` | `check shadow`, `uninit`, UNINIT-READ | indeterminate value used | `opnd_sim`, `phis_sim` |

`checks_eq_ubBin` proves that, on the flags `PrismSem.Flags` models (`nsw`/
`nuw` on `add`/`sub`/`mul`, division and shift-amount UB), these checks fire
exactly when `PrismSem.ubBin` — the UB definition behind
`PrismSem.instr_fail_ub_iff` — holds, and `checks_cover_ubBin` that they
cover it for every flag combination. `PrismSem.ubBin` does not model
`exact`, `disjoint`, `nneg`, `shl nsw/nuw` or the C signed-shift rule; the
C++ checks do, and those are proved here against the LangRef conditions.
`tests/cpp/test_pir_refinement.cpp` locks the check-name and operator-name
parity between `translate.cpp` and the Lean files without running Lean.

## Per-run correspondence checking

`src/prism/pir/export_lean.cpp`: when `PRISM_PIR_LEAN_EXPORT` is set (`1`
for `<out>/pir-lean/`, or a directory), the pir stage appends, per function,
the LLVM function in fragment syntax and the PIR `translate()` produced (or
its UNENCODED reason) to `<unit>.pirl`. Anything outside the fragment is
written as `L unsupported <why>`. With `PRISM_PIR_EXPORT_ONLY=1` as well,
the stage only exports and records each function `NOTRUN` (no model
checking).

`pir_lean_check FILE.pirl…` re-translates the LLVM side with the proved
translator and demands the C++ PIR be *exactly* its output (variables and
widths, blocks, phis, statements, operators, argument widths, check names
and classes, terminators). Verdicts:

| Verdict | Meaning |
|---|---|
| `agree` | base fragment; the base theorems hold of the PIR PRISM verified |
| `agree-ext` | extended fragment; the PIR is exactly `translateX`'s and the certificate checks, so the `_x` theorems hold of it |
| `agree-reject` | both translators refuse the function (unbound pointer parameter, recursion, …) |
| `outside` | outside the fragment; no claim |
| `MISMATCH` | the C++ PIR differs from the proved translator's (exit 1) |

`prism-qa pir-lean-check [TREE…] --bin build/prism`
(`src/tools/qa/pir_lean_check.cpp`) runs PRISM with both variables set and
prints a total line `total: agree=… agree-ext=… agree-reject=… outside=…
mismatch=… (functions in the proved fragment: …)`. `prism_tests` runs the
fixtures when `pir_lean_check` is built and reports NOTRUN otherwise
(`tests/cpp/test_qa.cpp`).

The 55 fixtures in `proofs/refinement/fixtures/` are real C++ output for
`testdata/pir` and hand-written cases, one per construct. The `dropped_*`
and `*_neg*` fixtures have a check removed or a field swapped and must be
reported as `MISMATCH` (or `outside` where the construct is refused).

### Coverage

Last full measurement: 2026-10-30, commit 30eca81b3, `prism-qa
pir-lean-check` over the five subdirectories of `testdata/conformance`
(concurrency, esbmc-cpp, libc-models, prism, sv-comp), one PRISM run each:

| `agree` | `agree-ext` | `agree-reject` | `outside` | `MISMATCH` |
|---|---|---|---|---|
| 249 | 785 | 288 | 652 | **0** |

The largest causes outside the fragment at 36dfbca60 (2026-10-30), counted
over the exported `.pirl` files (which repeat inlined callees): `call @abort`
341, `getelementptr` cases about 70, indirect calls 25,
`__cxa_allocate_exception` 17, `pthread_create` 16, `__throw_logic_error`
14, `pthread_mutex_lock` 12, `printf` 12, floating point about 42, raw
aggregate loads 14. `abort()` entered the fragment after that tally; the
coverage has not been re-measured since.

## Testing the formal semantics against `lli`

`prism-qa llvm-sem-vs-lli` (`src/tools/qa/llvm_sem_vs_lli.cpp`) runs every
fragment function of the given C files on edge-case and random inputs with
`llvm_eval` (LangRef, strict and PIR semantics), executes each input the
LangRef semantics says is defined with `lli` on the IR the pir stage
verifies (`pir::lower_to_ir`), and compares the printed result. It also
checks that the three Lean semantics agree as proved. It runs the given
programs under `lli`: use it on trusted test programs only. Exit 1 on any
disagreement, 2 when a tool is missing.

## Floating point

`Float.lean` models IEEE 754 binary formats (`binary32 = ⟨24, 8⟩`,
`binary64 = ⟨53, 11⟩`, `binary16 = ⟨11, 5⟩`; any precision `p ≥ 2` and
exponent width `ew ≥ 2`) at the bit level, with `decode` per IEEE 754-2019
§3.4. Every finite datum is an integer multiple of the smallest subnormal
`2^-D`, so sums are exact integers and products and quotients are rationals
`a / b`, rounded by `roundQ`/`roundF`. Round to nearest, ties to even
throughout.

| Theorem (`PrismRefine.Float.`) | Statement |
|---|---|
| `roundU_repr`, `roundU_nearest`, `roundU_tie_even` | round-to-nearest-even of an integer with unbounded exponent returns a `p`-bit value at least as close as every `p`-bit value; ties give an even significand |
| `roundQ_repr`, `roundQ_nearest`, `roundQ_tie_even` | the same for a rational `a / b` (`roundQ_one`: with `b = 1` it is `roundU`) |
| `round_correct`, `roundF_correct` | in the format: the nearest finite datum when it does not overflow; overflow to infinity only when the exact magnitude exceeds the largest finite one |
| `decode_encode` | every representable magnitude is encoded exactly |
| `add_correct`, `sub_correct` | bit-level `add` / `sub` of two finite data decode to the correctly rounded exact sum / difference, with IEEE's sign of an exact zero and overflow to infinity |
| `mul_correct`, `div_correct` | bit-level `mul` / `div` (non-zero divisor) of finite data decode to the correctly rounded exact product / quotient |

`add`, `sub`, `mul` and `div` also define every special case as IEEE 754
§6–7 (NaN operands, `∞ − ∞`, `0 × ∞`, `0 / 0`, `∞ / ∞`, `x / ±0`, `∞ / y`,
`x / ∞`).

**PRISM's floating-point checks.** `FloatOps.lean` mirrors, operator by
operator, the conditions `FpTr::checks` (`src/prism/pir/translate_fp.cpp`,
`--fp-checks`) builds for `fadd`/`fsub`/`fmul`/`fdiv`, and the
FLOAT-CAST-OVF condition the encoder builds for `fptosi`/`fptoui`
(`Op::FToSIOvf` / `Op::FToUIOvf` in `src/prism/pir/encode.cpp`).
`tests/cpp/test_repolint.cpp` locks every C++ expression to its Lean
definition. The IEEE side is written from IEEE 754-2019 §7.2–7.4 and C11
6.3.1.4, not from the code.

| Theorem | Statement |
|---|---|
| `prism_overflow_eq` | FLOAT-OVERFLOW **equals** IEEE overflow, for every operation and every pair of operands, special values included |
| `prism_invalid_eq` | FLOAT-INVALID **equals** IEEE invalid operation on quiet operands |
| `ieee_invalid_eq` | IEEE invalid = FLOAT-INVALID **or** a signalling-NaN operand. PRISM does not report the signalling-NaN case: Z3's FP theory has one NaN and LLVM's default floating-point environment does not preserve signalling NaNs |
| `prism_divzero_eq` | FLOAT-DIV-ZERO = IEEE divide-by-zero **or** `0 / 0` **or** `±∞ / 0` (wider than IEEE; `prism_divzero_extra` shows both extra cases are real) |
| `ieee_divzero_imp_prism` | every IEEE divide-by-zero is reported |
| `cast_ovf_iff` | for any format, any `k`, signed or unsigned, and any well-formed datum: FLOAT-CAST-OVF holds **iff** the value is outside the range of `iK` (C11 6.3.1.4), including formats where the bound is not representable |

Trusted, not proved: that Z3's floating-point theory implements IEEE 754,
and that `Z3_mk_fpa_numeral_double` rounds a numeral to nearest even. Not
covered: the other rounding modes; `frem`, `sqrt`, `fma`/`fmuladd` and libm;
NaN payloads; the underflow and inexact flags (PRISM does not check them);
the `fptrunc` overflow condition (same shape as the others, not a Lean
theorem); and the step from these Lean definitions to the Z3 terms beyond
the text-level lock of the test.

## Independent proof rechecking

`proofs/refinement/recheck.sh PROJECT ROOT…` runs, for one Lean project:

1. `lake build`;
2. a source scan (no `sorry`, `native_decide`, `bv_decide`, top-level
   `axiom` or `unsafe`);
3. `axiom_report`: the axioms of **every** declaration under the root
   modules; fails on anything but `propext`, `Classical.choice`,
   `Quot.sound` (so `sorryAx`, `Lean.ofReduceBool` and `Lean.trustCompiler`
   all fail);
4. `leanchecker` (shipped with the Lean toolchain): replays every
   declaration of every `.olean` module into a kernel environment;
5. **nanoda** (an independent Lean kernel in Rust) type-checks the
   `lean4export` export of the roots and all their dependencies from
   scratch. `Lean.trustCompiler` is permitted for the environment only (core
   `Init` declares it); step 3 already rejects any PRISM declaration that
   uses it.

`lean4export` and `nanoda` are pinned in `third_party/MANIFEST.toml` and
built by `prism-deps tool lean4export nanoda`. A missing checker is NOTRUN
(exit 3), never a pass. `.github/workflows/proofs-recheck.yml` runs the
recheck and each project's own check script for `proofs/`,
`proofs/semantics`, `proofs/techniques` and `proofs/refinement` on every
change under `proofs/`, uploads each axiom list, and attaches them to every
published release.

## Findings in `translate.cpp`

The proofs found places where the C++ translator was not a sound
over-approximation of the LangRef semantics. Source comments and doctests
cite them by number.

1. **Poison incoming to a phi was havocked without a check.** Fixed: a
   `check 1` (UB-POISON) is emitted on the poison edge (doctest "pir: poison
   flowing into a phi is a checked violation"). The Lean translator still
   refuses poison phi inputs, so such functions stay `outside`.
2. **`icmp samesign` was ignored.** Fixed: it fails a UB-POISON check (prop
   `samesign`) where the poison is created; on pointers it is `UNENCODED`.
   The Lean model has no such flag, so such functions stay `outside`.
3. **Checks at creation over-approximate.** PRISM fails a check where poison
   is *created*, even if the value is never used. For C this is the C
   semantics; for optimiser-introduced flags (`nuw`, `exact`, `disjoint`,
   `nneg`) it can report a failure LLVM would not have. `pir_faithful_fail`
   states this precisely: a PIR failure means UB *or poison creation*.
4. **`undef` outside `freeze` was havocked once.** Fixed: every value
   computed from `undef` carries a one-bit undef shadow; each non-phi use
   reads a fresh havoc, a store writes uninitialised bytes, and `br` on it,
   a `noundef` return or a `noundef` argument fails a UB-POISON check (prop
   `undef`). The Lean model accepts `undef` only under `freeze` and `store`,
   so functions with `undef` elsewhere stay `outside`.
5. **`freeze poison` was reported.** Fixed: translated exactly as
   `freeze undef` (one `havoc`, no check). The Lean checker reads a literal
   `poison` under `freeze` or `store` as `undef` (`fopnd?` in `Check.lean`).
6. **`llvm.memcpy` with the same source and destination was reported.**
   Fixed: the pir stage compiles with `-fno-builtin-memcpy`, so a C `memcpy`
   call reaches its library model (C's rule: any overlap is MEM-OVERLAP),
   while the intrinsic follows the LangRef (exact self-copy allowed). The
   Lean `cpyBad` and `overlapChk` changed with it (`overlapChk_run`,
   `cpyBad_eq`).
7. **`llvm.lifetime.start` on an object whose lifetime ended was
   reported.** Fixed in the C++ translator with a `revive` statement that
   makes a stack object live again. The Lean PIR has no such statement, so
   the Lean translator refuses `llvm.lifetime.start`: no theorem covers
   such functions. The
   pir stage compiles at `-O0`, where clang emits no lifetime markers. Open:
   `llvm.lifetime.end` on a pointer into a non-stack object ends that whole
   object, where the LangRef only makes its bytes poison.
8. **A false `llvm.assume` was not reported.** Fixed: it is a
   FUNC-CONTRACT check of the condition (prop `assume`), then the `assume`.
   This also exposed soundness bug S9 (`docs/CONFORMANCE.md`): an `assume`
   used to constrain checks before it in the same block; it now strengthens
   only the guard of later statements (proved in
   `proofs/techniques/PrismTechniques/LoopCut.lean`). The Lean translator
   still refuses `llvm.assume`.

A value difference that is not a soundness issue: after a failed division
check the C++ interpreter uses Z3's `bvudiv x 0 = ~0`, the Lean model
`x / 0 = 0`; the value is unobservable because the check already failed.

## What remains

* **Constructs outside the fragment.** Library calls the fragment does not
  inline or model (the format family, threads, unmodelled POSIX calls,
  `exit`), indirect calls, floating point, `inttoptr`, variable-size
  `alloca`, aggregate loads, globals whose initialiser holds pointers or
  `undef`, constant-expression `getelementptr` operands, a contract whose
  size is another parameter, catch through a base class,
  `llvm.stacksave`/`stackrestore`, the floating-point
  intrinsics, and `__prism.folded` / `__prism.poison`.
* **The C++ fixes not yet modelled in Lean**: `icmp samesign` (finding 2),
  `revive` (finding 7), `undef` outside `freeze`/`store` (finding 4),
  `llvm.assume` (finding 8), poison phi inputs (finding 1).
* **Certificates, not a translator theorem.** The extended theorems hold for
  every function whose certificate `validB` accepts, evaluated at each
  check; there is no theorem that `translateX` always produces a valid
  certificate.
* **The PIR semantics used here is the C++ CFG form.** It is linked to
  `PrismSem` through `evalBin`/`evalPred`/`ubBin` (`checks_eq_ubBin`), not
  through a proof that the CFG and `PrismSem.Stmt` programs are equivalent.
* **The C++ encoder.** Relating the Z3 formulas of `encode.cpp` to this PIR
  semantics is not done; `proofs/semantics` proves a model of the encoder
  ([PROOFS_SEMANTICS.md](PROOFS_SEMANTICS.md#the-gap-between-this-model-and-the-c-encoder)).
* **Not part of a normal run.** `pir_lean_check` runs from `prism-qa`,
  `prism_tests` and `proofs/refinement/check.sh`; a `prism PATH` run does
  not export or check its translations.
