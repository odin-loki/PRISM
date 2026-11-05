# PIR: the Clang/LLVM front end and the pir stage

The `pir` stage does PRISM's bounded model checking and k-induction for C and C++. It compiles each
unit with clang, rewrites the IR, translates it to PIR (PRISM's typed SSA over bitvectors), inserts
the property checks itself (Law 8) and asks Z3 and the solver portfolio about each one.

```
C/C++ unit ──clang -O0──▶ LLVM IR ──PRISM instrumentation──▶ opt (fixed passes)
          ──▶ textual IR parser ──▶ PIR (+ property checks) ──▶ SMT (QF_BV, FP)
          ──▶ FAILED / PROVED / BOUNDED / PROVED-UNBOUNDED / NEEDS-HARNESS / …
          ──(--allow-exec)──▶ translation validation against lli
```

Defaults and limits below are as of main `991c4eafa` (2026-11-05).

| Part | Code |
|---|---|
| Stage driver, clang/opt calls, reruns | `src/prism/pir/stage.cpp`, `stage_mem.cpp` |
| Front end probes, target flags | `src/prism/pir/frontend.cpp`, `include/prism/pir/frontend.hpp` |
| Pre-`opt` rewrites | `src/prism/pir/lower_ctl.cpp`, `folded_diag.cpp` |
| IR parser | `src/prism/pir/ir_parser.cpp` |
| PIR types, printer, interpreter | `include/prism/pir.hpp`, `src/prism/pir/pir.cpp` |
| Translation | `translate.cpp`, `translate_mem.cpp`, `translate_fp.cpp`, `translate_ctl.inc` |
| Memory model | `memory.cpp` (`SymMem`, `ConcMem`), `contracts.cpp` |
| Floating point | `fp.cpp`, `translate_fp.cpp` |
| Encoder, verdicts, k-induction | `encode.cpp` |
| Loop invariants (Houdini) | `houdini.inc` |
| Library models | `libc_models.cpp`, `libc_format.cpp`, `models/libc/`, `models/cxx/` |
| Lean export | `export_lean.cpp` |

Tests: `testdata/pir/*`, `testdata/conformance/{prism,libc-models}/`, and
`tests/cpp/test_pir_*.cpp`, `test_pir3.cpp`, `test_main.cpp` (`pir: …`), `test_cxx_*.cpp`.

## Front end

Clang and opt are external processes (argv, no shell). The tools come from `Config::llvm_tool` (the
pinned LLVM 23.1.4, `third_party/llvm/PIN.toml`, else PATH). Each unit is compiled as:

```
clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone -fno-discard-value-names \
      -gline-tables-only -std=c17|c++23 -fno-builtin-memcpy <UB-folding warnings> file
opt -passes=mem2reg,lower-switch,loop-simplify,lcssa,instnamer -S          (C)
opt -passes=function(mem2reg),coro-early,cgscc(coro-split),coro-cleanup,\
    function(lower-switch,fix-irreducible,loop-simplify,lcssa,instnamer)   (C++)
```

`base_flags` and `lower_to_ir` in `stage.cpp` build these lines.

No optimising pass runs (it could exploit the UB being checked) and no `-fsanitize` check is
inserted (Law 8). `-fno-builtin-memcpy` keeps a C `memcpy` a call, so its model checks C's
no-overlap rule. C units get `-Wno-error=implicit-function-declaration`: an undeclared call compiles
and becomes `UNENCODED: call @f` (front-end leniency, not a disabled check). C++ units get
`-D_GLIBCXX_ASSERTIONS` ("C++ library"). Fallbacks change only the language level: C17 → C23,
C++23 → C++14 on "ISO C++17 does not allow", C23 `_Decimal*` as C++23 with the shim
`models/cxx/prism_dfp.hpp`, `import std;` via the modelled `models/cxx/std.cppm`. A C header that
defines functions is also lowered alone; only functions no unit reported are kept
(`extra.lowered_as`; `PRISM_PIR_NO_HEADERS` turns this off).

Compiling scanned code does not execute it, so the stage needs no `--allow-exec`. Missing clang or
opt is `NOTRUN` with an install hint; a unit that does not compile is one `ERROR` row.

### Analysis target

Main lowers for Linux LP64 (`x86_64-unknown-linux-gnu`); SV-COMP's `--data-model ILP32` lowers for
`i386-unknown-linux-gnu`. Units and libc models use the same triple (`FrontendTargetScope`); the
pointer model follows the datalayout and the model cache is keyed by the triple.

Branch `port/winmsvc` (commit `862d6dac2`, landing) makes the target a setting: `--target TRIPLE` or
`PRISM_TARGET`: `x86_64-unknown-linux-gnu`, `i386-unknown-linux-gnu` or `x86_64-pc-windows-msvc`
(default: the host; an unsupported triple is exit 2). For the MSVC target (LLP64: 32-bit `long`,
16-bit `wchar_t`, 64-bit pointers) pir passes `-fno-wrapv` (clang's MSVC driver adds `-fwrapv`,
which would remove every `nsw` and so every signed-overflow check) and `_NO_CRT_STDIO_INLINE` (so
`printf` stays a modelled call). The probe needs the UCRT headers (`PRISM_MSVC_SYSROOT` off
Windows), else pir is `NOTRUN`. C++ is lowered against the MSVC STL headers, never the libstdc++
models, and `extra.cxx_models` says so. Non-default targets put `extra.target` on every row.
SV-COMP stays Linux. Tests: `tests/cpp/test_target.cpp`, `testdata/conformance/prism/datamodel/`.

**Windows host.** On main, a Windows build lowers for `x86_64-unknown-linux-gnu` against the pinned
Linux sysroot (`PRISM_LINUX_SYSROOT`, else `scripts/fetch_toolchain.py linux-sysroot` under
`~/.prism/toolchain/linux-sysroot/`). Without it pir is `NOTRUN`. `frontend_usable` also lowers a
probe file for that triple (a temp file, not stdin) and checks the IR's `target triple`.
`lp64_clang_driver_flags()` adds the target, the sysroot and `-idirafter models/libc/c23` (C23
headers such as `<stdbit.h>` a libc may lack). The SV-COMP debug IR uses the same flags.

### Instrumentation before mem2reg

Some facts would be lost by `mem2reg` or by clang's constant folding, so PRISM rewrites the `-O0` IR
before `opt`:

* **Uninitialised locals.** Every promotable `iN` alloca (except `%retval`) and every
  `half`/`float`/`double` alloca gets `store (call @__prism.uninit.<ty>()), %x`. After mem2reg a
  read before the first store uses that call. PIR tracks a one-bit "uninit" shadow through phis and
  checks every non-phi use: `UNINIT-READ`.
* **`undef` that remains** follows the LangRef: each use reads a fresh havoc, `freeze` picks one
  value. Branching on it or passing it as `noundef` is `UB-POISON`.
* **Folded UB to poison.** Clang folds `7 % 0`, `INT_MIN / -1`, `1 << 40` to `poison`. Each `iN
  poison` operand becomes `call @__prism.poison.iN()`, a reachability check (`UB-POISON`).
* **Folded UB to a value.** `1 << 31` or `INT_MAX + 1` fold to constants with only a warning. The
  warning's line:column is mapped to an instruction on that line and `call @__prism.folded(i32 k)`
  is inserted there (`folded_diag.cpp`). A warning with no instruction on its line makes the
  function `NEEDS-HARNESS`. Warnings in headers are not attributed.
* **setjmp functions.** Every alloca of a function that calls `setjmp` gets a `@__prism.keep` use,
  so mem2reg keeps it in memory ("setjmp/longjmp").

**C signed left shift.** IR does not say whether `shl` came from a signed C `<<`. For C units with a
`shl`, clang runs once more with `-fsanitize=shift-base` only to locate signed shifts by debug
location. Nothing else from that build is used. Each matching `shl` gets the C17 6.5.7p4 check.
C++20 defines signed `<<`, so C++ units get no base check.

## IR parser

`ir::parse_module` is a tokenizer and recursive-descent parser for what `opt -S` prints: types,
values, constant expressions (kept opaque), `!dbg` attachments and the debug-info nodes PRISM reads.
Every instruction line is parsed. An opcode outside the subset keeps its name with `parsed = false`,
so the translator can report it. `volatile` loads and stores are kept and checked like any access;
`atomic` ones are not parsed (`UNENCODED`).

Branch `port/precision` (commit `ab9e29bb6`, landing) adds LLVM 23's special float literals
(`+qnan`, `-inf`, `+nan(0x…)`, `+snan(0x…)`) and IR comments such as `/* (nan) */` after
`is.fpclass` masks. Before it, those functions were `UNENCODED`.

## PIR

PIR (`include/prism/pir.hpp`) is typed SSA over bitvectors of width 1..64. Statements are `assign`,
`check` (violation when the i1 argument is 1), `assume` and the memory statements; terminators are
`jmp`, `br`, `ret` and `stop` (the path ends); loops are back edges. Operators are the LLVM integer
operators, comparisons, casts, bit intrinsics, `havoc`, the i1 property predicates (`sadd.ovf`,
`shift.oob`, …) and IEEE operators on FP bits. `to_text` prints PIR; its SHA-256 is
`extra.pir_hash`.

Calls to functions defined in the unit (or a linked model) are **inlined**: depth 4, or 12 in C++
units. Recursion is `UNENCODED: recursive call @f`. `exit`/`_Exit`/`_exit`/`quick_exit` end the
path. `__VERIFIER_nondet_*` and `nondet_*` are havocs; `__VERIFIER_assume` is an assume.

### Properties (inserted by PRISM, Law 8)

| IR | check | class |
|---|---|---|
| `add/sub/mul nsw` (clang sets `nsw` exactly when signed overflow is UB) | signed overflow | INT-SIGNED-OVF |
| `sdiv/srem` | divisor 0; `INT_MIN / -1` | INT-DIV-ZERO / INT-SIGNED-OVF |
| `udiv/urem` | divisor 0 | INT-DIV-ZERO |
| `shl/lshr/ashr` | amount ≥ width | INT-SHIFT-UB |
| C signed `<<` | negative base or overflow | INT-SHIFT-UB |
| `nuw`, `exact`, `or disjoint`, `zext nneg` | the poison condition | UB-POISON |
| `llvm.abs(x, true)` | `x == INT_MIN` | INT-SIGNED-OVF |
| `llvm.ctlz/cttz(x, true)` | `x == 0` | INT-CLZ-ZERO |
| `unreachable`, `llvm.trap` | reached | CXX-UNREACHABLE |
| `__assert_fail`, `reach_error`, `__VERIFIER_error` | reached | FUNC-CONTRACT |
| `abort` | reached on a path where no library allocation failed (after a failed allocation it is out-of-memory handling; CONFORMANCE.md F9) | FUNC-CONTRACT |
| uninitialised local read | shadow set | UNINIT-READ |
| clang-folded UB | reached | per warning / UB-POISON |
| `fptosi`/`fptoui` | truncated value outside the type, NaN, ±inf | FLOAT-CAST-OVF |
| FP ops under `--fp-checks` (opt-in) | ÷ ±0; NaN from non-NaN; inf from finite | FLOAT-DIV-ZERO / FLOAT-INVALID / FLOAT-OVERFLOW |
| exception reaching `__clang_call_terminate` | throw out of `noexcept` | CXX-THROW-NOEXCEPT |
| exception leaving `main` | uncaught | CXX-UNCAUGHT |
| `std::terminate()`, `throw;` with none handled | reached | CXX-TERMINATE |
| `__cxa_pure_virtual` | reached | CXX-PURE-VIRTUAL |
| `longjmp` | its `setjmp` caller has returned | CTRL-LONGJMP-INVALID |
| call through a null function pointer | reached | PTR-NULL-DEREF |
| memory accesses, frees, pointer arithmetic | "Memory model" table | MEM-*, PTR-* |
| `std::__glibcxx_assert_fail` | reached | "C++ library" |

<a id="what-is-not-encoded-named-never-dropped--roadmap-21"></a>

### What is not encoded (named, never dropped)

A construct PIR cannot encode makes the function `NEEDS-HARNESS` with `UNENCODED: <construct>` in
the message. The main ones: integers wider than 64 bits; `long double` (x86_fp80), `__float128`,
bfloat, fast-math flags, fused multiply-add on half; external calls without a library model
(`UNENCODED: call @f`); `atomic` loads and stores; `thread_local` globals; extern arrays of unknown
size; vectors and first-class aggregates other than two scalar fields and the `{ ptr, i32 }`
exception pair; `ptrtoint` other than pointer differences and comparisons; `inttoptr` other than of
0; `switch` if lower-switch did not run; irreducible control flow; recursion and calls deeper than
the inline limit; dynamic exception specifications; indirect calls with more than 16 candidates;
inline assembly without a contract; objects of 2^47 bytes or more (2^21 under ILP32) or more objects
than the id bits hold; in C++ units, a function whose source contains `const_cast`. The full list is
every `Unenc{"UNENCODED: …"}` in `src/prism/pir/`.

Some constructs end only one path (a *soft* check, `prop = unmodelled`): reachable means
`NEEDS-HARNESS`, never FAILED, never proved away. Examples: a landing pad that cannot be translated,
an undecidable catch match, the indirect-call havoc fallback.

Pointer parameters are `NEEDS-HARNESS` by Law 6 unless a precondition gives the object size
("Pointer parameters").

**Gaps of the property set.** A PROVED means these were not checked: an indeterminate value that is
only copied (`int y = x;`, `y` unused); falling off the end of a non-void C function; sub-array
bounds of the last struct field (flexible-array idiom) and a separately computed one-past-the-end
sub-array address; memory leaks; effective types unless `--strict-aliasing`; pointer arithmetic on
an already freed object; a non-volatile local modified between `setjmp` and `longjmp` (C11
7.13.2.1p3); an exception that leaves a function other than `main` (the path ends;
`extra.throws_not_followed` names the types).

<a id="memory-model-roadmap-25"></a>

## Memory model

The model follows ESBMC/SMACK and the Lean model `proofs/semantics/PrismSem/Memory.lean`.

**Pointers** are one PIR value. Under LP64 the object id is bits 63..48 (`0` is null, never
allocated) and the byte offset bits 47..0 (`PtrModel kLP64`). ILP32 packs id 31..22 and offset 21..0
(`kILP32`). Every object is smaller than 2^47 (ILP32: 2^21) bytes. Every `getelementptr` is checked
to stay in its object or one past it. So on every execution without a reported violation the packed
value is the Lean pair `(obj, off)`.

**Objects** carry size (possibly symbolic: VLAs, `malloc(n)`), liveness, kind (`stack`, `heap`,
`static`, `const`, `new`, `new[]`, `extern` for contract objects, `FILE`) and base alignment. Each
byte is a cell: 8-bit value, an initialised bit and, with `--strict-aliasing`, an effective-type
tag.

**Statements**: `alloc`, `free`, `load`, `store`, `memcpy` (memmove semantics), `memset`,
`stacksave`, `stackrestore`, `revive` (`llvm.lifetime.start`). Queries `obj.size`, `obj.live`,
`obj.kind`, `obj.align` can appear in any expression. `translate_mem.cpp` turns each property into
ordinary `check`s:

| IR | check | class |
|---|---|---|
| load/store through object 0 | null dereference | PTR-NULL-DEREF |
| load/store, id not allocated | wild pointer | PTR-INVALID-DEREF |
| load/store, object not live | use after free / lifetime | MEM-UAF |
| load/store, `off + n > size` | out of bounds | MEM-OOB-READ / MEM-OOB-WRITE |
| load/store `align a` not met | misaligned access | MEM-MISALIGNED |
| store to a `const` object | write to read-only memory | MEM-WRITE-CONST |
| load of a byte never written | uninitialised read | UNINIT-READ |
| load, effective type differs (`--strict-aliasing`) | strict aliasing | MEM-STRICT-ALIAS |
| `getelementptr` offset overflow, `inbounds` outside `[0, size]`, nested-array index out of range | invalid pointer arithmetic | MEM-PTR-ARITH |
| ordered compare or difference of pointers | different objects | PTR-COMPARE |
| `llvm.memcpy`, C `memcpy` model | overlapping ranges | MEM-OVERLAP |
| `free`/`delete`/`delete[]`/`fclose` | not an object start, not heap | MEM-INVALID-FREE |
| same | wrong allocator | MEM-MISMATCHED-FREE |
| same | already released | MEM-DOUBLE-FREE |
| VLA `alloca T, iN n` | `n <= 0`, size overflow, too large | MEM-VLA-SIZE |
| `ret` of a pointer | into the function's own stack | MEM-STACK-ESCAPE |

`llvm.memcpy` with `d = s` exactly is allowed (the LangRef defines it: struct self-assignment). A C
`memcpy` call reaches its model, where it is an overlap (C17 7.24.2.1).

**Lifetimes.** An inlined callee's allocas end at its `ret`, VLAs at `llvm.stackrestore`;
`llvm.lifetime.end/start` end and restart a stack object (bytes indeterminate). Layout comes from
the module's `target datalayout`. An integer load of a whole aggregate (ABI coercion of a struct by
value) carries a per-byte initialised mask, checked where the value is used, since padding may be
indeterminate.

**Global state.** Globals the function uses are allocated in a prologue. `constant` globals hold
their initializer. Mutable globals hold arbitrary initialised bytes, except in `main`, which starts
from the initializers. A global over 4096 bytes whose initializer needs more than 256 stores holds
arbitrary bytes instead (CONFORMANCE.md F8). A FAILED that disappears with the initializers is
`NEEDS-HARNESS` (the global state is an unstated precondition; `extra.globals`). Branch
`port/precision` (commit `a8e9f1676`, landing): when the re-check with initializers fails on a
*different* property, that verdict replaces the first one, and the arbitrary-globals violation is
kept as `extra.arbitrary_globals_violation`. `stdin`/`stdout`/`stderr` point to valid `FILE`
objects.

### Encodings

The unrolled program is a DAG encoded in topological order. Every memory update is guarded by its
block instance's reach condition, so one global memory state is exact, and each `alloc` instance
gets the next constant object id (the Lean `next` counter). `MemEncoding::Bv` is the default
(QF_BV, what certified mode accepts): a load is an `ite` chain over the guarded writes before it
(read-over-write, as `PrismSem/MemEncode.lean` `menc`), and arbitrary bytes are fresh variables with
Ackermann constraints. `MemEncoding::Array` (one SMT array, ranged writes as lambdas) is slower on
symbolic lengths and not the default.

**Value sets (Bv).** `SymMem` computes, from a term's structure alone, a small set of guarded
constants the term equals (guards exhaustive and exclusive). Loads and writes at such addresses are
decided per constant without the solver, and word loads are built from whole writes
(`SymMem::word_at`). This keeps linked structures (the map/set models, lists) tractable.

`tests/cpp/test_pir_mem.cpp` checks that both encodings give the same verdict and class on every
property, and that the PIR interpreter (`ConcMem`) reproduces each counterexample.

### Correspondence to the Lean model

| `PrismSem/Memory.lean` | C++ |
|---|---|
| `Ptr = (obj, off : BitVec 64)` | packed id/offset bits (faithful on violation-free runs) |
| `Obj.size/live/data` | `SymMem::Obj::size/alive` + byte cells (`ConcMem` concretely) |
| `MState.next` | constant id per `alloc` instance, in encoding order |
| `MCond.live` / `inBounds` / `atBase` | `obj.live`; `off + n <= obj.size`; `off == 0` in free checks |
| `mrun` load/store UB | MEM-UAF / MEM-OOB-* / PTR-NULL-DEREF / PTR-INVALID-DEREF |
| `mrun` free UB | MEM-DOUBLE-FREE / MEM-INVALID-FREE |
| `MemEncode.menc`, `MSym.WF` | `MemEncoding::Bv` read-over-write + Ackermann constraints |

Not covered by the Lean proofs (trusted C++): multi-byte little-endian accesses, the
uninitialised-byte shadow, allocation kinds and their free checks, ranged `memcpy`/`memset`,
alignment, the Array encoding, the ILP32 packing, and the packing argument (a paper proof).

## Pointer parameters (Law 6)

A pointer parameter stays `NEEDS-HARNESS` unless a precondition gives its object's size. It goes in
the comment block before the function or the leading comment lines of its body (`contracts.cpp`):

```c
// requires: \valid(p + (0..n-1))        n elements, n an int parameter
// requires: \valid(p + (0..7))          8 elements
// requires: \valid(p)                   one element
// requires: \valid_read(p + (0..n-1))   read-only (writes are MEM-WRITE-CONST)
/*@ requires \valid(p + (0..n-1)); */   ACSL block
```

`p` is then bound to a fresh `extern` object of `max(n, 0) × elem` bytes with arbitrary contents,
16-byte aligned. The element size is that of the first access through `p`. A proof is
**PROVED-ASSUMING**, never PROVED, with every assumption in `extra.assumptions`. A violation is
FAILED under the stated precondition. Translation validation does not replay such functions (`tv =
PARTIAL`).

With `--pir-drafts` (opt-in), a C function without a clause may take its size from the deterministic
template harness draft (`ai::draft_harness`, never an LLM, Law 4). A violation found only under a
drafted precondition is NEEDS-HARNESS, never FAILED.

## Library models

Operational models are C++ sources with an `extern "C"` libc API in `src/prism/pir/models/libc/`.
CMake embeds the files listed in `PRISM_PIR_MODELS`. They are lowered once per run (plus
`-fno-builtin -ffreestanding`) and linked into every unit that declares a modelled symbol without
defining it (`link_models`, on the parsed module). Model code is checked like user code; a check
inside a model reports the call site and names the model ("… (in the strcpy library model)"). The
`__prism_*` intrinsics (`__prism_alloc`, `__prism_check`, `__prism_havoc_bytes`, …) are PIR
statements (`translate.cpp` `model_intrinsic`).

Linked into scanned units (embedded):

| file | functions |
|---|---|
| `string.cpp` | `strlen strnlen strcpy strncpy strcat strncat strcmp strncmp strcasecmp strncasecmp strcoll strchr strrchr strstr strspn strcspn strpbrk strdup strndup memcpy memmove memset memset_explicit memcmp memchr memrchr memccpy memmem` |
| `stdlib.cpp` | `malloc calloc realloc free aligned_alloc free_sized free_aligned_sized memalignment`, `operator new/new[]/delete/delete[]` (incl. sized), `abs labs llabs div ldiv lldiv atoi atol atoll strtol strtoll strtoul strtoull strtof strtod strtold qsort bsearch getenv rand srand`, `__VERIFIER_nondet_*` for float types |
| `stdio.cpp` | `fopen fclose fflush fgets fread fwrite fgetc getc getchar fputc putc putchar fputs puts ungetc fseek ftell rewind feof ferror clearerr` |
| `fmtdouble.cpp` | `__prism_fmtv`: exact `sprintf`/`snprintf` output for formats with a double |

Behaviour worth knowing: allocation may fail (NULL; the failure is recorded, so a later `abort()` is
not reported). `calloc` checks `n*size` overflow. `realloc(p, 0)` frees `p` and returns NULL
(glibc's choice). `operator new` never returns NULL. Output functions may return EOF. Input bytes,
`getenv` contents, `rand` values and conversion results are unknown. `strtold` returns an unknown
`long double`, which stays `UNENCODED` on LP64.

`printf`, `fprintf`, `dprintf`, `sprintf`, `snprintf` are lowered by the translator
(`libc_format.cpp`), not linked. With a literal format it checks argument count and IR types per
conversion (FMT-ARGS), reports `%n` (FMT-PERCENT-N), requires `%s` arguments to be strings, and
writes sprintf/snprintf output into the buffer. A non-literal format, `v*printf` and `scanf` are
`UNENCODED`.

`ctype.cpp`, `time.cpp` and `strings_compat.cpp` (`ffs ffsl ffsll`) exist and have contract
harnesses, but they are **not** in `PRISM_PIR_MODELS` on main, so scanned calls to them are
`UNENCODED: call @name`. `models/libc/c23/stdbit.h` and `forced/prism_c23_decls.h` supply C23
declarations a libc may lack.

Any other external call stays `NEEDS-HARNESS` (`UNENCODED: call @name`).

**Building the models.** Models are lowered before any unit, with `max(120 s, 4 × --timeout)` per
clang and opt call; a timed-out call is retried once with twice the time. The lowered IR is cached
on disk under `$XDG_CACHE_HOME/prism/pir-models/<key>/` or `~/.cache/prism/pir-models/<key>/`. The
key hashes `prism-pir-models-v5`, the model sources, the clang version, the tool paths and the
target triple. Files are written under a temporary name and renamed. If the models cannot be built,
the stage records one ERROR row saying why (Law 7), and every function stopped at an unmodelled call
is `NOTRUN` (`extra.models_error`), not `NEEDS-HARNESS`: the missing answer is the stage's.

### Windows disk cache (LP64)

On Windows the cache root is `%LOCALAPPDATA%\prism\pir-models\`. Before reuse,
`try_load_model_cache` loads every lowered libc model and `cached_model_ir_ok` rejects IR for the
wrong triple or native LLP64 IR (`e-m:w-`); a bad tree is deleted and lowered again. On
`port/winmsvc` the cache is keyed and validated per target, and other targets' valid trees are kept.

### C++ library

C++ units are compiled with `-D_GLIBCXX_ASSERTIONS`, which turns on libstdc++'s own precondition
checks. Library code is ordinary IR and is inlined. A reachable `std::__glibcxx_assert_fail` is a
violation: bounds (`operator[]`, `std::span`) are MEM-OOB-READ, `std::optional::operator*` on an
empty optional is CXX-OPTIONAL-NULL, `std::unique_ptr::operator*` on null is PTR-NULL-DEREF. Library
throws (`std::__throw_*`) throw the exception class they name along the explicit exception edges
("Exceptions"), so a `catch` of that class or a base catches them. libc++ (`_LIBCPP_HARDENING_MODE`)
is not handled. On the MSVC target (`port/winmsvc`) these checks are not run.

### C++ library models

Some libstdc++ code is too expensive or too recursive for the encoder (`vector::_M_realloc_insert`,
the red-black tree's recursive erase). PRISM ships model headers in `src/prism/pir/models/cxx/`,
embedded like the C models (`cxx/<path>`), written to a temporary directory once per run and put
first on the C++ include path (`-isystem`, `lower_to_ir`), so `#include <vector>` reaches the model.

A model must reach every behaviour the real library can. Containers keep libstdc++ 13's observable
behaviour (capacity growth, construction order, exceptions) and allocate through `std::allocator`
(the `operator new/delete` models), so an iterator or pointer kept across a reallocation or erase is
**MEM-UAF** when used. Preconditions are checked as `_GLIBCXX_ASSERTIONS` checks. Each model has a
differential trace test against libstdc++, `tests/cxx_models/<name>_trace.cpp`, run by
`tests/cpp/test_cxx_models.cpp` and `test_cxx_*.cpp`.

`models/cxx/` has 73 top-level entries (2026-11-05, main `991c4eafa`): 65 standard-header models,
the `bits/` and `iostream/` directories, and helpers.

| family | model files |
|---|---|
| sequences | `vector array deque list forward_list span mdspan valarray bitset initializer_list` |
| associative (`prism_tree.h`, `prism_hash_table.h`) | `map set unordered_map unordered_set flat_map flat_set` |
| adaptors | `stack queue priority_queue` |
| strings and text | `bits/basic_string.tcc` (std::string's out-of-line members), `string_view charconv format print regex sstream spanstream fstream syncstream` |
| vocabulary and utilities | `optional expected variant any tuple functional memory memory_resource typeindex source_location system_error stacktrace ratio` |
| numerics | `cmath complex numbers numeric bit random chrono` |
| algorithms and iterators | `algorithm iterator ranges execution` |
| concurrency (`bits/prism_sched.h`, `bits/prism_conc.h`) | `thread mutex shared_mutex condition_variable atomic future latch barrier semaphore stop_token` |
| coroutines | `coroutine generator` |
| other | `filesystem`; `std.cppm` (`import std;`) |
| helpers | `prism_fpfmt.h` (exact FP text), `prism_dfp.hpp`, `prism_dfp_mp.hpp` (C23 decimal FP shim), `iostream/prism_ostream.hpp` |

Several models are partial on purpose (`algorithm`, `numeric`, `cmath`, `ranges`, `ratio`, `regex`,
`semaphore`, `numbers`); each header's leading comment says what it covers. `<string>` itself is
libstdc++'s; only `bits/basic_string.tcc` is replaced, with element loops instead of ranged copies
(new ABI only). `iostream/` holds a recording buffer included by path; it is not a `<iostream>`
model. The thread models schedule threads at sync points (`bits/prism_sched.h`); the `conc` stage
uses `bits/prism_conc.h` (docs/CONCURRENCY.md).

**Not modelled.** `vector<bool>` and allocators other than `std::allocator<T>` (including `pmr::`
containers) are left undefined, and so are node handles of the associative containers. A unit that
uses them does not compile against the models. The stage then lowers it again with libstdc++'s
headers. Every function's `extra.cxx_models` says which library it was checked against: `"model:
vector, map"` (the model headers the IR used), or `"libstdc++ (fallback: …)"` with the compiler's
first error.

The encoder keeps a first-class struct of two scalars (`{ ptr, i8 }` for `pair<iterator, bool>`) as
two variables; other aggregates stay `UNENCODED`.

### Library models verified by PRISM

`testdata/conformance/libc-models/` holds contract harnesses that `#include` the model sources and
`assert()` each function's C-standard contract (e.g. "strlen returns the index of the first NUL")
on objects with symbolic contents and sizes. Each `_true` harness has `_false` twins that must be
refuted with the planted class (`expect_class:`), so a proof is not vacuous. `*_contracts.cpp`
harnesses do the same for the C++ models. They run in `build/prism-qa conformance`;
`tests/cpp/test_qa_conformance.cpp` checks that every libc model function is called by a harness.

**Scope.** Harness objects have `N` bytes (`harness.h`, default 4), so most verdicts are
size-bounded (Law 2): PROVED means "for every content and position of objects up to N bytes".
`prism-qa libc-bounds` re-runs each `_true` harness at `-DN=4, 8, 16, 32, 64`; on 2026-09-23 every
size-parametric harness was PROVED at N = 16. Two harness files prove contracts for any size below
2^40: `unbounded_contracts.cpp` (the loop-free ranged models `memcpy`, `memmove`, `memset`,
realloc's copy) and `unbounded_string_contracts.cpp` (byte-loop string models, closed by Houdini
invariants, "Loop invariants"). `getenv` stays BOUNDED (its string has unknown length).

The `printf` family is lowered in the translator, so `testdata/pir/mem_libc.c` checks it instead.

## C features

`_Generic` is resolved by clang. VLAs are symbolic-size allocas, checked positive and below the
object bound (MEM-VLA-SIZE). `setjmp`/`longjmp` are exception-like edges. C23 units are retried as
C23 ("Front end").

## Floating point

`half`, `float` and `double` are PIR bitvectors of width 16/32/64 holding IEEE bits (`Var::fp`). The
encoder uses Z3's floating-point theory with round to nearest even (PRISM does not model
`fesetround`). A NaN result has an unspecified payload. `frem` is C `fmod`; `llvm.fmuladd` is fused
or not (a free choice per call); `minnum`/`maxnum` of `+0`/`-0` may return either.

Translated: `fadd fsub fmul fdiv frem fneg fcmp` (all predicates), `fptosi fptoui sitofp uitofp
fpext fptrunc`, `bitcast` between FP and same-width integers, FP loads/stores/phis/selects, the
`llvm.*` intrinsics `fabs copysign sqrt fma fmuladd minnum maxnum minimum maximum floor ceil trunc
round roundeven rint nearbyint` and the same libm functions by name. Other
pure libm functions (`sin`, `exp`, `pow`, …) return an **unconstrained** value (`Op::FLibm`,
`extra.libm_unconstrained`) apart from range facts every IEEE libm keeps (`|sin| ≤ 1`, `exp ≥ +0`,
…). Any fast-math flag is `UNENCODED`. `errno` is not modelled.

Properties: FLOAT-CAST-OVF is always on (C11 6.3.1.4p1). With `--fp-checks` (opt-in; Annex F defines
these results) PRISM also reports FLOAT-DIV-ZERO, FLOAT-INVALID and FLOAT-OVERFLOW for `fdiv fadd
fsub fmul frem sqrt fma fptrunc`.

The interpreter (`fp.cpp`) computes float/double with the host's SSE arithmetic and half in double
with one final rounding. Results the encoder leaves open (`fmuladd`, `minnum(+0,-0)`) taint the
value, so translation validation skips inputs that depend on them. `testdata/pir/fp_arith.c`.

## Exceptions

Every call is inlined, so the enclosing `invoke`s of a throw are known. A throw (`__cxa_throw`,
`std::__throw_*`, `resume`, `throw;`) jumps to the innermost landing pad that catches the type or
has cleanup code, unwinding the frames in between. Matching uses typeinfo, public unambiguous bases
from `__si_/__vmi_class_type_info`, the libstdc++ exception hierarchy and `catch (...)`.

The exception object has a 32-byte PRISM header (type id, handler count, bound pointer);
`__cxa_begin_catch`/`__cxa_end_catch` follow the Itanium runtime's counting and free the object
with its last handler. A typeinfo the module only declares gets the Itanium `{ vptr, __name }`
layout, so `typeid(T).name()` reads defined bytes.

Escape from `noexcept` is CXX-THROW-NOEXCEPT, from `main` CXX-UNCAUGHT; `std::terminate()` is
CXX-TERMINATE. An exception leaving any other function ends the path (`extra.throws_not_followed`).
Soft (NEEDS-HARNESS when reachable): undecidable type matches, a base behind a virtual base,
handlers nested deeper than 8, untranslatable landing pads. `UNENCODED`: dynamic exception
specifications, `std::exception_ptr`, `std::uncaught_exceptions`. `testdata/pir/eh_*.cpp`.

## Coroutines

C++ units run LLVM's `coro-early`, `coro-split` and `coro-cleanup` after `mem2reg`. Each coroutine
becomes a ramp that allocates its frame with `operator new`, plus `.resume`/`.destroy`/`.cleanup`
functions. `fix-irreducible` gives loops re-entered at a suspend point a single header.
`coroutine_handle::resume()`/`destroy()` are indirect calls through the frame ("Indirect calls").
The frame is a `new` object, so use after `destroy()` and double destroy are memory-model
properties. The `coroutine` and `generator` model headers build on this.
`testdata/pir/coro_gen.cpp`.

## setjmp/longjmp

`setjmp` (`_setjmp`, `__sigsetjmp`, …) splits its block. It stores a site id in the first 8 bytes of
the `jmp_buf`; the continuation's phi is the return value (0 on the direct path). `longjmp` reads
the id back (an unset `jmp_buf` is UNINIT-READ) and jumps to that site's continuation if its frame
is active, unwinding frames in between; the value is `v`, or 1 for `v == 0`. A site whose function
has returned is CTRL-LONGJMP-INVALID. Locals of a `setjmp` function stay in memory, so they hold
their last stored value, as in the -O0 build. A `longjmp` from code the `setjmp` does not dominate
is irreducible control flow (`UNENCODED`). `testdata/pir/sjlj_basic.c`.

## Indirect calls

A function's address is a constant pointer with an object id in `[fn_obj_base, max_objects)`: never
allocated, never dereferenceable. A call through a pointer compares it with each candidate and
inlines the match:

* loaded from a vtable slot: the functions in the module's `_ZTV*` vtables with the call's signature
  (class hierarchy analysis);
* otherwise: every address-taken function with the call's signature.

A null pointer is PTR-NULL-DEREF. Any other value is the **havoc fallback**, a soft check
(`NEEDS-HARNESS` when reachable). Candidate choice affects only completeness, never soundness. More
than 16 candidates is `UNENCODED`. `testdata/pir/virt_dispatch.cpp`.

## Inline assembly

An `asm` call is `UNENCODED: inline assembly` unless the source states its effect on the asm line or
one of the three lines above:

```c
// prism: asm ensures r >= 0 && r <= 7
__asm__("movl %1, %0\n\tandl $7, %0" : "=r"(r) : "r"(x));
```

`cond` is `true` or comparisons of the output with integer constants joined by `&&`. PRISM assumes
the block writes only its integer output and that `cond` holds after it. The contract goes in
`extra.assumptions`; a proof is **PROVED-ASSUMING**. Memory operands, several outputs or non-integer
outputs stay `UNENCODED`. `testdata/pir/asm_contract.c`.

## Encoder and verdicts

Bounded unrolling over the loop nest (`encode.cpp`): each block is instantiated once per vector of
iteration counts of its enclosing loops; a back edge increments its loop's count; count = `--unwind`
(default 8) is an *unwinding cut*. Phis are `ite` chains over edge guards; each check contributes
`reach ∧ violation`; assumes contribute `reach → cond`. More than 6000 block instances is UNKNOWN.

A function with loops is first checked at unwind 4 when the requested unwind is larger. Only exact
answers are taken from it: FAILED, or PROVED with every loop closed (`extra.unwind`,
`extra.unwind_requested`). Anything else is decided at the requested unwind
(`extra.unwind_first_tried`). Certified mode skips the first try. A single loop still BOUNDED is
re-checked at twice the unwind, up to `PRISM_PIR_DEEPEN_MAX` (default 64, 0 = off), while the
function budget lasts; only a FAILED or a closed PROVED replaces the answer.

Verdicts the stage emits (definitions: docs/VERDICTS.md):

* **FAILED**: a check is satisfiable. `extra.cex` holds the parameter values, `extra.nondet` the
  values of the function's own `__VERIFIER_nondet_*` calls (replayed by the SV-COMP wrapper).
* **PROVED**: no check satisfiable and no unwinding cut reachable.
* **PROVED-UNBOUNDED**: BOUNDED, plus a closed k-induction step (one loop, k = 1 or 2) or closed
  loop invariants (`extra.k_induction = "closed-invariants"`). Only a closed step promotes (Law 2).
* **PROVED-ASSUMING**: proved under a stated precondition (pointer contract, asm contract, template
  draft).
* **PROVED-CERTIFIED**: PROVED, and every VC certified ("Solving").
* **BOUNDED**: no violation within the bound, but a cut is reachable.
* **NEEDS-HARNESS**: an `UNENCODED` construct, a reachable soft check, a Law 6 pointer parameter, or
  a violation that depends on unstated global state.
* **UNKNOWN**: solver unknown, unrolling too large, memory budget reached.
* **TIMEOUT**: the function budget ran out before every VC was answered.
* **ERROR**: the unit did not compile, or translation validation diverged.
* **NOTRUN**: clang/opt/Z3 or the sysroot missing, or the models could not be built.

`pir::pir_vcs(fn, unwind)` returns one solver-neutral SMT-LIB2 VC per property plus the unwinding
VC. `prism --pir-vcs FILE --out DIR` writes them and `prism --solve-smt2 VC [--z3-only]` answers one
(for `prism-qa solver-bench`).

<a id="solving-roadmap-31--32"></a>

### Solving

`check_function(fn, CheckOptions)` sends each VC to the solver library, `prism::solver::solve`
([SOLVERS.md](SOLVERS.md)): the portfolio (Z3 in-process, CaDiCaL/Kissat on bit-blasted CNF,
Bitwuzla when present, ProbSAT for counterexamples) and the query cache (`--solver-cache DIR`,
default `$XDG_CACHE_HOME/prism/solver`). Each query gets `max(2, cores / --jobs)` members and at
most `--timeout` seconds (default 30).

One VC per property (`assumptions ∧ reach ∧ violation`) is asked in order; the first SAT is the
FAILED verdict. With more than 16 properties a grouped "some property is violated" query comes
first, and unanswered groups are halved down to 16. When every property is UNSAT and a cut is
reachable, the unwinding VC decides PROVED (UNSAT) or BOUNDED. The k-induction step goes to Z3
in-process. `extra.solver` says which member answered each VC.

**Certified mode** (`--certified`). VCs are answered with plain queries first (one combined query
when there are two or more); certificates are asked for only once the function is PROVED. The
combined formula is bit-blasted to CNF, CaDiCaL writes an LRAT proof and cake_lpr must accept it;
with the Lean-proved bit-blaster (`prism-bitblast`) the Lean LRAT checker must accept it too. A
proof too large for a checker is split, down to per-VC proofs (`extra.certificate_scope`). A
function is PROVED-CERTIFIED only when every VC is certified (`extra.certificate = "checked"`,
`certificate_info`, `cnf_sha256`); otherwise it stays PROVED and `extra.certify_note` says why.
BOUNDED and PROVED-UNBOUNDED are never certified. Trust: docs/TRUSTED_BASE.md.

## k-induction with memory

The step case encodes the code before the loop concretely, then starts the loop from an arbitrary
header state and asks whether k violation-free iterations (k = 1, 2) can be followed by a violation.
For a function with memory the header state is the header phis **and the memory the loop may have
written**:

1. **Footprint.** Each store, `memcpy` and `memset` in the loop is mapped to its target object by
   the structural provenance analysis (`SymMem::known`). If some target is not resolved, the
   footprint is every non-`const` object allocated before the loop.
2. **Havoc.** At the header every byte of every footprint object gets an arbitrary value and tag
   (`SymMem::havoc_objects`). Its initialised flag becomes `old | arbitrary` when every write in the
   loop initialises what it writes; otherwise (a `memcpy` or a masked store) it is fully arbitrary.
3. **Allocation.** A loop that allocates, frees or restores the stack is not attempted
   (`extra.k_induction = "not-attempted (allocation or free in the loop)"`).

A closed step makes the function PROVED-UNBOUNDED; an open or unknown step leaves it BOUNDED
(Law 2). Evidence: `extra.k_induction_memory`, `extra.k_induction_footprint`. Precision is
object-granular: a loop that writes `d[1]` havocs all of `d`. The abstract argument is
`KInduction.kinduction_frame_sound` (`proofs/techniques/PrismTechniques/KInduction.lean`); the Lean
memory semantics still lacks the frame and provenance lemmas and per-byte initialised flags it would
need for PIR. Tasks: `testdata/pir/kind_mem.c`, `testdata/conformance/prism/kindmem/`.

## Loop invariants

When the unwinding assertion is open and k-induction does not close (or the function has several
loops), the stage searches for inductive loop invariants (`src/prism/pir/houdini.inc`). Only
invariants proved inductive are used, and only a closed proof changes the verdict (Law 2).

**Cut program.** The function is encoded with every loop cut once (`Encoding::cut_mode`). At each
header the *entry* state is recorded, then the phis and the loop's write footprint are havocked;
each back edge records the *next* state. The footprint follows "k-induction with memory", plus the
object a pre-loop pointer variable points to (`SymMem::havoc_pointed`). Loops that allocate or free
are not attempted (`extra.invariants_note`).

**Houdini.** Candidates are templates: comparisons, parity masks and differences of header phis,
scope values and constants, and quantified memory facts over an index prefix (`∀x < i: p[x] != 0`,
`p[x] == q[x]`, `p[x]` initialised, …). A candidate survives when it holds at entry (base) and at
every back edge (step), with the loop's survivors assumed in its havocked state. At the fixpoint the
function is PROVED-UNBOUNDED when no property of the cut program is violated with every survivor
assumed (`extra.pir_invariants`). An early optimistic query with all live candidates stops a search
that cannot prove. Outcomes are cached in `<solver cache>/houdini/`; a cached proof is re-checked.

**Why it is sound.** By induction over header visits before the first violation, every survivor
holds at each visit; havoc assumptions are guarded by "no property violated before this header". The
argument is proved in Lean for a structured model of the encoding (`LoopCut.loopcut_sound`,
`LoopCut.encViol_iff` in `proofs/techniques/PrismTechniques/LoopCut.lean`;
docs/PROOFS_TECHNIQUES.md); `tests/cpp/test_repolint.cpp` locks the encoder facts the model relies
on.

`extra.invariant_loops` and `extra.invariant_conjuncts` feed SV-COMP correctness witnesses
(docs/SVCOMP.md). Tasks: `libc-models/unbounded_string_contracts.cpp`, `testdata/pir/loops.c`.

## Budgets and reruns

| setting | default | effect |
|---|---|---|
| `--timeout S` | 30 | each VC query |
| `--unwind N` | 8 | loop bound |
| `PRISM_FUNCTION_BUDGET` | max(300 s, 10 × timeout); 0 = none | all plain queries, k-induction and Houdini of one function; spent → TIMEOUT, or the BOUNDED stays with a note |
| `PRISM_CERTIFY_BUDGET` | max(240 s, 8 × timeout); 0 = none | certificate queries of one function; spent → stays PROVED with `certify_note` |
| `PRISM_HOUDINI_BUDGET` | max(3600 s, 120 × timeout); 0 = none | Houdini seconds for the whole run |
| `PRISM_HOUDINI_FINAL_RESERVE` | 0.3 | share of the function budget kept for Houdini's final query |
| `PRISM_PIR_DEEPEN_MAX` | 64; 0 = off | largest unwind a BOUNDED single loop is re-checked at |
| `PRISM_PIR_BATCH` | all units | units per batch |
| `PRISM_PIR_DIVERT_S` | off | seconds in the main process before an UNKNOWN is re-checked in a child |
| `PRISM_PIR_MEM_MB` | — | memory budget when `PRISM_MEM_BUDGET_MB` is unset |
| `PRISM_PIR_DUMP_INVARIANTS`, `PRISM_HOUDINI_TRACE`, `PRISM_PIR_PROGRESS`, `PRISM_PIR_NO_HEADERS` | off | diagnostics |
| `PRISM_PIR_LEAN_EXPORT`, `PRISM_PIR_EXPORT_ONLY` | off | Lean export ("Translation validation") |
| `PRISM_PIR_ONLY` | — | internal: set in rerun children |

The registry with one line each is `prism::env_vars()` in `src/prism/config.cpp` (docs/USER_GUIDE.md
"Environment variables"). Houdini gets at most six times `--timeout` per search. A budget value that
is not a number of seconds ≥ 0 is ignored and every checked row says so in `extra.env_rejected`. The
function budget covers both unwind attempts and the global-state re-checks of the same function
(`tests/cpp/test_pir_budget.cpp`).

**Reruns.** Past Z3's hard memory limit Z3 cannot be used again in the process. The remaining
functions are marked `pir_rerun = pending` and checked again by a child PRISM (same command line,
pir only, `PRISM_PIR_ONLY=<unit>\t<fn>…`) whose verdicts replace the UNKNOWN rows
(`rerun_after_oom`, Linux only; elsewhere the rows stay UNKNOWN and say so).

## Memory budget

Law 7: running out of memory is a reported UNKNOWN, never a kernel OOM kill.
`src/prism/membudget.cpp` fixes one budget per process: `$PRISM_MEM_BUDGET_MB` (or
`$PRISM_PIR_MEM_MB`), else 75% of the smallest of the cgroup limit, MemAvailable at start and half
of RLIMIT_AS. The process is over budget at 80% of it in resident set, or when MemAvailable falls
under `$PRISM_MEM_RESERVE_MB`. The encoder (sampled), every query and a watchdog that interrupts
live Z3 contexts check it; the function then ends UNKNOWN and the next one runs normally. A call
that ignores interrupts for `PRISM_MEM_STUCK_MS` is failed through Z3's hard limit and the rest is
rerun ("Budgets and reruns"). Tests: `tests/cpp/test_membudget.cpp`.

<a id="translation-validation-roadmap-24"></a>

## Translation validation

For every function with a verdict, the PIR interpreter runs 64 input vectors (the counterexample,
boundary values, seeded random mixes) and drops inputs with a violation, failed assume, stop, step
limit or havoc-dependent return. A driver calling the function on the rest is appended to the
lowered module and run with `lli` in the Law 9 sandbox. Without `--allow-exec`, `extra.tv = "NOTRUN
(needs --allow-exec)"` and one stage-level NOTRUN row record the gap; the verdict stands. Agreement
is `PASS (n inputs agree with lli)`. A mismatch on a UB-free input turns the verdict into **ERROR**
(`extra.verdict_before_tv`) and drops any certificate. Nondet sources and object parameters are
`PARTIAL`.

This is testing, not proof. With `PRISM_PIR_LEAN_EXPORT=DIR` each translated function is also
written as an (LLVM fragment, PIR) pair; `prism-qa pir-lean-check` re-translates it with the Lean
translator the refinement theorems are about and checks the C++ output matches (`export_lean.cpp`,
docs/PROOFS_REFINEMENT.md).
