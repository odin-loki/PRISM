# PIR: the Clang/LLVM front end (roadmap Part 2, first slice)

The `pir` stage runs right after `bmc` (C++ engine only; the Python engine is
frozen as an oracle, roadmap D8, and records one `NOTRUN` row for the stage).

```
C/C++ unit ──clang -O0──▶ LLVM IR ──PRISM instrumentation──▶ opt (fixed passes)
          ──▶ textual IR parser ──▶ PIR (+ property checks) ──▶ Z3 bitvectors
          ──▶ FAILED / PROVED / BOUNDED / PROVED-UNBOUNDED / NEEDS-HARNESS
          ──(--allow-exec)──▶ translation validation against lli
```

Sources: `include/prism/pir.hpp`, `src/prism/pir/{ir_parser,pir,translate,encode,stage}.cpp`.
Tests: `tests/pir/*` (true and false variants), `tests/test_pir.py`, doctests
`pir: …` in `tests/cpp/test_main.cpp`. Differential oracle: `tools/pir_vs_bmc.py`.

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
`UNENCODED: <construct>`: `alloca/load/store/getelementptr` (memory model,
roadmap 2.5, not in this slice), floating point, integers wider than 64
bits, other calls (`UNENCODED: call @malloc`), `switch` if lowerswitch did not
run, `invoke`/exceptions, irreducible control flow, vectors and aggregates.
Pointer parameters are `NEEDS-HARNESS` by Law 6 (C++ member functions have
`this`). In C++ units, a function whose source contains `const_cast` is
`UNENCODED: const_cast` (a write to a const object is UB that the IR cannot
show). Known gaps of the *property set* (a PROVED means these were not
checked): writes to const objects via C casts, strict aliasing, reading an
indeterminate value that is only copied (`int y = x;` with `y` unused — the
read has no IR instruction after mem2reg), falling off the end of a non-void
C function (UB only when the caller uses the value).

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
