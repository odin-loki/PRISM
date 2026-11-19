# Contract library

Workstream W4 of [TARGETED_PROVING.md](TARGETED_PROVING.md): reference C
implementations of the data structures and loops that recur in kernel and
user-space code, each with an ACSL contract that PRISM proves itself, and
kernel callee contracts that answer a call to a function the unit declares
but does not define. Sources: `testdata/contracts/` (one directory per
topic, [README](../testdata/contracts/README.md)); the gap list (what is
not proved or not expressible, with the exact reason):
[testdata/contracts/GAPS.md](../testdata/contracts/GAPS.md).

Run: `prism testdata/contracts/<topic> --no-llm --stage
inventory,classify,contracts,pir`.

## How a contract is checked

**ACSL contracts (contracts stage, `src/prism/stages/contracts.cpp`).** The
block `/*@ ... */` right above a function (several lines allowed) is the
contract. The checker compiles the function with its `requires` as an early
return and each `ensures` asserted at every return, through the pir checker.
Accepted forms: C expressions with `\result`, `\old(e)` (evaluated once on
entry), `==>`, `<==>`, chained comparisons (`0 <= i < n` is `0 <= i && i <
n`, never C's `(0 <= i) < n`), `\true`, `\false`, `\null`; a formal parameter
in an `ensures` is its value on entry. In a `requires`, `\valid(p)`,
`\valid(p + (0..n-1))`, `\valid_read(...)` and `\separated(...)` bind the
pointer parameters: the pir stage gives `p` a fresh object of that size
(docs/PIR.md "Pointer parameters (Law 6)"), so a pointer function whose
every pointer parameter has such a clause is checked like a scalar one and
its proof is **PROVED-ASSUMING** with the binding listed, never PROVED. A
pointer function without one stays NEEDS-HARNESS ("would invent a buffer").
Any other `\construct` (`\forall`, `\exists`, `\at`, `\let`) is ERROR, never
a proof. `assigns` is not checked; the row says so (`extra.assigns`,
`assigns_checked = false`, "(assigns clause not checked)"). A violation is
reported at its line in the source (the inserted lines are renumbered away
with `#line`).

**Requires the pir stage does not assume.** The pir stage binds the objects
a `\valid` clause names but does not assume the other requires (`r->head <
8`, `0 <= n <= 16`). A violation it finds that such a requires excludes is
NEEDS-HARNESS ("violation … under a precondition the pir stage does not
assume", `extra.unbound_requires`, `extra.verdict_under_unbound_requires`),
not FAILED: the refutation depends on inputs the contract rules out. The
contracts stage, which assumes them, gives the answer for such functions.

**Harnesses.** `<fn>_contract` functions have scalar parameters only: they
build the objects with symbolic contents, assume the requires (an early
return), call the implementation, and assert what a clause cannot state
(quantifiers, list structure, permutations, FIFO order). Their objects have
a fixed size: the PROVED verdict is for every content of objects of that
size (size-bounded, gap list §2).

**Planted twins.** Every topic has `_bad` functions (a wrong ensures, a
violated requires, an off-by-one) that must be FAILED by the stage that
checks what they plant, so no proof is vacuous.

## Kernel callee contracts

`src/prism/pir/models/kernel/kernel.cpp` (embedded and lowered like the libc
models, docs/PIR.md "Library models") defines kernel functions as C models.
When a unit declares one of them and does not define it, the model is linked
in and the call is inlined:

* the model's first statement `__prism_contract("kernel:<name> (...)")`
  lists **"callee behaves as the library contract kernel:<name> (...)"** in
  the caller's `extra.assumptions`: a proof is PROVED-ASSUMING, never PROVED
  (the real callee is not checked);
* the requires are checked at the call site: every load, store and range
  check of the model goes through the memory model like user code (a
  `__set_bit(64, map)` on a one-word bitmap is MEM-OOB-READ "in the
  __set_bit library model"; `kzalloc` may return NULL; `kfree` twice is
  MEM-DOUBLE-FREE);
* the ensures are the operation itself for the loop-free models (list, bit,
  refcount, arithmetic, allocation) and are assumed on a fresh value for the
  searching loops (`find_*_bit`, `crc32_le`);
* a declaration with another signature is another function: the call stays
  NEEDS-HARNESS ("its declaration does not match the library contract's
  signature", translate.cpp `inline_call`).

Models (72): lists (`INIT_LIST_HEAD`, `__list_add`, `list_add`,
`list_add_tail`, `list_del`, `list_empty`, `list_is_head`); bits (`__set_bit`,
`set_bit`, `__clear_bit`, `clear_bit`, `test_bit`, `_test_bit`,
`arch_test_bit`, `const_test_bit`, `find_first_bit`, `find_next_bit`,
`find_first_zero_bit`, `find_next_zero_bit` and their `_`-prefixed forms);
arithmetic (`__ilog2_u32`, `__ilog2_u64`, `__roundup_pow_of_two`, `div_u64`,
`div_u64_rem`); `atomic_read/set/inc/dec/dec_and_test`; `refcount_set/read/
inc/dec_and_test`, `kref_init`, `kref_get`; allocation (`kmalloc`, `kzalloc`,
`kcalloc`, `kmalloc_array` and their `_noprof` forms, `kfree`); `strscpy`;
`crc32_le`; environment (static keys, `mem_alloc_profiling_enabled`,
`spinlock_check`, `spin_lock/unlock[_bh|_irqrestore]`, `mutex_lock/unlock`,
`rcu_read_lock/unlock`, `cpu_relax`, `might_resched`, `udelay`,
`usleep_range`, `msleep`, `msecs_to_jiffies`, `readl`, `__readl`, `writel`).
Each is the weakest contract that is still true (gap list §4: locks and
atomics single-threaded, allocation may always fail, MMIO and configuration
any value).

The models are checked by PRISM itself: `testdata/contracts/kmodels/`
`#include`s `kernel.cpp` (nothing is assumed about included model code) and
states each contract with `assert` on symbolic objects; `callers/` calls the
declared-only functions. The doctest `tests/cpp/test_contracts_library.cpp`
locks both mechanisms.

## Results (15 Nov 2026)

RESULTS-TABLE

## OS-Proving priority table

From `/home/user/odin-loki/os-proving/data` (model-written labels and mined
contracts: hypotheses, never inputs, Law 4). Kinds over all functions of
`rounds/verify/step12.jsonl.gz` (n = 1,135,593), the core tier's blocked
callees (`proving/core/labels.tsv.gz`, "callees-without-spec", 24,806
functions), and contract shapes in `errors/annotations-original.jsonl.gz`
(118,358 contracts).

| function kind | functions | covered by |
|---|---:|---|
| driver-glue | 417,958 | kernel callee contracts only (allocation, locks, MMIO, lists); the glue itself has no library shape |
| init-teardown | 142,465 | `kzalloc`/`kfree`/`INIT_LIST_HEAD`/lock models (callers/) |
| other | 117,346 | - |
| state-machine | 85,924 | `fsm/` (switch and table step, their equivalence) |
| table-lookup | 76,189 | `htab/` (open and chained), `fsm/` table, `search/` |
| arith-helper | 65,471 | `arith/` (ilog2, roundup, power of two, DIV_ROUND_UP, ALIGN, div_u64_rem, clamp), `crc/`, `fold/` |
| parser | 52,011 | `parse/` (TLV walk and find, LEB128) |
| error-path | 43,806 | none: `IS_ERR`/`PTR_ERR` need address values (gap 13) |
| allocator | 24,420 | `alloc/` (pool and free list, bitmap first-fit), `kmalloc*` models |
| string-op | 21,313 | `mem/` (memset, memcpy, memcmp, memchr, strnlen), `string/` (strscpy, strlcpy) |
| list-op | 19,454 | `list/` and the list models |
| lock-or-sync | 18,027 | lock models, single-threaded only (gap 15) |
| refcount | 17,579 | `refcount/` (saturating refcount_t, kref) and models |
| ring-buffer | 14,103 | `ring/` (mask with free-running indices, modulo with a count), `stackq/` queue |
| bitmap | 10,537 | `bitmap/` and the bit models |
| tree-op | 7,617 | none (no rbtree/radix contract yet) |
| bitmask / stack-op | 299 / 39 | `arith/`, `bitmap/` / `stackq/` |

Most frequent blocked callees of the core tier (functions blocked) and the
answer: `mem_alloc_profiling_enabled` 599, `static_key_false` 392,
`static_key_true` 386, `arch_static_branch` 385, `arch_static_branch_jump`
385 (modelled); `IS_ERR` 338, `PTR_ERR` 295 (not expressible, gap 13);
`spinlock_check` 252, `kzalloc_noprof` 231, `INIT_LIST_HEAD` 173 (modelled);
`fortify_memset_chk` 154, `fortify_memcpy_chk` 141 (not yet);
`spin_unlock_irqrestore` 135 (modelled); `get_current` 129 (not yet);
`list_is_head` 121, `spin_unlock` 120, `readl` 115, `spin_lock` 113
(modelled); `devm_kzalloc` 103 (not yet); `writel` 93, `kmalloc_array_noprof`
88, `__ilog2_u32` 86, `__ilog2_u64` 86, `udelay` 80, `_test_bit` 75,
`const_test_bit` 74, `find_next_bit` 72, `kmalloc_noprof` 72,
`list_add_tail` 70 (modelled).

Coverage of the core tier by the 72 models: 6,820 functions have at least
one callee without a spec (26,304 call edges); **5,093 edges (19.4%)** are
answered by a kernel contract; **2,046 functions (30.0%)** have at least one
blocked callee answered, **442 (6.5%)** all of them (by kind: init-teardown
142, arith-helper 116, allocator 41, error-path 39, lock-or-sync 23, bitmap
19, ring-buffer 19, refcount 19, list-op 14, parser 10).

Contract shapes in the mined contracts (share of 118,358; one contract can
have several): range requires `0 <= x` 7.3% (accepted), bit operations 6.6%
(accepted), `\result == e` 5.6% (accepted), `\valid_read` 5.4% and `\valid`
5.2% (accepted, bind pointer parameters; not of field paths, gap 12), `\old`
3.0% (accepted), error-code results `\result < 0` / `== 0 ||` 1.9%
(accepted), `\exists` 1.3% and `\forall` 1.3% (refused, gap 10), `\null`
1.1% (accepted), string validity 0.5% (`valid_string` refused; strings in
harnesses), `\separated` 0.1% (accepted), `assigns` 0.1% (not checked, gap
11), behaviors < 0.1% (not read, gap 21).
