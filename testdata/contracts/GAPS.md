# Contract library: gap list

What PRISM could not prove or could not express in `testdata/contracts/`,
with the exact reason (`prism DIR --no-llm --stage
inventory,classify,contracts,pir`, 15 Nov 2026, `--timeout 30`, `--unwind
8`). Each entry names the functions it concerns and what stands in for the
missing proof. Nothing here is counted as proved (Laws 2, 7).

## 1. Not proved

| # | functions | status | exact reason | what stands in |
|---|---|---|---|---|
| 1 | `div_round_up`, `div_u64_rem` (arith/), `kmodel_div_contract` (kmodels/) | contracts / pir `UNKNOWN` | the ensures are multiply/divide identities on 32/64-bit bit vectors (`\result * d >= n && (\result - 1) * d < n`, `\result * d + *rem == n`): z3 times out (`VC assert@…: solver timeout`, bucket `QF_BV|w32/w64|muldiv`), also at `--timeout 300`; sls finds no assignment; bitwuzla, cadical and kissat are not installed on this machine (`missing:` in the message, `prism-deps tool NAME`) | none. The kernel models `div_u64` / `div_u64_rem` are the operation itself (`/`, `%`, divisor checked non-zero), so a caller's proof does not depend on the identity; the identity itself is open |
| 2 | `list_add`, `list_add_tail`, `list_del` (list/) | contracts and pir `NEEDS-HARNESS` | their requires name an object reached through memory (`\valid(head->next)`, `\valid(node->prev)`): the pir stage binds only pointer *parameters* (docs/PIR.md "Pointer parameters"), so the neighbour is an unbound pointer and the stage reports the violation it finds under it as NEEDS-HARNESS (`a precondition the pir stage does not assume`) | `list_add_contract`, `list_add_tail_contract`, `list_del_contract` (pir PROVED): lists of 0..3 nodes in a local array, structure checked node by node; size-bounded |
| 3 | `slist_pop` (list/) | contracts and pir `NEEDS-HARNESS` | same as 2: `*head == \null \|\| \valid(*head)` is a validity of a pointer stored in memory | `slist_contract` (PROVED): push two, pop two |
| 4 | `wrong_signature` (callers/wrong_sig.c) | pir `NEEDS-HARNESS` | intended: a declaration named like a library contract with another signature is another function (`UNENCODED: call @__ilog2_u32 (its declaration does not match the library contract's signature)`) | - |

## 2. Proved only within a bound (size-bounded, never PROVED-UNBOUNDED)

| # | where | bound | exact reason |
|---|---|---|---|
| 5 | every `<fn>_contract` harness | its objects' size (lists of 0..3 nodes, `SN = 4` array elements, two-word bitmaps, `RING_SIZE`/`MRING_CAP`/`Q_CAP`/`POOL_N`/`HT_N` slots, 8-byte TLV buffers, 4-element folds) | the harnesses state what an ACSL clause here cannot (quantifiers, list structure, permutations, FIFO/LIFO order) on objects of a fixed size; the PROVED verdict is for every content of objects of that size, closed by unwinding (`unwinding assertion proved at unwind N`), not for every size |
| 6 | `lower_bound`, `bsearch_int` (search/) | at most `LB_STEPS = 4` halvings, `n <= 8` | the uncapped `while (lo < hi)` form got no answer within the 300 s function budget; the cap is part of the reference implementation (enough for `n < 16`) |
| 7 | `tlv_count`, `tlv_find` (parse/) | messages of at most `TLV_MAX_LEN = 8` bytes (longer ones are refused) | without the cap the walk loops did not close: BOUNDED after 300 s (the offset advances by a length read from the buffer) |
| 8 | ACSL contracts with `requires 0 <= n <= 16` (mem/, string/, fold/, crc/, parse/ `uleb128_u32`) | the stated range | the contracts stage checks the ensures under the requires; where pir closed the loop by k-induction (`bmemset`, `bmemcpy`, `bmemcmp`, `bmemchr`, `bstrnlen`, `strscpy`, `crc32_buf`, `inet_csum`, `fletcher16`, `argmin`, `argmax`) the memory-safety proof holds for every length (with the object-size assumption `n * size < 2^47`), the ensures only for the stated range |
| 9 | `sum_u8` (fold/) | pir `BOUNDED` (no violation within unwind 8; loops did not close) | the pir run of the implementation alone (no requires assumed, `n` unbounded) did not close the loop; the contracts stage proves `0 <= \result <= 255L * n` under `n <= 16`, and `sum_u8_contract` the exact sum for `n <= FN` |

## 3. Not expressible (refused, or written in a harness instead)

| # | construct | stage behaviour | where it is needed |
|---|---|---|---|
| 10 | `\forall`, `\exists`, `\at`, `\let`, … in a clause | contracts `ERROR` ("ACSL construct \forall is not encoded"), never a proof | sortedness and permutation (insertion sort), "no set bit before the answer" (find_*_bit), "every element before the lower bound is smaller", byte-wise copy results (memcpy, strscpy): stated with `assert` in the harnesses (5) |
| 11 | `assigns` (the frame) | not checked: the checker asserts the ensures only. The row says so (`extra.assigns`, `assigns_checked = false`, message "(assigns clause not checked)") | every `assigns` in the library; the harnesses check what they rely on (e.g. "untouched past n" in `insertion_sort_contract`, `bmemcpy_contract`) |
| 12 | `\valid` of a field path or of `*p` | not a pir binding (2, 3) | kernel lists, singly linked lists, any structure reached through a pointer field |
| 13 | address values (`ptrtoint` other than a same-object difference or comparison) | `UNENCODED: ptrtoint (address values are not modelled)` | `IS_ERR`, `PTR_ERR`, `ERR_PTR`, `IS_ERR_OR_NULL` (338 + 295 core-tier OS-Proving functions blocked, docs/CONTRACTS.md): not modelled |
| 14 | an array that is a struct's last member | exempt from its per-array bound (flexible array / struct hack, translate_mem.cpp): `s.v[-1]` in `struct stack { int top; int v[8]; }` reads `s.top`, inside the object, and is not reported | the planted `stack_pop_unchecked_bad` uses a local array instead (FAILED) |
| 21 | `behavior b: assumes A; ensures B;` | not read: the parser takes only `requires`/`ensures`/`invariant`/`decreases`/`assigns` clauses, so `assumes A` is dropped and `ensures B` is read as an unconditional ensures (a proof then holds for the stronger contract; a FAILED may be one that `A` excludes) | no library contract uses behaviors |

## 4. Model limits (the kernel callee contracts, `src/prism/pir/models/kernel/kernel.cpp`)

| # | models | limit |
|---|---|---|
| 15 | `spin_*`, `mutex_*`, `rcu_read_*`, `atomic_*`, `refcount_*`, `kref_*` | single-threaded: a lock changes no memory the caller sees, an atomic is a plain access. No data race, lock order or interleaving is checked (in each assumption id) |
| 16 | `kmalloc*`, `kzalloc*`, `kcalloc` | may always fail (NULL); the `gfp_t` flags are ignored, so a `__GFP_NOFAIL` caller without a NULL check is reported FAILED although the kernel never returns NULL to it |
| 17 | `readl`, `__readl`, `writel` | MMIO: a read is any value, a write changes no program memory; no device model |
| 18 | `static_key_*`, `arch_static_branch*`, `mem_alloc_profiling_enabled`, `msecs_to_jiffies` | configuration: any value on every call (two calls may disagree) |
| 19 | `crc32_le` | reads the `len` bytes (bounds checked); the value is any u32 (the CRC itself is proved only for the reference `crc32_buf`, crc/) |
| 20 | not modelled yet (most frequent core-tier callees left): `fortify_memset_chk` (154), `fortify_memcpy_chk` (141), `get_current` (129), `devm_kzalloc` (103), `dev_name` (89), `netdev_priv` (81), `__curthread` (85), `__kmem_cache_create` (75), `lockdep_init_map` (66), `atomic_fcmpset_long` (59) | the call stays `NEEDS-HARNESS (UNENCODED: call @name)`; `get_current` / `netdev_priv` / `__curthread` need the configuration's struct layout, `devm_*` a device-lifetime model |
