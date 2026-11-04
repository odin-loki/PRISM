# regex empty

Branch `finish/cxx-regex-empty`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`std::regex` is still the extensionless file `src/prism/pir/models/cxx/regex`.
The include guard is `_GLIBCXX_REGEX`. `#pragma GCC system_header`. Namespace
`std`. The object still owns a copy of the pattern characters. An empty
pattern stays empty. A null `const char*` is still `__prism_check` with
class `FUNC-CONTRACT` and the message
`std::regex constructed from a null pointer`, then `__builtin_trap`.

This header is not a proof that a string matches. `empty()` and
`mark_count()` are not PROVED and not CLEAN. `empty()` is `size() == 0`.
It does not decide a match. `mark_count()` returns 0. It does not scan the
pattern for a match, and 0 is not a claim that the pattern matches.

## What this slice changes

`empty()` was already declared. `size()` returns `_M_len`, so the previous
body `return _M_len == 0` was that same length. The body is now

```cpp
[[nodiscard]] bool empty() const noexcept { return size() == 0; }
```

It does not name `_M_str`, `_M_len`, or `_M_flags`. It does not call
`_M_own`, `_M_from_cstr`, or `__prism_obj_size`, and it does not free the
pattern. Length 0 is empty. The two characters of `.*` make `empty()`
false. The three characters of `(a)` make `empty()` false. Those results
are the owned length. They are not a match.

`mark_count()` was absent. It is

```cpp
[[nodiscard]] unsigned mark_count() const noexcept { return 0; }
```

The body is that return. It does not read `_M_str` or `_M_len`, and it
does not call `_M_own` or `_M_from_cstr`. `(a)` stays three owned
characters, and `mark_count()` is 0. The parentheses are not scanned.

`flags()` is unchanged: `return _M_flags`. A null `const char*` in
`regex(const char*)` and in `regex(const char*, unsigned)` is still
`_M_fail("FUNC-CONTRACT", "std::regex constructed from a null pointer")`
before the flag is stored. `_M_fail` is `__prism_check` then
`__builtin_trap`.

`regex_match`, `regex_search`, and `regex_replace` stay undeclared. The
header still does not include `<string>`, `<string_view>`, `<memory>`, or
`<locale>`. The only `::operator delete` is still the one in `_M_free`.

## Wiring

`src/prism/bmc.cpp` and `src/prism/pir/stage.cpp` were not edited.
`CMakeLists.txt` was not edited. No new test file was added, so no
`_prism_test_sources` line was added or dropped.
`tests/cpp/test_cxx_regex.cpp` was already listed. `PRISM_PIR_MODELS`
already lists `src/prism/pir/models/cxx/regex`.
`tests/cpp/test_qa_conformance.cpp` was not edited.

## Tests

`tests/cpp/test_cxx_regex.cpp` reads the header. `empty()` returns
`size() == 0` and that body does not name the pattern members.
`mark_count()` returns `0` and that body does not read the pattern.
`flags()` still returns `_M_flags`. The null path in `_M_from_cstr` is
still the `FUNC-CONTRACT` check. After comments are stripped,
`regex_match`, `regex_search`, and `regex_replace` are still not
identifiers. The only `::operator delete` is still in `_M_free`. Compiled
and run alone with clang++ 18.1.3 (`-std=c++23 -Wall -Wextra -Werror`,
doctest main), 143 assertions passed. `prism_tests` was not run.

`tests/cxx_models/regex_trace.cpp` includes `<regex>`. clang++ 18.1.3 with
the system libstdc++ (GCC 12 headers),
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -O1 -Wall -Wextra -Werror`, and `-I` the model
directory. Stdout:

```
empty size=0 empty=1 flags=0 marks=0 data=null
ptr size=2 empty=0 flags=0 marks=0 data=ab
ptr_empty size=0 empty=1 flags=0 marks=0 data=null
arr size=2 empty=0 flags=0 marks=0 data=ab
distinct=1
src_after_move size=0 empty=1 flags=0 marks=0 data=null
moved size=2 empty=0 flags=0 marks=0 data=ab
copy_after size=2 empty=0 flags=0 marks=0 data=ab
assign_distinct=1
sink size=2 empty=0 flags=0 marks=0 data=ab
assign_src size=0 empty=1 flags=0 marks=0 data=null
written size=2 empty=0 flags=0 marks=0 data=.*
def size=0 empty=1 flags=0 marks=0 data=null
flag_moved size=2 empty=0 flags=4 marks=0 data=ab
flag_src size=0 empty=1 flags=0 marks=0 data=null
flag_empty size=0 empty=1 flags=7 marks=0 data=null
flag_empty_copy size=0 empty=1 flags=7 marks=0 data=null
flag_zero size=2 empty=0 flags=0 marks=0 data=ab
star size=2 empty=0 flags=3 marks=0 data=.*
flag_copy size=2 empty=0 flags=4 marks=0 data=ab
flag_sink size=2 empty=0 flags=4 marks=0 data=ab
flag_assign_src size=0 empty=1 flags=0 marks=0 data=null
grouped size=3 empty=0 flags=0 marks=0 data=(a)
flag_distinct=1
```

`written` and `star` are the pattern `.*`. `empty()` is false because the
length is 2. `grouped` is the pattern `(a)`. `empty()` is false because
the length is 3, and `marks=0` on that line is `mark_count()`. `flag_empty`
is an empty pattern with flags 7 and `marks=0`. ASan stayed quiet on that
run (exit 0).

`argv` `null` aborts with
`CHECK FUNC-CONTRACT: std::regex constructed from a null pointer` and
prints no pattern line. `argv` `null-flag` is the same null pointer with
flag 9. It aborts with the same check and prints no `stored flags` line.
`argv` `bare` aborts with
`CHECK MEM-OOB-READ: std::regex C string is not terminated within its object`
and prints no pattern line.

A separate driver whose `__prism_check` returns, then constructs
`regex(nullptr, 9u)`, prints the `FUNC-CONTRACT` check and dies with
SIGILL from `__builtin_trap`. It prints no `stored flags` line.

`std::regex_match` fails to compile against this header
(`no member named 'regex_match' in namespace 'std'`).

cmake, ninja, `prism_tests`, and pir were not run.
