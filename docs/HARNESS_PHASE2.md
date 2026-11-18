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

(The first row of this document's brief, "body-less callee reached", does not appear in this data: with
`--assume-unknown-calls` every body-less call is already a havoc. It was the top blocker of the OS-Proving brute-force
pass, a different tool. What phase 2 adds for those callees is precision and checking: the call site is checked against
the contract, only what the contract allows to change is havocked, and the result is constrained.)

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
  used as a pointer ...`. A dereference of it is reported, and since kernel code does dereference fixed addresses, a violation
  in a function that used this model is **NEEDS-HARNESS** (`verdict_under_addr_model`), never FAILED.

## 4. Deeper pointer fields (`--pir-harness-deep`)

Depth 4 instead of 2 and 64 objects per parameter instead of 32 (an explicit larger `--pir-harness-depth` wins).
The bound is now recorded: the `auto harness for P` line ends with `deepest object at level R of D; pointer fields left
arbitrary: X past the depth bound, Y over the object cap of M, Z without pointee type evidence`, and the
`--jsonl` harness object gets `depth_bound`, `depth_reached`, `cut_past_depth`, `cut_over_cap`, `cut_no_type`,
`function_pointer_fields_bound` and `address_value_assumptions`.

## What is not done

See the end of the pull request description (`docs/HARNESS_PHASE2.md` is updated with the measurement).
