# Harness phase 2: contract stubs, ops tables, address values, deeper fields

Status: implemented, opt-in, every use a named assumption. Base: `integration-wave2` (contains the harness generator of
`docs/HARNESS_GENERATOR.md`, the W1 kernel specs of `docs/KERNEL_MODELS.md`, `--ops-table` and the device model).
Source: `src/prism/pir/callee_contract.cpp` (stubs), `ops_auto.cpp` (derived ops tables), `harness.cpp` (function-pointer
fields, depth record), `translate_mem.cpp` (`const_addr`, `contract_valid`), tests `tests/cpp/test_pir_harness2.cpp`,
data `testdata/pir/{callee_contract,ops_auto,addr_values,harness_deep}.*`.

## Why

The W8 scale run (Stage 1, 36 Linux and 15 FreeBSD units that finished, flags of `scripts/pir_batch.sh`
plus `--pir-discharge --pir-container-of --pir-vacuity --assume-unknown-calls`, 6 s per function) left 967 of 1,689
Linux functions and 482 of 733 FreeBSD functions NEEDS-HARNESS. By message (`data/pir-scale`, counted by script):

| blocker | Linux | FreeBSD |
|---|---|---|
| `UNENCODED: call depth > 4` | 205 | 203 |
| `UNENCODED: call @llvm.read_register / objectsize / prefetch` | 174 | 0 |
| violation only under the harness (of which `null-call`: a call through an arbitrary function-pointer field) | 28 | 130 |
| `UNENCODED: constant expression ptrtoint` | 87 | 0 |
| `UNENCODED: indirect call with N possible targets (> 16)` | 35 | 0 |
| `UNENCODED: extern object of unknown size` | 34 | 0 |
| `UNENCODED: initializer ... (inttoptr constant expression)` / `constant getelementptr on inttoptr` | 32 | 0 |

(The "body-less callee reached" blocker of the OS-Proving brute-force pass, a different tool, does not appear in this data as
such: with `--assume-unknown-calls` every body-less call is already a havoc, except the intrinsics in row 2. What phase 2 adds for
callees that have a contract is precision and checking: the call site is checked against the contract, only what the contract allows
to change is havocked, and the result is constrained.)

## 1. Contract stubs (`--callee-contracts FILE`)

A TSV, one row per callee, 7 columns: `function`, `signature`, `requires`, `assigns`, `ensures`, `status`, `source`
(format in `include/prism/pir.hpp` `CalleeContract`). `status` (UNVALIDATED, REVIEWED or VALIDATED) and `source` are
required: a row without them is a load error. The signature (`dev:ptr,n:u64,reg:s32->s32`) gives the type of every
parameter and the result: ACSL integers are mathematical, IR integers have no signedness, so the contract states it, and a
signature that does not match the types at the call (`u64` against an `i32` argument) is **rejected, not coerced**.

Clauses are a small ACSL subset: comparisons, `&&`, `||`, `!`, `+ - *` over the parameters and `\result`; `\null`, `\true`,
`\false`; and byte-granular memory clauses (element sizes are not in the IR of a declaration, so they are bytes):
`\valid_bytes(p, n)`, `\valid_read_bytes(p, n)` (requires), `\bytes(p, n)`, `\nothing`, `\anything` (assigns), and for a
pointer result `\valid_bytes(\result, n)` (a fresh object) or `\result == \null || \valid_bytes(\result, n)` (ensures).
Arithmetic is checked for overflow of 64 bits at translation time (u64/s64 values are only compared), a u64 is compared
only with a u64 or a non-negative constant. Anything else is **rejected with the reason**, and the call is then
handled exactly as without a contract (UNENCODED, or the `--assume-unknown-calls` havoc): never a silent assumption.

At a call of a callee that has a row, no body and no kernel spec (a body or a spec always wins; the same name in
`--kernel-api` is the spec) the translator emits:

1. **requires**: a check at the call site, class `FUNC-CONTRACT`, property `callee-requires` / `callee-null` /
   `callee-wild` / `callee-uaf` / `callee-oob` / `callee-const`: a violation is a finding about the *caller*;
2. **assigns**: a havoc of exactly the stated byte ranges (arbitrary initialised bytes); `\nothing` writes nothing; no
   `assigns` clause havocs the objects the pointer arguments point into, as `--assume-unknown-calls` (named);
3. **result and ensures**: an arbitrary result, the ensures assumed (`\false` ends the path); a `\valid_bytes` result
   is a fresh `Extern` object of that size, null only when the ensures allows it.

The whole stub is one assumption line, in `extra.assumptions` and in the `--jsonl` record (`callee_stubs`: callee, source,
status): `callee-contract: @f is replaced by its contract (--callee-contracts; source: S; status: UNVALIDATED; the contract
was not verified against the callee): requires [...] is checked at the call site; assigns ...; ensures [...] is assumed; the
callee is assumed to return, to free nothing, to retain no pointer argument and not to call back into the unit`.
A result that used a stub is PROVED-ASSUMING at best, never PROVED, whatever the status says.

### Soundness argument

Let `R` be the real relation of the callee (pre-state and arguments to post-state and result) and `C = (req, asg, ens)` its
contract. The stub's post-states are: pre-state with the bytes of `asg` arbitrary, any result satisfying `ens`.
*If* the callee satisfies `C` (on every call whose pre-state satisfies `req`, it writes only inside `asg`, its result satisfies
`ens`) *and* it does not free, retain or call back, then every real post-state is a stub post-state, so the stub
over-approximates the callee and a proof of the caller under the stub is a proof for the real callee. The requires are
asserted at the call site, so pre-states outside `req` (where `C` says nothing) are reported, not assumed away.
What is not proved is exactly the "if": it is the named assumption, with its status. The status is shown, never upgraded:
`VALIDATED` is a statement in the contract file, and still PROVED-ASSUMING.

What a stub cannot do: make a callee without a (usable) contract look specified. Findings (FAILED) that go through a stub also list
it (the contract may allow a value the callee never returns, as with any over-approximation).

Tests (`pir callee contracts:` in `tests/cpp/test_pir_harness2.cpp`): requires (`stub_requires_ok` / `_bad`), assigns bound
(`stub_assigns_only_ok` against `stub_assigns_written_bad`, `stub_nothing_ok`), ensures (`stub_ensures_ok` / `_bad`,
`planted_oob`), a nullable fresh result (`stub_alloc_*`), precedence (a callee with a body ignores its row), rejection
(unknown callee, wrong signature, unsupported clause), and:

* **soundness**: `stub_false_ensures` calls a callee whose contract has `ensures \false`: the caller is PROVED-ASSUMING with
  `callee-contract: @hw_false ... status: UNVALIDATED ... ensures [\false] is assumed ...` and without the contract (same
  function, `--assume-unknown-calls`) it is FAILED. The stub is the only reason the bug is not reported, and it is named;
* **planted bug**: `stub_ensures_bad`, `planted_oob`, `stub_requires_bad`, `stub_alloc_null_bad` are FAILED: a bug
  behind a faithful stub is found.

## 2. Function-pointer fields (`--pir-ops-auto`)

The harness leaves a function-pointer field arbitrary, so `hw->mac.ops.read_reg(...)` is a call through a null
pointer (121 of the 389 e1000e NEEDS-HARNESS; 130 of the FreeBSD ones). `--ops-table FILE` already resolves a call
through a known `(struct, field)` to its targets, but needs a file nobody has. `--pir-ops-auto` derives the table from the unit:
every `store ptr @f, getelementptr %struct.T, p, 0, K` and every function address in a constant initialiser of struct `T`
at field `K` makes `@f` a target of `(T, K)` (`derive_ops_table`, `role "unit-derived"`; explicit `--ops-table` entries win;
a field with more than 8 targets is left out). Two effects:

* an indirect call through such a field is a call of each target (the existing `--ops-table` path), with the
  existing "target outside the table" stop for any other value: closed-world only in the sense of that stop, never a
  silent proof;
* a harness object whose pointer field the function loads and calls through gets that field **bound to one of the targets**
  (a free choice among them, `HarnessFpField`), recorded as `<field> (function-pointer field %struct.T.K) holds one of {...}
  (ops table, unit-derived: the framework is assumed to have initialised it to a target the table names; a field set
  outside this unit may hold another function)`. This is an invented precondition like the rest of the harness: the
  result is PROVED-ASSUMING at best, a violation inside a target is NEEDS-HARNESS naming it, and a field nothing in the unit
  assigns stays arbitrary. The discharge (`--pir-discharge`) does not erase these lines (they do not carry the
  `(auto harness)` marker).

## 3. ptrtoint / inttoptr (`--pir-addr-values`)

The instruction forms already round-trip (a pointer is carried as its packed object/offset value, `inttoptr (ptrtoint x)`
is `x`, a non-ptrtoint `inttoptr` is an opaque value with the assumption `inttoptr-opaque`). The blocker was the
**constant expression** forms (87 + 32 Linux functions): `ptrtoint (ptr @g to i64)`, `inttoptr (i64 -22 to ptr)` as a value, in
`getelementptr`, or in a static initialiser. With the flag:

* `ptrtoint` of a constant pointer is its packed value (`MemTr::const_addr`), so pointer differences keep the
  `PTR-COMPARE` check (`is_ptrtoint` now knows the constant form) and an offset difference inside one object is exact.
  Assumption `addr-values: a constant ptrtoint is the packed (object, offset) representation ...; the absolute address of an
  object is not modelled`;
* `inttoptr (C)` with `C != 0` is the constant `C`: a pointer to no object. Assumption `addr-values: the integer constant C
  used as a pointer ...`. A dereference of it is reported.

Because the model does not know absolute addresses (kernel code dereferences fixed addresses, and adds per-cpu offsets to a
base, which mixes two objects' packed values), a violation in a function that used **either** assumption is
**NEEDS-HARNESS** (`verdict_under_addr_model` keeps the finding), never FAILED. A proof under them is PROVED-ASSUMING with the
`addr-values:` lines.

## 4. Deeper pointer fields (`--pir-harness-deep`)

Depth 4 instead of 2 and 64 objects per parameter instead of 32 (an explicit larger `--pir-harness-depth` wins).
The bound is now recorded: the `auto harness for P` line ends with `deepest object at level R of D; pointer fields left
arbitrary: X past the depth bound, Y over the object cap of M, Z without pointee type evidence`, and the
`--jsonl` harness object gets `depth_bound`, `depth_reached`, `cut_past_depth`, `cut_over_cap`, `cut_no_type`,
`function_pointer_fields_bound` and `address_value_assumptions`.

## 5. Built-in contracts for body-less intrinsics (`--pir-builtin-contracts`)

With `--kernel-api`, three intrinsics the kernel headers emit have no body and no spec: `llvm.read_register` (the stack pointer of
`ASM_CALL_CONSTRAINT`, 155 Linux functions in the W8 data), `llvm.prefetch` and `llvm.objectsize`. Each gets a contract of its own:
`read_register`: an arbitrary value (a pointer result is an arbitrary value that points to no object), no memory effect; `prefetch`:
no effect (the address is not accessed); `objectsize`: an arbitrary value (so a fortify-style check that depends on it is decided
neither way). Named `builtin-contract: @...`. Never active in user mode.

## 6. Measurement

Method. The W8 Stage 1 units that had run (36 Linux, 15 FreeBSD units of `data/pir-scale`, snapshot of the committed
`stage1-*.jsonl.gz`) were run again with the W8 flags and budgets (`tools/pir_scale_run.sh`: `--kernel-api`, `--pir-harness`,
`--pir-discharge --pir-container-of --pir-vacuity --assume-unknown-calls`, 6 s per function, 5 s per VC, Houdini 20 s, unit wall limit
1500 s, 4 workers, 6000 MB each) plus `--callee-contracts scripts/pir_callee_contracts_phase2.tsv --pir-ops-auto --pir-addr-values
--pir-harness-deep` (Linux also `--pir-builtin-contracts`; the FreeBSD run was made before that flag existed and has no
`llvm.read_register`). Functions are matched by (unit, function, start line). Driver: `scripts/pir_phase2_rerun.sh`; the comparison
script is `tools/pir_phase2_compare.py` in OS-Proving and the raw records are in `data/pir-scale/phase2/` there.

Caveats that decide how to read the table:

* The machine ran at load 55 to 80 (W8: 35 to 50), so the 6 s function budget produces more TIMEOUT. A run of the **same binary
  without the new flags** on 14 Linux units and all FreeBSD units ("same binary" below) separates the effect of the flags from the
  drift between the W8 binary and this one, and from the load: against W8 that run alone moved UNKNOWN 41 to 12 and PROVED-ASSUMING
  +18 (Linux, 14 units). Those changes are not the flags.
* Linux: 34 of the 36 units are in the comparison. `drivers/base/cacheinfo.c` hit the 1500 s unit limit in both after-runs (W8: 277 s;
  the same-binary run without the flags finished it) and `drivers/cxl/core/mbox.c` failed in the clang front end (the environment: it
  failed again on retry). Four other units failed in the front end during the first run and were rerun.
* The contract file holds 11 hand-written contracts (UNVALIDATED, the source named in each row), the only contract data these units
  have: no mined ACSL contract exists for them, and the clang ops data (W5) carries field names but not field indices (section 2),
  so it could not be used.

Linux (34 units, 1,684 functions), W8 to after:

| label | W8 | after | change |
|---|---:|---:|---:|
| PROVED | 40 | 42 | +2 |
| PROVED-UNBOUNDED | 1 | 1 | 0 |
| PROVED-ASSUMING | 502 | 527 | +25 |
| BOUNDED | 20 | 21 | +1 |
| FAILED | 37 | 38 | +1 |
| NEEDS-HARNESS | 897 | 870 | -27 |
| UNKNOWN | 68 | 46 | -22 |
| TIMEOUT | 119 | 139 | +20 |

FreeBSD (15 units, 790 functions), W8 to after: PROVED 18 to 19, PROVED-ASSUMING 189 to 189, BOUNDED 19 to 18, FAILED 5 to 5,
NEEDS-HARNESS 482 to 490, UNKNOWN 36 to 36, TIMEOUT 39 to 31. The functions that moved to NEEDS-HARNESS were TIMEOUT or BOUNDED under the
load. No function-pointer field was bound (the 130 `null-call` ones stay: their tables are assigned by other units).

Same binary, no new flags, to after (this is the effect of the flags):

| | NEEDS-HARNESS | PROVED-ASSUMING | label changes |
|---|---:|---:|---|
| Linux, 803 functions matched | 480 to 466 (-14) | 200 to 201 | 9 NEEDS-HARNESS to TIMEOUT, 5 to UNKNOWN, 1 to PROVED-ASSUMING |
| FreeBSD, 749 functions | 469 to 465 (-4) | 185 to 185 | 3 NEEDS-HARNESS to BOUNDED, 1 to TIMEOUT |

What the new mechanisms did (Linux, after): a contract stub is in the result of 139 functions, the address model in 31, a deeper harness
object in 13, a bound function-pointer field in 4 (FreeBSD: 11, 0, 7, 0). NEEDS-HARNESS by message, Linux:

| blocker | W8 | after |
|---|---:|---:|
| `UNENCODED: call @...` (`llvm.read_register`, `objectsize`, `prefetch`, others) | 172 | 64 |
| `UNENCODED: constant expression ptrtoint` | 60 | 0 |
| `UNENCODED: initializer ... / constant getelementptr on inttoptr` | 29 | 0 |
| new: `UNENCODED: inline assembly with outputs of type { iN x 5 or 6 }` and similar | 0 | 114 |
| new: `UNENCODED: constant pointer offset out of range` | 0 | 15 |
| new: violation only under the address-value model | 0 | 5 |
| `UNENCODED: extern object of unknown size` | 34 | 49 |
| `UNENCODED: call depth > 4` | 202 | 222 |

Reading: the blockers the items target are gone (about 200 functions), and nearly all of those functions stopped at the **next**
blocker: the inline asm with a 5 or 6 field struct result that the stack-pointer functions run into, call depth > 4, constant offsets
from an integer base, extern objects of unknown size. Some of the unblocked ones now run the solver and hit the 6 s budget
(NEEDS-HARNESS to TIMEOUT: 18 against W8, 9 against the same binary). The net change in NEEDS-HARNESS is -27 of 897 (3 %) on Linux against
W8, +8 on FreeBSD (load), and -14 and -4 against the same binary. Faithful summary: the mechanisms work and their uses are named, but
on these units they do not reduce NEEDS-HARNESS by much; the table says where to go next.

## 7. What is left

* **Inline asm with a struct result of more than two fields** (`{ i64 x 5 }`, `{ i64 x 6 }`): 114 Linux functions now stop here, mostly
  the `ASM_CALL_CONSTRAINT` paths that `--pir-builtin-contracts` opened. `Frame::pairs` carries only two-field aggregates; an n-field
  tuple for `asm_havoc` and `extractvalue` is the next change (W4 asm model).
* **Function-pointer fields for FreeBSD and for fields assigned in another unit**: the ops data of the clang pass
  (`data/clang/*/ops.tsv.gz`: struct, field name, function) has no field index, and the unit's IR has line tables only
  (`-gline-tables-only`), so a name cannot be mapped to the LLVM struct index. With the index (DWARF members, or the struct layouts of
  the headers) `--ops-table` binds them through the same code path as `--pir-ops-auto`. The in-unit derivation bound 4 Linux functions
  and no FreeBSD function.
* **Contract data**: only 11 hand-written rows exist. Mined ACSL for the callees of these units, or a status-carrying export of the
  W1 specs, would make the stubs matter; the loader and the status rule are ready. A contract is never validated against its callee by
  this change: a callee that does not satisfy it makes the proofs that used it wrong, which is exactly the named assumption.
* **Call depth > 4** (222 Linux, 203 FreeBSD functions) is the largest blocker left and is not a body-less callee: a contract stub for a
  callee *with* a body would need the contract proved against the body (an assume-guarantee proof of the callee), not built.
* The MMIO base object (`hw->hw_addr`, an `__iomem` field with no pointee type) is still an arbitrary pointer.
* **Variadic callees** (`sysfs_emit` 75 functions, `seq_printf`): a contract signature cannot say `...`; they stay unknown calls.
* Absolute addresses are not modelled; per-cpu offset arithmetic on a `ptrtoint` base is NEEDS-HARNESS, never a result.
* `cacheinfo.c` times out with the new flags (1500 s; W8 277 s; the same binary without the flags: 276 s). Isolated by flag on that unit
  (900 s limit): `--pir-harness-deep` alone 283 s, `--pir-ops-auto` alone 307 s, `--callee-contracts` with `--pir-builtin-contracts` 306 s,
  `--pir-addr-values` alone over 900 s (twice). The 21 functions of that unit that stopped at `UNENCODED: constant expression ptrtoint`
  now run the checks, and the pass that follows them (the call-site discharge, budget `PRISM_DISCHARGE_BUDGET` 900 s, plus the vacuity
  check) is the likely cost; the function-level cause was not located (the machine was at load 80 to 100).
