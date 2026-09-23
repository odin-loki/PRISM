# Evidence catalogue

Every artefact the assurance documents cite, with what it shows and, as
importantly, what it does not. Identifiers (E01...) are used by
[ASSURANCE_CASE.md](ASSURANCE_CASE.md) and the framework mappings.
`tools/assurance_check.py` verifies every path, test name, anchor and
theorem name below exists. "CI" means a workflow defined in this
repository; this package does not include CI run results.

## Formal proofs (Lean 4, core library only, no Mathlib)

The three Lean projects were built and audited locally on 2026-09-23 on the
branch that added this package, with Lean `v4.34.0` (the version pinned in
each project's `lean-toolchain`): `proofs/check.sh` passed all four steps
(no `sorry`, clean `lake build`, axioms within `propext`,
`Classical.choice`, `Quot.sound`, truth tables equal to
`tests/data/verdict_tables.json`), `proofs/semantics/check.sh` passed, and
`lake build` plus `lake env lean PrismTechniques/Audit.lean` in
`proofs/techniques/` passed. CI re-runs the same checks (workflows below).

### E01 The verdict vocabulary and laws (specification)

- Artefacts: `docs/VERDICTS.md`, `prism/laws.py`, `include/prism/laws.hpp`,
  `include/prism/verdict.hpp`, `src/prism/verdict/verdict.cpp`, the laws
  in `CLAUDE.md` and `docs/PLAN.md#laws-inherited-not-optional`.
- Shows: the closed status vocabulary (18 verdicts), which are proofs,
  which are answers, which stages may prove, and the nine laws.
- Does not show: that any stage computes its verdict correctly.

### E02 The verdict laws are proved for the Lean model

- Artefacts: `proofs/Prism/Verdict.lean`, axiom audit
  `proofs/Prism/Axioms.lean`, build and audit script `proofs/check.sh`,
  CI `.github/workflows/proofs.yml`.
- Theorems (namespace `Prism`):
  - Law 2 (never merge `PROVED` and `BOUNDED`): `thm:proved_bounded_never_merge`,
    `thm:formal_never_merge`, `thm:certified_never_merges_weaker`.
  - Law 3 (fuzzer `CLEAN` is not a proof): `thm:clean_never_promoted`,
    `thm:clean_never_rewritten_to_proof`, `thm:admit_fuzzer_never_proof`.
  - Law 4 (model output is not evidence): `thm:model_never_promoted`,
    `thm:admit_model_never_proof`, `thm:no_path_fuzzer_or_model_to_proof`.
  - Law 1 (`NOTRUN` is never clean): `thm:notrun_never_merges_clean`,
    `thm:notrun_never_becomes_clean`, `thm:notrun_never_clean`.
  - Certificates: `thm:admit_certified_iff`, `thm:certified_only_with_certificate`,
    `thm:audit_certified`.
  - Stage audit: `thm:proving_stages`, `thm:audit_nonproving`,
    `thm:audit_flags_nonproving`, `thm:audit_idem`.
  - Law 5 (confidence): `thm:confidence_zero_of_visibility_zero`,
    `thm:score_no_data`, `thm:confidence_le_one`, `thm:confidence_le_visibility`.
- `proofs/check.sh` fails on `sorry` or `native_decide` in the sources, on a
  build warning, on any audited theorem using an axiom other than
  `propext`, `Classical.choice`, `Quot.sound`, and when the exported truth
  tables differ from the committed ones.
- Does not show: anything about the C++ or Python code by itself (see E03).

### E03 The shipped verdict code equals the Lean model

- Artefacts: `tests/data/verdict_tables.json` (the model evaluated over its
  whole finite domain by `proofs/Prism/Export.lean`),
  `tests/test_verdict.py::TestLeanTables`,
  `tests/cpp/test_main.cpp::verdict module equals the Lean model`,
  explanation in `docs/VERDICTS.md#connecting-the-proof-to-the-code`.
- Shows: `src/prism/verdict/verdict.cpp`, the `laws.hpp` string API and
  `prism/laws.py` agree with the model on every merge pair, rewrite pair,
  admission and audit input (finite domains, so agreement on every input is
  equality of the functions).
- Does not show: confidence equality beyond the sampled grid (the laws
  themselves are proved for all inputs); that the tests ran on a particular
  release binary.

### E04 The verdict audit runs on every report

- Artefacts: the audit call in `src/prism/pipeline.cpp`,
  `docs/VERDICTS.md#the-verdict-audit`, `tests/test_verdict.py::TestAudit`.
- Shows: a formal verdict from a stage that may not prove is demoted to
  `UNKNOWN` with a recorded violation before the report is written.
- Does not show: that a proving stage's proof is correct.

### E05 PIR semantics and encoder soundness (model of the design)

- Artefacts: `proofs/semantics/PrismSem/Semantics.lean`,
  `proofs/semantics/PrismSem/Instrument.lean`,
  `proofs/semantics/PrismSem/Encode.lean`,
  `proofs/semantics/PrismSem/Memory.lean`,
  `proofs/semantics/PrismSem/MemEncode.lean`, audit
  `proofs/semantics/Audit.lean` and `proofs/semantics/check.sh`, CI
  `.github/workflows/proofs-semantics.yml`, write-up
  `docs/PROOFS_SEMANTICS.md`.
- Theorems: instrumentation is exact (`thm:instr_fail_ub_iff`,
  `thm:bigStep_instr_fail_ub_iff`); the bounded encoder is sound and
  complete (`thm:bmc_sound`, `thm:bmc_complete`, `thm:bmc_sound_unbounded`,
  `thm:instr_encode_sound`, `thm:loopFree_encode_exact`); the same for the
  memory model (`thm:Mem.mbmc_sound`, `thm:Mem.mbmc_complete`,
  `thm:Mem.minstr_fail_ub_iff`, `thm:Mem.oob_is_ub`, `thm:Mem.uaf_is_ub`).
- Does not show: that `src/prism/pir/` or `src/prism/bmc.cpp` implement
  this design correctly. The C++ encoder is not proved; the remaining steps
  are listed in
  `docs/PROOFS_SEMANTICS.md#the-gap-between-this-model-and-the-c-encoder`.

### E06 Verification techniques (models of the algorithms)

- Artefacts: `proofs/techniques/PrismTechniques/KInduction.lean`,
  `proofs/techniques/PrismTechniques/Houdini.lean`,
  `proofs/techniques/PrismTechniques/Contracts.lean`,
  `proofs/techniques/PrismTechniques/Bitblast.lean`,
  `proofs/techniques/PrismTechniques/BitblastEncode.lean`,
  `proofs/techniques/PrismTechniques/LazySeq.lean`,
  `proofs/techniques/PrismTechniques/FloatRound.lean`, audit
  `proofs/techniques/PrismTechniques/Audit.lean`, CI
  `.github/workflows/proofs-techniques.yml`, write-up
  `docs/PROOFS_TECHNIQUES.md`.
- Theorems: `thm:KInduction.kinduction_sound`,
  `thm:KInduction.kinduction_strengthened`, `thm:Houdini.houdini_sound`,
  `thm:Houdini.houdini_then_kinduction`, `thm:Contracts.modular_sound`,
  `thm:Contracts.proved_assuming_not_proved`,
  `thm:Contracts.harness_assumption_discharged`,
  `thm:Bitblast.toCNF_equisat`, `thm:Bitblast.certified_unsat`,
  `thm:LazySeq.lazy_seq_sound`, `thm:FloatRound.rne_nearest`.
- Does not show: that PRISM's certified path uses this bit-blaster (it
  uses Z3's tactics, see E08), or that the C++ k-induction/Houdini code
  matches the model; see
  `docs/PROOFS_TECHNIQUES.md#gap-to-the-c-implementation-what-these-proofs-do-and-do-not-cover`.

## Trusted base and certificates

### E07 The trusted base statement

- Artefact: `docs/TRUSTED_BASE.md`.
- Shows: what a proof verdict depends on, component by component, with
  the mitigation in place today.
- Known inconsistency: its section 3 table still says "No Lean proofs exist
  in the repository yet" for the Lean kernel row. That sentence predates
  E02, E05 and E06 and is out of date.
- Does not show: that any trusted component is correct.

### E08 Certified mode in the solver library

- Artefacts: `src/prism/solver/portfolio.cpp`, `src/prism/solver/query.cpp`,
  `docs/SOLVERS.md#certified-mode`, `docs/TRUSTED_BASE.md`,
  `tests/cpp/test_main.cpp::solver: certified unsat end to end (CaDiCaL LRAT checked by cake_lpr)`,
  `tests/cpp/test_main.cpp::solver: a cached plain unsat never satisfies a certified request`.
- Shows: the library can bit-blast a quantifier-free bitvector query to
  CNF, have CaDiCaL write an LRAT proof and accept `certified` only when
  cake_lpr (verified in CakeML) prints `s VERIFIED UNSAT` for the exact
  CNF (hash-checked).
- Does not show: any `PROVED-CERTIFIED` in a PRISM report. **No pipeline
  stage calls the solver library today** (the stages in `src/prism/pir/`
  and `src/prism/bmc.cpp` use Z3 directly), so the current pipeline never
  emits `PROVED-CERTIFIED`. The Z3 bit-blasting tactics are trusted (T3 in
  `docs/TRUSTED_BASE.md`).

## Testing and measurement

### E09 Conformance suite and release gate

- Artefacts: `tools/conformance.py`, `tests/conformance/SOURCES.md`, the
  tasks under `tests/conformance/prism/` and `tests/conformance/sv-comp/`,
  scorer tests `tests/conformance/test_conformance_suite.py`, CI
  `.github/workflows/conformance.yml`, metrics and history in
  `docs/CONFORMANCE.md`.
- Shows: soundness (wrong proofs), completeness, detection with replayed
  counterexamples and false alarms, measured per stage on tasks with known
  answers; the workflow fails on any wrong proof.
- Does not show: soundness outside the suite. `docs/CONFORMANCE.md#release-gate`
  records the gate as red on the C++ engine because of one `harness` row at
  the time it was written; commit `7ff59c5` ("harness: drafted assumptions
  never yield a proof class") addresses that row. Re-measured locally on
  2026-09-23 with the C++ engine built from this branch (engine sources as
  at `9135d97`; `PRISM_BIN=... python tools/conformance.py`, stages
  `inventory,classify,bmc,pir,harness`, 297 tasks): **release gate PASS,
  0 wrong proofs** in every stage; `bmc` completeness 82/148, detection with
  replayed counterexample 84/143, 1 false alarm; `pir` completeness 90/148,
  detection 90/143, 0 false alarms; Law 6 6/6. Juliet and the random
  programs were not re-run for this package.

### E10 Random-program soundness testing

- Artefacts: `tools/csmith_soundness.py`, the `random-programs` job in
  `.github/workflows/conformance.yml`,
  `docs/CONFORMANCE.md#random-programs-toolscsmith_soundnesspy-in-house-generator-300-programs-900-functions`.
- Shows: every `PROVED*` verdict on generated programs is executed under
  UBSan on inputs; any sanitizer report is a wrong proof.
- Does not show: absence of wrong proofs for program shapes the generators
  do not produce.

### E11 Differential testing between engines and encoders

- Artefacts: `tests/test_sarif.py::TestEngineParity`, `tools/pir_vs_bmc.py`,
  `docs/PIR.md#differential-oracle-vs-the-old-encoder-roadmap-28`,
  `tests/test_stage_order.py`.
- Shows: the C++ engine and the frozen Python engine (roadmap D8) agree on
  report shape and verdicts on shared inputs; the `pir` stage is compared
  with the older `bmc` encoder and every hard conflict was investigated.
- Does not show: correctness where both engines share a design error (the
  pre-fix conformance run found 11 such wrong proofs in both engines; see
  `docs/CONFORMANCE.md#before-the-fixes-2026-09-23-24a72d40b`).

### E12 Translation validation of the `pir` front end

- Artefacts: `src/prism/pir/stage.cpp`,
  `docs/PIR.md#translation-validation-roadmap-24`.
- Shows: for each proved or refuted function, PRISM runs the LLVM IR under
  `lli` on sample inputs and requires the PIR semantics to agree; a
  divergence turns the verdict into `ERROR`.
- Does not show: equivalence for all inputs (it is testing, not proof), or
  anything about the object code a production compiler emits.

### E13 Counterexample replay

- Artefacts: `replay` in `tools/conformance.py`, `replay` in
  `tools/svcomp/prism_svcomp.py`, `tests/test_svcomp.py::ReplayTest`.
- Shows: a `FAILED` verdict is counted as a detection only when the
  counterexample, compiled with sanitizers and executed, triggers the
  violation.
- Does not show: replay of whole programs with `__VERIFIER_nondet_*`
  inputs; the engines do not report nondet values yet (`docs/SVCOMP.md`).

### E14 Unit and integration tests

- Artefacts: `tests/cpp/test_main.cpp`, `tests/cpp/test_ai9.cpp`, the
  Python suite under `tests/`, CI `.github/workflows/ci.yml` (jobs `cpp`
  and `python`).
- Shows: the behaviour each test asserts, on every push.
- Does not show: structural coverage of PRISM's own code (none is measured).

### E15 Fuzzing PRISM's own input handling

- Artefacts: `tools/fuzz_self/fuzz_py.py`, `tools/fuzz_self/make_corpus.py`,
  `tools/fuzz_self/run_cpp.sh`, `tests/fuzz/fuzz_cparse.cpp` (CMake option
  `PRISM_FUZZ`), findings in `docs/FUZZ_SELF.md`.
- Shows: short campaigns against the C parser, the IR parser and the
  report/journal/manifest readers, with the findings (open, not fixed)
  recorded with reproducers.
- Does not show: robustness beyond the campaigns run; it is not run in CI.

## Safety of operation and honesty of reports

### E16 Executing untrusted code only on opt-in (Law 9)

- Artefacts: `docs/PLAN.md#running-on-untrusted-code-law-9`,
  `src/prism/sandbox.cpp`, `prism/sandbox.py`,
  `tests/test_exec_safety.py::TestHostileTreeWithoutAllowExec`,
  `tests/test_exec_safety.py::TestPolicyAndSandbox`.
- Shows: without `--allow-exec` no step runs code from the scanned tree; with
  it, binaries run under bubblewrap (when installed) and rlimits.
- Does not show: that the sandbox cannot be escaped.

### E17 What was not checked is reported (Laws 1 and 7)

- Artefacts: `tests/test_optional_honesty.py`, `tests/test_prism_optional.py`,
  taxonomy coverage `prism/taxonomy.py` / `src/prism/taxonomy.cpp` with
  `tests/test_taxonomy.py`, SARIF notifications in `src/prism/sarif.cpp`
  and `tests/test_sarif.py::TestSarifShape`.
- Shows: a missing tool or a stage that cannot run is a `NOTRUN` row, a
  defect class no stage covered is `GAP`, and both reach SARIF as tool
  execution notifications.
- Does not show: that every construct a stage silently misreads is caught
  (the known cases are listed in `docs/CONFORMANCE.md#known-issues`).

### E18 AI output never decides a verdict (Law 4)

- Artefacts: `docs/AI.md#the-rule`, `docs/AI.md#prompt-injection-safety-96`,
  `docs/AI.md#audit-log-96`, `src/prism/ai/core.cpp`, `tests/test_ai.py`,
  theorems `thm:admit_model_never_proof` and
  `thm:no_path_fuzzer_or_model_to_proof` (E02).
- Shows: model output is `HYPOTHESIS`/`READS` unless a checker re-proves
  it; model-drafted harnesses give `NEEDS-HARNESS`, not a proof.
- Does not show: the quality of any model.

## Configuration, supply chain and releases

### E19 Pinned dependencies, licence firewall, SBOM

- Artefacts: `third_party/MANIFEST.toml`, `scripts/fetch_deps.py`,
  `scripts/licence_check.py`, `scripts/sbom.py`, `docs/SUPPLY_CHAIN.md`,
  `tests/test_supply_chain.py::TestManifest`,
  `tests/test_supply_chain.py::TestLicenceFirewall`,
  `tests/test_supply_chain.py::TestSbom`, the `supply-chain` job in
  `.github/workflows/ci.yml`.
- Shows: every linked library is pinned by commit and tree digest and
  checked, linked components are permissively licensed, and a CycloneDX
  SBOM is generated.
- Does not show: that the pinned upstream code is free of defects.

### E20 Reproducible, signed releases

- Artefacts: `.github/workflows/release.yml`,
  `docs/SUPPLY_CHAIN.md#reproducible-signed-releases`.
- Shows: the release workflow builds twice from scratch, compares the
  outputs and signs every artefact with Sigstore.
- Does not show: that a release has been published (none is referenced
  here).

## Documentation and interfaces

### E21 User documentation and report formats

- Artefacts: `docs/USER_GUIDE.md`, `tests/test_docs_cli.py` (every CLI flag
  of both engines documented, every verdict anchor resolves),
  `src/prism/sarif.cpp`, `prism/sarif.py`, `docs/USER_GUIDE.md#8-ci-integration`.
- Does not show: that users read it.

### E22 SV-COMP readiness

- Artefacts: `tools/svcomp/prism.py` (BenchExec tool-info module),
  `tools/svcomp/prism_svcomp.py`, `tools/svcomp/witness.py`,
  `tools/svcomp/run_subset.py`, `tests/test_svcomp.py`, results in
  `docs/SVCOMP.md`.
- Shows: a local, unvalidated score on the 45-task pinned no-overflow
  subset.
- Does not show: an SV-COMP result. PRISM has not entered SV-COMP and its
  witnesses have not been validated.

### E23 Known issues

- Artefact: `docs/CONFORMANCE.md#known-issues`, `docs/FUZZ_SELF.md`.
- Shows: every known wrong proof, false alarm and robustness defect found
  so far, with reproducers.
