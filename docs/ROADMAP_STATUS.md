# Roadmap status

Status of every item in the PRISM roadmap (Odin Loch, 23 September 2026) as
of this commit. Each row is **DONE**, **PARTIAL** (what exists and what is
missing) or **NOT DONE** (why). Evidence points at files, tests or commands
in this repository; nothing here is claimed that the repository does not
contain. Proof claims are about Lean models unless a row says otherwise; see
[TRUSTED_BASE.md](TRUSTED_BASE.md) for what each verdict trusts.

Headline numbers (C++ engine, strict `python tools/conformance.py` at `f3790a3fb`; 249 true / 249 false tasks including the ESBMC C++ subset, libc-model contracts and the PIR round 3 tasks):

| stage | wrong proofs | proved (true tasks) | refuted with replayed counterexample (false tasks) |
|---|---|---|---|
| bmc (hand encoder) | **0** | 97/249 | 93/249 |
| pir (Clang/LLVM front end) | **0** | 203/249 | 144/249 |
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
| 1.3 reproducible build recipe | **DONE** — `Dockerfile` built twice with `--no-cache` at `0423d6c54`: identical `SHA256SUMS` for `prism`, `libprism_native.so` and the SBOM, with `prism_tests` passing in the container (`docs/SUPPLY_CHAIN.md`). This was checked on one machine; a tagged release on the GitHub runners is still the first check on another |
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
| 2.6 standard library | **PARTIAL** — libc operational models (`src/prism/pir/models/libc/`, contract-checked, 8.2); C++ model headers (`src/prism/pir/models/cxx/`, put first on the include path by the pir stage): `std::vector` modelled soundly (libstdc++ 13's capacity policy, allocation through the new/delete models so iterator invalidation is MEM-UAF, same exceptions and `_GLIBCXX_ASSERTIONS` preconditions; differential trace identical to libstdc++ under ASan/UBSan; `vector<bool>`/other allocators fall back to libstdc++ and say so in `extra.cxx_models`); `std::array`/`span`/`optional`/`unique_ptr`/`string` stay libstdc++'s own code with `_GLIBCXX_ASSERTIONS` (small, loop-free: their own sound model; `string` cannot be swapped). Measured (`pir`, esbmc-cpp + in-house cxx): proved 36/57 → 40/57, refuted 39/57 → 42/57, 0 wrong proofs. Not done: `std::string` out-of-line members, iostreams, associative containers (NEEDS-HARNESS), `vector::insert` in the middle not provable within 900 s, libc++ (`_LIBCPP_HARDENING_MODE`) not modelled |
| 2.6 floating point, exceptions, coroutines, setjmp, virtual dispatch | **DONE** (`docs/PIR.md` coverage table). Details: <br>• **Floating point:** half/float/double use Z3 FP (round to nearest even). Float-to-int overflow is always checked; `--fp-checks` adds divide-by-zero, invalid and overflow. Exact libm functions are modelled. Still `UNENCODED`: long double, fp128 and fast-math. <br>• **Exceptions:** typeinfo-matched handlers; escaping `noexcept` or `main` is `CXX-UNCAUGHT`. <br>• **Coroutines:** lowered by LLVM's coro passes. <br>• **setjmp/longjmp:** handled as exception-like edges. <br>• **Virtual and indirect calls:** dispatch over the candidate functions, with a `NEEDS-HARNESS` fallback. <br>• **Inline asm:** `NEEDS-HARNESS` unless it has a `// prism: asm ensures` contract, which makes it `PROVED-ASSUMING`. <br>• **Tests:** 30 new true/false tasks, 15/15 proved and 15/15 refuted with replayed counterexamples; 0 wrong proofs in strict conformance and in random-program soundness runs. <br>• **k-induction with memory:** the step havocs the loop's write footprint (objects resolved by the memory model's provenance, else every object allocated before the loop; initialised flags `old or arbitrary`, or arbitrary after a `memcpy`/masked store). Loops that allocate or free stay `BOUNDED` (not attempted). Measured at `claude/kind-memory`: 2 previously `BOUNDED` true conformance tasks become `PROVED-UNBOUNDED` (`arr_loop_true`, `uninit_elem_unbounded_true`), 1 in `tests/pir` (`kind_mem_write_closed`); the 13 new adversarial pairs in `tests/conformance/prism/kindmem` give 8/13 true `PROVED-UNBOUNDED`, 0/13 false proved; strict conformance 0 wrong proofs; `csmith_soundness.py` inhouse-ptr ×80 and inhouse ×60: 0 wrong proofs (`docs/PIR.md` "k-induction with memory") |
| 2.6 threads and atomics | **DONE (SC)** — `conc` stage, lazy sequentialisation; relaxed/acquire/release reported `NEEDS-HARNESS` |
| 2.6 modules, inline assembly | Clang handles modules; asm is `NEEDS-HARNESS` unless a contract is given |
| 2.7 conformance suite | **DONE** — `tests/conformance/` (in-house true/false pairs, SV-COMP subset, concurrency, memory, regressions), Juliet fetcher; ESBMC C++ regression tests: pinned fetcher/converter (`--fetch-esbmc`, 1906 tasks) and a 64-task label-checked subset in git; full set 0 wrong proofs after fixing S8 (dynamic initialisation before `main`); false alarms F8–F10 fixed (re-run, 1906 tasks without the `--error-label` ones: bmc 0 false alarms, pir 581/1253 proved, 343/653 refuted, 8 false alarms left, all F11 from PIR round 3's typeinfo/exception lowering); F11 fixed (runtime typeinfo objects, catch binds the adjusted pointer / thrown pointer, Itanium handler counts for rethrow): pir-only re-run of 1917 tasks 0 false alarms, 594/1254 proved, 344/663 refuted, 0 wrong proofs apart from the `--error-label` task (docs/CONFORMANCE.md) |
| 2.7 metrics + release gate | **DONE** — `tools/conformance.py`, `.github/workflows/conformance.yml` |
| 2.8 old encoder as differential oracle | **DONE** — `tools/pir_vs_bmc.py`; the old encoder is **not** retired yet (pir does not yet cover a superset) |
| 2.8 lints on the Clang AST | **PARTIAL** — a Clang-AST layer inside the `lints` stage (C++ engine, `src/prism/astlint*.cpp`, `compile_commands.json` flags). **Speed:** libclang's C API is loaded at run time (CMake `PRISM_LIBCLANG`, default ON when `<clang-c/Index.h>` is found; `astlint_libclang.cpp` converts the cursors of the checked files to the `-ast-dump=json` schema), and the layer falls back to clang as a process with the JSON dump when libclang is absent. `lints` stage on `testdata/` at `--jobs 4`: 106 s → 9.3 s, 113 s → 12.7 s and 246 s → 17.0 s in three back-to-back runs of the previous binary and this one (shared 4-core machine at load average 11–19, so the absolute numbers are inflated; the earlier quiet-machine figure was about 33 s). **Coverage:** 54 AST classes, 46 of them ported regex classes (NULL deref in and after the NULL branch, `printf` count/type/LP64-width mismatch, fallthrough, `u < 0`, `sizeof` of an array param, `strncpy` without NUL, stack address and `c_str()` of a local returned, uninitialised read, shadowing, `[[nodiscard]]`, virtual call in ctor/dtor, missing virtual dtor, self-move, dangling ref, straight-line use-after-free / double free / mismatched free, use after move, and more), plus the regex "return is discarded" family (466 functions, 260 classes) as one table generated from the regex rules. That is 304 of the ~591 regex classes with an AST counterpart. An AST row supersedes the regex row on the same line and class (`extra.supersedes`; 777 of the 792 AST rows on `testdata/`). **Scope:** the unit, the scanned project headers it includes (reported once), enums and members nested in classes and namespaces, and non-dependent template code. Planted true positives and false-positive guards for every ported class are in `tests/cpp/test_astlint.cpp`, and every case runs on both front ends. There are zero AST rows on `testdata_fp/`. **Missing:** code that depends on a template parameter is not checked; flow checks are straight-line only (no path merging across branches or loops); ~287 regex classes stay regex-only; a header that no parsed unit includes, and snippets that do not compile (520 of the 1,726 units in `testdata/`), are `NOTRUN` for the layer and keep regex lints only |

## Part 3 — Solvers, certificates, CUDA

| Item | Status |
|---|---|
| 3.1 portfolio + scheduler + query cache | **DONE** — Z3, Bitwuzla, CaDiCaL, Kissat, SLS; SAT answers re-validated in Z3; cache never lets a plain UNSAT answer a certified request |
| 3.1 exit criterion (beats Z3 alone) | **PARTIAL** — narrowly on pir VCs (211 s vs 229 s over 4,179). On the hard-query benchmark with Bitwuzla installed: 164.9 s vs 190.9 s, and the portfolio closes 3 of the 5 queries that Z3 alone times out on (`docs/SOLVERS.md`). Both were measured while Bitwuzla still rejected most pir VCs (Z3-only SMT-LIB2 syntax, fixed since: it is now the fastest member on half of them); the 4,179-VC comparison has not been rerun with the fix and the learned scheduler |
| 3.1 learned scheduler | **DONE** — on by default (built-in GBDT, `PRISM_SOLVER_PREDICT=0` restores the rules; certified requests keep the rules). Trained on every member timed alone on 2,060 distinct pir VCs of `tests/pir`, `tests/conformance` (incl. esbmc-cpp, sv-comp) and `testdata`; held out by source file (599 VCs, 133 files). Replay at 2 cores: 13.0 s vs rules 17.3 s and history 15.9 s (noise 10–16%); real portfolio, two interleaved runs on 781 held-out VCs: 35.9 s vs 43.4 s rules / 41.7 s history (noise 0.5%), and 56.7 s vs 60.0 / 55.9 s in a run with one unexplained stall. 0 answer disagreements. The gain is Bitwuzla getting the head start (a static rule matches it); censored-loss GBDT, k-NN and a winner classifier measured, none better (`docs/SOLVERS.md` "Learned scheduler") |
| 3.2 certified mode | **DONE** — pir `--certified`: bit-blast (Lean-proved `toCNF` when the Lean exes are built, else Z3 tactics), CaDiCaL LRAT, cake_lpr **and** Lean's LRAT checker |
| 3.2 exit criterion (all loop-free suite tasks certified) | **PARTIAL** — `conformance.py --certified` (491-task suite, 2026-09-24): 136 of the 158 loop-free `true` functions pir encodes are `PROVED-CERTIFIED`, all through the Lean bit-blaster (was 109/125 on the 367-task suite). Of the other 22: 14 have no VCs, 4 are floating point (not certifiable), 1 is `PROVED-ASSUMING`, and `fn_macro_true` / `long_mul_true` (wide multiplications) are not certified in the budget. 0 `false` functions certified; 0 wrong proofs. One combined certificate per function (disjunction of all VCs; halves when the checker runs out; per-VC fallback): 114 functions certified that way; certified-run timeouts 18 -> 14 (17 of the old 18 now finish, e.g. `mem_uninit_true` 4 min -> 44 s; the rest are newer tasks: ESBMC C++, libc models, coroutines). Since then (claude/certified-3): certificates only for functions already `PROVED` (nested6 73 s -> 1.5 s, coroutine timeouts -> 52-68 s, measured per task), a per-function certification budget, memory caps for the checkers, packed proof store capped at 2 GB with LRU pruning; the full-suite rerun is not measured yet — docs/CONFORMANCE.md |
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
| 9.3 solver/bound prediction | **PARTIAL** — solver prediction on by default (3.1 learned scheduler). Unwind prediction measured and **off**: on 525 held-out functions the GBDT is 20% faster than a fixed unwind 8 but loses verdicts (agreement 98.3% vs 99.4%, 24 vs 21 BOUNDED); k-NN keeps them (20 BOUNDED) at +70% time; not wired into `pir`. No policy can change soundness (a smaller unwind only gives an honest BOUNDED; 0 PROVED-then-FAILED contradictions) |
| 9.4 questions, report/assurance drafting | **DONE** — `prism ask`, `prism draft` |
| 9.4 GUI assistant | **DONE** except model-backed answers — `prism_gui` builds against Qt 6.4.2. A headless smoke run (`--smoke-screenshot`, in CI on every push) runs the real pipeline through the window and checks the findings table fills. The PySide6 GUI tests (43) now run in CI too, offscreen. The assistant panel answers structured queries without a model; LLM-backed answers need a model |
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
| 8.2 LLVM IR → PIR refinement | **PARTIAL** — proved for the integer fragment (`proofs/refinement`), and, per function under a certificate the checker validates, for an extended fragment: `freeze`, `undef` under `freeze`/`store`, direct calls to same-file functions (inlined as `translate.cpp` does), stack memory (`alloca`, integer `load`/`store`, `getelementptr`, with the translator's null/wild/freed/bounds/alignment/read-only/uninitialised-read and pointer-arithmetic checks, on PRISM's object/offset memory model shared by both sides); per-run checker: `testdata` 915 `agree` + 21 `agree-ext` of 2 506 functions, `tests/pir` 46 + 8 of 226, 0 mismatches; library calls and intrinsics (`memcpy`/`memset`/`lifetime`), globals, heap, pointer parameters/phis, `undef` elsewhere are not in the fragment. Refinement finding 4 fixed in the C++ translator: `undef` (and values computed from it) is a fresh value at every use, `br`/`noundef` on it is UB-POISON (7 adversarial IR cases were wrong PROVEDs before; checker still 0 mismatches) |
| 8.2 property instrumentation | **DONE for the fragment** — `checks_bad`, `instr_fail_ub_iff` |
| 8.2 floating point | **PARTIAL** — Lean (`proofs/refinement`, `Float.lean`, `FloatOps.lean`): correctly rounded `+ − × ÷` for any binary format (round to nearest even) with ±0, ±∞, NaN and the IEEE invalid / divide-by-zero / overflow flags. PRISM's conditions, mirrored from the C++ and locked to its text (`tests/test_proofs_float_conc.py`): FLOAT-CAST-OVF **equals** C11's out-of-range condition (any format, any `iK`, unrepresentable bounds included); FLOAT-OVERFLOW **equals** IEEE overflow; FLOAT-INVALID **equals** IEEE invalid on quiet operands (signalling NaNs not reported); FLOAT-DIV-ZERO is IEEE divide-by-zero **plus** `0/0` and `±∞/0` (wider than IEEE, proved). Not proved: other rounding modes, `frem`/`sqrt`/`fma`/libm, Z3's FP theory itself (trusted). Found and fixed: the `fptrunc` overflow check never fired (`checks` got no operands); it now checks finite-operand/infinite-result directly |
| 8.2 concurrency | **PARTIAL** — `LazySeqN.lean`: the `conc` schedule (`K` rounds of `N` threads plus the harness's final slot, locked to `lazy.cpp`), threads with any control flow including bounded loops, SC. Proved: sound (every run is a real interleaving); covers every schedule of round-robin shape — every interleaving with ≤ `K−1` switches for any `N`, ≤ `2K` for two threads; and that a per-thread switch bound is **not** enough for `N ≥ 3` (counterexample proved). Not proved: the Z3 slot encoding, the race/deadlock monitors, unbounded rounds, relaxed memory |
| 8.2 libc models verified by PRISM | **PARTIAL** — `tests/conformance/libc-models/`: contract harnesses include the model sources and assert the C standard's contract; every function defined in the C models has a harness (checked by `test_every_model_function_is_called`), including `fputc`/`putc`/`fputs`/`fflush`, `realloc(NULL, n)`/`realloc(p, 0)`/shrink/grow and sized delete. `pir`: 40/41 C contracts PROVED, `getenv` BOUNDED, 58/58 false twins refuted for the planted class; C++: 12/12 contracts PROVED (vector model, array/span/optional/unique_ptr/string), 14/14 twins refuted. Size bounds: size-parametric C harnesses PROVED at N = 16 bytes (`tools/libc_model_bounds.py`; size-bounded, not claimed for all sizes); memcpy/memmove/memset/realloc proved for any size below 2^40 (`unbounded_contracts.c`); the string models' byte loops do not close by k-induction, so they stay size-bounded. Model fix found: output functions never returned EOF (now they can); printf family lives in the translator and is not checkable this way (docs/PIR.md "Library models verified by PRISM") |
| 8.3 trusted base document | **DONE** — `docs/TRUSTED_BASE.md`, shipped with every report |
| 8.4 PRISM on PRISM | **DONE** — `.github/workflows/self-check.yml` runs nightly. It builds PRISM with `-DPRISM_SANITIZE=ON` (ASan + UBSan) and runs both suites and the conformance gate under it. PRISM also scans its own tree. The first local sanitizer run found a signed overflow in `stages_rest.cpp` (now `src/prism/stages/`; fixed) and Z3-dependent tests that did not guard for a missing solver (fixed). All 158 no-Z3 cases pass under ASan/UBSan |
| 8.5 proofs rechecked independently in CI | **DONE** — `proofs-recheck.yml`: `leanchecker` + `nanoda` on every Lean project |

## Part 6 — Engineering and release

| Item | Status |
|---|---|
| 6.1 CI + release gate | **DONE** — `ci.yml`, `conformance.yml`, `proofs*.yml`, `docs.yml`; self-hosted GPU runner **NOT DONE** (hardware) |
| 6.2 five testing layers | **DONE** — doctest, conformance, differential (vs the frozen Python engine), random programs, self-fuzzing (`docs/FUZZ_SELF.md`; all findings fixed) |
| 6.3 SV-COMP readiness | **PARTIAL** — BenchExec tool-info, benchmark definition, witness 2.0 writer (violation and correctness), scored subset (`docs/SVCOMP.md`). `true` answers carry an `invariant_set` correctness witness: `bmc` Houdini loop invariants at the loop keyword when the proof has them, else the empty (trivially valid) set; never an invariant the engine did not prove. A signed `<<` overflow counts as no-overflow (SV-COMP rules, C11 6.5.7p4) and replays under `-fsanitize=shift-base`. `bmc` refutations report nondet call sites (tagged before inlining). Local score: no-overflow 41/70, unreach-call 13/36, 0 incorrect; the same answers in BenchExec with the SV-COMP CPU/core limits (memory 12 GB: container cgroup cap). All 38 witnesses (16 correctness, 22 violation) confirmed by CPAchecker 4.2.2; UAutomizer 0.3.1 confirms 30 (3 correctness timeouts, 5 plain `x = nondet();` function_return rejections). Subset correctness witnesses are all empty (no proof there exports an invariant); no entry |
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
| M8 | **PARTIAL** — k-induction, Houdini, contracts, bit-blaster proved; memory model proved in the semantics project; float: IEEE `+ − × ÷`, flags, FLOAT-CAST-OVF and the `--fp-checks` conditions proved against IEEE/C11 (Z3's FP theory trusted; other rounding modes, `sqrt`/`fma`/`frem` not). k-induction with a framed havoc (`KInduction.kinduction_frame_sound`, the pir step for memory-writing loops) is proved abstractly; the frame lemma for PIR memory (writes only inside the footprint, per-byte initialised flags) is not yet stated over `PrismSem/Memory.lean` |
| M9 | **PARTIAL** — LLVM→PIR refinement for the integer fragment; freeze/undef, direct calls and stack memory proved per function under a checked certificate (docs/PROOFS_REFINEMENT.md) |
| M10 | **PARTIAL** — `N`-thread, `K`-round lazy-sequentialisation schedule proved sound and covering under SC (slot encoding and relaxed memory not proved); recheck in CI done |

## Owner actions that no automation can take

1. Approve and run `scripts/rewrite_history.sh` (rewrites history, force-push).
2. Legal review of `LICENSE`, the licence table, and export-control questions.
3. Push a `v*` tag to exercise `release.yml` (signing needs the GitHub OIDC token).
4. Provide the RTX 3090 runner, the Qwen GGUF and a prover model to measure the AI features and the GPU stages.
