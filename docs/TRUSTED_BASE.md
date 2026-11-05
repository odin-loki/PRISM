# PRISM trusted base

This file says what a PRISM proof verdict depends on. If a component listed
here is wrong, PRISM can print a proof that is false. Everything not listed
is quarantined: it can be buggy, but it cannot on its own make PRISM claim a
proof. **Every PRISM run writes a copy of this file next to `report.md`**,
`report.md` links to it, and SARIF `runs[0].properties.trustedBase` names it
with its SHA-256.

It describes the code as it is. Certified mode uses the Lean-proved
bit-blaster and Lean's LRAT checker when it can (section 1.1); Z3's tactics
are the fallback (section 1.2), and every certificate says which one made
its CNF.

## 1. What `PROVED-CERTIFIED` depends on

`PROVED-CERTIFIED` is emitted by one stage, `pir`, and only with
`--certified` (`Config.certified`). The pir stage turns each function into
verification conditions (VCs): one per inserted property (signed overflow,
division, shifts, uninitialised reads, clz/ctz of zero, `assert`, poison
flags, and the memory-safety checks of the memory model: null and wild
dereference, use after free, out of bounds, misalignment, invalid, double and
mismatched free, overlap, pointer arithmetic, ... — `docs/PIR.md`, "Memory
model"), plus the unwinding assertion when the function has a loop. Memory
is encoded by default as Ackermannised read-over-write chains
(`MemEncoding::Bv`), so every VC, memory VCs included, is quantifier-free
bitvector logic (QF_BV) and can be certified; the unbounded array encoding
(`MemEncoding::Array`) is not certifiable and is not the default. Every VC
goes through the solver library (`prism::solver::solve`: the portfolio and
the query cache). A function becomes `PROVED-CERTIFIED` only when:

- it would be plain `PROVED` (no property VC satisfiable, and every loop
  closes within `--unwind`, shown by the unwinding assertion), **and**
- it has at least one VC, **and**
- **every** one of its VCs, each property and the unwinding assertion, came
  back `certified` by the chain below, either
  - **all at once** (`extra.certificate_scope = "combined"`, tried first when
    a function has two or more VCs): one query, the disjunction of every VC
    under the shared path encoding, `base ∧ (viol_1 ∨ … ∨ viol_n ∨ cut)`,
    came back UNSAT with one checked certificate. A disjunction is
    unsatisfiable exactly when each disjunct is, so this is the same claim
    as n per-VC certificates, made with one CNF and one LRAT proof; or
  - **in batches** (`extra.certificate_scope = "batched"`): the combined
    query was UNSAT but a checker ran out of time on its proof, so it was
    split in halves (recursively) and each batch's disjunction came back
    UNSAT with its own checked certificate. The batches partition the VCs
    (`extra.certificate_covers` lists each batch), so again every VC is
    covered by exactly one checked proof; or
  - **one by one** (`extra.certificate_scope = "per-vc"`): each VC came back
    certified on its own. This runs whenever the combined certificate is not
    obtained; the combined attempt is then discarded (its outcome is
    recorded in `extra.certificate_combined`) and nothing from it is used.

  Certificates are asked only for a function whose plain answers already
  made it `PROVED` (certification can only add to that claim); the
  certificate queries of one function share a budget, and when it is spent
  the function stays `PROVED` with a `certify_note`.

The finding then carries `extra.certificate = "checked"` (the verdict audit
admits `PROVED-CERTIFIED` from the `pir` stage only with it),
`extra.certificate_info` (per VC, or for the one combined proof: bit-blaster,
solver build, LRAT steps, checker builds, CNF hash), `extra.certificate_bitblast`
(`N/M lean-proved`: how many of the M CNFs the Lean-proved bit-blaster made),
`extra.certificate_vcs` (the number of VCs the certificates cover),
`extra.certificate_proofs` (the number of checked LRAT proofs: 1 when
combined), `extra.certificate_covers` (combined: the label of every VC the
one proof covers; batched: one `[…]` group per proof, e.g. `ovf+@4, div0@7, unwind`) and `extra.cnf_sha256`
(one hash per checked CNF, comma-separated). If any VC is not certified, the
function stays `PROVED` and `extra.certify_note` names the first VC that was
not certified and why. `BOUNDED` and `PROVED-UNBOUNDED` are
never certified: the k-induction step is answered by Z3 in-process, and the
note says so. A function with no VCs at all (no property was inserted and no
loop cut is reachable, e.g. unsigned arithmetic only) stays `PROVED` with
`extra.certify_note = "no verification conditions (nothing to certify)"` and
`extra.certificate_vcs = "0"`: a certificate that checks nothing is not
labelled certified. A function proved under `// requires:` or contract
assumptions is `PROVED-ASSUMING` even when every VC was certified (the
certificate key is dropped and the note says so). When `--allow-exec`
translation validation later disagrees with a certified function, the
function becomes `ERROR` and the certificate is dropped.

The solver library (`include/prism/solver.hpp`, `src/prism/solver/`) returns
`certified = true`, which the verdict module reports as `PROVED-CERTIFIED`,
only when a checked certificate exists for the exact CNF of the query. There
are two ways the CNF is made; `certificate_info` always starts with which one
(`bitblast: lean-proved (toCNF_equisat) …` or `bitblast: z3 tactics …`).

`SolveOptions::bitblaster` chooses (`auto`, the default; `lean`; `z3`).
`auto` uses the Lean-proved bit-blaster for every certified request whose
formula is inside its fragment and whose two executables are built, and Z3's
tactics otherwise. A fallback is never silent: the note and the
`certificate_info` say `bitblast: z3 tactics, unproved (Lean bit-blaster not
used: <why>)`, where `<why>` is `formula outside the proved fragment:
operator bvsmod is outside the proved fragment`, `formula too large for the
Lean bit-blaster (DAG … MB > 2 MB; …)`, `prism-bitblast not found
(NOTRUN; …)` or `prism-lrat-check not found (NOTRUN; …)`.

Common first step: the formula is quantifier-free bitvector/Boolean, with no
arrays, floating point, uninterpreted functions, integers or reals
(`not_certifiable_reason`). Otherwise the result is the plain answer with the
note `not certifiable: <reason>`.

### 1.1 The Lean-proved path

1. **Serialize.** `to_lean_dag` (`src/prism/solver/leanbb.cpp`) writes the Z3
   term as the proved bit-blaster's input: `(dag (def W BASE e)* e)`
   (grammar in `proofs/techniques/PrismTechniques/BitblastSexp.lean`). Each
   Z3 operator maps to one `BVExpr` constructor, a few by definition
   (`bvuge x y` → `ule y x`, `=>` → `or (not a) b`, `bvnand` → `not (and …)`,
   rotations → `concat` of `extract`s, `bvredor` → `not (eq x 0)`). Free
   constants get disjoint bit blocks from 0; every subterm used more than
   once becomes a definition with a base above all inputs and all earlier
   definitions, so the text is linear in the Z3 DAG. Any other operator
   (`bvsmod`, arrays, UF, …) makes the whole query fall back (1.2).
2. **Bit-blast with the proved code.** `prism-bitblast` (a `lean_exe` of
   `proofs/techniques`) parses the text, checks `defsOK`, and prints
   `Std.Sat.CNF.dimacs (dagCNF g)` — exactly the function `toCNF` whose
   correctness is `toCNF_equisat` and `certified_dag_unsat`
   (`docs/PROOFS_TECHNIQUES.md`, section 4) — plus a variable map. PRISM
   checks the map against its own layout (input bit `j` is DIMACS variable
   `2j+1`), writes the DIMACS text byte for byte to `query.cnf` and records its
   SHA-256.
3. **Solve.** CaDiCaL runs on `query.cnf` with `--lrat=true --binary=false
   --unsat` (its option preset for unsatisfiable instances; it changes the
   search only) and writes `proof.lrat`. It must report UNSAT. CaDiCaL is
   **not** trusted.
4. **Check twice.** Both must accept (they run at the same time, on the same
   read-only files; each verdict is required on its own):
   - `cake_lpr query.cnf proof.lrat` prints the exact line `s VERIFIED UNSAT`;
   - `prism-lrat-check --dag query.dag query.cnf proof.lrat` — core Lean's
     verified LRAT checker (`Std.Tactic.BVDecide.LRAT.check`, soundness
     `LRAT.check_sound`). It rebuilds the CNF from `query.dag` with the proved
     bit-blaster, refuses unless `query.cnf` is byte for byte that CNF, and
     runs `checkDag g cert`, the exact premise of the theorem
     `checkDag_sound : checkDag g cert = true → ∀ ρ, g.eval ρ ≠ 1#1`. It must
     print `s VERIFIED UNSAT` and exit 0.
   drat-trim's `lrat-check` runs as well when installed and can only veto.
5. The SHA-256 of `query.cnf` is taken again after checking and must equal
   the recorded one.

`certificate_info` then reads, for example: `bitblast: lean-proved
(toCNF_equisat), prism-bitblast proofs/techniques, 2 shared definitions;
cadical c607304… lrat 224 steps, checked by cake_lpr …; and by Lean's
verified LRAT checker (prism-lrat-check --dag proofs/techniques,
checkDag_sound); lrat-check … agrees; cnf sha256 …`.

### 1.2 The Z3-tactics path (fallback, and `bitblaster = z3`)

1. **Bit-blast.** The formula is translated into a fresh private Z3 context.
   Each bitvector constant `x` of width `w` is replaced by the concatenation of
   `w` fresh Boolean constants, and the fixed Z3 tactic chain
   `simplify` → `bit-blast` → `simplify` → `tseitin-cnf` is applied
   (`bitblast` in `query.cpp`). The chain must produce exactly one goal made of
   clauses over Boolean constants. PRISM then reads those clauses one for one
   into DIMACS (`bitblast_fresh`, `to_dimacs`), with no reordering and no
   deletion except clauses that contain a literal `true`. The bit constants
   are numbered first, so the variable map back to `x` is known by
   construction. It does not go through Z3's model converter.
2. The DIMACS text is written to `query.cnf` and its SHA-256 is recorded.
3. **Solve.** CaDiCaL, as in 1.1.
4. **Check.** `cake_lpr query.cnf proof.lrat` must print the exact line
   `s VERIFIED UNSAT` (it exits 0 on rejection too, so only that line counts).
   If `prism-lrat-check` (Lean's checker, DIMACS mode) or drat-trim's
   `lrat-check` is installed, each runs as a further checker and can only
   veto: if one rejects, the result is not certified. When the CNF already
   contains the empty clause, neither applies and the certificate info says so.
5. The SHA-256 of `query.cnf` is taken again after checking and must equal
   the recorded one.

### Failure and caching (both paths)

If any step fails, `certified` stays `false` and the note gives the reason,
for example `not certified: cake_lpr rejected: ...`, `not certified: cake_lpr
accepted but Lean's LRAT checker rejected: ...`, `cadical not found
(NOTRUN)`, `cadical did not finish in time` or `cake_lpr ran out of memory
(CakeML heap exhausted) (heap cap 4096 MB; ...)`. The certificate tools run
under a memory cap (`PRISM_CHECKER_MEM`, default 4096 MB: cake_lpr's heap,
and the resident memory of `prism-bitblast` and `prism-lrat-check`, which
are killed past it); running out is reported as running out, never as a
rejection or an acceptance. A formula whose Lean DAG is larger than 2 MB is
bit-blasted by the Z3 path (1.2) and `certificate_info` says why: the Lean
bit-blaster's memory grows past the cap on such formulas. The plain answer
(`PROVED` trusts the solver) is still reported. The result is never quietly
upgraded.

A cached certified result is **not** trusted from disk. On a cache hit, PRISM
bit-blasts the formula again with the bit-blaster recorded in the entry
(`"bitblaster": "lean"` or `"z3"`; a request that would use the other one
solves again), requires the new CNF to hash to the stored `cnf_sha256`, and
runs the checkers of that path on the stored proof again, against that
fresh CNF. The store keeps only the proof, packed (a lossless varint
encoding of the LRAT text, `include/prism/solver_certs.hpp`), and unpacks it
into the request's private work directory; the packing is not trusted
either, since a damaged proof can only be rejected by the checkers. The
store has a size cap and is pruned least-recently-used; a pruned proof
means solving again. A cached plain `Unsat` is never certified: a certified
request solves again, and only if that gives no answer does the plain
`Unsat` stand, uncertified.

### Trusted components for `PROVED-CERTIFIED` on the Lean-proved path

This is the trusted base when `certificate_info` starts with
`bitblast: lean-proved`.

| # | Component | Why it is trusted | Mitigation |
|---|---|---|---|
| L1 | Clang (C/C++ to LLVM IR) | Produces the program that is verified | Pinned LLVM 23.1.4 (`third_party/llvm/PIN.toml`, fetched by `scripts/fetch_llvm.py`); `PRISM_LLVM_BIN` overrides it, and `extra.frontend` records the version used. |
| L2 | The LLVM→PIR translation (`src/prism/pir/translate*.cpp`) | If it changes the program, the VC is about another program | Proved in Lean for the functions in its fragment (`docs/PROOFS_REFINEMENT.md`): `pir_lean_check` (driven by `prism-qa pir-lean-check`, not part of a normal run) compares the C++ output with the proved translator's. With `--allow-exec` every verdict is translation-validated against `lli` on sample inputs. |
| L3 | PIR property instrumentation, the PIR encoder and the memory model (C++, `src/prism/pir/`: `encode.cpp`, `memory.cpp`, `translate_mem.cpp`) | If the VC does not mean "the property is violated", nothing downstream can notice | The design is modelled and proved in `proofs/semantics` (`docs/PROOFS_SEMANTICS.md`; the Bv memory encoding mirrors `PrismSem/MemEncode.lean`), but the C++ encoder is not proved to implement it. Counterexamples are replayed. |
| L4 | **The Z3 → S-expression serializer** (`to_lean_dag`, about 250 lines of C++) | A wrong mapping would bit-blast a different formula | Small, one operator per case. `tests/cpp/test_leanbb.cpp` evaluates Z3's term and the serialized formula on random and boundary assignments for every mapped operator at widths 8, 13, 32 and 64, against a C++ reference evaluator and against the Lean semantics itself (`prism-bitblast --eval`, i.e. `Dag.eval` of the parsed formula), and requires them to agree. SAT answers are still validated on the original Z3 term. |
| L5 | **The Lean compiler** (and the unproved S-expression parser in `BitblastSexp.lean`) | `prism-bitblast` and `prism-lrat-check` are compiled Lean: the executed code is the proved code (`toCNF`, `checkDag`, `LRAT.check`) only modulo the compiler. A parser bug can only change which formula is checked. | The round trip in L4 runs through the same parser and compiler. The Lean kernel checked every proof (`#assert_axioms`: standard axioms only). |
| L6 | The Lean kernel and the three standard axioms | Checks `toCNF_equisat`, `checkDag_sound` and core's `LRAT.check_sound` | `proofs/techniques` CI rebuilds and re-audits; `.github/workflows/proofs-recheck.yml` rechecks every Lean project with `leanchecker` and with nanoda, an independent kernel. |
| — | CaDiCaL | **Not trusted.** Its UNSAT counts only through the checked LRAT proof. | — |
| L7 | **cake_lpr** and **Lean's LRAT checker**, run together | The certificate checkers; both must accept | cake_lpr is verified in CakeML down to machine code (it still trusts the HOL4 kernel, CakeML's x86-64 model, `basis_ffi.c` and the OS). Lean's checker is verified in Lean (`LRAT.check_sound`) and runs on the CNF it rebuilds from the formula. A bug in one alone cannot certify a false claim. |
| L8 | The CNF file on disk, PRISM's SHA-256, the process runner and the verdict-line match | Identify and read back the exact CNF and the checkers' verdicts | As T6–T8 below. |
| L9 | The verdict mapping, the C++ compiler that builds PRISM, the hardware and the OS | As T9–T10 below | As T9–T10 below |

Compared with the Z3 path, T2–T4 (Z3's tactics, `Z3_translate`, PRISM's
clause reader) are gone from this base; L4 (the serializer) and L5 (the Lean
compiler) take their place.

### Trusted components for `PROVED-CERTIFIED` on the Z3-tactics path

| # | Component | Why it is trusted | Mitigation |
|---|---|---|---|
| T1 | The formula: Clang, the LLVM→PIR translation, property instrumentation and the PIR encoder that produce the Z3 bitvector VC (the `pir` stage, `src/prism/pir/`). A certificate says "this CNF is unsatisfiable", never "this CNF means the C function is safe". | If the VC does not mean "the property is violated", nothing downstream can notice | As L1–L3. Named `UNENCODED` constructs are `NEEDS-HARNESS`, never proved; the conformance suite (`src/tools/qa/conformance.cpp`) requires 0 wrong proofs. This library only checks SAT models against the VC. |
| T2 | `Z3_translate` into the fresh context | Copies the term. A bug would change the formula. | SAT answers are validated on the **original** term in the caller's context. Not used on the Lean-proved path. |
| T3 | **Z3's `simplify`, `bit-blast` and `tseitin-cnf` tactics** (Z3 4.13.4, vendored in `third_party/z3`) | These are the bit-blaster on this path. If they produce a CNF that is UNSAT while the formula is SAT, cake_lpr will correctly certify the wrong CNF. **This is the largest unproved part of this path.** | (a) Every SAT model found on the CNF, by CaDiCaL, Kissat or ProbSAT, is mapped back through the variable map and evaluated on the original formula in Z3, so a bad bit-blast shows up as a rejected model. (b) In certified mode, CaDiCaL's run on the CNF is always waited for, even when Z3 answered UNSAT first. If CaDiCaL then finds a model that validates, the counterexample wins and the note says `DISAGREEMENT:`. If its model does not validate, the result is not certified. (c) The doctest suite checks the variable map on known models. None of these is a proof. The Lean-proved bit-blaster (1.1) replaces this path for formulas in its fragment. |
| T4 | PRISM's clause reader and DIMACS writer (`bitblast_fresh`, `to_dimacs`, about 80 lines) | A dropped or changed clause would change the CNF | Unit tests: DIMACS round trip, and the kept CNF equals a fresh bit-blast. The reader accepts only `Or` of literals over Boolean constants and refuses anything else. |
| T5 | **cake_lpr** (`tanyongkiam/cake_lpr`, built from the shipped CakeML-compiled `cake_lpr.S`) | The certificate checker | Verified in CakeML: the proof covers the DIMACS and LRAT parsers and the checking algorithm down to the generated machine code. It still trusts the HOL4 kernel, CakeML's x86-64 ISA model, the small C FFI shim `basis_ffi.c` compiled with the host C compiler, and the OS. Lean's verified checker (`prism-lrat-check`, DIMACS mode) and drat-trim `lrat-check` (unverified C) run when present and can only veto. |
| T6 | The CNF file on disk between writing and checking | cake_lpr must check the exact CNF | SHA-256 recorded when the CNF is written and verified again after the checker runs. The file sits in a private temp directory. |
| T7 | PRISM's SHA-256 (`sha256_hex`) | Identifies the exact CNF and the cache entries | Tested against the FIPS 180-4 vectors (`tests/cpp/test_main.cpp`). |
| T8 | The process runner and the verdict-line match (`detail::run`, `check_lrat`) | A wrong parse could accept a rejection | Exact whole-line match on `s VERIFIED UNSAT`. The exit code is ignored for cake_lpr. A test runs a tampered proof. |
| T9 | `verdict_status` (`src/prism/solver/util.cpp`), the pir stage's certify step (`src/prism/pir/encode.cpp`) and the verdict audit, which together decide the word printed | Decides the word printed | The status is `laws::PROVED_CERTIFIED`, the verdict module's own spelling. `certified` is set only straight after the checkers accepted. The pir stage writes `PROVED-CERTIFIED` only when every VC of the function is certified, one by one or by one certificate for the disjunction of all of them (built by the same encoder from the same VC terms, and listed in `certificate_covers`), and the verdict audit (`laws::audit_report`, proved in `proofs/Prism/Verdict.lean`) demotes any `PROVED-CERTIFIED` without `certificate = "checked"` or from a stage that is not a solver stage. |
| T10 | The C++ compiler that builds PRISM, the Z3 library build, the hardware and the OS | Everything runs on them | Out of scope. They are listed so the reader knows they are assumed. |

### Not trusted (quarantined) on the certified path

- **CaDiCaL**: its UNSAT claim counts only through the checked LRAT proof. Its
  SAT claim counts only after its assignment satisfies the CNF **and** the
  mapped model satisfies the original formula in Z3.
- **Kissat, Bitwuzla, extra solvers from `SolveOptions::extra_solvers`**: SAT
  answers are validated in Z3. Their UNSAT answers are never certified.
- **The ProbSAT walker** (`probsat`, CPU) and its CUDA mirror
  (`src/cuda/probsat.cu`): they can only report a satisfying assignment,
  which is validated like any other model. "Not found" means nothing. The
  CUDA kernel also re-checks its assignment on the host.
- **The query cache and the solve-time history** (`~/.cache/prism/solver`):
  a cached SAT model is validated again, and a cached certificate is checked
  again (see above). A corrupt history file can only change solver order.
- **The C++ reference evaluator** of the S-expression format
  (`eval_lean_dag`): used only by tests.

## 2. What plain `PROVED` (from the solver library) depends on

Plain `PROVED` trusts L1–L3 (the formula) and whichever solver answered
UNSAT first:

- Z3 in-process: Z3 as a whole.
- Bitwuzla: Bitwuzla plus Z3's SMT-LIB2 printer (`Z3_benchmark_to_smtlib_string`).
- CaDiCaL or Kissat on DIMACS: that SAT solver plus the bit-blaster that made
  the CNF (T2–T4 for Z3's tactics; plain requests use them unless
  `bitblaster = lean`, which puts L4–L5 in their place).
- A cached plain UNSAT: whichever solver produced it (`winner` is kept).

`BOUNDED` and `PROVED-UNBOUNDED` (the k-induction step) trust Z3 in-process.
SAT answers (`FAILED`, counterexample) depend on the evaluator in Z3's model
code only, because every model is re-evaluated on the original formula.

## 3. Unproved components and how each is mitigated

| Trusted component | Mitigation | Proof |
|---|---|---|
| Clang (C/C++ to LLVM IR) | Pinned LLVM 23.1.4 (`third_party/llvm/PIN.toml`). With `--allow-exec`, each `pir` verdict is translation-validated against `lli`. The stages before `pir` use `src/prism/cparse.cpp` plus the adapters; none of them may prove. | None. |
| LLVM→PIR translation | `pir_lean_check` compares the C++ translator's output with the proved Lean translator per function (`docs/PROOFS_REFINEMENT.md`); `prism-qa llvm-sem-vs-lli` tests the Lean LLVM semantics against `lli`. | Proved in Lean for the fragment (`proofs/refinement`); functions outside it rely on the mitigations. |
| PIR encoder / VC generation and memory model | Counterexamples are replayed; translation validation against `lli` with `--allow-exec`; both memory encodings agree on every property (`tests/cpp/test_pir_mem.cpp`); the conformance suite. | The design is proved in Lean (`proofs/semantics`: `Memory.lean`, `MemEncode.lean`); the C++ is not. |
| Floating point in the `pir` stage (Z3's FP theory, FLOAT-CAST-OVF, `--fp-checks`) | Translation validation against `lli` with `--allow-exec`; `testdata/pir/fp_*`; `tests/cpp/test_repolint.cpp` locks the C++ condition text to the Lean definitions. | The conditions are proved against IEEE 754 and C11 (`proofs/refinement/PrismRefine/FloatOps.lean`). Still trusted: Z3's FP theory implementing IEEE 754 (and rounding `Z3_mk_fpa_numeral_double` to nearest even), and the step from the C++ text to Z3 terms. |
| Lazy-sequentialisation schedule of the `conc` stage | Conformance suite `testdata/conformance/concurrency`, `tests/cpp/test_conc.cpp`. | The schedule is proved sound and covering under SC (`proofs/techniques/PrismTechniques/LazySeqN.lean`). Still trusted: the Z3 slot formula in `lazy.cpp` and the race/deadlock monitors; relaxed memory is not modelled. |
| Verdict lattice (`laws`) | The C++ module is checked entry by entry against the Lean model's tables (`docs/VERDICTS.md`). | Proved in Lean (`proofs/Prism/Verdict.lean`); the C++ equals the model on the whole finite domain. |
| Lean kernel | `.github/workflows/proofs-recheck.yml` replays every project with `leanchecker` and type-checks it again with nanoda, an independent kernel; every declaration's axioms are audited. | — |
| Lean compiler (for `prism-bitblast` / `prism-lrat-check`) | The serializer round trip (`tests/cpp/test_leanbb.cpp`) runs through the compiled parser and `Dag.eval`, and cake_lpr checks every certificate independently. | — |
| The C++ compiler that builds PRISM | CI builds with the pinned clang on Linux and clang-cl on Windows (`.github/workflows/ci.yml`). | — |
| Hardware and operating system | Out of scope; stated here. | — |
| Solver portfolio, cache, scheduler, SLS, CUDA kernels | Quarantined. No answer from them is accepted without a model check or a checked certificate. | Not needed. |

## 4. Running scanned code

Nothing on the proof path executes the scanned program. Translation
validation (`lli`), counterexample replay, sanitizer runs, fuzzing and the
Lean proof search run code from the scanned tree or from a model, so they run
only with `--allow-exec` (Law 9; `docs/ARCHITECTURE.md`, "Running on
untrusted code"). Without it each such step is `NOTRUN` with a hint. With it,
the code runs in a bubblewrap jail: read-only `/usr`, `/bin`, `/lib*`, a
minimal `/etc`, the LLVM toolchain, the prism-deps tool home and the program
prefix; never `/` or `$HOME`; no network. The sandbox kind is `bwrap`,
`job-object` (Windows) or `rlimits-only` (other POSIX); only `bwrap`
isolates. Where bwrap cannot run, `--allow-exec` steps stay `NOTRUN` unless
`--allow-exec=unsandboxed` (`Config.allow_unsandboxed`) accepts resource
limits only. The sandbox protects the machine; it is not part of any
verdict's trusted base, and no proof covers a sandbox escape.

## 5. AI output

The AI stages run FORGE in process (`include/prism/forge_llm.hpp`,
`src/prism/ai/forge_llm.cpp`) on a FORGE `.fpk` model file. FORGE is pinned
by full commit SHA in `cmake/FORGE_PIN`. Neither FORGE nor the model is
trusted: model output is `HYPOTHESIS` or `READS` (Law 4;
`admit_model_never_proof` in `proofs/Prism/Verdict.lean`). A model-drafted
invariant, harness, contract or Lean proof counts only after a checker
re-proves it (the solver, or the Lean kernel with the axiom audit), and then
the trust is that checker's.

## 6. Solver tools

`prism-deps tool NAME` builds each pinned commit into
`~/.prism/tools/<name>/<commit>/`; the solver library looks there first,
then on `PATH`. The pins (commit, archive digest, licence) are in
`third_party/MANIFEST.toml`:

| Tool | Version in the manifest | Role |
|---|---|---|
| CaDiCaL | 3.0.1 | Portfolio member. Writes the LRAT proof in certified mode. |
| Kissat | 4.0.4 | Portfolio member (DIMACS) |
| cake_lpr | upstream default-branch commit (no release tags) | Verified LRAT checker (decides certification) |
| Bitwuzla | 0.9.1 | Portfolio member when present |
| Z3 | 4.13.4 (linked, vendored in `third_party/z3`) | In-process member, fallback bit-blaster, model validation |
| `prism-bitblast`, `prism-lrat-check` | built from `proofs/techniques` (`lake build`, Lean 4.34.0) | The proved bit-blaster and Lean's verified LRAT checker (1.1). Found under `~/.prism/tools`, on `PATH`, or in `proofs/techniques/.lake/build/bin` of the source tree PRISM was built from |
| drat-trim `lrat-check` | not in the manifest; used when found | Further LRAT checker (veto only) |
