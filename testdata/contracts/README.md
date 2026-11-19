# Contract library

Reference C implementations of the data structures and loops that recur in
kernel and user-space code, each with its ACSL contract, proved by PRISM
itself (`prism DIR --no-llm --stage inventory,classify,contracts,pir`).
Design and results: [docs/CONTRACTS.md](../../docs/CONTRACTS.md); what is not proved or
not expressible, with the exact reason: [GAPS.md](GAPS.md).

| directory | what |
|---|---|
| `ring/` | ring buffers: power-of-two mask with free-running indices; modulo with a count |
| `list/` | kernel circular doubly linked list (`__list_add`, `list_add`, `list_add_tail`, `list_del`, `list_empty`, `list_is_head`), `container_of`, singly linked stack |
| `bitmap/` | `__set_bit`, `__clear_bit`, `test_bit`, `find_first_bit`, `find_next_bit`, `find_first_zero_bit` |
| `refcount/` | saturating `refcount_t`, `kref` get/put |
| `alloc/` | fixed-size pool with a free list; bitmap first-fit allocator |
| `htab/` | hash tables: open addressing (linear probing) and chaining |
| `stackq/` | array stack and array queue |
| `search/` | lower bound / binary search, insertion sort |
| `mem/` | memset, memcpy, memcmp, memchr, strnlen loops |
| `string/` | bounded string copy (`strscpy`, `strlcpy` shapes) |
| `parse/` | TLV walk, TLV find, LEB128 varint |
| `fsm/` | state-machine step (switch and table, and their equivalence) |
| `crc/` | CRC-32 (bitwise and nibble table, equal), Internet checksum, Fletcher-16 |
| `fold/` | argmin, argmax, overflow-free sum |
| `arith/` | ilog2, roundup_pow_of_two, is_power_of_2, DIV_ROUND_UP, ALIGN, div_u64_rem, clamp |
| `kmodels/` | the kernel callee contracts (`src/prism/pir/models/kernel/kernel.cpp`) checked by including the model source |
| `callers/` | callers of declared-only kernel functions, answered by the callee contracts |

Conventions:

* Each file is self-contained (a short prelude declares `assert` and the
  `__VERIFIER_nondet_*` sources): the contracts stage compiles each function
  alone, without the directory on the include path.
* A function's contract is the ACSL block right above it. The contracts
  stage checks it (requires assumed, ensures asserted at every return);
  pointer parameters need a `\valid` / `\valid_read` requires, which the
  pir stage turns into objects of that size (PROVED-ASSUMING with that
  listed, never PROVED).
* `<fn>_contract` functions are harnesses with scalar parameters only: they
  build the objects with symbolic contents, assume the requires (an early
  return), call the implementation and assert the parts of the contract a
  clause cannot state (quantifiers, structure of a list, permutations).
  Their objects have a fixed size: size-bounded proofs.
* `_bad` functions are planted twins (a wrong ensures or a violated
  requires) and must be FAILED, so the proofs are not vacuous.
