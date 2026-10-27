# string_view slice

Branch `finish/cxx-string-view`. `std::string_view` is a header file,
`src/prism/pir/models/cxx/string_view`, the same shape as `vector`: include
guard `_GLIBCXX_STRING_VIEW`, `#pragma GCC system_header`, namespace `std`.
A directory of that name would make `#include <string_view>` fail and every
such unit would fall back to libstdc++.

The solver was not run. The yml files record the contract each harness asks
for. `expected` values are booleans.

## What the model covers

A pointer and a length. Nothing is allocated. The destructor does not free,
and copying the view copies the pointer.

- `basic_string_view()` is empty (`size() == 0`, `data() == nullptr`).
- `basic_string_view(const CharT*)` reads at most the bytes
  `__prism_obj_size` reports for that pointer, one character at a time, and
  stops at the terminator. A null pointer is
  `__prism_check(..., "FUNC-CONTRACT", ...)`, not an empty view and not a
  view of invented characters. A missing terminator inside the object is
  `MEM-OOB-READ`, not a made-up length.
- `basic_string_view(const CharT*, count)` stores the pointer and the count.
  It does not allocate and it does not read.
- `size()`, `empty()`, `data()`.
- `operator[]` in range returns the character at that index. Out of range is
  `__prism_check(..., "MEM-OOB-READ", ...)`, not `'\0'` and not a substitute
  character.
- `front()` / `back()` on an empty view are `FUNC-CONTRACT`.
- `remove_prefix` / `remove_suffix` with a count above `size()` are
  `FUNC-CONTRACT`. The pointer and length stay where they were, so the view
  does not slide onto characters outside the original range.

`using string_view = basic_string_view<char>` is
`basic_string_view<char, char_traits<char>>`. That is the type libstdc++'s
`<string>` converts to. The second parameter is `char_traits<_CharT>`
(`#include <bits/char_traits.h>`, not `<string>`). `std::__sv_check` and
`std::__sv_limit` stay so those string methods still instantiate. They are
not string_view operations.

## What falls through to libstdc++

Not declared, so a use fails to compile against this header and the pir
stage lowers the whole translation unit again with libstdc++
(`extra.cxx_models`):

- `compare`, `find`, `substr`, `starts_with`
- every other member that is not listed above (`at`, iterators, `swap`,
  `copy`, hash, the inserter, `wstring_view`, the range constructors)

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/string_view`

and `_prism_test_sources` gained `tests/cpp/test_cxx_string_view.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"string_view"` with the other
public model headers. The iostream and deque skips are unchanged.

## Tests

`tests/cxx_models/string_trace.cpp` gained the same host stubs. It constructs
`std::string` from `std::string_view("view")`, and with the model directory
on the include path that constructor calls the intrinsics. libstdc++ does
not, so the stubs are silent on the success path and the two string traces
still match. That trace no longer calls `string::starts_with`,
`ends_with`, or `contains`: libstdc++ implements those as
`string_view` members, which this header omits, so the call would fail to
compile and the pir stage would fall back.

`tests/cxx_models/string_view_trace.cpp` includes `<string_view>`.
`tests/cpp/test_cxx_string_view.cpp` compiles it twice (libstdc++ and
`-isystem` the model directory) with `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined`. The traces match: `operator[]` prints the
real character, `data()` is the caller's buffer, and that buffer is still
those characters after the view is destroyed. `front()` on an empty view
exits non-zero; the model binary's check is `FUNC-CONTRACT`. Without a
libstdc++ clang++ that can run ASan/UBSan the doctest is `NOTRUN`.

`testdata/conformance/prism/cxx/cxx_string_view_true.cpp` indexes in range
(`memsafety` true). `cxx_string_view_false.cpp` indexes one past the end
(`false`, witness `[3]`). `testdata/conformance/libc-models/string_view_contracts.cpp`
is the libc-contract harness the "every C++ library model header is embedded
and exercised" case requires: in-range index true; out-of-range index
`MEM-OOB-READ`; empty `front()` and a null `const char*` `FUNC-CONTRACT`.
