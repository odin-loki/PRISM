# string_view find

Branch `finish/cxx-string-view-find`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`basic_string_view::find(basic_string_view, pos)` and
`basic_string_view::rfind(basic_string_view, pos)` are members of the
existing header `src/prism/pir/models/cxx/string_view`. That path is a
file. A directory of the same name would make `#include <string_view>`
fail with EISDIR. The include guard is still `_GLIBCXX_STRING_VIEW`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. A returned index is the position the header computed. `npos`
is the miss result. It is not `CLEAN`: a fuzzer `CLEAN` is not a proof,
and `npos` is not a proof that the needle is absent. The comment on each
function says this is not PROVED and not CLEAN.

## What find and rfind do

Two overloads, each `constexpr` and `noexcept`:

- `size_type find(basic_string_view, size_type pos = 0) const`
- `size_type rfind(basic_string_view, size_type pos = npos) const`

`find` returns the first index at or after `pos` where the needle
matches, or `npos`. `rfind` returns the last index at or before `pos`
where the needle matches, or `npos`. The default `pos` for `rfind` is
`npos`, so that call is the last match in the view.

An empty needle does not read either pointer. `find` of an empty needle
returns `pos` when `pos <= size()`, and `npos` when `pos > size()`.
`rfind` of an empty needle returns `min(pos, size())`, including when
`pos` is `npos`, which is `size()`.

A non-empty needle in an empty view returns `npos` and does not read the
null pointer. `pos > size()` for `find` returns `npos` without reading.
A needle longer than the view returns `npos` without reading. A non-empty
needle that does match is compared with `char_traits::eq`, the same
equality `starts_with` uses. That is character equality. It is not a
locale search, and it is not the unsigned ordering `compare` uses. A
stored byte `0x80` matches another `0x80`.

The start index of a non-empty needle is at most `size() - needle.size()`.
That difference is formed only after the needle is known to be non-empty
and no longer than the view, so it does not wrap. The window then walks
one character at a time. The cursor advances only when another needle
character remains, and the function returns on the last matched character,
so no index past `size()` is formed and `size()` is not used as a
subscript. Nothing is allocated. A miss does not call `__prism_check`.

The `CharT` and `const CharT*` overloads are not declared.

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

`compare(basic_string_view)` is unchanged. Two empty views still return 0
before either pointer is read. Each compared byte is still an
`unsigned char`. The body still returns `-1` or `1`. It is still not a
locale collation.

`substr` is unchanged. A position past `size()` is still `MEM-OOB-READ`
and then `__builtin_trap`. The length is still `__count < __room` with
`__room = _M_len - __pos`. An empty view still has both returns:

```
if (_M_str == nullptr)
    return basic_string_view(nullptr, __n);
return basic_string_view(_M_str + __pos, __n);
```

The header still does not include `<string>` or `<memory>`. `std::__sv_check`
and `std::__sv_limit` are unchanged. `npos` is unchanged.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `find(CharT)`, `find(const CharT*)`, and the same for `rfind`
- `find_first_of`, `find_last_of`, `find_first_not_of`, `find_last_not_of`
- `contains`, `at`, iterators, `swap`, `copy`, hash
- every other `compare` overload

`find(basic_string_view, pos)` is the only `find(`. `rfind(` is the one
`rfind(`, and its spelling also contains the substring `find(`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `src/prism/bmc.cpp` was not
edited. `CMakeLists.txt` was not edited: `_prism_test_sources` already
lists `tests/cpp/test_cxx_string_view.cpp`. The conformance header array
in `tests/cpp/test_qa_conformance.cpp` is unchanged. `string_view` stays
in that list.

## Tests

`tests/cpp/test_cxx_string_view.cpp` reads the header. The pointer-plus-count
constructor line is still the noexcept store. The four prefix bodies are
unchanged. `substr(` is still one definition. Its body still checks
`MEM-OOB-READ` before `__builtin_trap`, and the trap is before the only
`return`. The empty-view returns are still both lines above. `compare(`
is still one definition. `find(` occurs twice in the code because `rfind(`
contains that substring, and `rfind(` occurs once. Each comment says this
is not PROVED and not CLEAN.

An empty needle returns before any `_M_str` load. `find` returns `npos`
for `pos > size()` before that load, and returns `pos` only after that
check. A non-empty needle in an empty view, a needle longer than the view,
and `pos` past the last start all return `npos` before a load. The
comparison is `_Traits::eq`. The needle index advances before the test
that the needle is exhausted, and the haystack cursor advances only after
that test fails, so the last character does not step to `size()`. Neither
body adds `__pos`, `__i`, or `__k`, neither subscripts `_M_str[_M_len`,
and neither contains `static_cast<unsigned char>`, `__prism_check`,
`new `, `delete`, or `locale`.

`string_view_trace.cpp` calls `find`, `rfind`, `compare`, `starts_with`,
`ends_with`, and `substr`. Constant checks cover a hit, a later hit, a
miss, an empty needle at `size()` and past it, `rfind` of an empty needle,
an empty view, a high byte, a repeated pattern, and a longer needle. The
empty needle used in those checks points one past the last character. The
empty view's pointer is null. Reading either is not a constant expression.

The same driver still compiles that trace twice, against libstdc++ and with
`-isystem` on the model directory, under `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra
-Werror`. Checked with clang++ 18.1.3 and libstdc++. The traces match,
including the previous prefix, substr, and compare lines. Find lines:

```
find_hit=0 2 2 npos
rfind_hit=2 2 0 0
find_ab=0 2 npos
rfind_ab=2 0
find_ba=1 3
find_aaa=0 1
find_pref=1 1
find_tail=2 npos 2 npos
find_miss=npos npos
find_hi=1 npos 1
find_empty=0 5 npos 0 3 5 5
find_past=npos npos npos npos
find_null=npos 0 npos npos 0 0
find_long=npos npos
find_endgap=0 2 npos 1 2
find_self=0 0
find_alive=ababa
```

`find_hit` is `"ababa"` searched for `"aba"`: index 0, then 2 from `pos`
1 and from `pos` 2, and `npos` from `pos` 3. `rfind_hit` is the last
`"aba"` at 2, the same at `pos` 2, and index 0 at `pos` 1 and at `pos` 0.
`find_aaa` is `"aaa"` searched for `"aa"`: the first start is 0 and the
last start is 1. `find_pref` is `"aab"` searched for `"ab"`, which is 1
and not the partial match at 0. `find_tail` is `"abcd"` searched for
`"cd"`: index 2, `npos` at `pos` 3, and `rfind` at `pos` 1 is `npos`.
`find_hi` is the byte `0x80` inside `"a\x80b"`. `find_empty` is an empty
needle: `find` at 0 and at `size()` (5), `npos` past `size()`, and
`rfind` at 0, 3, 9, and the default `npos`, which is `min` with `size()`.
`find_past` is a non-empty needle at `pos == size()` and `pos > size()`,
including a one-character object. `find_null` is a default view: a
non-empty needle is `npos`, and an empty needle is `min(pos, 0)`.
`find_long` is a two-character object searched for three characters.
`find_endgap` is an empty needle whose pointer is one past a two-character
object. `find_self` is the view searched for itself. `find_alive` is the
caller's buffer after those views are destroyed. ASan stayed quiet. Both
binaries returned 0.

`substr(5, 1)` on a 4-character view aborts both binaries. The model
stderr is `check MEM-OOB-READ: std::string_view::substr position past size()`.
`front()` on an empty view aborts both. The model stderr is
`check FUNC-CONTRACT: std::string_view::front() on an empty view`.

Without a usable clang++ the differential case in the doctest is `NOTRUN`.
The header read runs either way.

`testdata/conformance/libc-models/string_view_contracts.cpp` and its yml
were not edited. The harness still does not call `find`.
