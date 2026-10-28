# bitset XOR

Branch `finish/cxx-bitset-xor`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.
`operator^=`, `operator^`, and unary `operator~` are on the existing header
`src/prism/pir/models/cxx/bitset`. The header is still a file, not a
directory. There is no binary `operator~`.

`test` and `operator[]` are unchanged. An index `>= N` still throws
`std::out_of_range` from `test`, or hits the existing `operator[]`
precondition (`__position < this->size()`, `MEM-OOB-READ`). That path does
not return a padding bit from the last word. The four shift operators,
`operator&`, and `operator|` are unchanged. A shift of 0 leaves every bit
where it is. A shift of `N` or more clears every bit. AND still keeps a bit
only when both sides have it. OR still keeps a bit when either side has it.
`bitset::reference::operator~` is unchanged: it still returns the flipped
value of one bit as `bool`. `bitset<0>` still stores no word. There is no
heap allocation.

The solver was not run. The yml records the contract. Nothing here is a
proof. The trace is a differential check of the model against libstdc++; it
is not a CLEAN result.

## What XOR and complement do

`operator^=`, `operator^`, and unary `operator~`, all `constexpr` and
`noexcept`.

- Each XOR result bit is the XOR of the two bits at that position. A bit
  is set in the result when it is set on exactly one side. The operation
  does not invent a 1 from padding.
- `operator^` returns a copy. Both operands stay as they were.
- `operator^=` writes the result into the left-hand operand and leaves the
  right-hand operand unchanged.
- The XOR reads and writes only the stored words, then calls `_M_sanitize`.
  Padding in the last word is not a bit of the result. `bitset<3>` of
  `0xFF` XOR `bitset<3>` of `0xAA` is `to_ulong() == 5` and `count() == 2`.
  XOR of a 3-bit set with itself is `to_ulong() == 0` and `count() == 0`.
- Unary `operator~` flips each stored bit, then calls `_M_sanitize`, so
  padding does not become a bit. The operand is unchanged. `~bitset<3>(0x7)`
  is `to_ulong() == 0` and `count() == 0`. `~bitset<3>()` is
  `to_ulong() == 7` and `count() == 3`. `~bitset<3>(0xFF)` is that same zero
  result: constructor bits past `N` are not bits, and the flipped padding
  is not a bit either.
- `bitset<96>` set in every position XOR itself is `none()` and
  `count() == 0`. XOR with an empty set stays that set: `all()` is true and
  `count() == 96`. The complement of the full set is `none()` and
  `count() == 0`. The complement of the empty set is `all()` and
  `count() == 96`.
- `bitset<70>` XOR keeps a position only when the two sides differ,
  including bits past the first word (0 and 64 stay, 63 and 69 drop).
  The complement of bits 0, 63, and 69 has `count() == 67`, those three
  positions clear, and positions 1 and 64 set. Complementing again restores
  the original.
- `bitset<0>` stays empty: `any()` is false, `all()` is true, `count()` and
  `to_ulong()` are 0. `test(0)` still throws.
- Neither operation allocates. The returned object holds its words inline,
  the same way the rest of this header stores a bitset.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- string constructors and `to_string` (those need `<string>`)
- `std::hash<std::bitset<N>>`

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`tests/cpp/test_qa_conformance.cpp` was not edited; `bitset` was already in
the header-name array. `src/prism/bmc.cpp` was not edited.

## Tests

`testdata/conformance/libc-models/bitset_contracts.cpp` gained
`bitset_xor_true` (expected `true` in `bitset_contracts.yml`). It checks
per-position XOR, that `operator^` leaves both operands unchanged, unary
complement including a 3-bit value whose padding must stay out of
`to_ulong` and `count`, `bitset<70>` and `bitset<96>`, and `bitset<0>`.
`bitset_empty_true` XORs and complements `bitset<0>` and still expects
`test(0)` to throw. `bitset_shift_true`, `bitset_and_true`,
`bitset_or_true`, `bitset_index_oob_false`, `bitset_test_oob_false`, and
`bitset_to_ulong_overflow_false` are unchanged. Their `expect_class` values
stay `MEM-OOB-READ` and `CXX-THROW-NOEXCEPT`. No false harness was added.
No false harness calls `operator[]` at a new index.

Differential, clang++ 18.1.3, libstdc++ 14 (the GCC 14 headers clang uses),
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -O0`. `tests/cxx_models/bitset_trace.cpp` compiled
twice: once as libstdc++, once with `-isystem` on a directory that contains
only this header. The models directory was not that path, because it also
contains the partial `<bit>` header and libstdc++ 14 `<string>` includes
`<bit>`. `-H` names the model file first. Stdout matched, including
`xor ... ul=85`, `xoreq ... ul=85`, `xor0 ... ul=165`, `xorid ... ul=90`,
`xorself ... ul=0`, `not ... ul=90`, `notnot ... ul=165`, `not0 ... ul=255`,
`notall ... ul=0`, `n3xor ul=2 count=1`, `n3xorall ul=0 count=0`,
`n3mask ul=5 count=2`, `n3not ul=0 count=0 none=1`,
`n3not0 ul=7 count=3 all=1`, `n3notpad ul=0 count=0`,
`wxor count=2 t0=1 t63=0 t64=1 t69=0 lhs=3`,
`wxoreq count=2 eq=1 rhs=3`,
`wnot count=67 t0=0 t1=1 t63=0 t64=1 t69=0 src=3`, `wnot2 eq=1`,
`w96 count=0 all=1 zero=1 notc=0 notall=1`,
`xor-test4: [bitset::test: __position (which is 4) >= _Nb (which is 4)]`,
`not-test4: [bitset::test: __position (which is 4) >= _Nb (which is 4)]`,
`empty-xor any 0 all 1 count 0 eq 1`, `empty-not any 0 all 1 count 0 eq 1`,
and the unchanged OR, AND, and shift lines
`or ... ul=245`, `n3or ul=7 count=3`, `n3mask ul=7 count=3`,
`wor count=4 t0=1 t63=1 t64=1 t69=1 lhs=3`, `w96 count=96 all=1 zero=0`,
`and ... ul=160`, `n3and ul=5 count=2`, `n3mask ul=2 count=1`,
`wand count=2 t0=0 t63=1 t64=0 t69=1 lhs=3`, `w96 count=96 all=1 zero=1`,
`shl0 ... ul=165`, `shl8 ... ul=0`, `shlhuge ... ul=0`,
`n3shl ul=6 count=2`, `w64 count=2 t0=0 t5=0 t64=1 t69=1`, and
`empty-shift any 0 all 1 count 0 eq 1`. The same flags compiled and ran
`bitset_ops_true`, `bitset_empty_true`, `bitset_shift_true`,
`bitset_and_true`, `bitset_or_true`, and `bitset_xor_true` against
libstdc++ and against the model; both printed `contracts ok`.

No engine configure, no `prism_tests`, no conformance run.
