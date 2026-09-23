# PRISM trusted base

This file says what a PRISM proof verdict depends on. If a component listed
here is wrong, PRISM can print a proof that is false. Everything not listed
is quarantined: it can be buggy, but it cannot on its own make PRISM claim a
proof. Roadmap Parts 3.2, 5 and 8.3 require this file. **Every PRISM run
writes a copy of it next to `report.md`** (both engines), `report.md` links to
it, and SARIF `runs[0].properties.trustedBase` names it with its SHA-256.

It describes the code **as it is today**. Where the roadmap plans something
that does not exist yet (the Lean verdict module compiled into PRISM, per-run
translation validation), the file says "planned" and lists what stands in its
place now. The Lean-proved bit-blaster and Lean's LRAT checker exist and are
used by certified mode (section 1.1); Z3's tactics remain as the fallback
(section 1.2), and every certificate says which one made its CNF.

## 1. What `PROVED-CERTIFIED` depends on today

`PROVED-CERTIFIED` is emitted by one stage, `pir`, and only with
`--certified` (`Config.certified`). The pir stage turns each function into
verification conditions (VCs): one per inserted property (signed overflow,
division, shifts, uninitialised reads, clz/ctz of zero, `assert`, poison
flags, and the memory-safety checks of the memory model: null and wild
dereference, use after free, out of bounds, misalignment, invalid, double and
mismatched free, overlap, pointer arithmetic, ... — `docs/PIR.md#memory-model-roadmap-25`),
plus the unwinding assertion when the function has a loop. Memory is encoded
by default as Ackermannised read-over-write chains (`MemEncoding::Bv`), so
every VC, memory VCs included, is quantifier-free bitvector logic (QF_BV) and
can be certified; the unbounded array encoding (`MemEncoding::Array`) is not
certifiable and is not the default. Every VC goes through the solver library
(`prism::solver::solve`: the portfolio and the query cache). A function
becomes `PROVED-CERTIFIED` only when:

- it would be plain `PROVED` (no property VC satisfiable, and every loop
  closes within `--unwind`, shown by the unwinding assertion), **and**
- it has at least one VC, **and**
- **every** one of its VCs, each property and the unwinding assertion, came
  back `certified` by the chain below.

The finding then carries `extra.certificate = "checked"` (the verdict audit
admits `PROVED-CERTIFIED` from the `pir` stage only with it),
`extra.certificate_info` (one entry per VC: bit-blaster, solver build, LRAT
steps, checker builds, CNF hash), `extra.certificate_bitblast` (`N/M
lean-proved`: how many of the M CNFs the Lean-proved bit-blaster made),
`extra.certificate_vcs` (M, the number of checked certificates) and
`extra.cnf_sha256` (one hash per VC, comma-separated). If any VC is not
certified, the function stays `PROVED` and `extra.certify_note` names the
first VC that was not certified and why. `BOUNDED` and `PROVED-UNBOUNDED` are
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
operator bvsmod is outside the proved fragment`, `prism-bitblast not found
(NOTRUN; …)` or `prism-lrat-check not found (NOTRUN; …)`.

Common first step: the formula is quantifier-free bitvector/Boolean, with no
arrays, floating point, uninterpreted functions, integers or reals
(`not_certifiable_reason`). Otherwise the result is the plain answer with the
note `not certifiable: <reason>`.

### 1.1 The Lean-proved path (roadmap 3.2 steps 2–5)

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
3. **Solve.** CaDiCaL runs on `query.cnf` with `--lrat=true --binary=false` and
   writes `proof.lrat`. It must report UNSAT. CaDiCaL is **not** trusted.
4. **Check twice.** Both must accept:
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
cadical c607304… lrat 224 steps, checked by cake_lpr 2e3b2dc…; and by Lean's
verified LRAT checker (prism-lrat-check --dag proofs/techniques,
checkDag_sound); lrat-check a36874a… agrees; cnf sha256 …`.

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
(NOTRUN)` or `cadical did not finish in time`. The plain answer (`PROVED`
trusts the solver) is still reported. The result is never quietly upgraded
(roadmap 3.2).

A cached certified result is **not** trusted from disk. On a cache hit, PRISM
bit-blasts the formula again with the bit-blaster recorded in the entry
(`"bitblaster": "lean"` or `"z3"`; a request that would use the other one
solves again), requires the new CNF to hash to the stored `cnf_sha256`, and
runs the checkers of that path on the stored proof again. A cached plain
`Unsat` is never certified: a certified request solves again, and only if
that gives no answer does the plain `Unsat` stand, uncertified.

### Trusted components for `PROVED-CERTIFIED` on the Lean-proved path

This is the trusted base when `certificate_info` starts with
`bitblast: lean-proved`.

| # | Component | Why it is trusted | Mitigation today |
|---|---|---|---|
| L1 | Clang (C/C++ to LLVM IR) | Produces the program that is verified | Pinned release. Out of this library's scope (Part 2). |
| L2 | The LLVM→PIR translation | If it changes the program, the VC is about another program | Validated per run by the `pir` stage (roadmap 2.4: translation checked by execution). |
| L3 | PIR property instrumentation, the PIR encoder and the memory model (C++, `src/prism/pir/`: `encode.cpp`, `memory.cpp`, `translate_mem.cpp`) | If the VC does not mean "the property is violated", nothing downstream can notice | Modelled and proved in `proofs/semantics` (roadmap 5.3; the Bv memory encoding mirrors `PrismSem/MemEncode.lean`), but the C++ is not extracted from the model; what the Lean memory proofs do not cover is listed in `docs/PIR.md#correspondence-to-the-lean-model`. Counterexamples are replayed. |
| L4 | **The Z3 → S-expression serializer** (`to_lean_dag`, about 250 lines of C++) | A wrong mapping would bit-blast a different formula | Small, one operator per case. `tests/cpp/test_leanbb.cpp` evaluates Z3's term and the serialized formula on random and boundary assignments for every mapped operator at widths 8, 13, 32 and 64, against a C++ reference evaluator and against the Lean semantics itself (`prism-bitblast --eval`, i.e. `Dag.eval` of the parsed formula), and requires them to agree. SAT answers are still validated on the original Z3 term. |
| L5 | **The Lean compiler** (and the unproved S-expression parser in `BitblastSexp.lean`) | `prism-bitblast` and `prism-lrat-check` are compiled Lean: the executed code is the proved code (`toCNF`, `checkDag`, `LRAT.check`) only modulo the compiler. A parser bug can only change which formula is checked. | The round trip in L4 runs through the same parser and compiler. The Lean kernel checked every proof (`#assert_axioms`: standard axioms only). |
| L6 | The Lean kernel and the three standard axioms | Checks `toCNF_equisat`, `checkDag_sound` and core's `LRAT.check_sound` | Widely audited; `proofs/techniques` CI rebuilds and re-audits. |
| — | CaDiCaL | **Not trusted.** Its UNSAT counts only through the checked LRAT proof. | — |
| L7 | **cake_lpr** and **Lean's LRAT checker**, run together | The certificate checkers; both must accept | cake_lpr is verified in CakeML down to machine code (it still trusts the HOL4 kernel, CakeML's x86-64 model, `basis_ffi.c` and the OS). Lean's checker is verified in Lean (`LRAT.check_sound`) and runs on the CNF it rebuilds from the formula. A bug in one alone cannot certify a false claim. |
| L8 | The CNF file on disk, PRISM's SHA-256, the process runner and the verdict-line match | Identify and read back the exact CNF and the checkers' verdicts | As T6–T8 below. |
| L9 | The verdict mapping, the C++ compiler that builds PRISM, the hardware and the OS | As T9–T10 below | As T9–T10 below |

Compared with the Z3 path, T2–T4 (Z3's tactics, `Z3_translate`, PRISM's
clause reader) are gone from this base; L4 (the serializer) and L5 (the Lean
compiler) take their place.

### Trusted components for `PROVED-CERTIFIED` on the Z3-tactics path

| # | Component | Why it is trusted | Mitigation today | Roadmap target |
|---|---|---|---|---|
| T1 | The formula: Clang, the LLVM→PIR translation, property instrumentation and the PIR encoder that produce the Z3 bitvector VC (Parts 2 and 5.3; the `pir` stage, `src/prism/pir/`). A certificate says "this CNF is unsatisfiable", never "this CNF means the C function is safe". | If the VC does not mean "the property is violated", nothing downstream can notice | The pir stage (docs/PIR.md): named `UNENCODED` constructs are `NEEDS-HARNESS`, never proved; with `--allow-exec` every verdict is translation-validated against `lli` on 64 inputs; the conformance suite (`tools/conformance.py`, 0 wrong proofs required) and the differential oracle against the bmc stage. This library only checks SAT models against the VC. | Encoder soundness proved in Lean (5.3), LLVM→PIR refinement proof (8.2), per-run translation validation (2.4) |
| T2 | `Z3_translate` into the fresh context | Copies the term. A bug would change the formula. | Z3 is widely used. SAT answers are validated on the **original** term in the caller's context. | Replaced by the Lean-proved bit-blaster (1.1) for formulas in its fragment |
| T3 | **Z3's `simplify`, `bit-blast` and `tseitin-cnf` tactics** (Z3 4.13.4 vendored in `third_party/z3`) | These are the bit-blaster on this path. If they produce a CNF that is UNSAT while the formula is SAT, cake_lpr will correctly certify the wrong CNF. **This is the largest unproved part of this path.** | (a) Every SAT model found on the CNF, by CaDiCaL, Kissat or ProbSAT, is mapped back through the variable map and evaluated on the original formula in Z3, so a bad bit-blast shows up as a rejected model. (b) In certified mode, CaDiCaL's run on the CNF is always waited for, even when Z3 answered UNSAT first. If CaDiCaL then finds a model that validates, the counterexample wins and the note says `DISAGREEMENT:`. If its model does not validate, the result is not certified. (c) The doctest suite checks the variable map on known models. None of these is a proof. | The Lean-proved bit-blaster (1.1), which reuses `bv_decide`'s lemmas, already replaces it for formulas in the proved fragment |
| T4 | PRISM's clause reader and DIMACS writer (`bitblast_fresh`, `to_dimacs`, about 80 lines) | A dropped or changed clause would change the CNF | Unit tests: DIMACS round trip, and the kept CNF equals a fresh bit-blast. The reader accepts only `Or` of literals over Boolean constants and refuses anything else. | Emitted by the Lean bit-blaster (1.1) |
| T5 | **cake_lpr** (`tanyongkiam/cake_lpr`, built from the shipped CakeML-compiled `cake_lpr.S`) | The certificate checker | Verified in CakeML: the proof covers the DIMACS and LRAT parsers and the checking algorithm down to the generated machine code. It still trusts the HOL4 kernel, CakeML's x86-64 ISA model, the small C FFI shim `basis_ffi.c` compiled with gcc, and the OS. Further checkers: Lean's verified checker (`prism-lrat-check`, DIMACS mode) and drat-trim `lrat-check` (unverified C) run when present and can only veto. | Lean's checker required on this path too once it ships with PRISM |
| T6 | The CNF file on disk between writing and checking | cake_lpr must check the exact CNF | SHA-256 recorded when the CNF is written and verified again after the checker runs. The file sits in a private temp directory. | Same |
| T7 | PRISM's SHA-256 (`util.cpp`) | Identifies the exact CNF and the cache entries | Tested against the FIPS 180-2 vectors | Same |
| T8 | The process runner and the verdict-line match (`detail::run`, `check_lrat`) | A wrong parse could accept a rejection | Exact whole-line match on `s VERIFIED UNSAT`. The exit code is ignored for cake_lpr. A test runs a tampered proof. | Same |
| T9 | `verdict_status`, the pir stage's `certify` step (`src/prism/pir/encode.cpp`) and the verdict audit, which together decide the word printed | Decides the word printed | The status is `laws::PROVED_CERTIFIED`, the verdict module's own spelling. `certified` is set in exactly two places in the solver, both straight after `run_checkers` accepted. The pir stage writes `PROVED-CERTIFIED` only when every VC of the function is certified, and the verdict audit (`laws::audit_report`, proved in `proofs/Prism/Verdict.lean`) demotes any `PROVED-CERTIFIED` without `certificate = "checked"` or from a stage that is not a solver stage. | Verdict module in Lean, compiled into PRISM (5.1) |
| T10 | The C++ compiler that builds PRISM, the Z3 library build, the hardware and the OS | Everything runs on them | Out of scope. They are listed so the reader knows they are assumed. | Clang and GCC cross-builds, reproducible builds (8.3) |

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

Plain `PROVED` trusts whichever solver answered UNSAT first:

- Z3 in-process: Z3 as a whole.
- Bitwuzla: Bitwuzla plus Z3's SMT-LIB2 printer (`Z3_benchmark_to_smtlib_string`).
- CaDiCaL or Kissat on DIMACS: that SAT solver plus the bit-blaster that made
  the CNF (T2–T4 for Z3's tactics; plain requests use them unless
  `bitblaster = lean`, which puts L4–L5 in their place).
- A cached plain UNSAT: whichever solver produced it (`winner` is kept).

SAT answers (`FAILED`, counterexample) depend on the evaluator in Z3's model
code only, because every model is re-evaluated on the original formula.

## 3. Unproved components and how each is mitigated (roadmap 8.3)

This is the list roadmap 8.3 asks for. "Today" is what the repository does
now. "Planned" is what the roadmap will add.

| Trusted component | Mitigation (planned, roadmap 8.3) | Status today |
|---|---|---|
| Clang (C/C++ to LLVM IR) | Pinned release. Per-run concrete execution check of the LLVM IR against the source build. Csmith and YARPGen random testing. Conformance suite on every upgrade. | The `pir` stage (the only stage that can emit `PROVED-CERTIFIED`) lowers with the Clang/LLVM found on the machine (18 on the reference machine; the version is in `extra.frontend`). With `--allow-exec` each verdict is translation-validated against `lli`. Not proved. The older stages use `src/prism/cparse.cpp` plus the adapters, none of it proved. |
| Lean kernel | Small and widely audited. Every proof rechecked by an independent checker (lean4checker and a second implementation such as nanoda). | Lean proofs exist (`proofs/`: the verdict lattice, PIR semantics and memory model, techniques including the bit-blaster, LLVM refinement); `proofs/check.sh` rebuilds them and audits their axioms, and CI rechecks every Lean project independently (roadmap 8.5). |
| Lean compiler (for the verdict module) | Differential property testing of the compiled module against the Lean model with rapidcheck | Planned (5.1). Today the verdict module is plain C++ (`src/prism/laws.cpp`, `include/prism/laws.hpp`). |
| Lean compiler (for `prism-bitblast` / `prism-lrat-check`) | The compiled bit-blaster and checker are the proved Lean functions; the serializer round trip (`tests/cpp/test_leanbb.cpp`) runs through the compiled parser and `Dag.eval`, and cake_lpr checks every certificate independently. | In use (section 1.1, L5). |
| The C++ compiler that builds PRISM | Build with Clang and GCC and cross-check results on the conformance suite. Reproducible builds. | CI builds with Clang only (`.github/workflows/ci.yml`) |
| Hardware and operating system running PRISM | Out of scope, and stated in the report | Stated here |
| Formal LLVM semantics | Its assumptions are documented. It is tested against `lli` on generated programs. | Planned (8.2) |

Components that are unproved today but are meant to be **proved** (roadmap
8.2), so they belong to the trusted base until then:

| Component | Mitigation today | Proof planned |
|---|---|---|
| Certified-mode bit-blaster | **Proved** for the fragment PIR VCs use (`proofs/techniques`, `toCNF_equisat`, `checkDag_sound`) and used by certified mode (1.1). Formulas outside it (`bvsmod`, arrays, UF) fall back to Z3's tactics (T2–T4), with the reason in the note. | Done for the fragment (5.4); the Z3 → S-expression serializer (L4) stays trusted and tested |
| PIR encoder / VC generation and memory model (the `pir` stage) | Counterexamples are replayed; translation validation against `lli` with `--allow-exec`; both memory encodings agree on every property (`tests/cpp/test_pir_mem.cpp`); the conformance suite. The Lean model (`proofs/semantics`, incl. `Memory.lean`, `MemEncode.lean`) proves the encoding scheme, not the C++ | Lean (5.3), refinement (8.2) |
| Verdict lattice (`laws`) | Python and C++ parity tests for the law strings and merge rules (`tests/test_*`, `tests/cpp/test_main.cpp`) | Lean, compiled into PRISM (5.1) |
| LRAT checking | cake_lpr (verified in CakeML) and Lean's verified checker (`prism-lrat-check`, `LRAT.check_sound`) run together; on the Lean path both must accept (1.1). drat-trim `lrat-check` is a further veto. | Done (8.2) |
| Solver portfolio, cache, scheduler, SLS, CUDA kernels | Quarantined. No answer from them is accepted without a model check or a checked certificate. | None needed (8.1) |

## 4. Solver builds used when this was written

Built under `~/.prism/tools/<name>/<commit>/bin/` (the layout
`scripts/fetch_deps.py` uses). The solver library looks there first, then on
`PATH`:

| Tool | Commit | Role |
|---|---|---|
| CaDiCaL 3.0.1 | `c60730422e758ef1cebe7aeddf2dda31c996bf04` | Portfolio member. Writes the LRAT proof in certified mode. |
| Kissat | `8af8e56f174b778aef3aa45af9f739b2a5f492c2` | Portfolio member (DIMACS) |
| cake_lpr | `2e3b2dc0ecf938addbd779d42877b6ed69d9a985` | Verified LRAT checker (decides certification) |
| drat-trim (`lrat-check`) | `a36874a8b750b43fe4b385b8ddbf5b033e46a3fa` | Further LRAT checker (veto only) |
| `prism-bitblast`, `prism-lrat-check` | built from `proofs/techniques` (`lake build`, Lean 4.34.0) | The proved bit-blaster and Lean's verified LRAT checker (1.1). Found under `~/.prism/tools`, on `PATH`, or in `proofs/techniques/.lake/build/bin` of the source tree PRISM was built from |
| Bitwuzla | not built here (needs meson + GMP) | Portfolio member when present on `PATH` or under `~/.prism/tools/bitwuzla/` |
| Z3 | 4.13.4 (vendored `third_party/z3`) | In-process member, bit-blaster, model validation |
