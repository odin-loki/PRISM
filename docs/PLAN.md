# Helix — unified hybrid code-testing pipeline

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
5  contracts     Dafny-style + ACSL specs (PROVED-ASSUMING under requires)
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
6c concolic      KLEE-style seed + branch negation via concrete UB oracle
7  fuzz          FuSeBMC loop + Fuzz4All seeds + ChatFuzz stall mutants;
                 POINTER is NEEDS-HARNESS (missing harness), never ERROR
7b rapid         RapidCheck-style requires/ensures sampling (CLEAN ≠ proof)
7c muttest       operator mutants vs the same properties
8  ltl           Strix-style safety properties on extracted state machines
9  execute       concrete cex replay + OpenCodeInterpreter sandbox
10 repair        RLEF: patch → run → score → keep best, budget-bounded
11 unify         report + confidence product + taxonomy coverage
```

Each stage writes `helix-out/stages.jsonl`. The GUI tails it. `--resume` skips stages already
`ok`. A killed run loses at most the stage it was in.

## Also needed (and included as stages/adapters)

These are the holes the named tools do not fill:

- **KLEE / symbolic execution** — unbounded in a different direction than BMC.
- **AFL++ / libFuzzer** — when present, replace the in-process fuzzer.
- **ASan/UBSan/TSan** — the only instruments that trap at runtime.
- **Frama-C WP** — deductive verification for C without translating to Dafny.
- **Infer / CodeQL** — heap across TUs; taint. Adapters, `NOTRUN` if absent.
- **RapidCheck / property tests** — generative tests from `ensures` clauses.
- **Differential testing** — two implementations, same inputs, disagree = bug.
- **Mutation testing** — did the tests actually see the fault class?
- **Semgrep** — taught shapes, like Coccinelle, for non-C too.
- **Coccinelle** — `spatch` adapter plus shipped `.cocci` rules; missing is `NOTRUN`.

## What "done" means for this repo

A command `python -m helix path/to/code` that:

- runs every stage that *can* run on this machine,
- names every stage that cannot (`NOTRUN` + install line),
- proves at least one scalar function with Z3,
- crashes at least one planted bug with the fuzzer,
- emits a Qt window of the same report,
- never reports a missing ESBMC as a proof.
