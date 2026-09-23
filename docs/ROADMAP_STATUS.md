# Roadmap status

Status of every item in the PRISM roadmap (Odin Loch, 23 September 2026) as
of this commit. Each row is **DONE**, **PARTIAL** (what exists and what is
missing) or **NOT DONE** (why). Evidence points at files, tests or commands
in this repository; nothing here is claimed that the repository does not
contain. Proof claims are about Lean models unless a row says otherwise; see
[TRUSTED_BASE.md](TRUSTED_BASE.md) for what each verdict trusts.

Headline numbers (C++ engine, `python tools/conformance.py`):

| stage | wrong proofs | proved (true tasks) | refuted with replayed counterexample (false tasks) |
|---|---|---|---|
| bmc (hand encoder) | **0** | 94/172 | 93/167 |
| pir (Clang/LLVM front end) | **0** | 135/172 | 125/167 |
| conc (threads) | **0** (never proves) | BOUNDED 13/13 | 9/9 refuted |

Random programs (`tools/csmith_soundness.py`, in-house scalar and pointer
generators): 0 wrong proofs over every campaign run.

## Decisions (Part 0)

| # | Decision | Status |
|---|---|---|
| D1 | pinned external tools, link only permissive libs | **DONE** — `third_party/MANIFEST.toml`, `scripts/licence_check.py` in CI |
| D2 | Clang front end → PIR | **PARTIAL** — `src/prism/pir/` is a working Clang → LLVM IR → PIR → Z3 stage; Clang runs as a process, not linked as a library; `cparse.cpp` + `bmc.cpp` still run as the differential oracle |
| D3 | Z3 + Bitwuzla + CaDiCaL + Kissat; LRAT certified mode | **DONE** — `src/prism/solver/`; certified mode checks CaDiCaL LRAT with cake_lpr and Lean's verified checker |
| D4 | GPU for batched queries / SLS / preprocessing / fuzzing | **PARTIAL** — CPU ProbSAT walker in the portfolio; CUDA kernel `src/cuda/probsat.cu` type-checked only (no GPU in this environment) |
| D5 | LLM proposes, prover decides | **DONE** — `src/prism/ai/`, audit log, verdict audit |
| D6 | Lean 4 | **DONE** — `proofs/`, `proofs/semantics`, `proofs/techniques`, `proofs/refinement` (core Lean, no Mathlib) |
| D7 | Linux primary, WSL for Windows | **DONE** — MinGW rejected in CMake; Windows command-line quoting kept correct |
| D8 | Python engine frozen as oracle | **DONE** — new stages run in C++ only; the Python engine lists them and records one `NOTRUN` row; correctness fixes still land in both |
| PROVED-CERTIFIED | new verdict, never merged | **DONE** — both engines, Lean lattice (`proofs/Prism/Verdict.lean`), audit gate |

## Part 1 — Repository, supply chain, licences

| Item | Status |
|---|---|
| 1.1 manifest with SHAs, archive sha256, SPDX, kind | **DONE** — `third_party/MANIFEST.toml`; `scripts/fetch_deps.py` fails closed on hash mismatch (`tests/test_supply_chain.py`) |
| 1.1 delete the fifteen mined trees | **DONE** — tracked tree 981 MB → ~140 MB |
| 1.1 tool SHA in every finding | **DONE** — `extra.tool_sha` from every external-tool adapter, both engines |
| 1.1 history rewrite + force-push | **NOT DONE (owner action)** — `scripts/rewrite_history.sh` is ready and tested on a mirror; a force-push rewrites the default branch and needs the owner's explicit approval |
| 1.1 repository under 50 MB | **PARTIAL** — ~140 MB tracked; llama.cpp (88.6 MB) is vendored because it is linked |
| 1.2 licence firewall in CI | **DONE** — `scripts/licence_check.py`, `ci.yml` |
| 1.2 CodeQL adapter removed | **DONE** — both engines |
| 1.2 every external invocation through one argv path | **DONE** — POSIX `fork`/`execvp`; Windows `CreateProcessW` with a round-trip-tested quoter (no `_popen`) |
| 1.2 PRISM licence | **PARTIAL** — `LICENSE` is a clearly marked DRAFT source-available evaluation licence; needs an Australian IP lawyer |
| 1.3 CycloneDX SBOM in CI | **DONE** — `scripts/sbom.py`, validated against CycloneDX 1.5 |
| 1.3 signed releases (cosign) | **PARTIAL** — `.github/workflows/release.yml` (keyless cosign, double build + hash compare); runs on the first tag push |
| 1.3 reproducible build recipe | see "Docker" below |
| 1.4 remove `*_tmp.py` and `/mnt/c` workarounds | **DONE** |
| 1.4 split `stages_rest.cpp` per stage | **DONE** — `src/prism/stages/`: one file per stage (`taint`, `thread`, `contracts` (+ `prove_with_contract`), `wp`, `harness_bmc`, `concolic`, `fuse`, `diff`, `rapid` (+ `muttest`), `ltl`, `hypothesize`, `execute_cex`, `rlef`), plus `interp.cpp` (concrete interpreter, `concrete_execute`), `llm.cpp` (LLM engine, sandboxed run of LLM-written C), `platform.cpp` (`run_argv`, plain HTTP) and `common.cpp`; shared helpers are declared in `common.hpp` / `interp.hpp` / `llm.hpp` (namespace `prism::stages_detail`), single-use helpers stay file-local |
| 1.4 pure verdict module | **DONE** — `src/prism/verdict/`, `include/prism/verdict.hpp` (no I/O) |

## Part 2 — Clang/LLVM front end, C++23

| Item | Status |
|---|---|
| 2.2 Clang → LLVM IR → passes → PIR → solvers | **DONE (process-based)** — `src/prism/pir/` |
| 2.3 normalisation passes | **DONE** — `mem2reg, lowerswitch, loop-simplify, lcssa`; PRISM inserts its own checks (Law 8) |
| 2.4 PIR with property assertions | **DONE** — `include/prism/pir.hpp` |
| 2.4 per-run translation validation against `lli` | **DONE** — with `--allow-exec` (Law 9), sandboxed |
| 2.5 memory model | **DONE** — object id + offset, liveness, kinds, OOB/UAF/double free/mismatched delete/misalignment/null/uninit memory/const writes; QF_BV encoding by default; strict aliasing opt-in |
| 2.6 templates/overloads/lambdas/constexpr | **DONE** (resolved by Clang) |
| 2.6 standard library | **PARTIAL** — libc operational models (`src/prism/pir/models/libc/`), libstdc++ with `_GLIBCXX_ASSERTIONS`; not a verified libc++ model set |
| 2.6 floating point, exceptions, coroutines, setjmp, virtual dispatch | see "PIR round 3" at the end |
| 2.6 threads and atomics | **DONE (SC)** — `conc` stage, lazy sequentialisation; relaxed/acquire/release reported `NEEDS-HARNESS` |
| 2.6 modules, inline assembly | Clang handles modules; asm is `NEEDS-HARNESS` unless a contract is given |
| 2.7 conformance suite | **DONE** — `tests/conformance/` (in-house true/false pairs, SV-COMP subset, concurrency, memory, regressions), Juliet fetcher; ESBMC C++ regression tests **NOT DONE** |
| 2.7 metrics + release gate | **DONE** — `tools/conformance.py`, `.github/workflows/conformance.yml` |
| 2.8 old encoder as differential oracle | **DONE** — `tools/pir_vs_bmc.py`; the old encoder is **not** retired yet (pir does not yet cover a superset) |
| 2.8 lints on the Clang AST | **PARTIAL** — a Clang-AST layer inside the `lints` stage (C++ engine, `src/prism/astlint.cpp`; clang as a process, `-ast-dump=json`, `compile_commands.json` flags) checks eight classes precisely: assignment as condition, `sizeof(pointer param)` as a mem* length, signed/unsigned loop condition, enum switch hole, self-assignment, dead store, `memset(p, c, 0)`, integer division to floating (`tests/cpp/test_astlint.cpp`; zero AST rows on `testdata_fp/`). It supersedes a regex row only on the same line and class; the other ~590 regex lint classes are not ported, headers are not parsed on their own, and snippets that do not compile (about 520 of the 1,726 units in `testdata/`) are `NOTRUN` for the layer and keep regex lints only |

## Part 3 — Solvers, certificates, CUDA

| Item | Status |
|---|---|
| 3.1 portfolio + scheduler + query cache | **DONE** — Z3, Bitwuzla, CaDiCaL, Kissat, SLS; SAT answers re-validated in Z3; cache never lets a plain UNSAT answer a certified request |
| 3.1 exit criterion (beats Z3 alone) | **PARTIAL** — narrowly on pir VCs (211 s vs 229 s over 4,179). On the hard-query benchmark with Bitwuzla installed: 164.9 s vs 190.9 s, and the portfolio closes 3 of the 5 queries that Z3 alone times out on (`docs/SOLVERS.md`) |
| 3.1 learned scheduler | **PARTIAL** — GBDT predictor built and measured; off by default because it did not beat the rules |
| 3.2 certified mode | **DONE** — pir `--certified`: bit-blast (Lean-proved `toCNF` when the Lean exes are built, else Z3 tactics), CaDiCaL LRAT, cake_lpr **and** Lean's LRAT checker |
| 3.2 exit criterion (all loop-free suite tasks certified) | **PARTIAL** — `conformance.py --certified`: 109 of the 125 loop-free `true` functions pir proves are `PROVED-CERTIFIED`, all 109 through the Lean bit-blaster. Of the other 16, 11 have no VCs (stay `PROVED`) and 5 wide multiplications ran out of time in CaDiCaL LRAT (stay `PROVED`). 0 `false` functions certified; 0 wrong proofs |
| 3.3 GPU stages | **NOT DONE on GPU** — no GPU here; CPU reference walker only |
| 3.4 upstream forks | **NOT DONE** — owner activity |

## Part 4 / 9 — AI

The rule holds everywhere: model output is `HYPOTHESIS`/`READS` until a
checker accepts it, drafted assumptions never yield a proof (Law 6), and every
model call is in `<out>/ai_audit.jsonl`. **No model is present in this
environment**, so every model half is tested with fakes and reports `NOTRUN`
at run time; the numbers below are for the deterministic halves.

| Item | Status |
|---|---|
| 4.2 invariant synthesis + Houdini | **DONE** — templates alone move BOUNDED → PROVED-UNBOUNDED on the corpus; LLM half fake-tested |
| 4.2 harness drafting | **DONE** — drafts stay `NEEDS-HARNESS` with `draft_verdict` and the `// requires:` lines to confirm |
| 4.2 contract drafting, 9.3 contracts from requirements | **DONE** — traced to requirement sentences; only human-approved clauses prove |
| 4.2 counterexample explanation, verified repair | **DONE** — repair verdicts are about the patch (`HYPOTHESIS`, `patch_verdict`) |
| 4.1 GBNF grammars, prompt-injection fencing | **DONE** — `grammars/*.gbnf` |
| 4.3 LoRA fine-tuning | **NOT DONE** — needs the RTX 3090 and the model |
| 9.2 Lean proof search | **DONE** — `prism prove`; real Lean kernel + axiom audit + `lake build` gate |
| 9.2 proof repair | **DONE** — proof store, PROOF-REGRESSION |
| 9.3 assumption auditing | **DONE** — Z3 vacuity check always; model flags READS |
| 9.3 regression test generation | **DONE** — `prism regress` |
| 9.3 triage | **DONE** — ordering only |
| 9.3 solver/bound prediction | **PARTIAL** — built, off by default |
| 9.4 questions, report/assurance drafting | **DONE** — `prism ask`, `prism draft` |
| 9.4 GUI assistant | **PARTIAL** — source written for Qt and PySide6; Qt not buildable here |
| 9.7 per-feature metrics | **DONE** for deterministic halves (`docs/AI.md`); model metrics need a model |

## Part 5 / 8 — Proofs (Lean 4, no `sorry`, standard axioms only)

| Item | Status |
|---|---|
| 5.1 verdict lattice laws | **DONE** — `proofs/Prism/Verdict.lean`; C++ and Python checked equal to the Lean model over the whole finite domain (`tests/data/verdict_tables.json`) |
| 5.2 PIR semantics | **DONE** for the model — `proofs/semantics` |
| 5.3 encoder soundness | **DONE for the Lean model** (straight-line, branches, bounded loops, symbolic heap); the C++ encoder is not extracted from it |
| 5.4 bit-blaster correctness | **DONE** — `proofs/techniques` (`toCNF_equisat` incl. division, shifts, overflow predicates) and it is the code that runs in certified mode |
| 5.5 front-end validation | **DONE** — translation validation, differential testing, random programs |
| 8.2 k-induction, Houdini, contracts | **DONE** — `proofs/techniques` |
| 8.2 LLVM IR → PIR refinement | **PARTIAL** — proved for the integer fragment (`proofs/refinement`); per-run checker: 900/915 testdata functions identical to the proved translator, 0 mismatches; calls, memory, undef/freeze not in the fragment |
| 8.2 property instrumentation | **DONE for the fragment** — `checks_bad`, `instr_fail_ub_iff` |
| 8.2 floating point | **PARTIAL** — rounding and correctly-rounded addition proved; full IEEE 754 out of reach |
| 8.2 concurrency | **PARTIAL** — two straight-line threads, round-robin scheme |
| 8.2 libc models verified by PRISM | **PARTIAL** — `tests/conformance/libc-models/`: contract harnesses include the model sources and assert the C standard's contract; `pir`: 29/30 contracts PROVED for objects up to 4 bytes (size-bounded: every content/position/length within that size, not arbitrary lengths), `getenv` BOUNDED, 35/35 false twins refuted for the planted class; no harness yet for `fputc`/`putc`/`fputs`/`realloc(p, 0)`; printf family lives in the translator and is not checkable this way (docs/PIR.md "Library models verified by PRISM") |
| 8.3 trusted base document | **DONE** — `docs/TRUSTED_BASE.md`, shipped with every report |
| 8.4 PRISM on PRISM | **DONE** — `.github/workflows/self-check.yml` runs nightly. It builds PRISM with `-DPRISM_SANITIZE=ON` (ASan + UBSan) and runs both suites and the conformance gate under it. PRISM also scans its own tree. The first local sanitizer run found a signed overflow in `stages_rest.cpp` (now `src/prism/stages/`; fixed) and Z3-dependent tests that did not guard for a missing solver (fixed). All 158 no-Z3 cases pass under ASan/UBSan |
| 8.5 proofs rechecked independently in CI | **DONE** — `proofs-recheck.yml`: `leanchecker` + `nanoda` on every Lean project |

## Part 6 — Engineering and release

| Item | Status |
|---|---|
| 6.1 CI + release gate | **DONE** — `ci.yml`, `conformance.yml`, `proofs*.yml`, `docs.yml`; self-hosted GPU runner **NOT DONE** (hardware) |
| 6.2 five testing layers | **DONE** — doctest, conformance, differential (vs the frozen Python engine), random programs, self-fuzzing (`docs/FUZZ_SELF.md`; all findings fixed) |
| 6.3 SV-COMP readiness | **PARTIAL** — BenchExec tool-info, witness 2.0 writer, scored subset (`docs/SVCOMP.md`). `bmc` counterexamples carry nondet values, so its refutations replay and the witnesses get `function_return` waypoints. Local unvalidated score: no-overflow 35/70, unreach-call 9/36, 0 incorrect. `pir` does not report nondet values yet; no witness validator has been run; no entry |
| 6.4 documentation | **DONE** — `VERDICTS.md`, `TRUSTED_BASE.md`, `CONFORMANCE.md`, `USER_GUIDE.md`; every report finding links its verdict definition |
| 6.5 assurance packaging | **DONE as mappings** — `docs/assurance/` (DO-333, DO-330, Def Stan 00-055, ISM); no qualification is claimed |
| 6.6 SARIF | **DONE** |

## Part 7 / 10 — milestones

| Milestone | Status |
|---|---|
| M1 | **DONE** except the history rewrite (owner approval) and legal review |
| M2 | **DONE for C** (Clang front end + certified mode), with Clang as a process |
| M3 | **DONE** for the deterministic halves; model yield unmeasured (no model) |
| M4 | **PARTIAL** — C++23 coverage measured in `docs/PIR.md`; portfolio narrowly beats Z3; GPU stages not built; Juliet scored for bmc |
| M5 | **DONE for the Lean model** (loop-free code with memory); not extracted to C++ |
| M6 | **PARTIAL** — proof search and repair built and tested with the real kernel and a fake prover |
| M7 | **PARTIAL** — features shipped; model metrics need a model |
| M8 | **PARTIAL** — k-induction, Houdini, contracts, bit-blaster proved; memory model proved in the semantics project; float partial |
| M9 | **PARTIAL** — LLVM→PIR refinement for the integer fragment |
| M10 | **PARTIAL** — two-thread concurrency proof; recheck in CI done |

## Owner actions that no automation can take

1. Approve and run `scripts/rewrite_history.sh` (rewrites history, force-push).
2. Legal review of `LICENSE`, the licence table, and export-control questions.
3. Push a `v*` tag to exercise `release.yml` (signing needs the GitHub OIDC token).
4. Provide the RTX 3090 runner, the Qwen GGUF and a prover model to measure the AI features and the GPU stages.
