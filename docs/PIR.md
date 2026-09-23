# PIR: the Clang/LLVM front end (roadmap Part 2, first slice)

The `pir` stage runs right after `bmc` (C++ engine only; the Python engine is
frozen as an oracle, roadmap D8, and records one `NOTRUN` row for the stage).

```
C/C++ unit ──clang -O0──▶ LLVM IR ──PRISM instrumentation──▶ opt (fixed passes)
          ──▶ textual IR parser ──▶ PIR (+ property checks) ──▶ Z3 bitvectors
          ──▶ FAILED / PROVED / BOUNDED / PROVED-UNBOUNDED / NEEDS-HARNESS
          ──(--allow-exec)──▶ translation validation against lli
```

Sources: `include/prism/pir.hpp`, `src/prism/pir/{ir_parser,pir,translate,encode,stage}.cpp`;
memory model `src/prism/pir/{memory,translate_mem,stage_mem,contracts}.cpp`; library
models `src/prism/pir/{libc_models,libc_format}.cpp` + `src/prism/pir/models/libc/*.c`.
Tests: `tests/pir/*` (true and false variants; `mem_*.c` for the memory
model), `tests/test_pir.py`, doctests `pir: …` in `tests/cpp/test_main.cpp` and
`pir mem: …` in `tests/cpp/test_pir_mem.cpp`. Differential oracle: `tools/pir_vs_bmc.py`.

## Front end

Clang and opt are external processes (`detail::run_process`, argv, no shell);
nothing is linked yet. For every `.c` unit:

```
clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone -fno-discard-value-names \
      -gline-tables-only -std=c17 <UB-folding warnings on> file.c
opt -passes=mem2reg,lowerswitch,loop-simplify,lcssa,instnamer -S
```

`.cc/.cpp/.cxx` units use `clang++ -std=c++23` and the same passes. No
optimising pass runs (they may exploit the UB being checked) and no
`-fsanitize` check is inserted (Law 8: PRISM owns the check polarity). For C
units `-Wno-error=implicit-function-declaration` keeps a unit with an
undeclared call compiling (C89 rule); the call itself is an unknown external
and becomes `UNENCODED: call @f` — leniency of the front end, not a disabled
check. Compiling scanned code does not execute it, so the stage needs no
`--allow-exec`; clang/opt missing is `NOTRUN` with an install hint; a unit
that does not compile is one `ERROR` row ("clang front end failed: …").

### Instrumentation before mem2reg

Three things would be lost by `mem2reg` or by clang's constant folding, so
PRISM rewrites the `-O0` IR before `opt`:

* **Uninitialised locals.** Every promotable `iN` alloca (except `%retval`)
  gets `store (call @__prism.uninit.iN()), %x`. After mem2reg a read before
  the first store is a use of that call (mem2reg would otherwise fold the
  `undef` away). PIR tracks a one-bit "uninit" shadow through phis and checks
  every non-phi use: `UNINIT-READ`.
* **Folded UB to poison.** Clang folds e.g. `7 % 0`, `INT_MIN / -1`,
  `1 << 40` to `poison`; each `iN poison` operand becomes
  `call @__prism.poison.iN()` at that instruction, which PIR turns into a
  reachability check (`UB-POISON`).
* **Folded UB to a value.** `1 << 31`, `INT_MAX + 1`, `-1 << 3` fold to plain
  constants with only a warning (`-Wshift-sign-overflow`, `-Winteger-overflow`,
  `-Wshift-negative-value`, `-Wshift-overflow`). The warning's line:column is
  mapped to the instruction on that line with the largest column not past the
  warning, and `call @__prism.folded(i32 k)` is inserted there (a reachability
  check with the warning's class). A warning with no instruction on its line
  makes the enclosing function `NEEDS-HARNESS` ("clang-folded UB … not
  attributable"). Warnings inside headers are not attributed.

**C signed left shift.** IR does not say whether `shl` came from a signed C
`<<`. For C units that contain a `shl`, clang runs a second time with
`-fsanitize=shift-base` only to *locate* signed shifts (the ubsan source
locations); nothing from that build is used otherwise. Each `shl` whose
`!dbg` line:column matches gets the C17 6.5.7p4 check (negative base, or
`a·2^b` not representable). C++20 and later define signed `<<`, so C++ units
get no base check (`tests/pir/cxx.cpp:cxx_shl_ok` is PROVED).

## IR parser

`ir::parse_module` is a tokenizer plus recursive-descent parser for what
`opt -S` prints: types (`iN`, `ptr`, floats, structs, packed structs,
arrays, vectors, function types), values (locals, globals, integer literals,
`true/false/undef/poison/null/zeroinitializer`, constant expressions kept
opaque), metadata attachments (`!dbg`), `DILocation`, `DISubprogram`,
`DIFile`. Every instruction line is parsed; opcodes outside the subset keep
their name (`parsed = false`) so the translator can report them.

## PIR

Typed SSA over bitvectors of width 1..64 (`include/prism/pir.hpp`):

* statements: `assign` (operators below), `check` (PRISM property:
  violation when the i1 argument is 1), `assume`;
* terminators: `jmp`, `br`, `ret`, `stop` (path ends without returning,
  e.g. `exit`);
* phis per block; loops are back edges.

Operators: `add sub mul udiv sdiv urem srem shl lshr ashr and or xor`,
comparisons, `select zext sext trunc`, `smax smin umax umin abs ctlz cttz
ctpop bswap`, `copy`, `havoc`, and the i1 property predicates
(`sadd.ovf … sdiv.ovf shift.oob shl.signed.ovf shl.nsw.ovf shl.nuw.ovf
lshr/ashr.inexact udiv/sdiv.inexact`). `to_text` prints PIR; its SHA-256 is
`extra.pir_hash`.

### Properties (inserted by PRISM, Law 8)

| IR | check | class |
|---|---|---|
| `add/sub/mul nsw` (clang marks signed C arithmetic `nsw` exactly when overflow is UB) | signed overflow | INT-SIGNED-OVF |
| `sdiv/srem` | divisor 0; `INT_MIN / -1` | INT-DIV-ZERO / INT-SIGNED-OVF |
| `udiv/urem` | divisor 0 | INT-DIV-ZERO |
| `shl/lshr/ashr` | amount ≥ width (negative amounts included) | INT-SHIFT-UB |
| C signed `<<` (located as above) | negative base or overflow | INT-SHIFT-UB |
| `nuw`, `exact`, `or disjoint`, `zext nneg` | the poison condition | UB-POISON |
| `llvm.abs(x, true)` | `x == INT_MIN` | INT-SIGNED-OVF |
| `llvm.ctlz/cttz(x, true)` | `x == 0` | INT-CLZ-ZERO |
| `unreachable`, `llvm.trap` | reached | CXX-UNREACHABLE |
| `__assert_fail`, `reach_error`, `__VERIFIER_error`, `abort` | reached | FUNC-CONTRACT |
| uninitialised local read | reached with shadow set | UNINIT-READ |
| clang-folded UB | reached | per warning / UB-POISON |

Modelled calls: `llvm.{s,u}{add,sub,mul}.with.overflow` (+ `extractvalue`),
`llvm.{s,u}{max,min}`, `abs`, `ctlz`, `cttz`, `ctpop`, `bswap`, `expect`,
`assume`; `llvm.dbg.*` etc. ignored; `exit/_Exit/_exit/quick_exit` end the
path; `__VERIFIER_nondet_*` / `nondet_*` are havocs; `__VERIFIER_assume` is
an assume. Calls to functions defined in the same unit are **inlined**
(depth 4; recursion is `UNENCODED: recursive call @f`).

### What is not encoded (named, never dropped — roadmap 2.1)

Everything else makes the function `NEEDS-HARNESS` with
`UNENCODED: <construct>`: floating point, integers wider than 64 bits,
external calls without a library model (`UNENCODED: call @f`), `switch` if
lowerswitch did not run, a reachable throw whose exception would reach
catch or cleanup code (see "C++ library"), irreducible control flow,
vectors and first-class aggregates, `ptrtoint` other than pointer
differences, `inttoptr` other than of 0, volatile/atomic loads and stores,
`thread_local` globals, function pointers, extern arrays of unknown size,
`llvm.lifetime.*`, and `setjmp`/`longjmp` (`UNENCODED: call @_setjmp`; not
modelled as exception-like edges yet). Pointer parameters are
`NEEDS-HARNESS` by Law 6 unless a precondition gives the object size
("Pointer parameters" below; C++ member functions have `this`). In C++
units, a function whose source contains `const_cast` is
`UNENCODED: const_cast`. Known gaps of the *property set* (a PROVED means
these were not checked): reading an indeterminate value that is only copied
(`int y = x;` with `y` unused — the read has no IR instruction after
mem2reg), falling off the end of a non-void C function (UB only when the
caller uses the value), sub-array bounds of the last struct field
(flexible-array idiom) and dereferences of a one-past-the-end sub-array
address computed separately, memory leaks (valid-memtrack), effective types unless `--strict-aliasing`, pointer
arithmetic on an already freed object, and paths that leave the function by
a C++ library throw (`std::__throw_*`: the path ends; the finding lists them
in `extra.throws_not_followed`).

## Memory model (roadmap 2.5)

Follows ESBMC/SMACK and the Lean model `proofs/semantics/PrismSem/Memory.lean`.

**Pointers** are one 64-bit PIR value: object id in bits 63..48 (`0` is the
null object, never allocated), byte offset in bits 47..0. Every object is
smaller than 2^47 bytes and every `getelementptr` is checked to stay inside
its object (or one past its end), so on every execution without a reported
violation the packed value *is* the Lean pair `(obj, off)`: `p + d` keeps the
object bits and adds `d` to the offset (proof sketch: `obj·2^48 + off + d ≡
obj'·2^48 + off' (mod 2^64)` with `off, off' ∈ [0, 2^48)` and `d ∈ [-2^63,
2^63)` forces `obj' = obj`, `off' = off + d` exactly when the checked
offset stays in range).

**Objects** carry size (64-bit, possibly symbolic: VLAs, `malloc(n)`),
liveness, allocation kind (`stack`, `heap`, `static`, `const` (string
literals, `constant` globals), `new`, `new[]`, `extern` (contract objects),
`FILE`) and base alignment. Every memory byte is a *cell*: value (8 bits),
initialised (1 bit) and, with `--strict-aliasing`, an effective-type tag.

**PIR statements**: `alloc` (size, kind, initial contents: uninitialised,
zero, or initialised with arbitrary bytes), `free` (end of lifetime),
`load` (value + "some byte uninitialised" + "effective-type mismatch"),
`store`, `memcpy` (memmove semantics), `memset`, `stacksave`,
`stackrestore`; queries `obj.size`, `obj.live`, `obj.kind`, `obj.align`
usable in any expression. The translator (`translate_mem.cpp`) turns every
property into ordinary `check`s over these (Law 8):

| IR | check | class |
|---|---|---|
| load/store through object 0 | null dereference | PTR-NULL-DEREF |
| load/store, object id not allocated | wild pointer | PTR-INVALID-DEREF |
| load/store, object not live | use after free / after its lifetime | MEM-UAF |
| load/store, `off + n > size` | out of bounds | MEM-OOB-READ / MEM-OOB-WRITE |
| load/store `align a`, `off % a ≠ 0` or object alignment `< a` | misaligned access | MEM-MISALIGNED |
| store to a `const` object | write to read-only memory | MEM-WRITE-CONST |
| load of a byte never written | uninitialised memory read | UNINIT-READ |
| load, effective type differs (`--strict-aliasing`) | strict aliasing | MEM-STRICT-ALIAS |
| `getelementptr` offset `idx·size` / sum overflows | offset overflow | MEM-PTR-ARITH |
| `getelementptr inbounds` on null with offset ≠ 0, or result outside `[0, size]` | invalid pointer arithmetic | MEM-PTR-ARITH |
| `getelementptr` index into a nested array `[N x T]` (not the last struct field): `≥ N` when dereferenced, `> N` otherwise | sub-array bounds (`a[1][7]` in `int a[4][5]`, C17 J.2) | MEM-PTR-ARITH |
| `icmp ult/ule/…` on pointers, `ptrtoint`-`sub` | pointers into different objects | PTR-COMPARE |
| `llvm.memcpy` (not memmove) | overlapping ranges | MEM-OVERLAP |
| `free`/`delete`/`delete[]`/`fclose` | not an object, not its start, not heap memory | MEM-INVALID-FREE |
| same | allocated by another allocator (malloc/new/new[]) | MEM-MISMATCHED-FREE |
| same | already released | MEM-DOUBLE-FREE |
| `alloca T, iN n` (VLA) | `n <= 0` (C17 6.7.6.2p5), size overflow, size `>= 2^47` | MEM-VLA-SIZE |
| `ret` of a pointer | into the function's own stack object | MEM-STACK-ESCAPE |

Lifetimes: `alloca`s of an inlined callee end at its `ret`; VLAs end at
`llvm.stackrestore` (objects allocated after the matching `stacksave`).
Struct layout, field offsets, sizes and alignments come from the module's
`target datalayout` (`Layout`, x86-64 defaults). A `byval` parameter is a
fresh copy for the callee; `sret` is a fresh uninitialised return slot.

**Uninitialised bytes.** A load checks that its bytes are initialised
(UNINIT-READ), except an integer load of a whole aggregate location (the
ABI coercion of a struct passed or returned by value, e.g. `load i64` of a
`std::optional<int>`): padding bytes may legitimately be indeterminate, so
such a load carries a per-byte mask that follows the value (stores copy it
back to memory, inlined calls and returns pass it on) and is checked only
where the value is used.

**Global state.** Globals referenced by the function are allocated in a
prologue. `constant` globals hold their initializer; mutable globals hold
arbitrary initialised bytes, except when the function is `main` (program
entry: initializers). A `FAILED` that disappears when the mutable globals
hold their initializers is reported `NEEDS-HARNESS` ("the global state the
function is called in is an unstated precondition"; `extra.globals`).
`stdin`/`stdout`/`stderr` point to valid `FILE` objects.

### Encodings

The unrolled program is a DAG encoded in topological order, and every memory
update is guarded by the reach condition of its block instance: guards of
instances off the executed path are false, so one global memory state is
exact (no merge at joins). Each `alloc` instance of the DAG runs at most once
per path and gets the next constant object id (the Lean `next` counter).

* `MemEncoding::Array` (`EncodeOptions`, unbounded mode): one SMT array
  from address to cell; ranged writes (`memcpy`, `memset`, arbitrary-byte
  initialisation) are lambdas. Z3 handles the lambdas of symbolic-length
  `memcpy` poorly (a random pointer program: 53 s vs 0.24 s for Bv), so it
  is not the stage default.
* `MemEncoding::Bv` (default of `check_function`, the stage and `pir_vcs`;
  QF_BV, what the certified back end accepts):
  arrays eliminated. A load is an ite chain over the guarded writes before
  it (read-over-write, as `PrismSem/MemEncode.lean` `menc`), writes to other
  objects are skipped when both object ids are known; arbitrary initial bytes
  are fresh variables with pairwise Ackermann constraints
  (`a = a' → h = h'`). The VCs are QF_BV.

`tests/cpp/test_pir_mem.cpp` checks that both encodings give the same verdict
and class on every property (true and false variants) and that the PIR
interpreter (`interpret`, concrete memory `ConcMem`) reproduces each
counterexample. k-induction is not attempted for functions with memory
(`extra.k_induction = "not-attempted (memory)"`): their loops are PROVED
only when the unwinding assertion closes, else BOUNDED.

### Correspondence to the Lean model

| `PrismSem/Memory.lean` | C++ |
|---|---|
| `Ptr = (obj, off : BitVec 64)` | packed 16/48 bits (faithful on violation-free runs, above) |
| `Obj.size/live/data` | `SymMem::Obj::size/alive` + byte cells (`ConcMem` concretely) |
| `MState.next` (fresh ids from 1) | constant id per `alloc` instance, in encoding order |
| `MCond.live` / `inBounds` / `atBase` | `obj.live`; `off + n <= obj.size`; `off == 0` in the free checks |
| `mrun` `.load`/`.store` UB (dead or out of bounds) | MEM-UAF / MEM-OOB-* / PTR-NULL-DEREF / PTR-INVALID-DEREF checks |
| `mrun` `.free` UB (dead or not at base) | MEM-DOUBLE-FREE / MEM-INVALID-FREE checks |
| `MemEncode.menc` select chains, `MSym.WF` congruence | `MemEncoding::Bv` read-over-write + Ackermann constraints |
| `uaf_is_ub`, `double_free_is_ub` | doctests `pir mem: every property …` |

Not covered by the Lean proofs (trusted C++ code): multi-byte little-endian
loads/stores (sequences of byte cells), the uninitialised-byte shadow (Lean
objects are zero-filled), allocation kinds and their free checks, ranged
`memcpy`/`memset`, alignment, the Array encoding, and the packing argument
above (a paper proof).

## Pointer parameters (Law 6)

A pointer parameter stays `NEEDS-HARNESS` unless a precondition gives the
size of its object, in the comment block right before the function or the
leading comment lines of its body:

```c
// requires: \valid(p + (0..n-1))        n elements, n an int parameter
// requires: \valid(p + (0..7))          8 elements
// requires: \valid(p)                   one element
// requires: \valid_read(p + (0..n-1))   read-only (writes are MEM-WRITE-CONST)
/*@ requires \valid(p + (0..n-1)); */   ACSL block
```

With `--pir-drafts` (opt-in; the default keeps Law 6's NEEDS-HARNESS) and
without such a clause (C units), the deterministic template harness draft
(`ai::draft_harness`, the one the `harness` stage uses; never an LLM here,
Law 4) may supply the size: "`a` points to exactly `n` int elements"
(a length parameter) with its drafted range "`1 <= n <= 4`", or "at least
K elements" when an early return bounds the index. A size read off the
largest literal index is not used (it would make exactly those accesses
in bounds by construction). The draft's assumptions are listed like a
`requires` clause (`source: harness (template draft)`). A violation found
only under a drafted precondition is reported NEEDS-HARNESS (the
precondition was invented), never FAILED.

The element size is that of the first access through `p` in the IR. `p` is
bound to a fresh `extern` object of `max(n, 0) × size` bytes, initialised
with arbitrary bytes, 16-byte aligned; several such parameters are distinct
objects. A proof is then **PROVED-ASSUMING**, never PROVED, with every
assumption in `extra.assumptions` and the message; a violation is FAILED
under the stated precondition. Translation validation does not replay such
functions (`tv = PARTIAL`).

## Library models (roadmap 2.6)

Operational models are C files in `src/prism/pir/models/libc/`
(`string.c`, `stdlib.c`, `stdio.c`, `prism_model.h`), embedded in the binary
at build time (CMake), lowered once per run with the same clang/opt pipeline
(`-fno-builtin -ffreestanding`), and linked into every unit that declares a
modelled symbol and does not define it (`link_models`, on the parsed
module: model metadata and private globals are renamed, so no `llvm-link`
run is needed). Model code is checked like user code, so the models'
preconditions (`requires:` comments) are enforced by the memory model;
checks inside a model report the call site's line and name the model
("… (in the strcpy library model)"). Model intrinsics `__prism_alloc`,
`__prism_free`, `__prism_check`, `__prism_assume`, `__prism_memcpy`,
`__prism_havoc_bytes`, `__prism_fresh_cstr`, … are PIR statements
(`translate.cpp` `model_intrinsic`).

| models | behaviour |
|---|---|
| `malloc`, `calloc`, `realloc`, `free` | may fail (NULL); calloc zero-fills and checks `n*size` overflow; `realloc(p, 0)` frees p and returns NULL (glibc) |
| `operator new/new[]/delete/delete[]` (`_Znwm` … `_ZdaPvm`) | never NULL; kinds checked against the matching delete |
| `strlen`, `strnlen`, `strcpy`, `strncpy`, `strcat`, `strncat`, `strcmp`, `strncmp`, `strchr`, `strrchr`, `strdup` | C loops over the bytes (bounds and termination checked) |
| `memcpy`, `memmove`, `memset`, `memcmp`, `memchr` | ranges checked; memcpy overlap |
| `abs`, `labs`, `llabs` | `-x` is a checked `sub nsw` (abs(INT_MIN)) |
| `atoi`, `atol`, `strtol`, `strtoul` | argument must be a string; value unknown |
| `getenv` | NULL or a read-only string of unknown contents |
| `rand`, `srand` | unknown non-negative value |
| `fopen`, `fclose`, `fgets`, `fread`, `fwrite`, `fgetc`, `getc`, `getchar`, `fputc`, `putc`, `putchar`, `fputs`, `puts`, `fflush` | fopen may fail; FILE objects checked (double fclose); input bytes unknown |
| `printf`, `fprintf`, `dprintf`, `sprintf`, `snprintf` (translator, `libc_format.cpp`) | literal format: argument count and IR types per conversion (FMT-ARGS), `%n` (FMT-PERCENT-N), `%s` arguments must be strings, sprintf/snprintf write up to the maximal output length into the buffer; non-literal format, `v*printf`, `scanf` are UNENCODED |

Unmodelled external calls stay `NEEDS-HARNESS` (`UNENCODED: call @name`).
If the models cannot be built (no clang), the stage records one ERROR row
saying so (Law 7).

**C++ library.** C++ units are compiled with `-D_GLIBCXX_ASSERTIONS`, which
turns on libstdc++'s own precondition checks; library code is ordinary IR
and is inlined (depth 12 for C++). A reachable `std::__glibcxx_assert_fail`
is a violation: bounds (`__n < this->size()`, `std::span`, `std::array`
`operator[]`) are MEM-OOB-READ, `std::optional::operator*` on an empty
optional is CXX-OPTIONAL-NULL, `std::unique_ptr::operator*` on null is
PTR-NULL-DEREF. `invoke` is translated as the call followed by its normal edge; landing
pads and the cleanup code after them are not translated (exception edges
are lowered by separate work, roadmap 2.6 "Exceptions"). A throw PRISM sees
(`std::__throw_*`, e.g. `vector::at`, `optional::value`, the vector
length check) ends its path and is listed in `extra.throws_not_followed`:
when no enclosing `invoke` exists the exception leaves the analysed
function (not UB); under an `invoke` whose landing pad calls
`std::terminate` (a `noexcept` boundary) it is a violation
(CXX-THROW-NOEXCEPT); under a landing pad with catch or cleanup code the
throw is a *soft* check: if it is reachable the function is
`NEEDS-HARNESS` ("UNENCODED: exception path reachable …"), never PROVED
and never FAILED. Throws in user code (`__cxa_throw`) stay
`UNENCODED: call @__cxa_allocate_exception`. With this, `std::vector`
(`operator[]`), `std::span`, `std::array`, `std::optional` (`operator*`),
`std::unique_ptr` (`operator*`) and `std::string_view` work
(`tests/pir/mem_stl.cpp`). The platform C++ library is
libstdc++ (D7, Linux); libc++ would need the same treatment of
`_LIBCPP_HARDENING_MODE`.

## C features

* `_Generic`: resolved by clang (`tests/pir/mem_libc.c:generic_ok` PROVED).
* VLAs: `alloca T, iN n` with a symbolic size; the size must be positive
  (checked on the value before its `zext`/`sext`) and below 2^47; the
  object ends at `llvm.stackrestore`.
* `setjmp`/`longjmp`: `NEEDS-HARNESS` (`UNENCODED: call @_setjmp`); modelling
  them as exception-like edges is future work (roadmap 2.6 stretch).

## Encoder and verdicts

Bounded unrolling over the loop nest: each block is instantiated once per
vector of iteration counts of its enclosing natural loops (outer → inner); a
back edge increments its loop's count; count = `--unwind` is an *unwinding
cut*. Values follow SSA dominance (LCSSA makes every use see its definition in
the same iteration of each shared loop). Phis are `ite` chains over incoming
edge guards; each check contributes `reach ∧ violation`; assumes contribute
`reach → cond`.

* **FAILED** — some check is satisfiable. `counterexample` = `extra.cex` =
  parameter values (signed decimal), `extra.prop`, `cls`, `line`.
* **PROVED** — no check is satisfiable and no unwinding cut is reachable
  (loop-free, or every loop closes within the bound: the unwinding assertion
  is proved).
* **BOUNDED** — no violation within the bound, but a cut is reachable.
* **PROVED-UNBOUNDED** — BOUNDED plus exactly one loop whose k-induction step
  closes (k = 1, 2): from an arbitrary header state (header phis havocked),
  k violation-free iterations that loop back imply iteration k and the code
  after the loop are violation-free. Same discipline as the old stage: only a
  closed step promotes, BOUNDED is never folded into a proof (Law 2).
* **UNKNOWN** — solver unknown / timeout / unrolling over 6000 block
  instances.

`std::vector<pir::Vc> pir::pir_vcs(fn, unwind)` returns one solver-neutral
SMT-LIB2 VC per property (SAT = violated) plus the unwinding VC, for the
certified back end (roadmap 3.2).

## Translation validation (roadmap 2.4)

For every function with a verdict: 64 input vectors (the counterexample,
`INT_MIN, -1, 0, 1, INT_MAX` per width, then seeded random mixes). The PIR
interpreter runs each; inputs where PIR reports a violation, an assume
failure, a stop, a step-limit or a return that depends on a havoc are
excluded (no UB inputs are compared). A driver `@__prism_tv_main` calling the
function on the remaining inputs is appended to the lowered module
(`__prism.*` markers get trivial bodies), reduced with
`opt -passes=internalize,globaldce`, and run with
`lli --entry-function=__prism_tv_main` inside the Law 9 sandbox
(`sandbox::wrap_argv` = bwrap, plus `prlimit` with the sandbox rlimits).

* without `--allow-exec`: `extra.tv = "NOTRUN (needs --allow-exec)"`,
  `extra.exec = NOTRUN`, one stage-level NOTRUN row; the verdict stands and
  the missing validation is recorded, not hidden;
* agreement: `extra.tv = "PASS (n inputs agree with lli)"`;
* any mismatch or missing lli result on a UB-free input: the verdict becomes
  **ERROR** "translation validation diverged: f(args) PIR=… lli=…"
  (`extra.verdict_before_tv` keeps the old one);
* functions using nondet sources: `PARTIAL (… not validated)`.

## Differential oracle vs the old encoder (roadmap 2.8)

`python tools/pir_vs_bmc.py testdata --bin build/prism` runs
`prism testdata --no-llm --stage inventory,classify,bmc,pir` and prints the
per-function matrix (rows = bmc, columns = pir):

Run of this branch (clang 18.1.3, `--unwind 8`, 1726 units: 1225 C, 498 C++):

```
functions: 3559 (bmc 3543, pir 2478)
rows = bmc, columns = pir
               PROVED*  BOUNDED   FAILED  NEEDS-HARNESS  ERROR  UNKNOWN  absent
PROVED*            829        0        0              5      0        0     348
BOUNDED              0        2        0              0      0        0       0
FAILED               3        0       25              8      0        0       1
NEEDS-HARNESS       40        0        7           1542      0        0     732
ERROR                1        0        0              0      0        0       0
UNKNOWN              0        0        0              0      0        0       0
absent               0        0        0             16      0        0       0
same bucket where both report: 2398/2462
hard conflicts (PROVED* vs FAILED): 3
```

pir verdicts on `testdata/`: 863 PROVED, 10 PROVED-UNBOUNDED, 2 BOUNDED,
32 FAILED, 1571 NEEDS-HARNESS, 523 units that clang rejects (ERROR), 0
UNKNOWN. With `--allow-exec`, of the 907 formal verdicts 891 passed
translation validation against `lli` (857 on all 64 inputs, the rest on the
UB-free subset) and 16 had no UB-free terminating input (`tv = NONE`);
**0 divergences**.

Independent check (not in CI; a scratch script): 8×12 random loop-free /
small-loop C functions per seed over `int/unsigned/short/long/unsigned char`
with `+ - * / % << >> & | ^ ?:` and casts, 5 seeds (≈480 functions):
every PROVED function ran ~220 inputs under `-fsanitize=undefined` with no
report (0 wrong PROVED), and every FAILED counterexample was confirmed by
UBSan except where clang had constant-folded the UB (UBSan cannot see it,
e.g. `(-5) << 7`).

### Hard conflicts (PROVED* vs FAILED) — all investigated

All three are **false alarms of the old bmc encoder** (a wrong FAILED, not a
wrong PROVED); PIR is right. `bmc.cpp` is left unchanged (it is the oracle):

1. `testdata/interval_ops.c::wrap_u_local`, `wrap_u_branch` — bmc reports
   `ovf+: INT-SIGNED-OVF` on `unsigned x = 2000000000; return x + 2000000000;`.
   Unsigned arithmetic wraps by definition (C17 6.2.5p9); there is no UB.
   Repro: `prism testdata/interval_ops.c --no-llm --stage inventory,classify,bmc,pir`.
2. `testdata/trunc.c::trunc_bad` — bmc reports `ovf+` with `n=0x40000000` on
   `char c = n; short s = n; char d = (char)n; return c + s + d;`. After the
   (implementation-defined, not UB) conversions `c,d ∈ [-128,127]`,
   `s ∈ [-32768,32767]`; the sum cannot overflow `int`. The old encoder keeps
   `c` and `s` as 32-bit values (it does not model the narrowing).

No wrong PROVED was found in either engine on `testdata/`.

### Other disagreements (not conflicts)

* bmc NEEDS-HARNESS → pir FAILED: new real defects PIR finds, e.g.
  `builtin_clz.c` / `clz_zero.c` (`__builtin_clz(0)`), `builtin_vacuous.c`
  (`__builtin_trap`, `__builtin_unreachable` reached), `wide_string.c` /
  `widech_unenc.c` (`L'A' + n` overflows), `sandbox_abort.c` (`abort()`).
* bmc NEEDS-HARNESS → pir PROVED: constructs Clang resolves that the old
  parser refused (`_Generic`, `_Alignas`, statement expressions, anonymous
  enums, `const` locals, `vector_size` locals left unused, `try` without
  throwing calls, `[[assume]]`, …).
* bmc FAILED/PROVED → pir NEEDS-HARNESS: arrays and address-taken locals
  (`alloca`, memory model not in this slice) and C++ member functions (`this`).
* Units that clang rejects (undeclared identifiers, missing headers in the
  lint-oriented `_api`/`_lint` snippets) are one `ERROR` row per unit; the
  old parser still extracts functions from them (the "absent" column).
