# bitset OR

Branch `finish/cxx-bitset-or`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.
`operator|=` and `operator|` are on the existing header
`src/prism/pir/models/cxx/bitset`. The header is still a file, not a
directory.

`test` and `operator[]` are unchanged. An index `>= N` still throws
`std::out_of_range` from `test`, or hits the existing `operator[]`
precondition (`__position < this->size()`, `MEM-OOB-READ`). That path does
not return a padding bit from the last word. The four shift operators and
`operator&` are unchanged. A shift of 0 leaves every bit where it is. A
shift of `N` or more clears every bit. AND still keeps a bit only when both
sides have it. `bitset<0>` still stores no word. There is no heap
allocation.

The solver was not run. The yml records the contract. Nothing here is a proof.

## What OR does

`operator|=` and `operator|`, both `constexpr` and `noexcept`.

- Each result bit is the OR of the two bits at that position. A 1 in
  either operand stays 1. The operation does not drop a 1.
- `operator|` returns a copy. Both operands stay as they were.
- `operator|=` writes the result into the left-hand operand and leaves the
  right-hand operand unchanged.
- The OR reads and writes only the stored words, then calls `_M_sanitize`.
  Padding in the last word is not a bit of the result. `bitset<3>` of all
  ones OR itself is `to_ulong() == 7` and `count() == 3`. A value whose
  constructor bits sit past `N` does not contribute those bits:
  `bitset<3>(0xFF) | bitset<3>(0xAA)` is `to_ulong() == 7` and `count() == 3`.
- `bitset<96>` set in every position stays `count() == 96` and `all()` after
  OR with itself. OR with an empty set stays that set: `all()` is true and
  `none()` is false.
- `bitset<70>` OR keeps every position set on either side, including bits
  past the first word (0, 63, 64, and 69 stay).
- `bitset<0>` stays empty: `any()` is false, `all()` is true, `count()` and
  `to_ulong()` are 0. `test(0)` still throws.
- The OR does not allocate. The returned object holds its words inline, the
  same way the rest of this header stores a bitset.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- string constructors and `to_string` (those need `<string>`)
- `operator^` and `operator~`
- `std::hash<std::bitset<N>>`

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`tests/cpp/test_qa_conformance.cpp` was not edited; `bitset` was already in
the header-name array. `src/prism/bmc.cpp` was not edited.

## Tests

`testdata/conformance/libc-models/bitset_contracts.cpp` gained
`bitset_or_true` (expected `true` in `bitset_contracts.yml`). It checks
per-position OR, that `operator|` leaves both operands unchanged, a 3-bit
value whose padding must stay out of `to_ulong` and `count`, `bitset<70>`
and `bitset<96>`, and `bitset<0>`. `bitset_empty_true` ORs `bitset<0>` and
still expects `test(0)` to throw. `bitset_shift_true`, `bitset_and_true`,
`bitset_index_oob_false`, and `bitset_test_oob_false` are unchanged.
No false harness calls `operator[]` at a new index.

Differential, clang++ 18.1.3, libstdc++ 14 (the GCC 14 headers clang uses),
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -O0`. `tests/cxx_models/bitset_trace.cpp` compiled
twice: once as libstdc++, once with `-isystem` on a directory that contains
only this header. The models directory was not that path, because it also
contains the partial `<bit>` header and libstdc++ 14 `<string>` includes
`<bit>`. `-H` names the model file first. Stdout matched, including
`or ... ul=245`, `oreq ... ul=245`, `or0 ... ul=165`, `orid ... ul=255`,
`orself ... ul=165`, `n3or ul=7 count=3`, `n3orall ul=7 count=3 all=1`,
`n3mask ul=7 count=3`, `wor count=4 t0=1 t63=1 t64=1 t69=1 lhs=3`,
`woreq count=4 eq=1 rhs=3`, `w96 count=96 all=1 zero=0`,
`or-test4: [bitset::test: __position (which is 4) >= _Nb (which is 4)]`,
`empty-or any 0 all 1 count 0 eq 1`, and the unchanged AND and shift lines
`and ... ul=160`, `n3and ul=5 count=2`, `n3mask ul=2 count=1`,
`wand count=2 t0=0 t63=1 t64=0 t69=1 lhs=3`, `w96 count=96 all=1 zero=1`,
`shl0 ... ul=165`, `shl8 ... ul=0`, `shlhuge ... ul=0`,
`n3shl ul=6 count=2`, `w64 count=2 t0=0 t5=0 t64=1 t69=1`, and
`empty-shift any 0 all 1 count 0 eq 1`. The same flags compiled and ran
`bitset_ops_true`, `bitset_empty_true`, `bitset_shift_true`,
`bitset_and_true`, and `bitset_or_true` against libstdc++ and against the
model; both printed `contracts ok`.

No engine configure, no `prism_tests`, no conformance run.
