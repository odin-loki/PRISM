# bitset operator[] index trap

Branch `finish/cxx-bitset-index-trap`, base `54bc116e1ace138cb8c9461ba314916a6439e353`.
Both `operator[]` overloads are on the existing header
`src/prism/pir/models/cxx/bitset`. The header is still a file, not a
directory.

The solver was not run. `prism_tests` was not run. cmake and ninja were not
run. This header is not a proof. Nothing here is `PROVED`. Nothing here is
`CLEAN`.

## What operator[] does

An index that is one of the N bits returns that bit. The const overload
loads the word only in that arm. The reference overload returns
`reference(*this, __position)` and does not load a word; a later conversion
of that reference reads the bit. Neither arm runs when the index is out of
range.

An index `>= N` takes this arm in both overloads:

```
if (__position >= _Nb) {
    __prism_check(0, "MEM-OOB-READ", "std::bitset::operator[] index is outside the bitset");
    __builtin_trap();
}
```

The check is class `MEM-OOB-READ` with message
`std::bitset::operator[] index is outside the bitset`. `__builtin_trap` is
the next statement. The arm has no `return`. It does not return a
fabricated bit, and it does not return a reference. `_M_words` is not named
in that arm, so no word is read.

`bitset<0>::operator[](0)` is that same arm: every `size_t` index is
`>= 0`. The other arm of an empty bitset is also `__builtin_trap`. The word
load is the discarded `else` of `if constexpr (_Nb == 0)`, so `bitset<0>`
does not instantiate a word read. There is no stored bit to return.

An in-range index does not call the check. `static_assert` on `[0]` and
`[7]` of `bitset<8>(0xA5)` is a constant expression equal to those bits.
`bitset<3>(0xFF)[0]`, `[1]`, and `[2]` are the three stored bits.
`to_ulong()` of that value stays 7.

When the host `__prism_check` returns on a failing condition, the process
dies with SIGILL from `__builtin_trap` and does not print a loaded bit.

## What stayed

`set`, `reset`, `flip`, `test`, `count`, `to_ulong`, `to_ullong`, `all`,
`any`, `none`, `operator&=`, `operator|=`, `operator^=`, `operator~`, and
`operator==` are unchanged. `test(pos)` for `pos >= N` still throws
`std::out_of_range` and does not yield a bit. `bitset<0>::test(0)` still
throws. The shifts, AND, OR, XOR, and complement are unchanged. A padding
bit of the last word is still not a bit of those results. `bitset<0>` still
stores no word. There is no heap allocation. `bit_cast` is not in the
header.

`_S_no_bit` is still the backstop for the unchecked helpers. `operator[]`
does not call it.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the pir
stage lowers the translation unit again with libstdc++ (`extra.cxx_models`).
They are not stubbed:

- string constructors and `to_string` (those need `<string>`)
- `std::hash<std::bitset<N>>`

## Wiring

`CMakeLists.txt` gained one line, `tests/cpp/test_cxx_bitset.cpp`, in
`_prism_test_sources`. No line was removed. `src/prism/pir/models/cxx/bitset`
was already in `PRISM_PIR_MODELS`. `tests/cpp/test_qa_conformance.cpp` was
not edited. `bitset` stays in the header-name array. `src/prism/bmc.cpp`,
`src/prism/bmc_unenc.inc`, and `src/prism/pir/stage.cpp` were not edited.
`testdata/conformance/libc-models/bitset_contracts.cpp` and its yml were not
edited. `bitset_index_oob_false` is still `operator[]` at `size()`.
`bitset_test_oob_false` is still `test(4)` leaving a `noexcept` function.

## Tests

`tests/cpp/test_cxx_bitset.cpp` reads the header. Both `operator[]`
overloads have `if (__position >= _Nb)`, then the `MEM-OOB-READ` check with
`std::bitset::operator[] index is outside the bitset`, then
`__builtin_trap();`. The text of that arm does not name `_M_words` and does
not `return`. The const overload's word load is after that trap, in the
`else`. The reference overload does not name `_M_words`. Neither body
contains `__glibcxx_assert` or `return false`. `test` still calls
`_M_check` for `bitset::test` and does not trap. `set`, `reset`, `flip`,
`count`, `to_ulong`, `to_ullong`, `all`, `any`, `none`, `operator&=`,
`operator|=`, `operator^=`, `operator~`, and `operator==` do not call
`__prism_check`. The sentence `This is not PROVED and not CLEAN.` is in the
header. The message string occurs twice.

`tests/cxx_models/bitset_trace.cpp` still does not call `operator[]` at an
index `>= N`. It gained constant checks for the eight bits of
`bitset<8>(0xA5)` and for an in-range reference assignment. A host
`__prism_check` is defined so the model links. In-range `operator[]` does
not call it.

Checked with Ubuntu clang++ 18.1.3 against libstdc++, `-std=c++23
-D_GLIBCXX_ASSERTIONS -Wall -Wextra -Werror -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. The trace was compiled twice (`-isystem`
a directory that contains only this header, and without it). The models
directory was not that path. The dependency dump names this header. Stdout
matched, including the previous shift, AND, OR, XOR, complement, and
`out_of_range` lines. The same flags compiled a small driver with `-isystem`
on this header. In-range indexes printed

```
b0=1 b1=0 b2=1 b7=1
w0=1 w1=0 w2=1 count=2
n0=1 n1=1 n2=1 ul=7
test0=1
```

and exited 0, with no check on stderr. `operator[](0)` on `bitset<0>`,
const and not const, aborted in the check (`MEM-OOB-READ`,
`std::bitset::operator[] index is outside the bitset`) and printed no
`loaded=` line. The same message aborted `operator[](4)` on `bitset<4>`
(const and not const), `operator[](3)` on `bitset<3>`, and `operator[](100)`
on `bitset<4>`. A driver whose `__prism_check` returns on failure died with
SIGILL from `__builtin_trap` on `bitset<0>::operator[](0)` and printed no
`loaded=` line. Not an ASan heap error. The binaries were written on ext4.

g++ 13.3.0 compiled the same driver with `-std=c++23 -D_GLIBCXX_ASSERTIONS
-Wall -Wextra -Werror` and printed the same in-range lines, the same check
line on the empty subscript and on `operator[](4)` of a const `bitset<4>`,
and SIGILL when the check returns. The trace under those flags matched
libstdc++ except the existing `to_ullong` overflow text (`_M_do_to_ulong`
from this header, `_M_do_to_ullong` from that libstdc++). `to_ullong` was
not edited.
