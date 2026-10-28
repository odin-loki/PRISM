# bitset shifts

Branch `finish/cxx-bitset-shift`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.
The four shift operators are on the existing header
`src/prism/pir/models/cxx/bitset`. The header is still a file, not a
directory.

`test` and `operator[]` are unchanged. An index `>= N` still throws
`std::out_of_range` from `test`, or hits the existing `operator[]`
precondition (`__position < this->size()`, `MEM-OOB-READ`). That path does
not return a padding bit from the last word. `bitset<0>` still stores no
word. There is no heap allocation.

The solver was not run. The yml records the contract. Nothing here is a proof.

## What the shifts do

`operator<<=`, `operator>>=`, `operator<<`, and `operator>>`, all `constexpr`
and `noexcept`.

- A count of 0 leaves every bit where it is.
- A count of `N` or more, including `numeric_limits<size_t>::max()`, clears
  every bit.
- Bits shifted in are 0. Bits shifted past `N` are dropped.
- `operator<<` and `operator>>` return a copy. The left-hand operand stays
  as it was.
- The shift reads and writes only the stored words. The word count of a
  count in `1 .. N-1` is less than the number of words, so the index stays
  inside that array. Whole words move high to low on a left shift and low
  to high on a right shift, so a source word is still the old value when it
  is read.
- Padding in the last word is not a bit. A right shift clears it first, so
  it cannot move into a position the set owns. A left shift clears it
  afterwards, so `count`, `all`, `to_ulong`, and `operator==` do not see it.
  `bitset<3>(0x7) <<= 1` is `to_ulong() == 6` and `count() == 2`; shifting
  that result right by 1 yields 3, not 7.
- `bitset<0>` stays empty for a count of 0 and for any larger count:
  `any()` is false, `all()` is true, `count()` and `to_ulong()` are 0.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- string constructors and `to_string` (those need `<string>`)
- bitwise operators
- `std::hash<std::bitset<N>>`

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`tests/cpp/test_qa_conformance.cpp` was not edited; `bitset` was already in
the header-name array. `src/prism/bmc.cpp` was not edited.

## Tests

`testdata/conformance/libc-models/bitset_contracts.cpp` gained
`bitset_shift_true` (expected `true` in `bitset_contracts.yml`). It checks a
count of 0, a count `>= N`, the zero bits shifted in, a 3-bit value whose
padding must stay out of `to_ulong`, and `bitset<70>` shifts by 1, 32, 64,
and 65. `bitset_empty_true` shifts `bitset<0>` and still expects `test(0)`
to throw. `bitset_index_oob_false` and `bitset_test_oob_false` are unchanged.
No false harness calls `operator[]` at a new index.

Differential, clang++ 18.1.3, libstdc++ 14 (the GCC 14 headers clang uses),
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -O0`. `tests/cxx_models/bitset_trace.cpp` compiled
twice: once as libstdc++, once with `-isystem` on a directory that contains
only this header. The models directory was not that path, because it also
contains the partial `<bit>` header and libstdc++ 14 `<string>` includes
`<bit>`. `-H` names the model file first. Stdout matched, including
`shl1 ... ul=74`, `shlhuge ... ul=0`, `n3shl ul=6 count=2`,
`w64 count=2 t0=0 t5=0 t64=1 t69=1`, `w64back count=2 t0=1 t5=1 t64=0`,
`shift-test4: [bitset::test: __position (which is 4) >= _Nb (which is 4)]`,
and `empty-shift any 0 all 1 count 0 eq 1`. The same flags compiled and ran
`bitset_ops_true`, `bitset_empty_true`, and `bitset_shift_true` against
libstdc++ and against the model; both printed `contracts ok`.

No engine configure, no `prism_tests`, no conformance run.
