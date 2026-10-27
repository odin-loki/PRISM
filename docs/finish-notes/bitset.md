# bitset slice

Branch `finish/cxx-bitset`, base `66d1b3373ddf27f0a3187c35d2d19bd533a599cc`.
`std::bitset` is a header file, `src/prism/pir/models/cxx/bitset`, the same
shape as `vector`. A directory of that name would make `#include <bitset>`
fail with EISDIR and every such unit would fall back to libstdc++.

The solver was not run. The yml files record the contract each harness asks
for. Nothing here is a proof.

## What the model covers

`std::bitset<N>` stores N bits in the object: one `unsigned long` per word,
the same word width libstdc++ uses. There is no heap allocation. `bitset<0>`
has no word.

`set`, `reset`, and `flip` cover the whole set and one position. One-position
`set` / `reset` / `flip` and `test` throw `std::out_of_range` when the index
is `>= N`, with libstdc++'s format string (`bitset::test: __position (which
is %zu) >= _Nb (which is %zu)` and the same shape for set, reset, and flip).
`test` does not return a bit on that path. `N == 0` is empty: `any()` is
false, `all()` and `none()` are true, `count()` and `to_ulong()` are 0, and
`test(0)` throws.

`operator[]` uses the same `_GLIBCXX_ASSERTIONS` precondition as
`vector::operator[]`: `__position < this->size()` (PIR classifies a condition
that mentions `size()` as `MEM-OOB-READ`). The subscript does not return a
padding zero from the last word, and it does not return a bit for
`bitset<0>`. libstdc++'s own `operator[]` does not check; programs that meet
the standard's precondition match the library. A trace therefore does not
call `operator[]` at an index `>= N`.

`to_ulong()` returns the low `unsigned long`. Every value fits when `N` is at
most the width of `unsigned long` (32 or 64, whichever the platform gives
that type). A set bit at or above that width throws `std::overflow_error`
(`_Base_bitset::_M_do_to_ulong`). `to_ullong()` follows libstdc++ 14: when
`unsigned long long` has the same width it is `to_ulong()` (same overflow
message); when it is wider it uses the low two words and throws
`_M_do_to_ullong` if a higher word is set.

Also present: the default constructor, the `unsigned long long` constructor
(bits of the value past `N` are dropped), `operator==`, and
`bitset::reference` so `b[i] = true` writes a real bit.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models` records the
fallback). They are not stubbed:

- string constructors and `to_string`
- shift and bitwise operators
- `std::hash<std::bitset<N>>`

## Wiring

`src/prism/pir/stage.cpp` already writes every embedded `cxx/` model and
passes that directory as `-isystem`. It was not edited. `PRISM_PIR_MODELS`
lists `src/prism/pir/models/cxx/bitset`, so the embedded name is `cxx/bitset`.
`tests/cpp/test_qa_conformance.cpp` names `bitset` in the header array. The
iostream file and the `deque/deque` skip in that test are unchanged.

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return "is not a
bitset model" from the source text, before the header is inlined. Those
exits are still in place. `tests/cpp/test_concolic.cpp` still expects
`bitset_unenc_bad` to be `NEEDS-HARNESS` until they go. `houdini.inc` was
not edited.

## Tests

`testdata/conformance/libc-models/bitset_contracts.cpp` is the libc-contract
harness (`#include <bitset>`). `bitset_index_oob_false` is `operator[]` at
`size()` (`MEM-OOB-READ`). `bitset_test_oob_false` is `test(4)` on
`bitset<4>` leaving a `noexcept` function (`CXX-THROW-NOEXCEPT`): past `N`,
`test` throws `std::out_of_range` and does not return a bit.
`bitset_to_ulong_overflow_false` is a set bit that does not fit in
`unsigned long` (`CXX-THROW-NOEXCEPT`). `bitset_empty_true` catches
`test(0)` on `bitset<0>`.

`testdata/conformance/prism/cxx/cxx_bitset_true.cpp` guards the subscript.
`cxx_bitset_false.cpp` is `operator[]` at 4 on `bitset<4>` (`no-oob`,
witness `[4]`).

Differential, clang++ 18, libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined`. `tests/cxx_models/bitset_trace.cpp` compiled
twice (`-isystem` the model directory, and without it). Stdout matches,
including the `out_of_range` and `overflow_error` text. No doctest driver
was added, and `tests/cpp/test_cxx_models.cpp` was not edited. No engine
configure, no `prism_tests`, no conformance run.
