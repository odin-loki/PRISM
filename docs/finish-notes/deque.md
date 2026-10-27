# std::deque model (finish/cxx-deque)

Minimal operational model of `std::deque<T, std::allocator<T>>`. It is not libstdc++'s block map and does not invent one. Each element is its own node from `::operator new` / `::operator delete` (the PIR new/delete models). Iterators hold a pointer to a heap anchor. Operations that invalidate every iterator replace that anchor and `operator delete` the old one, so a later iterator use loads freed memory (**MEM-UAF**). `pop_back` and erasure of a suffix do not invalidate other iterators; they delete an end cookie instead, and the erased element's node, so only those iterators dangle.

Checked with clang++-18, `-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined -O1`, against libstdc++ 14's `<deque>`: `tests/cxx_models/deque_trace.cpp` prints the same trace both ways. `deque_trace uaf` (iterator used after `push_back`) is ASan `heap-use-after-free` on the model anchor (`operator delete` in `emplace_back`). libstdc++ does not free a block on that push, so the doctest runs `uaf` only on the model binary.

The header is **not** on the pir include path yet. Tests pass `-isystem src/prism/pir/models/cxx/deque` so `#include <deque>` is this file. `tests/cpp/test_cxx_deque.cpp` is not in the `prism_tests` source list yet.

## Coverage

Implemented, and matched against libstdc++ for values (and for constructor/destructor order where the trace uses a user type):

- Constructors: default, allocator, count, count+value, iterator range, `initializer_list`, copy, move
- `assign` (count, range, initializer list) and copy/move/`initializer_list` assignment
- `begin`/`end`/`rbegin`/`rend` and the const overloads; random-access iterator (`++`, `--`, `+=`, `+`, `-`, `[]`, `==`, `<=>`)
- `size`, `empty`, `max_size` (`min(PTRDIFF_MAX, allocator_traits::max_size)`)
- `operator[]`, `at` (same `deque::_M_range_check` message), `front`, `back`
- `emplace`/`push`/`pop` at either end; single-element `emplace`/`insert`; fill, range, and initializer-list `insert`
- `erase` (one element and a range), `clear`, `resize`, `swap`
- `==`, `<=>`, `std::erase`, `std::erase_if`
- Preconditions under `_GLIBCXX_ASSERTIONS`: `operator[]` is `__n < this->size()` (MEM-OOB-READ); `front`/`back`/`pop_*` are `!this->empty()` (FUNC-CONTRACT); `erase`/`insert` reject an iterator from another deque

`push`/`insert` at either end invalidate every iterator and do not move surviving elements, so references to those elements stay at the same node (the trace checks this). Erasing an element `operator delete`s that node.

## Fall-throughs (not modelled)

A use that does not compile against this header is how the pir stage, once the header is wired, drops back to libstdc++ for the whole translation unit (`extra.cxx_models`).

- Allocators other than `std::allocator<T>` (`static_assert`, including `std::pmr::deque`)
- Over-aligned `T` (`alignof(T)` above `__STDCPP_DEFAULT_NEW_ALIGNMENT__`)
- `shrink_to_fit`
- C++23 `from_range`, `append_range`, `prepend_range`, `insert_range`, `assign_range`
- `constexpr` deque (the members are not `constexpr`)
- libstdc++'s block-map growth (512-byte nodes, initial map of 8, when a block is freed). Not implemented, not stubbed
- Middle `insert`/`erase` splice nodes instead of move-assigning the nearer side. The final sequence matches; a traced type's assignment count may not. The anchor is still freed, so every iterator is MEM-UAF. A reference to an element that was not itself destroyed stays live. The standard also invalidates those references; libstdc++ does not free their storage either
- Copy and `assign` are copy-then-swap. Final values match; a traced type sees extra constructions and then destructions of the old elements, not in-place assignment. That also frees the old nodes, so references into the target are MEM-UAF
- Range and fill `insert` are repeated single inserts. Inserting at either end matches one construction per element. A middle range does not reproduce libstdc++'s shift
- `pop_back`, `clear`, and suffix `erase` allocate a replacement end cookie (`::operator new`). libstdc++ does not. `pop_back` is `noexcept`, so that `bad_alloc` terminates. Move construction allocates a fresh empty control block for the source; libstdc++ only steals pointers
- Iterator arithmetic walks the node list (linear in the distance). It is not constant-time block arithmetic. `operator[]` walks `n` links, so a symbolic index needs an unwind bound of at least `n`

## Wiring follow-up

Do not edit `src/prism/pir/stage.cpp` for this. The include path is already `-isystem` on the directory `write_cxx_models` fills (`lower_to_ir` inserts `-isystem` + `fe.cxx_models`). The gap is the **embed name**.

1. `CMakeLists.txt` `PRISM_PIR_MODELS`: add `src/prism/pir/models/cxx/deque/deque`. The loop that sets `_mn` turns a path under `models/cxx/` into `cxx/<relative>`. That would embed `cxx/deque/deque` and write `<models>/deque/deque`, which `#include <deque>` does not open (it looks for a **file** named `deque`). Special-case this one path so the embedded name is `cxx/deque` (do not flatten `bits/basic_string.tcc`). `write_cxx_models` then writes `<models>/deque` next to `vector` and `map`, and the existing `-isystem` is enough.
2. `CMakeLists.txt` `_prism_test_sources`: add `tests/cpp/test_cxx_deque.cpp` next to `tests/cpp/test_cxx_models.cpp`.
3. Early "not a deque model" exits still fire on the source text, before the header is inlined:
   - `src/prism/bmc.cpp` (`low.find("deque")`)
   - `src/prism/bmc_unenc.inc` (`deque\s*<`)
   Remove those once the header is actually on the include path. `tests/cpp/test_concolic.cpp` expects `deque_unenc_bad` to be NEEDS-HARNESS until that changes.
4. `tests/cpp/test_qa_conformance.cpp` ("every C++ library model header is embedded and exercised") lists every file under `models/cxx/`. `deque/deque` has a slash, so it is treated as an internal header unless the `pub` map gains `pub["deque/deque"] = "deque"` (same idea as `bits/basic_string.tcc` → `string`). The case then requires `#include <deque>` both in `testdata/conformance/prism/cxx/` (the new `cxx_deque_true.cpp` / `cxx_deque_false.cpp` already do) and in some `testdata/conformance/libc-models/*.cpp` harness, which does not exist yet. Until the CMake line and that `pub` entry land, this case fails.
5. `docs/PIR.md` still lists `std::deque` under "Not modelled". Update that paragraph when the header is wired. Not in this slice.

Until (1) and (3), `cxx_deque_true` / `cxx_deque_false` still take the unmodelled-deque NEEDS-HARNESS path. After wiring, the false task is an iterator used after `push_back` (MEM-UAF on the anchor) and the true task is a reference kept across `push_back`.
