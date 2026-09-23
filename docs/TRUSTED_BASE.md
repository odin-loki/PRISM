# PRISM trusted base

This file says what a PRISM proof verdict depends on. If a component listed
here is wrong, PRISM can print a proof that is false. Everything not listed
is quarantined: it can be buggy, but it cannot on its own make PRISM claim a
proof. Roadmap Parts 3.2, 5 and 8.3 require this file, and it is meant to be
shipped with every report.

It describes the code **as it is today**. Where the roadmap plans something
that does not exist yet (the Lean-proved bit-blaster, the Lean verdict module,
per-run translation validation), the file says "planned" and lists what stands
in its place now.

## 1. What `PROVED-CERTIFIED` depends on today

The solver library (`include/prism/solver.hpp`, `src/prism/solver/`) returns
`certified = true`, which the verdict module reports as `PROVED-CERTIFIED`,
only when **all** of these steps succeed for one query:

1. The formula is quantifier-free bitvector/Boolean: no arrays, floating
   point, uninterpreted functions, integers or reals
   (`not_certifiable_reason`). Otherwise the result is the plain answer with
   the note `not certifiable: <reason>`.
2. **Bit-blast.** The formula is translated into a fresh private Z3 context.
   Each bitvector constant `x` of width `w` is replaced by the concatenation of
   `w` fresh Boolean constants, and the fixed Z3 tactic chain
   `simplify` → `bit-blast` → `simplify` → `tseitin-cnf` is applied
   (`bitblast` in `query.cpp`). The chain must produce exactly one goal made of
   clauses over Boolean constants. PRISM then reads those clauses one for one
   into DIMACS (`bitblast_fresh`, `to_dimacs`), with no reordering and no
   deletion except clauses that contain a literal `true`. The bit constants
   are numbered first, so the variable map back to `x` is known by
   construction. It does not go through Z3's model converter.
3. The DIMACS text is written to `query.cnf` and its SHA-256 is recorded.
4. **Solve.** CaDiCaL runs on that file with `--lrat=true --binary=false` and
   writes `proof.lrat`. It must report UNSAT. CaDiCaL itself is **not**
   trusted.
5. **Check.** `cake_lpr query.cnf proof.lrat` must print the exact line
   `s VERIFIED UNSAT` (it exits 0 on rejection too, so only that line counts).
   If drat-trim's `lrat-check` is installed, it runs as a second checker and
   can only veto: if it rejects, the result is not certified. When the CNF
   already contains the empty clause, `lrat-check` does not apply and the
   certificate info says so.
6. The SHA-256 of `query.cnf` is taken again after checking and must equal
   the recorded one.

If any step fails, `certified` stays `false` and the note gives the reason,
for example `not certified: cake_lpr rejected: ...`, `cadical not found
(NOTRUN)` or `cadical did not finish in time`. The plain answer (`PROVED`
trusts the solver) is still reported. The result is never quietly upgraded
(roadmap 3.2). `certificate_info` records the solver and checker builds, the
number of LRAT steps and the CNF hash, for example
`cadical c607304… lrat 131 steps, checked by cake_lpr 2e3b2dc…; lrat-check a36874a… agrees; cnf sha256 …`.

A cached certified result is **not** trusted from disk. On a cache hit, PRISM
bit-blasts the formula again, requires the new CNF to hash to the stored
`cnf_sha256`, and runs cake_lpr on the stored proof again. A cached plain
`Unsat` never answers a certified request.

### Trusted components for `PROVED-CERTIFIED` (today)

| # | Component | Why it is trusted | Mitigation today | Roadmap target |
|---|---|---|---|---|
| T1 | The formula: Clang, the LLVM→PIR translation, property instrumentation and the PIR encoder that produce the Z3 bitvector VC (Parts 2 and 5.3; the `pir` stage) | If the VC does not mean "the property is violated", nothing downstream can notice | Out of this library's scope. It is described by the `pir` stage. This library only checks SAT models against the VC. Replaying a counterexample on the real program is the caller's job. | Encoder soundness proved in Lean (5.3), LLVM→PIR refinement proof (8.2), per-run translation validation (2.4) |
| T2 | `Z3_translate` into the fresh context | Copies the term. A bug would change the formula. | Z3 is widely used. SAT answers are validated on the **original** term in the caller's context. | Replaced by the Lean-proved bit-blaster reading the VC directly (5.4) |
| T3 | **Z3's `simplify`, `bit-blast` and `tseitin-cnf` tactics** (Z3 4.13.4 vendored in `third_party/z3`) | These are the bit-blaster. If they produce a CNF that is UNSAT while the formula is SAT, cake_lpr will correctly certify the wrong CNF. **This is the largest unproved part of the certified path.** | (a) Every SAT model found on the CNF, by CaDiCaL, Kissat or ProbSAT, is mapped back through the variable map and evaluated on the original formula in Z3, so a bad bit-blast shows up as a rejected model. (b) In certified mode, CaDiCaL's run on the CNF is always waited for, even when Z3 answered UNSAT first. If CaDiCaL then finds a model that validates, the counterexample wins and the note says `DISAGREEMENT:`. If its model does not validate, the result is not certified. (c) The doctest suite checks the variable map on known models. None of these is a proof. | A bit-blaster proved correct in Lean, reusing `bv_decide` (roadmap 5.4). That removes T2 and T3. |
| T4 | PRISM's clause reader and DIMACS writer (`bitblast_fresh`, `to_dimacs`, about 80 lines) | A dropped or changed clause would change the CNF | Unit tests: DIMACS round trip, and the kept CNF equals a fresh bit-blast. The reader accepts only `Or` of literals over Boolean constants and refuses anything else. | Emitted by the Lean bit-blaster (5.4) |
| T5 | **cake_lpr** (`tanyongkiam/cake_lpr`, built from the shipped CakeML-compiled `cake_lpr.S`) | The certificate checker | Verified in CakeML: the proof covers the DIMACS and LRAT parsers and the checking algorithm down to the generated machine code. It still trusts the HOL4 kernel, CakeML's x86-64 ISA model, the small C FFI shim `basis_ffi.c` compiled with gcc, and the OS. Second checker: drat-trim `lrat-check` (unverified C) runs when present and can only veto. | Also run Lean's LRAT checker (roadmap 3.2 step 4, 8.2) |
| T6 | The CNF file on disk between writing and checking | cake_lpr must check the exact CNF | SHA-256 recorded when the CNF is written and verified again after the checker runs. The file sits in a private temp directory. | Same |
| T7 | PRISM's SHA-256 (`util.cpp`) | Identifies the exact CNF and the cache entries | Tested against the FIPS 180-2 vectors | Same |
| T8 | The process runner and the verdict-line match (`detail::run`, `check_lrat`) | A wrong parse could accept a rejection | Exact whole-line match on `s VERIFIED UNSAT`. The exit code is ignored. A test runs a tampered proof. | Same |
| T9 | `verdict_status` / the verdict module mapping `certified` to `PROVED-CERTIFIED` | Decides the word printed | The solver library uses a local `kProvedCertified` constant until the integrator switches it to `laws::PROVED_CERTIFIED`. `certified` is set in exactly two places, both straight after `run_checkers` accepted. | Verdict module in Lean, compiled into PRISM (5.1) |
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

## 2. What plain `PROVED` (from the solver library) depends on

Plain `PROVED` trusts whichever solver answered UNSAT first:

- Z3 in-process: Z3 as a whole.
- Bitwuzla: Bitwuzla plus Z3's SMT-LIB2 printer (`Z3_benchmark_to_smtlib_string`).
- CaDiCaL or Kissat on DIMACS: that SAT solver plus T2–T4 (Z3 bit-blast and
  PRISM's DIMACS writer).
- A cached plain UNSAT: whichever solver produced it (`winner` is kept).

SAT answers (`FAILED`, counterexample) depend on the evaluator in Z3's model
code only, because every model is re-evaluated on the original formula.

## 3. Unproved components and how each is mitigated (roadmap 8.3)

This is the list roadmap 8.3 asks for. "Today" is what the repository does
now. "Planned" is what the roadmap will add.

| Trusted component | Mitigation (planned, roadmap 8.3) | Status today |
|---|---|---|
| Clang (C/C++ to LLVM IR) | Pinned release. Per-run concrete execution check of the LLVM IR against the source build. Csmith and YARPGen random testing. Conformance suite on every upgrade. | Planned (Part 2). Today's C/C++ front end is `src/prism/cparse.cpp` plus the adapters, and none of it is proved. |
| Lean kernel | Small and widely audited. Every proof rechecked by an independent checker (lean4checker and a second implementation such as nanoda). | No Lean proofs exist in the repository yet (Part 5). |
| Lean compiler (for the verdict module) | Differential property testing of the compiled module against the Lean model with rapidcheck | Planned (5.1). Today the verdict module is plain C++ (`src/prism/laws.cpp`, `include/prism/laws.hpp`). |
| The C++ compiler that builds PRISM | Build with Clang and GCC and cross-check results on the conformance suite. Reproducible builds. | CI builds with Clang only (`.github/workflows/ci.yml`) |
| Hardware and operating system running PRISM | Out of scope, and stated in the report | Stated here |
| Formal LLVM semantics | Its assumptions are documented. It is tested against `lli` on generated programs. | Planned (8.2) |

Components that are unproved today but are meant to be **proved** (roadmap
8.2), so they belong to the trusted base until then:

| Component | Mitigation today | Proof planned |
|---|---|---|
| Certified-mode bit-blaster (Z3 tactics, T2–T4 above) | Model validation of every SAT answer on the original formula. Z3 and CaDiCaL answers compared. DIMACS round-trip tests. | Lean, reusing `bv_decide` (5.4) |
| PIR encoder / VC generation (the `pir` stage) | Counterexamples are replayed. Engine parity tests. | Lean (5.3) |
| Verdict lattice (`laws`) | Python and C++ parity tests for the law strings and merge rules (`tests/test_*`, `tests/cpp/test_main.cpp`) | Lean, compiled into PRISM (5.1) |
| LRAT checking | cake_lpr is already verified in CakeML. drat-trim `lrat-check` is a second opinion. | Add Lean's checker alongside cake_lpr (8.2) |
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
| drat-trim (`lrat-check`) | `a36874a8b750b43fe4b385b8ddbf5b033e46a3fa` | Second LRAT checker (veto only) |
| Bitwuzla | not built here (needs meson + GMP) | Portfolio member when present on `PATH` or under `~/.prism/tools/bitwuzla/` |
| Z3 | 4.13.4 (vendored `third_party/z3`) | In-process member, bit-blaster, model validation |
