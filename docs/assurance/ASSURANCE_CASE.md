# PRISM assurance case (skeleton)

A Goal Structuring Notation (GSN) style argument, written as Markdown. It
argues about **PRISM itself**: when may a user rely on a PRISM verdict? The
leaves are evidence items from [EVIDENCE.md](EVIDENCE.md). Every leaf names
an artefact that exists (checked by `src/tools/qa/docscan.cpp`,
`prism_docs_check assurance-check`); a leaf that
has no evidence yet is marked **UNDEVELOPED** and says what is missing.

Notation: **G** goal, **C** context, **A** assumption, **J** justification,
**S** strategy, **Sn** solution (evidence). Indentation is the
"supported by" relation.

This is a skeleton, not an accepted case: no reviewer has assessed it, and
several goals are undeveloped.

---

- **G1** A PRISM report does not overstate what was verified: a proof verdict
  is only printed when the stated properties hold for the analysed code, and
  everything that was not checked is reported as such.
  - **C1** Scope: the C++ engine, the `pir` stage as the proving stage, C
    and C++ source analysed by the pinned clang (LLVM 23.1.4,
    `third_party/llvm/PIN.toml`) on Linux x86-64; properties as listed in
    `docs/PIR.md` ("Properties").
  - **C2** Verdict vocabulary and laws: `docs/VERDICTS.md` (E01).
  - **A1** The hardware, operating system, C++ compiler that builds PRISM,
    clang (front end), Z3 and the Lean kernel behave as specified
    (`docs/TRUSTED_BASE.md`, E07).
  - **S1** Argue separately over (a) the word printed, (b) the analysis
    behind a proof word, (c) the reporting of what was not checked, and
    (d) the integrity of the delivered tool.
    - **G2** (a) The verdict module never prints a stronger word than the
      evidence allows.
      - **S2** Prove the laws for a model, then show the code equals the
        model, then show every report passes through it.
        - **G2.1** The laws hold for the model.
          - **Sn1** E02: `thm:proved_bounded_never_merge`,
            `thm:notrun_never_clean`, `thm:no_path_fuzzer_or_model_to_proof`,
            `thm:certified_only_with_certificate`, `thm:audit_nonproving`,
            `thm:confidence_zero_of_visibility_zero`; audit
            `proofs/Prism/Axioms.lean`; CI `.github/workflows/proofs.yml`.
        - **G2.2** The C++ verdict code equals the model.
          - **J1** Finite domains: agreement on every input is equality
            (`docs/VERDICTS.md#connecting-the-proof-to-the-code`).
          - **Sn2** E03: `testdata/fixtures/verdict_tables.json`,
            `tests/cpp/test_main.cpp::verdict module equals the Lean model`.
        - **G2.3** Every report is audited before it is written.
          - **Sn3** E04: `src/prism/pipeline.cpp`,
            `tests/cpp/test_main.cpp::verdict audit demotes a non-proving stage`.
    - **G3** (b) When a proving stage prints `PROVED`, `PROVED-UNBOUNDED`
      or `PROVED-ASSUMING`, the encoded properties hold.
      - **S3** Argue over the design (proved), the translation (proved for
        a fragment, checked per function), the encoder implementation
        (tested and measured) and the front end (validated).
        - **G3.1** The encoding design is sound.
          - **Sn4** E05: `thm:bmc_sound`, `thm:bmc_sound_unbounded`,
            `thm:instr_fail_ub_iff`, `thm:Mem.mbmc_sound`;
            E06: `thm:KInduction.kinduction_sound`,
            `thm:Houdini.houdini_sound`, `thm:LoopCut.loopcut_sound`,
            `thm:Contracts.modular_sound`.
        - **G3.2** The LLVM→PIR translation keeps every UB.
          - **Sn5** E24: `thm:pir_sound`, `thm:pir_sound_x` for the
            fragment; `src/tools/qa/pir_lean_check.cpp` checks per function
            that the C++ translator's output is the proved translator's.
          - **UNDEVELOPED** for functions outside the fragment, and in a
            normal run, which does not run the check
            (`docs/PROOFS_REFINEMENT.md#what-remains`).
        - **G3.3** The C++ encoder implements the design.
          - **UNDEVELOPED** No proof connects `src/prism/pir/encode.cpp` to
            the Lean model
            (`docs/PROOFS_SEMANTICS.md#the-gap-between-this-model-and-the-c-encoder`).
            Interim evidence only:
          - **Sn6** E09: conformance suite and release gate (wrong proofs
            must be 0), `src/tools/qa/conformance.cpp`,
            `.github/workflows/conformance.yml`.
          - **Sn7** E10: random programs, every proof executed under UBSan,
            `src/tools/qa/soundness.cpp`.
          - **Sn8** E11: the formal LLVM semantics tested against `lli`,
            `src/tools/qa/llvm_sem_vs_lli.cpp` (`prism-qa llvm-sem-vs-lli`).
            The second encoder (`bmc`) that `pir` was once compared with
            was deleted on 2026-10-31 (commit c66284cde).
        - **G3.4** The front end presents the program PRISM analyses
          faithfully.
          - **Sn9** E12: translation validation against `lli` with
            `--allow-exec`, `src/prism/pir/stage.cpp` (testing, not proof;
            clang itself stays trusted, A1).
        - **G3.5** The solver's `unsat` answers are correct.
          - **Sn10** E08: with `--certified` the `pir` stage sends every VC
            through the solver library and emits `PROVED-CERTIFIED` only
            when each has an LRAT proof accepted by cake_lpr (and by Lean's
            checker when the Lean-proved bit-blaster made the CNF):
            `src/prism/pir/encode.cpp`, `src/prism/solver/portfolio.cpp`,
            `src/prism/solver/leanbb.cpp`, `docs/SOLVERS.md`,
            `tests/cpp/test_certified.cpp::certified: loop-free safe function is PROVED-CERTIFIED with one certificate per VC`,
            `tests/cpp/test_leanbb.cpp::leanbb: small overflow VCs are PROVED-CERTIFIED through the Lean-proved bit-blaster`;
            bit-blaster proof `thm:Bitblast.certified_unsat` (E06).
          - **Partly undeveloped**: without `--certified`, and for
            `BOUNDED` and `PROVED-UNBOUNDED`, a proof trusts Z3 (E08).
        - **G3.6** A refutation (`FAILED`) is real.
          - **Sn11** E13: counterexample replay under sanitizers in
            `src/tools/qa/support/replay.cpp` and `src/prism/svcomp/replay.cpp`.
        - **G3.7** Pointer-parameter functions are never proved without a
          precondition (Law 6), and model-drafted preconditions never give a
          proof.
          - **Sn12** `thm:Contracts.proved_assuming_not_proved`,
            `testdata/conformance/prism/ptrparam` tasks (`expect_status:
            NEEDS-HARNESS`), E18 (`docs/AI.md`).
    - **G4** (c) Everything that was not checked is reported.
      - **Sn13** E17: `tests/cpp/test_adapters.cpp` (optional adapter honesty),
        `tests/cpp/test_sarif.cpp`, taxonomy `GAP` rows;
        Law 1 theorem `thm:notrun_never_becomes_clean`.
      - **Sn14** E21: every CLI option documented and every verdict anchor
        resolves, `tests/cpp/test_qa.cpp::docs: every --help flag of prism and prism prove is in docs/USER_GUIDE.md`,
        `tests/cpp/test_qa.cpp::docs: every verdict has the VERDICTS.md anchor reports link to`.
      - **Sn15** Known limitations are published with reproducers (E23).
    - **G5** (d) The delivered tool is the reviewed one and is safe to run
      on untrusted code.
      - **Sn16** E19: pinned, checked dependencies and SBOM,
        `third_party/MANIFEST.toml`, `tests/cpp/test_deps.cpp::every manifest component is well formed`.
      - **Sn17** E20: reproducible, signed releases,
        `.github/workflows/release.yml`.
      - **Sn18** E16: no execution of analysed code without `--allow-exec`,
        and then only in the bubblewrap jail unless the user accepts
        resource limits only,
        `tests/cpp/test_main.cpp::pipeline: hostile tree runs nothing without --allow-exec`.
      - **G5.1** PRISM's own parsers are robust to malformed input.
        - **Sn19** E15: the self-fuzzing findings F1–F4 are fixed
          (`docs/FUZZ_SELF.md`).
        - **UNDEVELOPED** The self-fuzzing harness is not run in CI, and
          `docs/FUZZ_SELF.md` lists what it does not cover.

---

## Undeveloped goals, in the order they matter

1. **G3.3** implementation soundness of the C++ encoder; until then, G3
   rests on measured soundness (Sn6 to Sn8).
2. **G3.2** the LLVM→PIR translation outside the proved fragment, and a
   per-run check in normal runs.
3. **G3.5** certified proofs beyond `pir --certified`: the k-induction step
   (`PROVED-UNBOUNDED`) and `BOUNDED` still trust Z3.
4. **G5.1** self-fuzzing in CI.
5. Independent review of this case.
