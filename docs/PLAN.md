# PRISM — unified hybrid code-testing pipeline

A single tool that runs **deterministic instruments first**, then **bounded
proofs**, then **fuzzing**, then **LLM-guided generation and repair**. The
model is Qwen 3.5 9B. Inference is **llama.cpp** (CUDA). The GUI is **Qt**.
Hot numeric paths use **xsimd**. Mutation and coverage hashing use **CUDA**.

ParanoidBSD's verify tree is not a plugin we wrap and forget. It is the
source of the *laws* this pipeline is not allowed to break.

## Laws (inherited, not optional)

1. A missing tool is `NOTRUN`. It is never a clean result.
2. `PROVED` and `BOUNDED` are never merged. K-induction that closes is
   `PROVED-UNBOUNDED` and is never folded down into a bounded proof.
3. A fuzzer that finds nothing is `CLEAN`, which is **not a proof**.
4. LLM output is `HYPOTHESIS` / `READS`. It cannot make a defect class
   COVERED. Silence of the model is worth nothing.
5. Confidence is the product `visibility × answer × resolution`. A scope
   with no data scores 0, not n/a.
6. Pointer-parameter functions are not model-checked unguarded. That
   reports the absence of a precondition, not a defect.
7. A stage that cannot run writes that down. Nothing is skipped quietly.
8. Check polarity: never pass a flag that silently disables a check.
9. Executing code from the scanned tree (or written by the LLM) requires
   `--allow-exec`. Without it every such step is `NOTRUN` with the
   `--allow-exec` hint — never CLEAN. With it, built binaries run in a
   sandbox. See "Running on untrusted code" below.

## What we mine from each project (algorithms, not wrappers)

| Source | What we take | What we do not take |
|---|---|---|
| **ESBMC** | Incremental BMC, k-induction, SMT encoding of UB properties, inverted check polarity vs CBMC, concurrency as a *different question* | The Clang/GOTO binary itself (adapter when `esbmc` is on PATH) |
| **FuSeBMC** | BMC counterexamples → fuzzer seeds; fuzzer coverage → new BMC goals; scalar-only harness honesty | The SV-COMP driver binary |
| **Strix** | LTL → deterministic safety automaton; reactive synthesis for protocol-shaped code | Full parity-game synthesis (optional adapter) |
| **Fuzz4All** | Autoprompt distillation; LLM as universal generator; mutate interesting inputs | The original model weights |
| **ChatFuzz** | Greybox loop; when coverage stalls, LLM emits *semantically valid* mutants | ChatGPT API |
| **Dafny** | `requires`/`ensures`/`invariant`/`decreases`; VC generation; specs as first-class artefacts | The Dafny compiler (adapter when present) |
| **RLEF** | Execution feedback as the reward; keep the best candidate; iterate under a budget | Training a new policy |
| **OpenCodeInterpreter** | Generate → sandbox execute → feed stdout/stderr/exit back → refine | The original notebook UI |
| **ParanoidBSD** | Status vocabulary, classify SCALAR/POINTER, pattern lints, taxonomy, confidence product, sweep orchestration | HardeningBSD-specific include shims (used when scanning that tree) |

## Stack

| Layer | Choice | Why |
|---|---|---|
| Model | Qwen 3.5 9B (`qwen3.5:9b`, Q4_K_M) already on Ollama | 9.7B, 256k context, thinking + tools, 24 GB 3090 |
| Inference lib | llama.cpp with GGML CUDA | User requirement; Ollama is the *current GGUF host*, not the API we design around |
| GUI | Qt 6 (C++ target + PySide6 so the GUI runs today without MSVC) | User requirement |
| SIMD | xsimd | Portable AVX2/AVX-512 on the 3090's host CPU |
| GPU | CUDA 13.2 | Mutation havoc + coverage hashing; llama.cpp CUDA for the model |
| SMT | Z3 | Same backend family ESBMC/Dafny already speak |
| Compile/fuzz host | gcc (present) / clang (adapter) / MSVC (when present) | We compile harnesses. We do not pretend to. |

Ollama stays as a **fallback backend** that already has the GGUF on the 3090.
The native engine loads that same blob through llama.cpp. A clean result from
Ollama and a clean result from llama.cpp are the same model, different
loaders — the *finding* vocabulary does not care which loader answered.

## Pipeline (order is the method)

```
0  inventory     translation units, functions, loc
1  classify      SCALAR | POINTER | VOID | OTHER   (soundness split)
2  lints         ParanoidBSD-shape checkers (lock, mask, null-branch, UAF, format)
3  taint/thread  CodeQL-shaped sinks; unsynchronized globals
3b interval      path-sensitive integer ranges (FAILED is not a proof)
4  adapters      cppcheck / clang / esbmc / dafny / cbmc / semgrep / infer / frama-c if on PATH
4b polyglot      every non-C language: syntax (python/json/toml helper, node, bash, gofmt,
                 ruby, php, perl, luac), linters (ruff|pyflakes, eslint, shellcheck,
                 clippy, yamllint), types (mypy, tsc); built-in conflict-marker and
                 leaked-credential scan over every text file. Missing tool is NOTRUN;
                 silent tool is UNKNOWN; syntax-broken files are kept from type checkers
5  contracts     Dafny-style + ACSL specs (PROVED-ASSUMING under requires)
5b wp            in-tree Frama-C WP after contracts (`prism/wp.py`); closed is PROVED-ASSUMING — see docs/MINED.md
6  bmc           k-induction + incremental BMC via Z3 (ESBMC method);
                 C subset includes do-while, continue, sizeof, ternary,
                 comma; unsigned params use unsigned compares and wrap;
                 `long long` / `int64_t` are 64-bit bitvectors;
                 nested loops are unwound (k-induction step tries k=1 then
                 k=2; nested step stays unencoded);
                 VLA, OTHER, IEEE float, recursive self-calls,
                 libc string copies, alloca, C++ throw, asm, _Generic,
                 GNU statement expressions, try/catch, offsetof,
                 volatile/_Atomic, pthread_mutex_t/mtx_t,
                 const/struct/unknown-typedef locals,
                 static/extern/__auto_type locals, anonymous enum objects,
                 _Alignas, compound literals, memcpy/mkstemp/tmpnam/chroot,
                 char t[] = "…", call-site address-of,
                 _Thread_local, _Complex, typeof, GNU nested functions,
                 computed goto (plain goto stays ERROR), designated
                 initializers, alignof, va_arg, C++ range-for/lambda/
                 const_cast, umask/srand/signal/mktemp,
                 dynamic_cast/typeid/reinterpret_cast, packed, coroutines,
                 GNU &&label, wide L"…", fork/exec/mmap/ioctl/wcscpy,
                 __int128, GNU case-range, bit_cast, if constexpr,
                 std::thread/optional/variant/span, dlopen/accept/chmod,
                 __builtin_clz, C23 nullptr, std::launder, fold
                 expressions, _Decimal, catch (...)/throw new/vector<,
                 restrict, _Float16, start_lifetime_as, C++ requires,
                 __builtin_choose_expr, typeof_unqual, setuid/socket/bind/
                 unlink/mkfifo, listen/connect/pipe/dup/fcntl/wait,
                 select/send/shutdown/kill/getaddrinfo,
                 pthread_join/sem_wait/openat/flock/chown/symlink,
                 opendir/setrlimit/getsockopt/stat/mkdir/getpwuid,
                 clock_gettime/shm_open/posix_spawn/glob/fseek/sleep/access,
                 getopt/uname/sendfile/memfd_create/prctl/tcgetattr,
                 sysconf/getrusage/nftw/wordexp/getlogin/inet_pton,
                 mlock/splice/inotify/fsync/getrandom/getline,
                 strlcpy/isatty/ptsname/mount/fmemopen/explicit_bzero,
                 setxattr/sched_setaffinity/aio_read/io_uring/statx/pidfd,
                 std::expected/format/async/future/function/jthread/mdspan/mutex,
                 condition_variable/shared_mutex/atomic_ref/generator/[[assume(/
                 std::bind(/any/filesystem/regex/latch/from_chars/to_chars/visit/
                 initializer_list/source_location/stacktrace/stop_token/flat_map/
                 chrono/function_ref/flat_set/views/inplace_vector/hive/bitset/
                 indirect/stringstream/hazard_pointer/text_encoding/simd/rcu/
                 linalg/#embed/std::meta/sync_wait/contract_assert/out_ptr/
                 flat_multimap/spanstream/task,
                 <=>, GNU cleanup/vector_size, constexpr, and
                 setjmp/longjmp/va_list are
                 NEEDS-HARNESS, never a closed proof;
                 incremental unwind k=1,2,4,…,K; static SCALAR callees
                 in the same file are inlined one level first
6b harness       POINTER → SCALAR under `// requires:` (never unguarded)
6c review        (C++ engine; docs/AI.md) Z3 vacuity audit of every requires /
                 drafted range (VACUOUS-ASSUMPTION), approved contracts
                 (contracts.approved.json → PROVED-ASSUMING, callers checked),
                 drafted contracts from code/comments/--requirements (HYPOTHESIS,
                 trace links), model assumption audit (READS), proof store and
                 PROOF-REGRESSION (<out>/proof_store.json); model half NOTRUN without a model
6c concolic      KLEE-style seed + branch negation via concrete UB oracle
7  fuzz          FuSeBMC loop + Fuzz4All seeds + ChatFuzz stall mutants;
                 POINTER is NEEDS-HARNESS (missing harness), never ERROR
7b rapid         RapidCheck-style requires/ensures sampling (CLEAN ≠ proof)
7c muttest       operator mutants vs the same properties
8  ltl           Strix-style safety properties on extracted state machines
9  execute       concrete cex replay + OpenCodeInterpreter sandbox
10 repair        RLEF: patch → run → score → keep best, budget-bounded
11 unify         report + confidence product + taxonomy coverage
                 (report.json, report.md, report.sarif; --fail-on never|defect|gap)
```

Each stage writes `prism-out/stages.jsonl`. The GUI tails it. `--resume` skips stages already
`ok`. A killed run loses at most the stage it was in.

## Running on untrusted code (Law 9)

PRISM is pointed at code it does not trust. Reading, parsing, model checking
and compiling that code is safe; *running* it is not (a `void
cleanup_everything(void)`, a Perl `BEGIN` block, a Cargo `build.rs`, an
`eslint.config.js` all run with the user's privileges). So every step that
executes code derived from the scanned tree or from the LLM is opt-in:

- `--allow-exec` (`Config.allow_exec`, default false, both engines). Without
  it the step does not run and writes `NOTRUN` with
  `message = "<step>: executes code from the scanned tree; re-run with
  --allow-exec (only on code you trust)"`, `extra.install` = the hint and
  `extra.reason = "executes-scanned-code"`. A "part" stage keeps its analysis
  half and adds one such row for the held-back half.
- The policy lives in `prism/sandbox.py` / `src/prism/sandbox.cpp`
  (`allowed()`, set for the run by the pipeline); stages that get a Config
  read `allow_exec` directly. The stage table is `EXEC_STAGES`
  (`prism/pipeline.py`) = `exec_stages()` (`src/prism/pipeline.cpp`).
- Sandbox (with the flag): on Linux with a working `bwrap`, built binaries
  run as `bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp --bind
  <scratch> <scratch> --unshare-all --die-with-parent` (read-only root,
  private /tmp, only the scratch dir writable, no network). Always, on POSIX,
  rlimits: CPU (timeout + 1 s), address space 2 GiB (not for ASan/TSan
  builds), 256 open files, 64 MiB file size, no core. Findings record
  `extra.sandbox = "bwrap" | "rlimits-only" | "none"` (Windows).
- sanitize never calls an arbitrary function, even with the flag: only a
  zero-argument, non-static function marked `// prism: run` (on the
  definition or in the comment block directly above it). Scalar functions
  fed chosen inputs are the fuzz stage's harness.
- Windows (C++): adapters no longer go through `cmd.exe` (`_popen`); the
  command line is quoted for `CommandLineToArgvW` and started with
  `CreateProcessW`. A `.bat`/`.cmd` target is quoted for `cmd.exe` or refused
  when an argument holds `%`, `!`, `"` or a newline.

| stage | executes scanned code? | without `--allow-exec` | with `--allow-exec` |
|---|---|---|---|
| inventory, classify, lints, taint, thread, interval | no (parse / in-process analysis) | runs | runs |
| warnings | no (`-fsyntax-only` compiler diagnostics) | runs | runs |
| cppcheck, esbmc, dafny | no (static analyzers) | runs | runs |
| pbsd | PRISM portable copies: no. The ParanoidBSD tree (only when named with `--pbsd PATH` / `PRISM_PBSD`; no guessed default): **yes** — its `tools/verify` modules are imported (external code) | portable copies run; the tree half is NOTRUN | tree modules imported |
| sanitize | **yes** — compiles and runs the unit | NOTRUN | calls only a zero-argument function marked `// prism: run`, in the sandbox; never "any `void(void)`" |
| optional | clang-tidy, cbmc, infer (compile only, scratch results dir), frama-c, semgrep, strix: no. **klee**: yes (external calls run natively). **spatch** rules with `@script:`/`@initialize:`/`@finalize:` blocks: yes | klee and script rules NOTRUN, the rest run | klee in the sandbox; script rules run |
| polyglot | `perl -c` (BEGIN/use), `cargo clippy` (build.rs, proc macros), `eslint` (eslint.config.js): **yes** (`Tool.executes`). python `compile()`, ruff, pyflakes, mypy (`--config-file=`, so a project `mypy.ini` cannot load plugins), `node --check`, tsc (explicit files), `bash -n`, shellcheck, `gofmt -e`, `ruby -wc`, `php -l`, `luac -p`, yamllint: no | the three are NOTRUN, the rest run | the three run (with the user's privileges: they are the project's own build/lint code) |
| contracts, wp, bmc, harness | no (in-process Z3) | runs | runs |
| review (C++ engine) | no (in-process Z3; the model reads source). `prism prove` (Lean elaboration of the project and of model tactics) is a separate command that needs `--allow-exec` | runs | runs |
| pir (C++ engine) | Clang/opt compile only, PIR + Z3 in-process: no. Translation validation (`lli` on the lowered IR): **yes** | verdicts run; validation is NOTRUN (`extra.tv`, `extra.exec = NOTRUN` + one stage row) | `lli` in the sandbox (bwrap + prlimit) |
| conc (C++ engine) | no (Clang/opt compile only; lazy sequentialisation + Z3 in-process, docs/CONCURRENCY.md) | runs | runs |
| concolic | no (in-process KLEE-style engine over the concrete interpreter) | runs | runs |
| fuzz | concrete oracle: no. Compiled harness, AFL++ (`PRISM_AFL=1`), libFuzzer (`PRISM_LIBFUZZER=1`): **yes** | concrete oracle runs; the binary half is NOTRUN (`extra.binary = NOTRUN` + one stage row) | binary half in the sandbox |
| diff | **yes** — compiles and runs both functions | NOTRUN | sandbox |
| rapid, muttest | interpreter: no; gcc fallback harness: **yes** | interpreter runs; the fallback is NOTRUN | fallback in the sandbox |
| ltl | no (strix only synthesizes from `.ltl` specs) | runs | runs |
| llm | no (the model reads source) | runs | runs |
| execute | concrete cex replay: no. LLM-written C (interpreter loop): **yes** | replay runs; the LLM half is NOTRUN | LLM half in the sandbox |
| repair | **yes** — compiles and runs LLM-written candidates | NOTRUN | sandbox |
| unify | no | runs | runs |

## Also needed (and included as stages/adapters)

These are the holes the named tools do not fill:

- **KLEE / symbolic execution** — unbounded in a different direction than BMC.
- **AFL++ / libFuzzer** — when present, replace the in-process fuzzer.
- **ASan/UBSan/TSan** — the only instruments that trap at runtime.
- **Frama-C WP** — deductive verification for C without translating to Dafny.
- **Infer** — heap across TUs; taint. Adapter, `NOTRUN` if absent. (The CodeQL
  adapter was removed: its engine terms restrict commercial use; roadmap 1.2.)
- **RapidCheck / property tests** — generative tests from `ensures` clauses.
- **Differential testing** — two implementations, same inputs, disagree = bug.
- **Mutation testing** — did the tests actually see the fault class?
- **Semgrep** — taught shapes, like Coccinelle, for non-C too.
- **Coccinelle** — `spatch` adapter plus shipped `.cocci` rules; missing is `NOTRUN`.

## What "done" means for this repo

A command `python -m prism path/to/code` that:

- runs every stage that *can* run on this machine,
- names every stage that cannot (`NOTRUN` + install line),
- proves at least one scalar function with Z3,
- crashes at least one planted bug with the fuzzer,
- emits a Qt window of the same report,
- never reports a missing ESBMC as a proof.
