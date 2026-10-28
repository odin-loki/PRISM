# regex flags

Branch `finish/cxx-regex-flags`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`std::regex` is still the extensionless file `src/prism/pir/models/cxx/regex`.
The include guard is `_GLIBCXX_REGEX`. `#pragma GCC system_header`. Namespace
`std`. The object still owns a copy of the pattern characters. An empty
pattern stays empty. A null `const char*` is still `__prism_check` with
class `FUNC-CONTRACT` and the message
`std::regex constructed from a null pointer`, then `__builtin_trap`.

This header is not a proof that a string matches. The stored flag is not
PROVED and not CLEAN. `flags()` returns the unsigned stored on the object.
It does not read the pattern to decide a match, and 0 is not a claim that
the pattern matches.

## What this slice adds

An `unsigned` member `_M_flags`, and `flags() const` which returns that
member. The body is `return _M_flags`. It does not read `_M_str` or
`_M_len`, and it does not call `_M_own` or `_M_from_cstr`.

The default constructor, `regex(const char*)`, and
`regex(const char (&)[N])` store 0 in the member initializer. Copy
constructs with `_M_flags(__o._M_flags)`. Move constructs with the same
copy, then stores 0 in the source. Move-assign copies the flag after it
takes the pointer, then stores 0 in the source. `_M_swap` exchanges the
flag, so copy-assign keeps it. `_M_free` and `_M_own` do not write
`_M_flags`. The only `::operator delete` is still the one in `_M_free`.

`regex(const char*, unsigned)` uses the same null check as
`regex(const char*)`:

`if (__s == nullptr) _M_fail("FUNC-CONTRACT", "std::regex constructed from a null pointer");`

That check is before `_M_flags = __f`. The mem-initializer does not name
`_M_flags`, so the caller's flag is stored only after that check. A null
pointer is not stored. `_M_from_cstr` runs after the store and still owns
the pattern. A terminated empty string stays empty: length 0, no
allocation, and the flag is whatever unsigned was passed. A missing
terminator is still the `MEM-OOB-READ` check inside `_M_from_cstr`, before
`_M_own`.

`regex_match`, `regex_search`, and `regex_replace` stay undeclared. The
header still does not include `<string>`, `<string_view>`, `<memory>`, or
`<locale>`.

## Wiring

`src/prism/bmc.cpp` and `src/prism/pir/stage.cpp` were not edited.
`CMakeLists.txt` was not edited. `_prism_test_sources` already lists
`tests/cpp/test_cxx_regex.cpp`. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/regex`. `tests/cpp/test_qa_conformance.cpp` was
not edited.

## Tests

`tests/cpp/test_cxx_regex.cpp` reads the header. The new constructor's null
check is before `_M_flags = __f`, and that store is before `_M_from_cstr`.
`flags()` returns `_M_flags`. The three existing constructors store
`_M_flags(0)`. Copy and move copy `_M_flags`. The move and the move-assign
store 0 in the source after the copy. `_M_free` is still the only
`::operator delete`. After comments are stripped, `regex_match`,
`regex_search`, and `regex_replace` are still not identifiers. Compiled
alone with clang++ 18.1.3 (`-std=c++23`, doctest main), 119 assertions
passed. `prism_tests` was not run.

`tests/cxx_models/regex_trace.cpp` includes `<regex>`. clang++ 18.1.3 with
libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -O1 -Wall -Wextra -Werror`, and `-I` the model
directory. Stdout:

```
empty size=0 empty=1 flags=0 data=null
ptr size=2 empty=0 flags=0 data=ab
ptr_empty size=0 empty=1 flags=0 data=null
arr size=2 empty=0 flags=0 data=ab
distinct=1
src_after_move size=0 empty=1 flags=0 data=null
moved size=2 empty=0 flags=0 data=ab
copy_after size=2 empty=0 flags=0 data=ab
assign_distinct=1
sink size=2 empty=0 flags=0 data=ab
assign_src size=0 empty=1 flags=0 data=null
written size=2 empty=0 flags=0 data=.*
def size=0 empty=1 flags=0 data=null
flag_moved size=2 empty=0 flags=4 data=ab
flag_src size=0 empty=1 flags=0 data=null
flag_empty size=0 empty=1 flags=7 data=null
flag_empty_copy size=0 empty=1 flags=7 data=null
flag_zero size=2 empty=0 flags=0 data=ab
star size=2 empty=0 flags=3 data=.*
flag_copy size=2 empty=0 flags=4 data=ab
flag_sink size=2 empty=0 flags=4 data=ab
flag_assign_src size=0 empty=1 flags=0 data=null
flag_distinct=1
```

`written` is the pattern `.*` with flags 0. `star` is the same two
characters with flags 3. `flag_zero` is `ab` with flags 0. `flag_empty` is
an empty pattern with flags 7, and its copy is empty with the same flag.
ASan stayed quiet on that run (exit 0).

`argv` `null` aborts with
`CHECK FUNC-CONTRACT: std::regex constructed from a null pointer` and
prints no pattern line. `argv` `null-flag` is the same null pointer with
flag 9. It aborts with the same check and prints no `stored flags` line.
`argv` `bare` aborts with
`CHECK MEM-OOB-READ: std::regex C string is not terminated within its object`
and prints no pattern line.

A separate driver whose `__prism_check` returns, then constructs
`regex(nullptr, 9u)`, dies with SIGILL from `__builtin_trap` and prints no
`stored flags` line.

`std::regex_match` fails to compile against this header
(`no member named 'regex_match' in namespace 'std'`).

cmake, ninja, `prism_tests`, and pir were not run.
