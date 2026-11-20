# PRISM user guide

PRISM (Performance, Regression, Integration and Security Module) is a code
checker: one command that finds every error it can in a code base and says,
just as plainly, what it could not check. This guide covers installing it,
running it, reading its reports and wiring it into CI.

- Design and stage order: [PLAN.md](PLAN.md)
- Every verdict and the laws that govern it: [VERDICTS.md](VERDICTS.md)
- How far PRISM's verdicts can be trusted, measured: [CONFORMANCE.md](CONFORMANCE.md)

## 1. Install

The checker is the C++ engine (`src/prism/`, `include/prism/`). Linux x86-64
is the primary platform (Windows builds natively with MSVC `cl`, tested in CI;
see [MSVC_PORT.md](MSVC_PORT.md). WSL also works).

### Build

Requirements: CMake 3.20+, Ninja, clang (the pinned release is
`third_party/llvm/PIN.toml`; GCC 13 also builds), Python 3.11 for the tooling.
Z3, PCRE2, nlohmann/json and doctest are vendored under `third_party/` and
built with PRISM. FORGE is the one dependency that is not vendored: pass
`-DFORGE_SOURCE_DIR=PATH`, or leave it empty and CMake fetches odin-loki/FORGE
`main`.

```
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DPRISM_CUDA=OFF -DPRISM_Z3=ON
cmake --build build            # the first build compiles Z3 and is slow
./build/prism_tests            # unit tests
./build/prism --list-stages
```

Options: `-DPRISM_CUDA=ON`
builds the GPU mutator. FORGE is always linked (`-DFORGE_SOURCE_DIR=PATH` uses a
local checkout; empty fetches odin-loki/FORGE `main`); it is also the LLM engine.
Add `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`


### Optional external tools

PRISM calls these as external programs when they are on `PATH` (or named
with `--tool NAME=PATH`). A tool that is not installed is reported as
`NOTRUN` with an install hint; it is never counted as a clean result
(Law 1).

| kind | tools |
|---|---|
| C/C++ analyzers | cppcheck, clang-tidy, ESBMC, CBMC, Infer, CodeQL, Semgrep, Coccinelle (`spatch`, tree-local `*.cocci` in the scanned tree only), Frama-C, KLEE, Strix |
| fuzzers | AFL++ (`PRISM_AFL=1`), libFuzzer (`PRISM_LIBFUZZER=1`) |
| other languages | ruff or pyflakes, mypy, node, tsc, eslint, bash, shellcheck, gofmt, cargo clippy, ruby, php, perl, luac, yamllint |
| sandbox | bubblewrap (`bwrap`) for `--allow-exec` |
| LLM | a FORGE `.fpk` model (`PRISM_FPK`, default `~/.prism/models/qwen3.5-9b.fpk`; `PRISM_DEVICE` picks cpu/cuda/vulkan) |

## 2. Run

```
./build/prism path/to/code --no-llm            # whole pipeline, no LLM
./build/prism src/foo.c --stage inventory,classify,bmc,harness
./build/prism path/to/code --skip fuzz,diff --jobs 8
./build/prism path/to/code --resume            # continue an interrupted run
./build/prism --list-stages
```

| option | meaning |
|---|---|
| `PATH` | a file or directory (default `testdata`) |
| `--out DIR` | report directory (default `prism-out/`) |
| `--stage a,b` / `--skip a,b` | run only / leave out these stages. Left-out stages are listed as `skipped` in the report, never silently dropped |
| `--no-llm` | do not run the LLM stage |
| `--preset prove` | the proving path only: `--stage inventory,classify,pir --no-llm` (adds to other `--stage` names). pir checks every function as its own task on `--jobs` threads and answers unchanged functions from the verdict cache ([PIR.md](PIR.md) "Parallel checking", "Function verdict cache") |
| `--unwind K` | loop unwinding bound for BMC (incremental: 1, 2, 4, … K) |
| `--jobs N`, `-j N` | worker count for lints (0 = half the CPUs) |
| `--fuzz-budget S`, `--fuzz-iters N` | fuzzing time / iteration budget |
| `--repair-rounds N` | budget for the repair stage |
| `--tool NAME=PATH` | use this binary for an adapter (searched before `~/.prism/tools/<name>/<commit>/bin` and `PATH`) |
| `--allow-exec` | allow steps that run code from the scanned tree (section 6) |
| `--certified` | the `pir` stage asks for a checked certificate of every verification condition; a function whose VCs are all certified is `PROVED-CERTIFIED` (section 9) |
| `--compile-commands FILE` | the pir stage compiles each unit with its entry in this `compile_commands.json` (GNU C level, defines, include path, ABI flags; warning flags and flags that define UB are never taken). A Linux kernel unit needs it; see [PIR.md](PIR.md) "GNU C units and a unit's own flags" |
| `--kernel-api linux\|freebsd` | the pir stage links the kernel API specs of that kernel and models inline asm, asm goto, atomics and barriers conservatively; every one is a named assumption (PROVED-ASSUMING). Off by default; see [KERNEL_MODELS.md](KERNEL_MODELS.md) |
| `--assume-unknown-calls` | a call without a spec havocs its result and the objects its pointer arguments point to (assumption listed). Opt-in; the default is `UNENCODED: call @f` |
| `--solver-cache DIR` | solver query cache and solve-time history (default `$XDG_CACHE_HOME/prism/solver`, else `~/.cache/prism/solver`) |
| `--timeout S` | solver seconds per query (default 30) |
| `--complexity N` | Z3 resource-limit (rlimit) units per pir query, so "not decided" is the same on every machine and core count; the wall clock stays as a larger backstop; 0 turns the limit off ([PIR.md](PIR.md) "Complexity limits") |
| `--fail-on never\|defect\|gap` | exit-code policy for CI (section 5) |
| `--resume` | reuse stages already `ok`/`NOTRUN` in `--out/stages.jsonl` (`report.json` as fallback) |
| `--pbsd PATH` | ParanoidBSD tree for the `pbsd` stage (else `PRISM_PBSD`; no default). Importing its modules also needs `--allow-exec` |
| `--requirements PATH` | C++ engine only. Markdown/text requirement documents (a file or a directory; repeatable); the `review` stage drafts contracts traced to their sentences ([AI.md](AI.md)) |
| `--strict-aliasing` | C++ engine only. The `pir` stage also checks effective types / strict aliasing (C11 6.5p6-7) per byte; opt-in because real code often breaks the rule on purpose ([PIR.md](PIR.md)) |
| `--pir-drafts` | C++ engine only. Let `pir` size pointer parameters from the template harness draft when no `// requires:` gives them; a violation found only under a drafted size stays `NEEDS-HARNESS` (Law 6) |
| `--pir-harness` | C++ engine only. `pir` binds a pointer parameter that no `// requires:` clause covers to an object built from the IR types (struct layout, nested pointer fields to `--pir-harness-depth` levels); every choice is a named assumption, so the best verdict is `PROVED-ASSUMING` and a violation found only under it is `NEEDS-HARNESS` ([PIR.md](PIR.md) "Automatic pointer harness") |
| `--pir-harness-depth N`, `--pir-harness-elems N`, `--pir-harness-bytes N` | bounds of `--pir-harness`: nested pointer levels (default 2), default element count of an array-like pointee (16), size of an object whose pointee type is never accessed (64 bytes) |
| `--pir-discharge` | C++ engine only. With `--pir-harness`, the harness of a static, non-address-taken, non-recursive function is a proof obligation checked at every call site of the unit; discharged, the invented assumption is replaced by the call sites ([HARNESS_GENERATOR.md](HARNESS_GENERATOR.md)) |
| `--pir-container-of` | C++ engine only. With `--pir-harness`, a parameter used at a negative constant offset gets its containing object (`container_of`) |
| `--pir-vacuity` | C++ engine only. A harnessed function whose exit is unreachable under its assumptions is `NEEDS-HARNESS` (vacuous), never a proof |
| `--ops-table FILE` | C++ engine only. An indirect call through a field of a known ops table (TSV: struct type, field, targets, role, optional field index) is checked against each target, at most 8 (more stays `UNENCODED`). Off by default ([PIR.md](PIR.md)) |
| `--callee-contracts FILE` | C++ engine only. Assume-guarantee stubs for callees without a body (TSV: function, signature, requires, assigns, ensures, status, source). The requires are checked at the call site, only the assigns are havocked, the ensures are assumed; the status of each contract used is named in the verdict, which is never `PROVED`. A callee without a usable contract is handled as before ([HARNESS_PHASE2.md](HARNESS_PHASE2.md)) |
| `--pir-ops-auto` | C++ engine only. Ops tables derived from the unit's own function-pointer stores and initialisers; a harness function-pointer field is bound to one of its targets (a named assumption). Off by default |
| `--pir-addr-values` | C++ engine only. Model constant `ptrtoint` / `inttoptr` expressions and integer-address initialisers (named assumptions); a violation that rests on the address model is `NEEDS-HARNESS`, never `FAILED` |
| `--pir-harness-deep` | C++ engine only. With `--pir-harness`: depth 4 and 64 objects per parameter; the depth bound applied and what it cut are recorded in the assumptions and the `--jsonl` record |
| `--pir-build-semantics` | C++ engine only. With `--compile-commands`: a unit whose compile command has `-fwrapv` or `-fno-strict-overflow` (the Linux and FreeBSD kernels) is lowered with `-fwrapv` and checked with that semantics: signed `+ - *` overflow and signed `<<` into or past the sign bit (or of a negative value) wrap and are not reported; division (`INT_MIN / -1`, by zero), shift amounts, pointers and memory are checked as before. Named in the verdict (`front end flags: ... taken (--pir-build-semantics): -fwrapv`). Without the flag those options are not taken (ISO C) |
| `--pir-kernel-stops` | C++ engine only. With `--kernel-api`: Linux `BUG()` (the `ud2` / `__bug_table` asm followed by `unreachable`, which `BUG_ON` expands to) and calls of `panic`, `vpanic` and `kassert_panic` (FreeBSD `KASSERT`) end the path, like `exit`. The verdict holds only for executions in which none of them triggers, and says so: `kernel-stop: assuming the BUG() / BUG_ON() condition never triggers (line N) ...` (one per site); without the flag a reachable `panic()` is a `FUNC-CONTRACT` finding and `BUG()` reaches `unreachable` (`CXX-UNREACHABLE`) |
| `--pir-kernel-current` | C++ engine only. With `--kernel-api`: the running thread exists. Linux `current` (an inline asm `%gs` read whose operand is `current_task`, `const_current_task` or `pcpu_hot`) and FreeBSD `curthread` (the `%gs:0` pointer read of `__curthread`) are a valid object of 1 MiB arbitrary bytes, never NULL; a named assumption. Without the flag the asm output is an arbitrary value (NULL included) |
| `--pir-global-harness` | C++ engine only. With `--kernel-api`: a mutable global of pointer type (an MMIO base, a bus handle or a driver singleton that the unit's init / probe code sets) points at entry to a valid object of 1 MiB arbitrary bytes, never NULL; one named assumption per global (`global harness: @g ...`). Only scalar pointer globals, never in the initial-values recheck. Without the flag such a global's entry value is arbitrary (NULL included) |
| `--pir-lock-wrappers` | C++ engine only. With `--kernel-api`: a function whose call tree acquires locks and releases none (`rtnl_lock`, `ssb_buses_lock`) returns with them held by design, and one that releases and acquires none (`rtnl_unlock`) releases locks its caller holds: `LOCK-HELD-ON-RETURN` (respectively `LOCK-UNHELD-UNLOCK`) is not checked for it, as a named assumption (`lock-wrapper: ...`). A function that does both, or has a `--lock-contracts` row, is checked as before |
| `--pir-icall-havoc` | C++ engine only. With `--kernel-api`: an indirect call with more than 16 candidate targets, or the path of one whose target is outside the unit's address-taken functions, is one execution of an arbitrary function: every object's contents become arbitrary, the result is arbitrary, and it is assumed to return and to free nothing (named `icall-havoc: ...`). Without the flag those calls are UNENCODED |
| `--pir-field-invariants FILE` | C++ engine only. Value invariants of struct fields, applied to every automatic-harness object of that struct: TSV `struct offset width kind a b status source [scope]`, kind `valid` (a pointer field points to a fresh object of `a` bytes), `nonnull`, or `range` (signed `a <= v <= b`). A field that already has its own harness object is left alone. The optional `scope` (`fn:GLOB`, `unit:GLOB`, `module:GLOB` over the unit's path; empty = global) limits where a row applies, and the assumption names it. A proof keeps only the rows it needs (each used row is dropped and the function re-checked). Each row used is a named assumption (`field-invariant: ...` with its status and source); nothing here is checked. `PRISM_PIR_MINE_FIELDS=FILE` writes the store evidence they are derived from |
| `--pir-callback-models FILE` | C++ engine only. Driver environment models: preconditions the kernel core guarantees for the parameters of driver callbacks (a probe's id is an entry of the driver's own id table, a `write` count is at most `MAX_RW_COUNT`, a driver's `net_device` has its private area, a sysfs `show` buffer is `PAGE_SIZE`). TSV `interface container field_off callback param kind a b status source`; a row applies only to a function the unit registers at that container field or registration call. Each use is a named assumption (`driver environment model: ...`); a pointer bound by a model is an invented object, so a violation under it is NEEDS-HARNESS ([DRIVER_ENV_MODELS.md](DRIVER_ENV_MODELS.md)) |
| `--pir-builtin-contracts` | C++ engine only. With `--kernel-api`: contracts for the body-less intrinsics `llvm.read_register`, `llvm.prefetch` and `llvm.objectsize` (named assumptions) |
| `--lock-contracts FILE` | C++ engine only. TSV of lock contracts for unmodified kernel code (function, lock `global:NAME\|arg:N`, `acquires\|releases\|requires-held`, optional `mutex\|spin`); needs `--kernel-api` |
| `--jsonl FILE` | C++ engine only. `pir` appends one JSON line per function: key (`unit:start-end`), engine, exact verdict, proof level L0-L3, the full assumption list, time, peak memory, Lean-fragment coverage; counts in `FILE.summary.json` ([PIR.md](PIR.md) "Machine-readable output") |
| `--fp-checks` | C++ engine only. `pir` also reports IEEE floating-point division by zero, invalid operations (NaN from non-NaN operands) and overflow to infinity (FLOAT-DIV-ZERO / FLOAT-INVALID / FLOAT-OVERFLOW); off by default because Annex F defines these results. An out-of-range float-to-integer conversion (FLOAT-CAST-OVF) is always checked |
| `--contracts-approved PATH` | C++ engine only. Approvals of drafted contracts (default `<root>/contracts.approved.json`); only approved clauses give `PROVED-ASSUMING` |
| `--list-stages` | print the stage order and exit |
| `--version`, `-V` | print the PRISM version and engine, then exit |
| `--help`, `-h` | print the option summary and exit |

The stages run in a fixed order (`--list-stages`): inventory, classify,
lints, taint, thread, interval, warnings, cppcheck, pbsd, sanitize, optional,
polyglot, esbmc, dafny, contracts, wp, pir, conc, harness, review,
concolic, fuzz, diff, rapid, muttest, ltl, llm, execute, repair, unify. What
each one does is in [PLAN.md](PLAN.md#pipeline-order-is-the-method). `pir` (the
Clang/LLVM front end, [PIR.md](PIR.md)), `conc` (threads,
[CONCURRENCY.md](CONCURRENCY.md)) and `review` ([AI.md](AI.md)) run in
the C++ engine only; the frozen Python engine lists them and records one
`NOTRUN` row each (roadmap D8).

In the C++ engine the `lints` stage also has a Clang-AST layer (roadmap 2.8,
`src/prism/astlint*.cpp`). Each C/C++ translation unit is parsed in process
through libclang's C API (flags from `compile_commands.json` in the root or
`build/` when present). The library is loaded at run time, so building needs
only `<clang-c/Index.h>` (`apt install libclang-18-dev`; CMake option
`PRISM_LIBCLANG`, on by default when the header is found). Without libclang
the layer runs `clang -fsyntax-only -Xclang -ast-dump=json` per unit instead;
both front ends give the same rows (`tests/cpp/test_astlint.cpp` runs every
case on both), and each row says which one ran in `extra.ast_backend`.

What it checks: the unit's own code, the scanned project headers it includes
(each header row reported once, however many units include it; system headers
are not checked), members and enums nested in classes and namespaces, and the
non-dependent code of templates. Code that depends on a template parameter is
not checked. Checks: assignment as a condition, `sizeof` of a pointer or array
parameter, signed/unsigned comparisons (`u < 0` included), enum switch holes,
self-assignment and self-move, dead stores, `memset` argument swaps, integer
division converted to floating point, shift and division UB, integer
truncation, `printf`/`scanf` format strings (argument count, type and LP64
width, `%n`, non-literal format), `strncpy` without terminator, fixed-size
`strcpy`/`strcat` overflow, overlapping copies, `p = realloc(p, ...)`,
unannotated switch fallthrough, shadowed locals, returning the address of a
local or `c_str()` of a local string, and, in straight-line code,
use-after-free, double free, mismatched `free`/`delete`/`delete[]`, NULL
dereference (inside the branch that tested for NULL, and after an `if` whose
NULL branch falls through), reads of uninitialised locals and use after
`std::move`. For C++ it also checks ignored `[[nodiscard]]` results, virtual
calls in constructors and destructors, `delete` through a base class that has
no virtual destructor, `std::move` of a `const` object, dangling references,
catch by value, throwing from destructors or `noexcept` functions,
`throw new` and `delete this`. It also covers the regex lints' whole
"`<fn>()` return is discarded" family (466 library functions,
`prism/astlint_discard.inc`, generated at build time by
`src/tools/gen_astlint_discard.cpp` from the regex rules).

Its rows carry `extra.engine = "clang-ast"`; one that replaces a regex lint
row on the same line and class also carries `extra.supersedes = "regex"`.
If neither libclang nor clang is available, or a unit does not parse, the
layer writes a `NOTRUN` row with `extra.layer = "clang-ast"` and the regex
lints still run for that file. Such a row does not make the `lints` stage
itself `NOTRUN`. A scanned header that no parsed unit includes is counted in
one `NOTRUN` row and gets regex lints only. The Python engine runs the regex
lints and records one `NOTRUN` row for the AST layer (roadmap D8).

### Report subcommands (C++ engine only)

These read a finished `report.json` and never change a verdict ([AI.md](AI.md)).

| command | options |
|---|---|
| `prism regress` | `--report OUT/report.json` (default `prism-out/report.json`); `--write-tests DIR` writes the generated regression tests into DIR instead of `<out>/regression_tests/`; `--framework auto\|gtest\|catch2\|ctest\|pytest\|plain`; `--run` compiles and runs them (needs `--allow-exec`, Law 9) |
| `prism ask "<question>"` | `--report ...`; `--json` prints the structured query and answer as JSON; `--no-llm` uses only the deterministic grammar |
| `prism draft` | `--report ...`; `--kind report\|assurance`; `--proofs DIR` where Lean theorems are looked up. Every claim links to a finding, verdict, certificate or theorem |
| `prism triage [OUT]` | `--threshold T` cosine threshold for clustering (default 0.7); `--no-embed` skips the optional embedding model |

### `prism prove` (C++ engine only)

`prism prove FILE.lean THEOREM` searches for a Lean proof of one theorem
(roadmap 9.2): a prover model proposes tactics, the Lean kernel checks each
one, and the axioms must stay within `propext`, `Classical.choice` and
`Quot.sound`. Details are in [AI.md](AI.md). Elaborating Lean runs code, so
it needs `--allow-exec` (Law 9); without it the result is `NOTRUN`. Exit
code: 0 proved, 1 not proved, 3 `NOTRUN`, 2 error (with `--all`: 0 only
when every theorem is proved, 3 only when every one is `NOTRUN`).

| option | meaning |
|---|---|
| `FILE.lean THEOREM` | the Lean file and the theorem to prove |
| `--all [ROOTS...]` | try every theorem with a `sorry` under ROOTS (default `proofs`, `proofs/semantics`, `proofs/techniques`; `.lake` is skipped) and write `<out>/prove/summary.json`; with `--list`, only list them |
| `--list` | print the theorems in `FILE.lean` that still contain a `sorry` |
| `--write` | write the proof back into the file and add it to the lemma library (default `proofs/lemmas.jsonl`) |
| `--allow-exec` | allow Lean elaboration (required) |
| `--project DIR` | the Lake project to elaborate in (default: the nearest parent directory with a lakefile) |
| `--lemmas PATH` | lemma library to read and extend (default `<project>/lemmas.jsonl` under `proofs/`, else `proofs/lemmas.jsonl`) |
| `--budget N`, `--beam N` | model calls allowed (default 24) and partial proofs kept (default 4) |
| `--timeout S` | Lean time limit per check in seconds (default 120, minimum 5) |
| `--samples N` | model replies per expanded node, checked together in one batch (default 1) |
| `--lean-jobs N` | Lean processes a batch of candidates is split over (default: half the cores) |
| `--no-closers` | do not try the core closers (`omega`, `simp`, `grind`, `decide`, ...) before the model; model only |
| `--out DIR` | output directory (default `prism-out`) |
| `--prover-fpk PATH`, `--prover-model NAME` | the FORGE prover model (`.fpk`, else `PRISM_PROVER_FPK`) and the model name recorded in the audit (else `PRISM_PROVER_MODEL`) |
| `--json` | print only the JSON result |
| `--help`, `-h` | print the option summary and exit |

### `prism svcomp` (C++ engine only)

`prism svcomp --prop PROPERTY.prp TASK.c` runs PRISM as an SV-COMP verifier
on one task and prints `PRISM-SVCOMP-RESULT: true | false(<property>) |
unknown | error` with a witness (format 2.0). `true` needs a proof of `main`
that covers the property; `false` needs a counterexample that replays, which
runs the task, so without `--allow-exec` (Law 9) every refutation is
`unknown`. Options: `--data-model ILP32|LP64`, `--out DIR`, `--witness FILE`.
`prism svcomp score` scores the pinned subset and `prism svcomp pack` builds
the tool archive (`--help` for each). Details are in [SVCOMP.md](SVCOMP.md).


## 3. Read the report

Everything lands in `--out` (default `prism-out/`):

| file | contents |
|---|---|
| `report.md` | the human report: confidence, stage table, findings |
| `report.json` | the full machine report (`stages[].findings[]`) |
| `report.sarif` | SARIF 2.1.0 (section 4) |
| `stages.jsonl` | one line per stage as it finishes; `--resume` reads it |
| `functions.json` | the inventory: every function and its class |
| `taxonomy.json` | which defect classes were COVERED / PARTIAL / GAP |
| `TRUSTED_BASE.md` | what a proof in this report depends on (a copy of [TRUSTED_BASE.md](TRUSTED_BASE.md); SARIF names it with its SHA-256) |
| `VERDICTS.md` | the verdict definitions; every finding in `report.md` links its verdict here |

### Findings

Each finding carries a `stage`, a `status` (the verdict), the `file`,
`function` and `line`, a defect class `cls` (for example `INT-SIGNED-OVF`,
`INT-DIV-ZERO`, `INT-SHIFT-UB`, `MEM-OOB-READ`), a `message`, a `strength`
(how much the silence of that stage is worth: `PROVES`, `FINDS`, `SOME`,
`READS`) and, for refutations, a `counterexample`.

### Verdicts

The status vocabulary is closed (`include/prism/laws.hpp`);
[VERDICTS.md](VERDICTS.md) defines each one and the laws that relate them.
In short:

| verdict | reads as |
|---|---|
| [`PROVED-CERTIFIED`](VERDICTS.md#verdict-proved-certified) | proved, and the solver's answer was checked by a verified proof checker |
| [`PROVED-UNBOUNDED`](VERDICTS.md#verdict-proved-unbounded) | the encoded properties hold for every input and every number of loop iterations |
| [`PROVED`](VERDICTS.md#verdict-proved) | the encoded properties hold; loops closed within the bound |
| [`PROVED-ASSUMING`](VERDICTS.md#verdict-proved-assuming) | proved under an explicit precondition (`requires`) you wrote |
| [`BOUNDED`](VERDICTS.md#verdict-bounded) | nothing found within the unwind bound. **Not a proof** (Law 2) |
| [`FAILED`](VERDICTS.md#verdict-failed) | a counterexample exists; see `counterexample` |
| [`NEEDS-HARNESS`](VERDICTS.md#verdict-needs-harness) | PRISM will not guess preconditions (pointer parameters, Law 6) or cannot model a construct yet |
| [`UNKNOWN`](VERDICTS.md#verdict-unknown), [`TIMEOUT`](VERDICTS.md#verdict-timeout), [`ERROR`](VERDICTS.md#verdict-error) | the tool ran and did not conclude |
| [`NOTRUN`](VERDICTS.md#verdict-notrun) | the tool or step did not run (missing tool, or `--allow-exec` not given). Never a clean result (Law 1) |
| [`CRASH`](VERDICTS.md#verdict-crash), [`SANFAIL`](VERDICTS.md#verdict-sanfail) | a fuzzer or sanitizer observed the defect |
| [`CLEAN`](VERDICTS.md#verdict-clean) | a fuzzer found nothing. **Not a proof** (Law 3) |
| [`HYPOTHESIS`](VERDICTS.md#verdict-hypothesis), [`READS`](VERDICTS.md#verdict-reads) | LLM output: a lead to check, never evidence (Law 4) |

Two things to keep in mind:

- A proof covers the *encoded* properties (signed overflow, division by zero,
  shift range, array bounds, the `assert`s in the code) of the function as
  PRISM's front end understood it. [CONFORMANCE.md](CONFORMANCE.md) lists
  the constructs where the current engine is known to prove something false;
  until that list is empty, treat a proof of code that uses them with care.
- Pointer-parameter functions are never model-checked unguarded (Law 6).
  Write preconditions (one `// requires:` per line, at the top of the body)
  to get a verdict; a proof under them is `PROVED-ASSUMING`, never `PROVED`
  (see `testdata/planted/ptr_copy.c`):

  ```c
  void copy(char *dst, char *src, int n)
  {
      // requires: n >= 0
      // requires: n < 8
      for (int i = 0; i < n; i++) dst[i] = src[i];
  }
  ```

### Confidence

`report.md` opens with `confidence = visibility × answer × resolution`
(Law 5): how much of the code PRISM could see, how much of it got an answer,
and how much of the answer is resolved. No data scores 0; 0 means "no data",
not "clean".

## 4. SARIF

`report.sarif` is SARIF 2.1.0. Only defects (`FAILED`, `CRASH`, `SANFAIL`)
become `results`; `HYPOTHESIS` becomes a `note`; stages that did not run or
failed become tool execution notifications, so a viewer shows what was not
checked. Load it into GitHub code scanning, VS Code (SARIF Viewer) or any
SARIF dashboard.

## 5. Exit codes and `--fail-on`

| `--fail-on` | exit 1 when |
|---|---|
| `never` (default) | never; the report is the output |
| `defect` | any `FAILED`, `CRASH` or `SANFAIL` finding |
| `gap` | a defect, or anything `NOTRUN`, `ERROR` or `TIMEOUT` (nothing was left unchecked) |

A stage that crashed is exit 2 regardless of policy.

## 6. Untrusted code and `--allow-exec`

PRISM never runs the code it checks unless you pass `--allow-exec` (Law 9).
Without it, sanitizer runs, compiled fuzz/diff harnesses, AFL++/libFuzzer,
KLEE, `perl -c`, `cargo clippy`, `eslint`, Coccinelle script rules and
LLM-written programs are `NOTRUN` with the hint `re-run with --allow-exec
(only on code you trust)`. Parsing, lints, BMC, the concrete interpreter and
compile-only diagnostics still run.

With `--allow-exec`, built binaries run in a bubblewrap jail when `bwrap` is
installed (read-only root, private `/tmp`, no network) and always under
rlimits; each finding records `extra.sandbox`. The sanitize stage only calls
zero-argument functions you mark with `// prism: run`. The full per-stage
table is in [PLAN.md](PLAN.md#running-on-untrusted-code-law-9).

## 7. Other languages (polyglot)

The `polyglot` stage checks every non-C file in the tree with the standard
tool for its language: Python (`compile()`, ruff or pyflakes, mypy), JSON,
TOML, JavaScript (`node --check`, eslint), TypeScript (tsc), shell (`bash
-n`, shellcheck), Go (gofmt), Rust (cargo clippy), Ruby, PHP, Perl, Lua and
YAML (yamllint). Every text file is also scanned for merge-conflict markers
and leaked credentials. A linter that is not installed is `NOTRUN`; a linter
that ran and said nothing is `UNKNOWN`, not clean; syntax-broken files are
kept away from type checkers.

```
./build/prism . --no-llm --stage inventory,lints,polyglot
```

## 8. CI integration

A GitHub Actions job that gates on defects and uploads SARIF to code scanning:

```yaml
jobs:
  prism:
    runs-on: ubuntu-24.04
    permissions:
      contents: read
      security-events: write
    steps:
      - uses: actions/checkout@v4
      - name: Build PRISM
        run: |
          sudo apt-get update && sudo apt-get install -y ninja-build clang
          git clone --depth 1 <your PRISM mirror> prism-src
          cmake -S prism-src -B prism-build -G Ninja -DCMAKE_C_COMPILER=clang \
            -DCMAKE_CXX_COMPILER=clang++ -DPRISM_CUDA=OFF \
            -DPRISM_Z3=ON
          cmake --build prism-build --target prism
      - name: Check
        run: prism-build/prism . --no-llm --fail-on defect --out prism-out
      - uses: github/codeql-action/upload-sarif@v3
        if: always()
        with:
          sarif_file: prism-out/report.sarif
      - uses: actions/upload-artifact@v4
        if: always()
        with:
          name: prism-report
          path: prism-out/
```

Notes:

- Use `--fail-on gap` when "not checked" must fail the build too.
- Leave `--allow-exec` off in CI that runs on pull requests from forks.
- Cache `prism-build/` (or use ccache): the first build compiles Z3.
- This repository's own CI is `.github/workflows/ci.yml` (build, both test
  suites, SARIF self-check) and `.github/workflows/conformance.yml` (the
  release gate below).

## 9. How much to trust a proof

PRISM's release gate is the conformance suite (roadmap 2.7, 6.1): a set of
C/C++ tasks with known answers, a pinned subset of SV-COMP and, nightly, the
NIST Juliet CWE190/191/369/476/680 tests. Any wrong proof blocks a release.

```
build/prism-qa conformance                  # metrics + gate (engine: --prism, $PRISM_BIN, else build/prism)
build/prism-qa conformance --self-check     # validate the task labels
build/prism-qa soundness -n 300             # random programs
```

The metric definitions, the current numbers and every known wrong proof with
a reproducer are in [CONFORMANCE.md](CONFORMANCE.md).

### Certified mode

A plain `PROVED` trusts the solver that answered. With `--certified`, the
`pir` stage (Clang → LLVM IR → PIR, [PIR.md](PIR.md)) asks for more: every
verification condition of a function (each inserted property, including the
memory-safety properties of the QF_BV memory model, and, for a loop, the
unwinding assertion) is bit-blasted to CNF, CaDiCaL writes an LRAT proof that
the CNF is unsatisfiable, and the formally verified checker `cake_lpr` must
accept it. When the Lean tools are built (`lake build` in
`proofs/techniques`), the CNF comes from the Lean-proved bit-blaster
(`prism-bitblast`) and Lean's verified LRAT checker (`prism-lrat-check`) must
accept the proof as well; otherwise Z3's tactics make the CNF and
`certificate_info` says so. Only when every VC of the function passes is the
function `PROVED-CERTIFIED`, with `extra.certificate = "checked"`,
`extra.certificate_info`, `extra.certificate_bitblast` (how many VCs the
Lean-proved bit-blaster made) and `extra.cnf_sha256` (one hash per VC).

```
build/prism-deps tool cadical cake_lpr   # once
(cd proofs/techniques && lake build)                          # optional: Lean bit-blaster
./build/prism src/ --stage inventory,classify,pir --certified
```

Anything short of that stays `PROVED` and `extra.certify_note` says why (a
checker or CaDiCaL not installed is named as `NOTRUN` there). A function with
no verification condition at all stays `PROVED` with
`certify_note = "no verification conditions (nothing to certify)"`: a
certificate that checks nothing is not labelled certified. `BOUNDED` and
`PROVED-UNBOUNDED` are never certified, and a certified proof under
`// requires:` assumptions is `PROVED-ASSUMING`. A certificate is about the
CNF: what it still trusts (Clang, the PIR encoder and memory model, and the
bit-blaster when it is Z3's) is listed in the `TRUSTED_BASE.md` that every
run writes next to the report. `build/prism-qa conformance --certified`
measures how many loop-free functions of the suite become `PROVED-CERTIFIED`.

### Batch runs over a compile database

`scripts/pir_batch.sh --db compile_commands.json --out DIR` runs the `pir`
stage on every unit of a compile database, one `prism` process per unit, with
`--pir-harness --jsonl`. Results are append-only JSON-line shards
(`DIR/shards/shard-NN.jsonl`), finished units are listed in `DIR/manifest.tsv`
(a stopped run resumes by skipping them), and `--summary` prints counts. The
memory budget per worker is `PRISM_MEM_BUDGET_MB`, derived by default from the
peak RSS measured on the first units, and the worker count is
`floor((MemAvailable - 10 GB) / budget)`; never use `ulimit -v` for PRISM.
Details: [PIR.md](PIR.md) "Batch mode".
