# Def Stan 00-055 mapping

UK Defence Standard 00-055 (Requirements for Safety of Programmable Elements
in Defence Systems; Part 1 requirements, Part 2 guidance) asks for an
evidence-based safety argument for each programmable element, with the rigour
of the evidence proportionate to the element's contribution to risk. It does
not prescribe a verification method, so PRISM's evidence would appear as
**items of evidence inside a supplier's safety argument**, not as a
conformance claim of its own.

This document says which parts of such an argument PRISM's evidence can
support. It does not reproduce the standard; clause numbers are deliberately
not given, because they must be taken from the issue a contract invokes (the
text is not in this repository). No independent safety auditor has reviewed
PRISM. Evidence identifiers are defined in [EVIDENCE.md](EVIDENCE.md).

| area of the standard | status | how PRISM's evidence contributes | not addressed |
|---|---|---|---|
| Safety management, safety plan, roles, competence | Not addressed | | Organisational; outside a tool. |
| Hazard identification and risk assessment for the element | Not addressed | | PRISM does not identify hazards or derive safety requirements. |
| Safety requirements for the programmable element and their traceability | Not addressed | Preconditions (`// requires:`) and ACSL-style contracts can express software safety requirements at function level and be checked (E06: `thm:Contracts.modular_sound`). | Deriving, validating and tracing those requirements. |
| Evidence that the implementation meets its safety requirements (verification) | Partial | Formal analysis of source code for the encoded properties, with `PROVED*` verdicts that are never merged with bounded results (`thm:proved_bounded_never_merge`) and refutations confirmed by counterexample replay (E13). Measured soundness on a suite with known answers (E09) and on random programs (E10). | Properties outside PRISM's encoded set (timing, stack, floating point, concurrency in the proving stages); object code (see `docs/assurance/DO-333.md`). |
| Evidence of absence of run-time errors / undefined behaviour | Partial | The `pir` and `bmc` stages target exactly the UB classes listed in `docs/PIR.md#properties-inserted-by-prism-law-8`; lints, cppcheck, clang-tidy and sanitizer runs add non-proof evidence, each labelled by strength (`PROVES`/`FINDS`/`SOME`/`READS`) and never promoted to a proof (`thm:admit_fuzzer_never_proof`, `thm:admit_model_never_proof`). | Memory safety through pointers in the `pir` stage (no memory model in C++ yet; `docs/PIR.md#what-is-not-encoded-named-never-dropped--roadmap-21`). |
| Diverse / independent evidence | Partial | Two engines (C++ and the frozen Python oracle) and two encoders (`bmc`, `pir`) are compared (E11); external provers (ESBMC, CBMC) run as separate adapters when installed. | The two engines share the encoder design, and shared design errors were found in both (E11); diversity is not independence of the development team. |
| Counter-evidence and its resolution | Partial | Every known wrong proof, false alarm and robustness defect is recorded with a reproducer (E23: `docs/CONFORMANCE.md#known-issues`, `docs/FUZZ_SELF.md`); the release gate fails on any wrong proof (E09). | Not a formal counter-evidence process. |
| Tools used to produce evidence: their integrity and justification | Partial | Trusted base statement (E07), proved verdict laws with code checked against the model (E02, E03), verdict audit (E04), design-level proofs of the verification algorithms (E05, E06), tool-qualification gap analysis in `docs/assurance/DO-330.md`. | No tool qualification or tool safety justification for a particular project; the C++ encoders are not proved. |
| Configuration management of the element and of the tools | Partial, for PRISM itself | Pinned and checked dependencies, licence firewall, SBOM (E19); reproducible, signed release workflow (E20). | Configuration management of the analysed software is the user's. `prism` has no `--version` output yet. |
| Security of the development environment / untrusted inputs | Partial | PRISM never executes analysed code without `--allow-exec`, and then in a bubblewrap jail with rlimits (E16). | Hardening of the host and CI environment. |
| Independent Safety Auditor | Not addressed | | No ISA has been engaged. |
| The safety case document | Not addressed | [ASSURANCE_CASE.md](ASSURANCE_CASE.md) is a skeleton argument for PRISM's own trustworthiness, which a supplier's safety case could reference. | A safety case for any fielded system. |
