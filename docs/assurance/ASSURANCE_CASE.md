# PRISM assurance case (skeleton)

A Goal Structuring Notation (GSN) style argument, written as Markdown. It
argues about **PRISM itself**: when may a user rely on a PRISM verdict? The
leaves are evidence items from [EVIDENCE.md](EVIDENCE.md). Every leaf names
an artefact that exists (checked by `tools/assurance_check.py`); a leaf that
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
  - **C1** Scope: the C++ engine, the stages `bmc` and `pir` as proving
    stages, C and C++ source analysed by clang 18 on Linux x86-64
    (roadmap D7); properties as listed in
    `docs/PIR.md#properties-inserted-by-prism-law-8`.
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
        - **G2.2** The C++ and Python verdict code equals the model.
          - **J1** Finite domains: agreement on every input is equality
            (`docs/VERDICTS.md#connecting-the-proof-to-the-code`).
          - **Sn2** E03: `tests/data/verdict_tables.json`,
            `tests/test_verdict.py::TestLeanTables`,
            `tests/cpp/test_main.cpp::verdict module equals the Lean model`.
        - **G2.3** Every report is audited before it is written.
          - **Sn3** E04: `src/prism/pipeline.cpp`,
            `tests/test_verdict.py::TestAudit`.
    - **G3** (b) When a proving stage prints `PROVED`, `PROVED-UNBOUNDED`
      or `PROVED-ASSUMING`, the encoded properties hold.
      - **S3** Argue over the design (proved), the implementation
        (tested and measured) and the front end (validated).
        - **G3.1** The encoding design is sound.
          - **Sn4** E05: `thm:bmc_sound`, `thm:bmc_sound_unbounded`,
            `thm:instr_fail_ub_iff`, `thm:Mem.mbmc_sound`;
            E06: `thm:KInduction.kinduction_sound`,
            `thm:Houdini.houdini_sound`, `thm:Contracts.modular_sound`.
        - **G3.2** The C++ encoders implement the design.
          - **UNDEVELOPED** No proof connects `src/prism/pir/` or
            `src/prism/bmc.cpp` to the Lean model
            (`docs/PROOFS_SEMANTICS.md#the-gap-between-this-model-and-the-c-encoder`).
            Interim evidence only:
          - **Sn5** E09: conformance suite and release gate (wrong proofs
            must be 0), `tools/conformance.py`,
            `.github/workflows/conformance.yml`.
          - **Sn6** E10: random programs, every proof executed under UBSan,
            `tools/csmith_soundness.py`.
          - **Sn7** E11: engine and encoder differential testing,
            `tests/test_sarif.py::TestEngineParity`, `tools/pir_vs_bmc.py`.
        - **G3.3** The front end presents the program PRISM analyses
          faithfully.
          - **Sn8** E12: translation validation against `lli`,
            `docs/PIR.md#translation-validation-roadmap-24` (testing, not
            proof; clang itself stays trusted, A1).
        - **G3.4** The solver's `unsat` answers are correct.
          - **Sn9** E08: with `--certified` the `pir` stage sends every VC
            through the solver library and emits `PROVED-CERTIFIED` only
            when each has an LRAT proof accepted by cake_lpr (and by Lean's
            checker when the Lean-proved bit-blaster made the CNF):
            `src/prism/pir/encode.cpp`, `src/prism/solver/portfolio.cpp`,
            `src/prism/solver/leanbb.cpp`, `docs/SOLVERS.md#certified-mode`,
            `tests/cpp/test_certified.cpp::certified: loop-free safe function is PROVED-CERTIFIED with one certificate per VC`,
            `tests/cpp/test_leanbb.cpp::leanbb: small overflow VCs are PROVED-CERTIFIED through the Lean-proved bit-blaster`;
            bit-blaster proof `thm:Bitblast.certified_unsat` (E06).
          - **Partly undeveloped**: without `--certified`, and for
            `BOUNDED`, `PROVED-UNBOUNDED` and the `bmc` stage, a proof
            trusts Z3 (E08).
        - **G3.5** A refutation (`FAILED`) is real.
          - **Sn10** E13: counterexample replay under sanitizers in
            `tools/conformance.py` and `tools/svcomp/prism_svcomp.py`.
        - **G3.6** Pointer-parameter functions are never proved without a
          precondition (Law 6), and model-drafted preconditions never give a
          proof.
          - **Sn11** `thm:Contracts.proved_assuming_not_proved`,
            `tests/conformance/prism/ptrparam` tasks (`expect_status:
            NEEDS-HARNESS`), E18 (`docs/AI.md#harness-drafting-42-92`).
    - **G4** (c) Everything that was not checked is reported.
      - **Sn12** E17: `tests/test_optional_honesty.py`,
        `tests/test_sarif.py::TestSarifShape`, taxonomy `GAP` rows;
        Law 1 theorem `thm:notrun_never_becomes_clean`.
      - **Sn13** E21: every CLI option documented and every verdict anchor
        resolves, `tests/test_docs_cli.py`.
      - **Sn14** Known limitations are published with reproducers (E23).
    - **G5** (d) The delivered tool is the reviewed one and is safe to run
      on untrusted code.
      - **Sn15** E19: pinned, checked dependencies and SBOM,
        `third_party/MANIFEST.toml`, `tests/test_supply_chain.py::TestManifest`.
      - **Sn16** E20: reproducible, signed releases,
        `.github/workflows/release.yml`.
      - **Sn17** E16: no execution of analysed code without `--allow-exec`,
        `tests/test_exec_safety.py::TestHostileTreeWithoutAllowExec`.
      - **G5.1** PRISM's own parsers are robust to malformed input.
        - **UNDEVELOPED** Open findings from self-fuzzing (E15,
          `docs/FUZZ_SELF.md`): a crafted C file makes the C++ parser run
          in quadratic time, and malformed `report.json` / `stages.jsonl`
          raise instead of being rejected.

---

## Undeveloped goals, in the order they matter

1. **G3.2** implementation soundness of the C++ encoders (roadmap 5.3 and
   8.2); until then, G3 rests on measured soundness (Sn5 to Sn7).
2. **G3.4** certified proofs beyond `pir --certified`: the k-induction step
   (`PROVED-UNBOUNDED`) and the `bmc` stage still trust Z3.
3. **G5.1** the open self-fuzzing findings.
4. Independent review of this case.
