# string_view compare

Branch `finish/cxx-string-view-compare`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.

`basic_string_view::compare(basic_string_view)` is a member of the existing
header `src/prism/pir/models/cxx/string_view`. That path is a file. A
directory of the same name would make `#include <string_view>` fail with
EISDIR. The include guard is still `_GLIBCXX_STRING_VIEW`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. A returned `-1`, `0`, or `1` is the `int` the header returns.
It is not a proof that a caller stayed in range. It is not a proof about
locales. The order is the unsigned char value of each stored character.
No collation table is consulted.

## What compare does

One overload, `constexpr` and `noexcept`:

`int compare(basic_string_view) const`

Two empty views return `0` before either pointer is read. That includes
two default views, whose pointers are null, and two zero-length views
whose pointers address different characters. A read of those characters
would order them. The length check returns first, so they compare equal.

Otherwise the compared length is `min(size(), other.size())`. A length of
0 does not enter the load. An empty view compared with a non-empty view
does not load the empty pointer, and it does not load the other view.
Each compared character is an `unsigned char`. The first difference
returns `-1` when this view's byte is less, and `1` when it is greater.
A stored byte `0x80` is greater than `'a'`.

When the shared prefix matches, the lengths decide. The shorter view is
less, including when one view is a proper prefix of the other. The return
is `-1` or `1`. It is not the difference of the two lengths. Equal views
return `0`.

The loop index stays below the minimum length, so neither view is read
past `size()`. The function does not allocate, and it does not call
`__prism_check`.

## What stayed

The constructors are unchanged. `basic_string_view(const CharT*, count)` is
still `constexpr` and `noexcept`. It stores the pointer and the count. It
does not check null and it does not read the characters. The default
constructor is still an empty view (`size() == 0`, `data() == nullptr`).
The destructor is still defaulted.

`operator[]`, `front`, `back`, `remove_prefix`, and `remove_suffix` are
unchanged.

`starts_with` and `ends_with` are unchanged. An empty needle is still true.
The characters compared are still the overlap, at most `size()`, and a
compared length of 0 does not read the pointer.

`substr` is unchanged. A position past `size()` is still `MEM-OOB-READ`
and then `__builtin_trap`. The length is still `__count < __room` with
`__room = _M_len - __pos`. An empty view still has both returns:

```
if (_M_str == nullptr) return basic_string_view(nullptr, __n);
return basic_string_view(_M_str + __pos, __n);
```

The header still does not include `<string>` or `<memory>`. `std::__sv_check`
and `std::__sv_limit` are unchanged. `npos` is unchanged.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `find`
- every other `compare` overload (`const CharT*`, a position and a count)
- `contains`, `at`, iterators, `swap`, `copy`, hash

`compare(basic_string_view)` is the only `compare(` in the header.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited:
`_prism_test_sources` already lists `tests/cpp/test_cxx_string_view.cpp`.
The conformance header array in `tests/cpp/test_qa_conformance.cpp` is
unchanged. `string_view` stays in that list.

## Tests

`tests/cpp/test_cxx_string_view.cpp` reads the header. The pointer-plus-count
constructor line is still the noexcept store. The four prefix bodies are
unchanged. `substr(` is still one definition. Its body still checks
`MEM-OOB-READ` before `__builtin_trap`, and the trap is before the only
`return`. The empty-view returns are still both lines above. `compare(` is
one definition. Two lengths of 0 return `0` before any `_M_str` load. The
load sits inside `if (__n != 0)`, and `__n` is the minimum length. Each
byte is `static_cast<unsigned char>`. The body returns `-1` and `1`, and
it has no `__prism_check`, `new `, or `delete`. `find(` is absent.
`string_view_trace.cpp` calls `compare`, `starts_with`, `ends_with`, and
`substr`, and still does not call `find`.

The same driver still compiles that trace twice, against libstdc++ and with
`-isystem` on the model directory, under `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra
-Werror`. Checked with clang++ 18.1.3 and libstdc++ 14. The traces match,
including the previous prefix and substr lines. Compare lines:

```
cmp_empty=0 cmp_empty_a=-1 cmp_a_empty=1 cmp_aa=0 cmp_ab=-1 cmp_ba=1
cmp_prefix=-1 cmp_longer=1 cmp_mid=-1 cmp_pref3=-1 cmp_high=1 cmp_zz=0 cmp_end=0
```

`cmp_empty` is two default views. `cmp_empty_a` and `cmp_a_empty` are an
empty view against a one-character view. `cmp_ab` is `'a'` against `'b'`.
`cmp_prefix` is `'a'` against `'ab'`. `cmp_mid` is `'ab'` against `'ac'`.
`cmp_pref3` is `'a'` against `'abc'`: the model returns `-1`, and
libstdc++'s length delta is `-2`, so the trace prints the sign. `cmp_high`
is the byte `0x80` against `'a'`. `cmp_zz` is `substr(0, 0)` of `'a'`
against `substr(0, 0)` of `'b'`. `cmp_end` is two zero-length views one
past a two-character object. ASan stayed quiet. Both binaries returned 0.

`substr(5, 1)` on a 4-character view aborts both binaries. The model
stderr is `check MEM-OOB-READ: std::string_view::substr position past size()`.
libstdc++ throws `std::out_of_range` and `std::terminate` aborts.
`front()` on an empty view aborts both. The model stderr is
`check FUNC-CONTRACT: std::string_view::front() on an empty view`.

A separate constexpr program static-asserts the model: two empty views are
`0`, two zero-length views of `'a'` and `'b'` are `0`, `'a'` against `'b'`
is `-1`, `'a'` against `'abc'` is `-1`, and the byte `0x80` against `'a'`
is `1`. The same program against libstdc++ rejects the length-delta
asserts, because `compare` of those sizes evaluates to `-2` and `2`. The
header read runs either way. Without a usable clang++ the differential
case in the doctest is `NOTRUN`.

`testdata/conformance/libc-models/string_view_contracts.cpp` and its yml
were not edited. The harness still does not call `compare`.
