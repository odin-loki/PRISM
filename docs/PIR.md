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
floating point `src/prism/pir/{fp,translate_fp}.cpp`; exceptions, setjmp/longjmp,
indirect calls and inline assembly `src/prism/pir/translate_ctl.inc` (+ `lower_ctl.cpp`);
memory model `src/prism/pir/{memory,translate_mem,stage_mem,contracts}.cpp`; library
models `src/prism/pir/{libc_models,libc_format}.cpp` + `src/prism/pir/models/libc/*.c`.
Tests: `tests/pir/*` (true and false variants; `mem_*.c` for the memory
model; `fp_*`, `eh_*`, `coro_*`, `sjlj_*`, `virt_*`, `asm_*` for roadmap 2.6), `tests/test_pir.py`,
doctests `pir: …` in `tests/cpp/test_main.cpp`, `pir mem: …` in `tests/cpp/test_pir_mem.cpp`
and `pir3 …` in `tests/cpp/test_pir3.cpp`. Differential oracle: `tools/pir_vs_bmc.py`.

## Front end

Clang and opt are external processes (`detail::run_process`, argv, no shell);
nothing is linked yet. For every `.c` unit:

```
clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone -fno-discard-value-names \
      -gline-tables-only -std=c17 <UB-folding warnings on> file.c
opt -passes=mem2reg,lowerswitch,loop-simplify,lcssa,instnamer -S
```

`.cc/.cpp/.cxx` units use `clang++ -std=c++23` and the same passes with
LLVM's coroutine lowering in between (roadmap 2.3):
`function(mem2reg),coro-early,cgscc(coro-split),coro-cleanup,function(lowerswitch,fix-irreducible,loop-simplify,lcssa,instnamer)`
("Coroutines" below). `invoke`/`landingpad` are lowered to explicit
exception edges by the translator itself ("Exceptions"). No
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
* **Floating-point locals.** `alloca half|float|double` get the same
  initial `@__prism.uninit.<type>()` value (`lower_ctl.cpp`
  `uninit_fp_locals`): UNINIT-READ covers them too.
* **setjmp functions.** In a function that calls `setjmp`, every alloca
  gets a `call void @__prism.keep(ptr %x)` use so mem2reg keeps it in
  memory (`keep_setjmp_locals`): after a `longjmp` the program reads what
  the -O0 build reads (the last stored value), not the SSA value of the
  `setjmp` call ("setjmp/longjmp").

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
ctpop bswap`, `copy`, `havoc`, the i1 property predicates
(`sadd.ovf … sdiv.ovf shift.oob shl.signed.ovf shl.nsw.ovf shl.nuw.ovf
lshr/ashr.inexact udiv/sdiv.inexact`), and the IEEE operators on the bits
of half/float/double values (`fadd fsub fmul fdiv frem fsqrt ffma fmuladd
fminnum fmaxnum fminimum fmaximum ffloor fceil ftrunc fround froundeven`,
`fcmp.oeq/olt/ole/uno`, `fptosi fptoui sitofp uitofp fpconv`, the range
predicates `fptosi.ovf fptoui.ovf`, `fisnan fiszero fisinf`, and `libm`,
an unconstrained libm result; "Floating point" below). `to_text` prints PIR; its SHA-256 is
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
| `fptosi`/`fptoui` | value (truncated) outside the integer type, NaN, ±inf (C11 6.3.1.4p1) | FLOAT-CAST-OVF |
| `fdiv`/… under `--fp-checks` (opt-in) | division by ±0; NaN from non-NaN operands; infinite result from finite operands | FLOAT-DIV-ZERO / FLOAT-INVALID / FLOAT-OVERFLOW |
| exception reaching `__clang_call_terminate` | a `noexcept` function (or a destructor during unwinding) throws | CXX-THROW-NOEXCEPT |
| exception leaving `main` | uncaught: `std::terminate` | CXX-UNCAUGHT |
| `std::terminate()`, `throw;` with no handled exception | reached | CXX-TERMINATE |
| `__cxa_pure_virtual` | pure virtual call | CXX-PURE-VIRTUAL |
| `longjmp` | its `setjmp` caller has returned (C11 7.13.2.1p2) | CTRL-LONGJMP-INVALID |
| call through a null function pointer | reached | PTR-NULL-DEREF |

Modelled calls: `llvm.{s,u}{add,sub,mul}.with.overflow` (+ `extractvalue`),
`llvm.{s,u}{max,min}`, `abs`, `ctlz`, `cttz`, `ctpop`, `bswap`, `expect`,
`assume`; `llvm.dbg.*` etc. ignored; `exit/_Exit/_exit/quick_exit` end the
path; `__VERIFIER_nondet_*` / `nondet_*` are havocs; `__VERIFIER_assume` is
an assume. Calls to functions defined in the same unit are **inlined**
(depth 4; recursion is `UNENCODED: recursive call @f`).

### What is not encoded (named, never dropped — roadmap 2.1)

Everything else makes the function `NEEDS-HARNESS` with
`UNENCODED: <construct>`: floating-point formats other than
half/float/double (`long double` = x86_fp80, `__float128`, bfloat),
fast-math flags, fused multiply-add on half, integers wider than 64 bits,
external calls without a library model (`UNENCODED: call @f`), `switch` if
lowerswitch did not run, irreducible control flow (e.g. a `longjmp` from
code the `setjmp` does not dominate), vectors and first-class aggregates
other than the `{ ptr, i32 }` exception pair, `ptrtoint` other than pointer
differences, `inttoptr` other than of 0, volatile/atomic loads and stores,
`thread_local` globals, extern arrays of unknown size, dynamic exception
specifications (`__cxa_call_unexpected`, `filter` clauses), indirect calls
with more than 16 candidate targets, and inline assembly without a
contract. Some constructs end only *one path* instead of the whole function
(a *soft* check, `prop = unmodelled`: reachable means `NEEDS-HARNESS`,
never FAILED and never proved away): a landing pad whose code cannot be
translated, a thrown type whose match against a catch clause PRISM cannot
decide, an indirect call whose pointer is none of its candidate targets
(the "havoc fallback"), or an exception type it cannot identify. Pointer parameters are
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
arithmetic on an already freed object, reads of a non-volatile local
modified between `setjmp` and `longjmp` (C11 7.13.2.1p3 makes its value
indeterminate; PIR reads the stored value, as the -O0 build does), and
exceptions that leave a function other than `main` (not UB there: the
path ends and `extra.throws_not_followed` names the types).

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
counterexample. For functions with memory, the k-induction step havocs the
loop's write footprint ("k-induction with memory" below); a loop that
allocates or frees memory is not attempted (`extra.k_induction =
"not-attempted (allocation or free in the loop)"`), so it is PROVED only
when the unwinding assertion closes, else BOUNDED.

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
PTR-NULL-DEREF. Library throws (`std::__throw_*`, e.g. `vector::at`,
`optional::value`, the vector length check) throw the libstdc++ exception
class they name (`std::out_of_range`, `std::length_error`, …) along the
explicit exception edges of "Exceptions" below, so a `catch` of that
class or one of its bases catches them. With this, `std::vector`
(`operator[]`), `std::span`, `std::array`, `std::optional` (`operator*`),
`std::unique_ptr` (`operator*`) and `std::string_view` work
(`tests/pir/mem_stl.cpp`). The platform C++ library is
libstdc++ (D7, Linux); libc++ would need the same treatment of
`_LIBCPP_HARDENING_MODE`.

### C++ library models (roadmap 2.6)

Some libstdc++ code is too expensive for the encoder: `std::vector`'s
`_M_realloc_insert` (relocation through `__relocate_a`, `memmove` of a
symbolic length, the `_M_check_len` arithmetic, exception guards) makes Z3
run out of memory after two `push_back`s on an empty vector. For such
containers PRISM ships **model headers** in `src/prism/pir/models/cxx/`,
embedded in the binary like the C models (CMake list `PRISM_PIR_MODELS`,
names `cxx/<header>`), written to a temporary directory once per run and put
first on the C++ include path (`-isystem`, `lower_to_ir`), so the unit's
`#include <vector>` reaches the model instead of libstdc++'s header.

A model replaces library code only where it is sound — it must reach every
behaviour the real library can:

* **`<vector>`** (`std::vector<T, std::allocator<T>>`): a plain three-pointer
  implementation of the whole C++23 interface with libstdc++ 13's own
  capacity policy (`reserve`/`assign`/copy allocate exactly; growth is
  `size + max(size, n)` clamped to `max_size()`), so the same insertions
  reallocate and `capacity()`/`data()` read the same values; storage from
  `std::allocator<T>` (the `operator new/delete` models), so a pointer,
  reference or iterator kept across a reallocation points into a freed
  object and its use is **MEM-UAF** (iterator invalidation); the same
  exceptions (`length_error`, `out_of_range` from `at`) with the strong
  guarantee for throwing element copies; the same element construction and
  destruction order (relocation for nothrow-movable elements, move-if-noexcept
  otherwise); the `_GLIBCXX_ASSERTIONS` preconditions with libstdc++'s
  condition text (`operator[]`: `__n < this->size()` → MEM-OOB-READ;
  `front`/`back`/`pop_back`: `!this->empty()` → FUNC-CONTRACT) plus the
  standard's preconditions on `insert`/`erase` positions (an iterator into
  another vector, `erase(end())` → FUNC-CONTRACT). The model was checked
  differentially against libstdc++ under ASan/UBSan: a program exercising
  every member (growth sequence, fill/range/initializer-list insertion,
  erase, resize, shrink_to_fit, assign, copy/move, comparisons, erase_if,
  `at` and `reserve` exceptions, a throwing-copy element type, strings,
  `unique_ptr` elements, deduction guides) prints an identical trace of
  sizes, capacities, contents and constructor/destructor calls with both.
* **Not modelled, and why.** `vector<bool>` (a bit container) and allocators
  other than `std::allocator<T>` (including `pmr::vector`) are left
  undefined in the model: a unit that uses them does not compile against
  it, and the pir stage lowers it again with libstdc++'s header — every
  function's `extra.cxx_models` says which library it was checked against
  (`"model: vector"`, or `"libstdc++ (fallback: …)"` with the compiler's
  reason). `std::string` is not replaced: `<string>` is reached from every
  iostream/exception header and `basic_string<char>` is an explicit
  instantiation in `libstdc++.so`, so its inline code is checked as it is
  (with `_GLIBCXX_ASSERTIONS`); out-of-line members it calls stay
  `NEEDS-HARNESS`. `std::array`, `std::span`, `std::optional` and
  `std::unique_ptr` are not replaced either: their libstdc++ code is small,
  loop-free and already checks exactly the standard's preconditions under
  `_GLIBCXX_ASSERTIONS`, so the library code is its own sound model.

Encoder support added for the model: a use of a loop value outside its loop
that LLVM's LCSSA form never has, but that the translator creates on an
exception path leaving a loop through an inlined callee (the end of the
unwound frames' stack objects), takes the value of the iteration the path
left from (an implicit LCSSA phi over the node's incoming edges) instead of
giving up with `UNENCODED: value used outside its loop`.

### Library models verified by PRISM (roadmap 8.2)

The models are checked by PRISM itself: `tests/conformance/libc-models/`
holds contract harnesses that `#include` the model sources
(`string.c`, `stdlib.c`, `stdio.c`; the C++ allocation models through the
linked copy of `stdlib.c`, since their names are mangled) and state each
function's contract from the C standard with `assert()`. Every harness
builds its own objects with symbolic contents and symbolic sizes/positions
(scalar parameters, so no pointer-parameter precondition is assumed, Law 6),
calls the model, and asserts the contract, e.g. "strlen returns the index of
the first NUL", "strcmp's sign is the sign of the first differing byte as
`unsigned char`, and it is antisymmetric", "memcpy copies n bytes and leaves
the rest of the destination untouched", "strchr(s, 0) points at the
terminator", "realloc keeps min(old, new) bytes and leaves the old object
valid on failure", "fgets NUL-terminates within n". Every load and store in
the model is checked by the memory model on the way. Each `_true` harness
has `_false` twins (a wrong contract, or a precondition violation such as an
unterminated string) that must be refuted for the class they plant
(`expect_class:` in the task file), so a proof is not vacuous. They run in
the conformance suite (`python tools/conformance.py`).

**Scope of the proofs.** The objects in the harnesses are small (`N = 4`
bytes; 8–9 for a `strcat`/`strncat` destination), so the model loops run at
most N + 1 times and close within `--unwind 8`: the verdict is PROVED (the
unwinding assertion is proved), but what is proved is the contract **for
every content, position and length of objects up to that size**, not for
strings of arbitrary length. That is a size-bounded result; it is not
claimed as a proof for all sizes.

Results (C++ engine, `pir` stage, 2026-09-23):

| model | contract harness | verdict | false twins refuted (planted class) |
|---|---|---|---|
| `strlen` | first NUL index | PROVED (objects ≤ 4 bytes) | earlier NUL (FUNC-CONTRACT), unterminated (MEM-OOB-READ) |
| `strnlen` | min(strlen, n), reads ≤ n bytes | PROVED (≤ 4) | n past the object (MEM-OOB-READ) |
| `strcpy` | copies through the NUL, returns d | PROVED (≤ 4) | short destination (MEM-OOB-WRITE) |
| `strncpy` | exactly n bytes: string then NULs | PROVED (≤ 4) | "always terminates" (FUNC-CONTRACT) |
| `strcat` | appends at d's NUL | PROVED (d ≤ 8, s ≤ 4) | overflowing destination (MEM-OOB-WRITE) |
| `strncat` | appends ≤ n chars + NUL | PROVED (d ≤ 9, s ≤ 4) | "appends n chars" (FUNC-CONTRACT) |
| `strcmp` | 0 iff equal, antisymmetric, sign of first difference as `unsigned char` | PROVED (≤ 4) | signed-char comparison (FUNC-CONTRACT) |
| `strncmp` | n = 0 gives 0, equality up to n, antisymmetric | PROVED (≤ 4) | "ignores n" (FUNC-CONTRACT) |
| `strchr` | first `(char)c`, NUL included | PROVED (≤ 4) | `strchr(s, 0) == NULL` (FUNC-CONTRACT) |
| `strrchr` | last `(char)c` | PROVED (≤ 4) | "first occurrence" (FUNC-CONTRACT) |
| `memcpy` (`__prism_memcpy`) | n bytes copied, rest untouched, returns d | PROVED (≤ 4) | overlap (MEM-OVERLAP) |
| `memmove` | overlapping copy as through a temporary | PROVED (≤ 5) | source too short (MEM-OOB-READ/WRITE) |
| `memset` (`__prism_memset`) | `(unsigned char)c` into n bytes | PROVED (≤ 4) | "stores the int" (FUNC-CONTRACT) |
| `memcmp` | sign of first difference as `unsigned char` | PROVED (≤ 4) | "stops at NUL" (FUNC-CONTRACT) |
| `memchr` | first `(unsigned char)c` in n bytes | PROVED (≤ 4) | n past the object (MEM-OOB-READ) |
| `strdup` | NULL or a distinct copy | PROVED (≤ 4) | unchecked NULL (PTR-NULL-DEREF) |
| `malloc` / `free` | NULL or n writable bytes; `free(NULL)` | PROVED | one past the end (MEM-OOB-WRITE), read before write (UNINIT-READ), double free, free of a stack array |
| `calloc` | zero-filled; `n*size` overflow gives NULL | PROVED | unchecked NULL (PTR-NULL-DEREF) |
| `realloc` | keeps min(old, new) bytes; old object valid on failure | PROVED (≤ 8) | old pointer after success (MEM-UAF) |
| `abs`, `labs`, `llabs` | `|x|` for x ≠ MIN | PROVED | `abs(INT_MIN)` (INT-SIGNED-OVF) |
| `strtol`, `strtoul`, `atoi`, `atol` | `*end` within `[s, s + strlen(s)]`; argument a string | PROVED (≤ 4) | unterminated argument (MEM-OOB-READ) |
| `rand`, `srand` | 0 ≤ r | PROVED | "r < 100" (FUNC-CONTRACT) |
| `getenv` | NULL or a string | **BOUNDED** (the returned string has unknown length; `strlen` over it does not close) | unchecked NULL (PTR-NULL-DEREF) |
| `fopen`, `fclose`, `fflush` | NULL or an open stream; one close | PROVED | double `fclose` (MEM-DOUBLE-FREE), unchecked NULL |
| `fgets` | returns buf, NUL within n | PROVED | n larger than the buffer (MEM-OOB-WRITE) |
| `fread`, `fwrite` | return ≤ nmemb | PROVED | fread past the buffer (MEM-OOB-WRITE) |
| `fgetc`, `getc`, `getchar` | EOF or 0..255 | PROVED | "never EOF" (FUNC-CONTRACT) |
| `putchar`, `puts` | returns `(unsigned char)c`; puts ≥ 0 | PROVED | unterminated `puts` argument (MEM-OOB-READ) |
| `operator new[]/new/delete[]/delete` | n usable elements, never NULL | PROVED | `delete` of `new[]` (MEM-MISMATCHED-FREE), one past the end (MEM-OOB-WRITE) |

Totals: 29 of 30 contract harnesses PROVED (size-bounded as above), 1
BOUNDED (`getenv`), 35 of 35 false twins refuted for the planted class.
The `bmc` stage has no preprocessor and does not see the models: it answers
`NEEDS-HARNESS` on 63 of 65 functions (it refutes `abs(INT_MIN)` and the
`rand` bound through its own built-in models).

Not checked this way: `fputc`, `putc`, `fputs` and `realloc(p, 0)` have no
harness yet; the printf family is modelled in the translator
(`libc_format.cpp`), not in C, and cannot be run through PRISM as a model
(its checks are unit-tested in `tests/pir/mem_libc.c`); the `__prism_*`
intrinsics themselves are PIR statements whose encoding the harnesses
exercise (memcpy/memset/memmove contracts) but do not verify in isolation.

## C features

* `_Generic`: resolved by clang (`tests/pir/mem_libc.c:generic_ok` PROVED).
* VLAs: `alloca T, iN n` with a symbolic size; the size must be positive
  (checked on the value before its `zext`/`sext`) and below 2^47; the
  object ends at `llvm.stackrestore`.
* `setjmp`/`longjmp`: exception-like edges ("setjmp/longjmp" below).

## Floating point (roadmap 2.6)

`half`, `float` and `double` values are PIR bitvectors of width 16/32/64
holding their IEEE bits (`Var::fp` marks them); `long double` (x86_fp80),
`__float128`, `bfloat` and vectors stay `UNENCODED`. The encoder uses Z3's
floating-point theory with round to nearest even (the C default
environment; PRISM does not model `fesetround`): every FP operator reads its
arguments with `(_ to_fp e s)` and gives back the bits of the result through
a fresh bitvector `b` with `to_fp(b) = result`, so a NaN result has an
unspecified payload (LLVM's NaN semantics). `frem` is C `fmod` (derived from
the IEEE remainder, exact), `llvm.fmuladd` is fused *or* not (a free choice
per call), `minnum`/`maxnum` of `+0`/`-0` may return either.

Translated: `fadd fsub fmul fdiv frem fneg fcmp` (all 16 predicates from
`oeq olt ole uno`), `fptosi fptoui sitofp uitofp fpext fptrunc`, `bitcast`
between FP and integers of the same width, loads/stores/phis/selects of FP
values, `llvm.{fabs,copysign,sqrt,fma,fmuladd,minnum,maxnum,minimum,maximum,
floor,ceil,trunc,round,roundeven,rint,nearbyint}` and the same libm functions
by name (`sqrt`, `fabs`, `floor`, `fmod`, `fmin`, … and their `f` forms).
Other pure libm functions (`sin cos tan asin acos atan atan2 sinh cosh tanh
asinh acosh atanh exp exp2 expm1 log log2 log10 log1p pow cbrt hypot erf
erfc tgamma` and `f` forms) return an **unconstrained** value (`Op::FLibm`,
listed in `extra.libm_unconstrained`) apart from range facts every IEEE libm
keeps: `|sin|`, `|cos|`, `|tanh|` ≤ 1 and `exp`, `exp2`, `cosh` ≥ +0 (or
NaN). Any fast-math flag makes the instruction `UNENCODED` (the flags allow
results IEEE does not). errno is not modelled (a program reading `errno`
calls `__errno_location`, which is `UNENCODED`).

Properties: `fptosi`/`fptoui` whose truncated value is outside the target
type, or NaN/±inf, is FLOAT-CAST-OVF (always on: C11 6.3.1.4p1 undefined
behaviour; `-0.9 → unsigned` is fine, it truncates to 0). With
`--fp-checks` (opt-in; Annex F defines these results) PRISM also reports
division by ±0 (FLOAT-DIV-ZERO), a NaN produced from non-NaN operands
(FLOAT-INVALID) and an infinite result from finite operands
(FLOAT-OVERFLOW), for `fdiv fadd fsub fmul frem sqrt fma fptrunc`.

The interpreter (`fp.cpp`) computes float/double with the host's SSE
arithmetic (round to nearest even, no excess precision on x86-64) and half
in double with one final rounding (exact for `+ - * / sqrt` since
53 ≥ 2·11 + 2); results the encoder leaves open (`fmuladd`, `minnum(+0,-0)`)
taint the value, so translation validation skips inputs that depend on them,
and an FP return compares NaN with any NaN. Translation validation passes FP
arguments as LLVM hex literals and draws ordinary values (0.5, -1000.25,
3e9, 1e300, ±inf, NaN, 16777217, …) besides raw bit patterns.
`tests/pir/fp_arith.c`.

## Exceptions (roadmap 2.3, 2.6)

Every call is inlined, so the chain of enclosing `invoke`s of a throw is
known while it is translated. `invoke` becomes its call plus the normal
edge, with its landing pad pushed as the exception destination of
everything inside the call. A throw (`__cxa_throw`, a library
`std::__throw_*`, `resume`, `throw;`) becomes an explicit jump to the
innermost landing pad whose clauses catch the thrown type — exact typeinfo,
public unambiguous bases read from the module's `__si_class_type_info` /
`__vmi_class_type_info` typeinfo, the libstdc++ exception hierarchy,
`catch (...)` — or that has cleanup code; the frames between the throw and
that landing pad are unwound (their locals end). The landing pad's
`{ ptr, i32 }` value is a phi over those edges (exception pointer and the
`llvm.eh.typeid.for` selector of the matching clause). The exception object
gets a 16-byte PRISM header in front of the thrown object (type id,
"rethrown" flag): `__cxa_begin_catch`/`__cxa_end_catch` keep a stack of
handled exceptions, `__cxa_end_catch` runs the thrown type's destructor and
frees the object unless it was rethrown, `throw;` re-raises the innermost
handled exception (`throw;` with none is CXX-TERMINATE), and `resume` re-throws
with the type read back from the header. Landing pads are translated after
the normal blocks of their function, and only when a throw reaches them.
An exception reaching `__clang_call_terminate` (a `noexcept` boundary or a
destructor throwing during unwinding) is CXX-THROW-NOEXCEPT, one that
leaves `main` is CXX-UNCAUGHT, `std::terminate()` is CXX-TERMINATE, a pure
virtual call is CXX-PURE-VIRTUAL. An exception that leaves any other
analysed function is not a defect there: the path ends and the type is
listed in `extra.throws_not_followed`. Soft (NEEDS-HARNESS when reachable):
a type match PRISM cannot decide (pointer conversions, ambiguous bases,
typeinfo not in the module), handlers nested deeper than 8, landing pad
code that cannot be translated. Not modelled (`UNENCODED`): dynamic
exception specifications (`filter`, `__cxa_call_unexpected`),
`std::exception_ptr`, `std::uncaught_exceptions`. `tests/pir/eh_*.cpp`.

## Coroutines (roadmap 2.3, 2.6)

C++ units run LLVM's coroutine passes (`coro-early`, `coro-split`,
`coro-cleanup`) after `mem2reg`: each coroutine becomes a ramp function
that allocates its frame with `operator new`, plus `.resume`, `.destroy`
and `.cleanup` functions that switch on the suspend index stored in the
frame. A loop around a `co_yield` is re-entered at the suspend point in the
resume function (a second loop entry); `fix-irreducible` gives such loops a
single header with a dispatch phi, so the encoder's natural-loop unrolling
applies. PRISM encodes the result like any other code: `coroutine_handle::
resume()`/`destroy()` are indirect calls through the frame's function
pointers ("Indirect calls"), the frame is a `new` object of the memory
model (use after `destroy()` and double destroy of the frame are
memory-model properties), and `llvm.lifetime.end` of a frame temporary
ends its lifetime (`llvm.lifetime.start` makes its bytes indeterminate
again; an object already ended stays dead, so a later access is reported,
never assumed valid). `tests/pir/coro_gen.cpp`.

## setjmp/longjmp (roadmap 2.6)

`setjmp` (`_setjmp`, `__sigsetjmp`, …) splits its block: it stores a site
id in the first 8 bytes of the `jmp_buf` and continues in a new block whose
phi is the return value (0 on the direct path). `longjmp` reads the id back
(an unset `jmp_buf` is UNINIT-READ, a bad pointer a memory violation) and
jumps to the continuation of the site with that id whose frame is still
active, unwinding the frames in between; the value is `v`, or 1 for
`v == 0`. A site whose function has returned is C11 7.13.2.1p2 undefined
behaviour: CTRL-LONGJMP-INVALID. Locals of a function that calls `setjmp`
stay in memory (pre-mem2reg `@__prism.keep`), so after the jump they hold
their last stored value, as in the -O0 build; C11 7.13.2.1p3 calls a
non-volatile local modified in between indeterminate, which PRISM does not
report (a gap listed above). A `longjmp` from code the `setjmp` does not
dominate makes the control flow irreducible (`UNENCODED`).
`tests/pir/sjlj_basic.c`.

## Indirect calls (roadmap 2.6)

The address of a function is a constant pointer whose object id lies in
`[kFnObjBase, kMaxObjects)` (0xF000 … 0xFFFE): never allocated, never
dereferenceable (loads and stores through it are memory violations;
allocations stay below `kFnObjBase`). A call through a pointer compares the
pointer with each candidate target and inlines the matching one:

* the pointer is loaded from a vtable slot (`load (gep (load p))`): the
  functions stored in the module's `_ZTV*` vtables with the call's
  signature — class hierarchy analysis over the classes the unit defines;
  virtual functions declared but defined elsewhere are soft branches;
* otherwise (or when no vtable function fits): every address-taken function
  of the module with the call's signature.

A null pointer is PTR-NULL-DEREF; any other value — a function outside the
candidate set — is the **havoc fallback**: a soft check reported as
`NEEDS-HARNESS` ("indirect call to a target outside …") when reachable.
Choosing candidates therefore only affects completeness, never soundness.
More than 16 candidates is `UNENCODED`. `tests/pir/virt_dispatch.cpp`.

## Inline assembly (roadmap 2.6)

An `asm` call is `UNENCODED: inline assembly` unless the source states its
effect on the asm statement's line or one of the three lines above it:

```c
// prism: asm ensures r >= 0 && r <= 7
__asm__("movl %1, %0\n\tandl $7, %0" : "=r"(r) : "r"(x));
```

`cond` is `true` or comparisons (`== != < <= > >=`) of the output with
integer constants joined by `&&`. PRISM then assumes the block writes only
its (integer) output and that `cond` holds afterwards; the contract is
listed in `extra.assumptions` and a proof is **PROVED-ASSUMING**, never
PROVED. Asm with memory operands, several outputs or non-integer outputs
stays `UNENCODED` even with a contract. `tests/pir/asm_contract.c`.

## k-induction with memory (roadmap 2.6)

The step case encodes the code before the loop concretely, then starts the
loop from an arbitrary header state and asks whether k violation-free
iterations can be followed by a violation (iteration k or the code after the
loop). For a function with memory the header state is the header phis
**and the memory the loop may have written**:

1. **Footprint.** Every store, `memcpy` and `memset` in a block of the loop
   is mapped to the object its target points into by the memory model's
   provenance analysis (`SymMem::known`: allocation results, checked pointer
   arithmetic, selects and phis whose inputs agree). Provenance is
   structural — never read from memory or from the havocked phis — so an
   object it names is the target of that write in every iteration of every
   run that has reported no violation (pointer arithmetic is checked to stay
   in its object). The footprint is the set of those objects, computed on
   the bounded encoding and re-checked on the step encoding. If some write's
   target is not resolved (a pointer loaded from memory, a phi or select over
   different objects, a havocked induction pointer), the footprint is every
   object allocated before the loop except `const` ones. That set is finite
   and sound: the loop allocates nothing, and a write that reports no
   violation goes to a live, non-`const` object that already exists.
2. **Havoc.** At the header, every byte of every footprint object gets an
   arbitrary value and tag (`SymMem::havoc_objects`, both encodings; Bv: one
   fresh cell per read address with Ackermann constraints). Its
   *initialised* flag becomes `old | arbitrary` when every write in the loop
   initialises the bytes it writes (plain stores with a constant all-ones
   mask, `memset`): a byte that may be uninitialised before the loop stays
   "maybe initialised", never assumed initialised. When the loop has a
   `memcpy` or a store with a per-byte mask (which can copy uninitialised
   bytes), the flag is fully arbitrary.
3. **Allocation.** A loop that allocates, frees or restores the stack is not
   attempted (`extra.k_induction = "not-attempted (allocation or free in the
   loop)"`): the number and liveness of objects would change across
   iterations, which the havoc does not cover. Sizes, kinds and liveness of
   the objects that exist are unchanged by the loop and are not havocked.

The base case is the concrete unrolling of `--unwind` iterations (k ≤ unwind).
A closed step makes the function **PROVED-UNBOUNDED** exactly as for scalar
loops; an open or unknown step leaves it **BOUNDED** (Law 2). Evidence in the
verdict: `extra.k_induction_memory` (`read-only loop` for an empty footprint,
else `write footprint havocked`) and `extra.k_induction_footprint` (objects
havocked, whether the footprint was resolved, and the initialised-flag
mode).

Soundness in short: on any run whose first violation is at iteration
N ≥ unwind, the state at header visit N − k agrees with the prefix outside
the footprint, has the prefix's objects, and has footprint bytes whose
initialised flags only grew (when every write initialises); the havocked
header state covers it, and the step encoding then follows the run exactly
for k + 1 iterations, so the step query is satisfiable. The abstract form of
this argument is `PrismTechniques.KInduction.kinduction_frame_sound`
(`proofs/techniques`, frame predicate `J`); what the Lean memory semantics
(`proofs/semantics/PrismSem/Memory.lean`) would still need to prove it for
PIR is a frame lemma "a loop body with no `alloc`/`free` whose stores all
target objects in F leaves every other object's bytes, every object's size
and liveness, and `next` unchanged", a provenance lemma for `gep`, and a
per-byte initialised flag in `Obj` (the Lean model zero-fills fresh objects
and has no uninitialised memory yet).

Precision is object-granular: a loop that writes `d[1]` havocs all of `d`,
so a later read of `d[0]` is arbitrary (`conformance/prism/kindmem/
kindmem_overwrite_true` stays BOUNDED). No loop invariant is inferred, so
"`a[0..i)` is initialised" style facts are not available to the step: after
the loop, a read of an element written by an earlier iteration stays BOUNDED
when the array was uninitialised before the loop, while one written by the
last k iterations is seen by the step itself (`array/arr_loop_true` reads
`a[9]`, written by the final iteration).

Tasks: `tests/pir/kind_mem.c`; `tests/conformance/prism/kindmem/` (13
true/false pairs: prefix writes then a read beyond the prefix, even-only
writes, late out-of-bounds writes, writes through aliasing, selected and
loaded pointers, `memcpy` of uninitialised bytes, pointer walks, struct
buffers, globals, frees and allocations inside the loop); doctests `pir mem:
k-induction havocs the write footprint …` (each footprint rule, both
encodings; mutating the havoc to "no havoc", "assume initialised" or
"memcpy initialises" makes them fail).

Measured (unwind 8): of the 13 `kindmem` pairs, 8 true functions are
PROVED-UNBOUNDED and none of the 13 false ones is proved (the other five
true ones stay BOUNDED: allocation or free in the loop, a `memcpy`, an
object-granular overwrite, a havocked pointer walk). On the rest of the
conformance suite two BOUNDED true tasks become PROVED-UNBOUNDED
(`array/arr_loop_true`, `regress/uninit_elem_unbounded_true`), in
`tests/pir` one (`kind_mem.c`); the strict gate has 0 wrong proofs and
`tools/csmith_soundness.py` (`--generator inhouse-ptr -n 80`, `--generator
inhouse -n 60`) finds 0 wrong proofs. Those generators produce few
single-loop functions that write memory, so a targeted run was added (an
ad-hoc generator of 300 single-loop functions writing partly initialised
arrays through plain, aliased, selected and loaded pointers, with late
conditional writes and `memcpy`): 49 PROVED-UNBOUNDED, each executed on a
228-point input grid under ASan+UBSan and again under MSan (which sees
uninitialised reads): 0 sanitizer reports.

## Roadmap 2.3 / 2.6 coverage

Normalisation (2.3): `mem2reg`, `lowerswitch`, `loop-simplify`, `lcssa`
(all units); coroutine lowering `coro-early`, `cgscc(coro-split)`,
`coro-cleanup` and `fix-irreducible` (C++ units); `invoke`/`landingpad` →
explicit exception edges (in the translator, "Exceptions"). No optimising
pass; no `-fsanitize` check insertion (Law 8).

| 2.6 row | how PIR handles it | status | evidence |
|---|---|---|---|
| Templates, concepts, overloading, `constexpr`/`consteval`, `if consteval`, deducing `this`, lambdas | resolved by Clang before IR | DONE (no encoder work) | `tests/pir/cxx.cpp`, `tests/conformance/prism/cxx` |
| Classes, inheritance, virtual dispatch | vtable loads are memory reads; indirect calls dispatch over the module's vtable functions (class hierarchy analysis) or address-taken functions; any other target is the havoc fallback (NEEDS-HARNESS) | DONE | `tests/pir/virt_dispatch.cpp`, `conformance/prism/virt` |
| Exceptions | explicit exception edges, catch matching (typeinfo hierarchy), cleanup, rethrow; escape from `noexcept` (CXX-THROW-NOEXCEPT) or `main` (CXX-UNCAUGHT) is a violation | DONE (dynamic exception specs, `exception_ptr`: UNENCODED) | `tests/pir/eh_*.cpp`, `conformance/prism/eh` |
| Coroutines | LLVM coroutine passes, then normal encoding (frame = `new` object, resume/destroy = indirect calls) | DONE | `tests/pir/coro_gen.cpp`, `conformance/prism/coro` |
| Standard library | libc operational models; libstdc++ inlined with `_GLIBCXX_ASSERTIONS`; library throws are real exceptions; unmodelled calls NEEDS-HARNESS | PARTIAL (no verified libc++ models of containers yet) | "Library models" |
| Floating point | Z3 floating-point theory (RNE) for half/float/double; FLOAT-CAST-OVF; `--fp-checks` | DONE (x86_fp80/fp128/bfloat, fast-math: UNENCODED) | `tests/pir/fp_arith.c`, `conformance/prism/fp` |
| Threads and atomics | separate `conc` stage (docs/CONCURRENCY.md) | other work (not in this slice) | docs/CONCURRENCY.md |
| Modules (`import std;`) | handled by Clang; PRISM consumes the IR | NOT TESTED (Clang 18 needs a prebuilt `std` module) | — |
| Inline assembly | NEEDS-HARNESS unless `// prism: asm ensures <cond>`; then PROVED-ASSUMING listing the contract | DONE | `tests/pir/asm_contract.c`, `conformance/prism/asm` |
| C (C11 to C23): `_Generic`, VLAs, `setjmp`/`longjmp` | `_Generic` by Clang; VLAs in the memory model; setjmp/longjmp as exception-like edges (CTRL-LONGJMP-INVALID) | DONE | `tests/pir/mem_libc.c`, `tests/pir/sjlj_basic.c`, `conformance/prism/sjlj` |
| k-induction for functions using memory | step case havocs the loop's write footprint (provenance-resolved objects, else every pre-loop object; initialised flags `old \| arbitrary` or arbitrary) | DONE for loops that do not allocate or free (those stay BOUNDED, `not-attempted`); object-granular, no invariant inference | `tests/pir/kind_mem.c`, `conformance/prism/kindmem`, `extra.k_induction_footprint` |

## Encoder and verdicts

Bounded unrolling over the loop nest: each block is instantiated once per
vector of iteration counts of its enclosing natural loops (outer → inner); a
back edge increments its loop's count; count = `--unwind` is an *unwinding
cut*. Values follow SSA dominance (LCSSA makes every use see its definition in
the same iteration of each shared loop). Phis are `ite` chains over incoming
edge guards; each check contributes `reach ∧ violation`; assumes contribute
`reach → cond`.

* **FAILED** — some check is satisfiable. `counterexample` = `extra.cex` =
  parameter values (signed decimal), `extra.prop`, `cls`, `line`. When the
  function calls `__VERIFIER_nondet_*` itself (not inside a library model),
  `extra.nondet` lists the values those calls return on the violating path,
  in call order, as `fn=value, ...` (signed per the C type, the same shape
  as the `bmc` stage), and `extra.nondet_loc` the calls' debug locations
  `line:col, ...` (`0:0` when unknown). A call is listed when the model makes
  its block instance reachable and it runs before the violated check (the
  reachable instances form one chain, and topological order is execution
  order along it). The values come from one Z3 model of the same VC with
  the parameters and nondet values pinned to the winning solver's model; if
  that re-query finds no model, `extra.nondet_note` says so and there is no
  `nondet` key. The SV-COMP wrapper replays these values and writes a
  `function_return` waypoint at each call ([SVCOMP.md](SVCOMP.md)).
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

### Solving (roadmap 3.1 / 3.2)

`check_function(fn, CheckOptions)` asks one query per verification condition
and sends every one of them to the solver library,
`prism::solver::solve` ([SOLVERS.md](SOLVERS.md)): the portfolio (Z3
in-process, CaDiCaL / Kissat on the bit-blasted CNF, Bitwuzla when present,
the ProbSAT walker for counterexamples) and the query cache.

1. One VC per property: `assumptions ∧ reach ∧ violation`. The properties are
   asked in order; the first SAT answer is the `FAILED` verdict. Its model
   was already evaluated on the VC in Z3 by the solver library; the
   counterexample is the model's parameter values (`extra.cex_solver` names
   the member that found it). An answer that is neither SAT nor UNSAT makes
   the function `UNKNOWN` unless a later property is SAT.
2. When every property VC is UNSAT and a loop cut is reachable in the
   unrolling, the unwinding VC `assumptions ∧ (cut₁ ∨ cut₂ ∨ …)`: UNSAT means
   `PROVED` (the loops close), anything else `BOUNDED`.
3. The k-induction step (single-loop `BOUNDED` functions) is not a property
   VC: Z3 answers it in-process (`extra.k_induction_solver`).

`extra.solver` summarises the run: the number of VCs, which member answered
each (`z3:3, cadical:1`, `(cache)` for a cache hit) and the cache hits.

The stage uses `--solver-cache DIR` (default `$XDG_CACHE_HOME/prism/solver`)
and gives each query `max(2, cores / --jobs)` solver members at once.
`--timeout S` (default 30) is the budget of **each** VC.

**Certified mode** (`--certified`, roadmap 3.2). Each VC is also bit-blasted
to CNF, CaDiCaL writes an LRAT proof of its unsatisfiability, and cake_lpr
must accept it. The bit-blaster is chosen by the solver library
(`bitblaster = auto`): the Lean-proved bit-blaster (`prism-bitblast`) when the
VC is inside its fragment and the Lean tools are built, and then Lean's
verified LRAT checker (`prism-lrat-check`) must accept the proof too;
otherwise Z3's tactics, and the certificate says so. Memory VCs are certified
like any other: the default memory encoding (`MemEncoding::Bv`) is QF_BV.
A function is `PROVED-CERTIFIED` only when it is `PROVED`, has at least one
VC, and **every** VC (each property and the unwinding assertion) is
certified. It then carries `extra.certificate = "checked"` (what the verdict
audit requires of a `PROVED-CERTIFIED` from this stage),
`extra.certificate_info` (one entry per VC: `<prop>@<line>: bitblast: … ;
cadical … lrat N steps, checked by cake_lpr …; cnf sha256 …`),
`extra.certificate_bitblast` (`N/M lean-proved`) and `extra.cnf_sha256` (one
hash per VC, comma-separated). Otherwise the verdict is unchanged and
`extra.certify_note` names the first VC that was not certified and why (for
example `not certified: cake_lpr not found (NOTRUN)`). `BOUNDED` and
`PROVED-UNBOUNDED` are never certified; a function with no VC at all stays
`PROVED` with `extra.certify_note = "no verification conditions (nothing to
certify)"` (a certificate that checks nothing is not one); and a certified
proof under assumptions is `PROVED-ASSUMING`.
Translation validation that diverges turns a certified function into `ERROR`
and drops the certificate. What a certificate still trusts is in
[TRUSTED_BASE.md](TRUSTED_BASE.md).

`prism --pir-vcs FILE --out DIR` writes the VCs of every function of one unit
as SMT-LIB2 files, and `prism --solve-smt2 VC [--z3-only]` answers one; both
print JSON and exist for `tools/solver_bench.py`.

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
