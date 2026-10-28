# string_view prefix

Branch `finish/cxx-string-view-prefix`, base `b9b25b0c57a337947a22e655e0af197b6f619b6b`.

`starts_with` and `ends_with` are members of the existing header
`src/prism/pir/models/cxx/string_view`. That path is a file. A directory of
the same name would make `#include <string_view>` fail with EISDIR. The
include guard is still `_GLIBCXX_STRING_VIEW`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. A `false` result from these members is the boolean the header
returns. It is not a precondition failure and it is not a proof that a
string has a given prefix.

## What the new members do

Four overloads, each `constexpr` and `noexcept`:

- `starts_with(basic_string_view)`
- `starts_with(CharT)`
- `ends_with(basic_string_view)`
- `ends_with(CharT)`

An empty needle matches, including an empty view. A needle longer than the
view is false. The characters compared are the overlapping prefix, or the
overlapping suffix for `ends_with`. That length is `min(size(), needle.size())`,
so it is at most `size()` and the view is not read past `size()`.

The character pointer is `_M_str`. A compared length of 0 does not read it:
the load sits inside `if (__n != 0)` for a view needle, and a `CharT` needle
returns false on an empty view before it touches `_M_str`. A one-character
needle reads `_M_str[0]` or `_M_str[_M_len - 1]` only when `size()` is at least 1.

A mismatch is false. It does not call `__prism_check`. The view does not
allocate, and these members do not free.

## What stayed

The constructors are unchanged. `basic_string_view(const CharT*, count)` is
still `constexpr` and `noexcept`. It stores the pointer and the count. It
does not check null and it does not read the characters. The default
constructor is still an empty view (`size() == 0`, `data() == nullptr`).
The destructor is still defaulted.

`operator[]`, `front`, `back`, `remove_prefix`, and `remove_suffix` are
unchanged.

The header still does not include `<string>` or `<memory>`.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `compare`, `find`, `substr`
- `contains`, `at`, iterators, `swap`, `copy`, hash

`tests/cxx_models/string_trace.cpp` was left unchanged. It still does not
call `string::starts_with`, `ends_with`, or `contains`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited:
`_prism_test_sources` already lists `tests/cpp/test_cxx_string_view.cpp`.
The conformance header array in `tests/cpp/test_qa_conformance.cpp` is
unchanged. `string_view` stays in that list.

## Tests

`tests/cpp/test_cxx_string_view.cpp` reads the header. The pointer-plus-count
constructor line is still the noexcept store. The four bodies compare at
most the overlap, and `_M_str` is only reached after a non-zero length
check. `compare(`, `find(`, and `substr(` are absent from the code.
`string_view_trace.cpp` calls `starts_with` and `ends_with` and still does
not call `compare`, `find`, or `substr`.

The same driver still compiles that trace twice, against libstdc++ and with
`-isystem` on the model directory, under `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined`. The traces match. An empty needle prints 1.
A longer needle prints 0, including a needle whose overlap matches (`ab`
against `abc`, and `ab` against `xab`) and an empty view against a
non-empty needle. A one-character needle prints the first or last character.
Without a usable clang++ the differential case is `NOTRUN`. The header
read runs either way.

`testdata/conformance/libc-models/string_view_contracts.cpp` no longer lists
`starts_with` as omitted. Its harness functions are unchanged: a false
prefix is not a contract violation. The yml is unchanged.
