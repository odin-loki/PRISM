# regex swap

Branch `finish/cxx-regex-swap`, base `b703c118fcf4cbb6757f1b7b40a8b740d56dba7b`.

`std::regex` is still the extensionless file `src/prism/pir/models/cxx/regex`.
The include guard is `_GLIBCXX_REGEX`. `#pragma GCC system_header`. Namespace
`std`. The object still owns a copy of the pattern characters. An empty
pattern stays empty. A null `const char*` is still `__prism_check` with
class `FUNC-CONTRACT` and the message
`std::regex constructed from a null pointer`, then `__builtin_trap`.

This header is not a proof that a string matches. `basic_regex::swap` is
not PROVED and not CLEAN. It exchanges the pattern pointer, the length,
and the flags. It does not decide a match.

## What this slice adds

`basic_regex` is this `regex`. It is not a template.

```cpp
using basic_regex = regex;

inline void basic_regex::swap(basic_regex& __o) noexcept {
    if (this == &__o) return;
    _M_swap(__o);
}
```

`_M_swap` exchanges `_M_str`, `_M_len`, and `_M_flags`. It does not
dereference the pointer, so two empty patterns stay empty and the swap
does not read a character. Self-swap returns before that exchange, so the
one block stays and the destructor still frees it once later. Swap does
not call `_M_free` and does not call `::operator delete`.

`std::swap` on two `basic_regex` objects is that member:

```cpp
inline void swap(basic_regex& __a, basic_regex& __b) noexcept { __a.swap(__b); }
```

The generic `std::swap` move-assigns. Move-assign of a different object
calls `_M_free`. This overload does not.

## What stayed

`empty()` is still `return size() == 0`. `size()` is still `return _M_len`.
`mark_count()` is still `return 0`. `flags()` is still `return _M_flags`.
The constructors are unchanged. The default constructor,
`regex(const char*)`, and `regex(const char (&)[N])` still store
`_M_flags(0)`. `regex(const char*, unsigned)` still rejects a null pointer
before `_M_flags = __f`:

`if (__s == nullptr) _M_fail("FUNC-CONTRACT", "std::regex constructed from a null pointer");`

`_M_from_cstr` still does that same check before it reads, and a missing
terminator is still the `MEM-OOB-READ` check. A terminated empty string
stays empty. `_M_fail` is still `__prism_check` then `__builtin_trap`.

The only `::operator delete` is still the one in `_M_free`. The destructor
is still `_M_free()`.

`regex_match`, `regex_search`, and `regex_replace` stay undeclared.
`wregex`, `regex_traits`, `match_results`, and the iterators stay omitted.
`std::basic_regex<char>` is ill-formed against this header
(`expected unqualified-id` at `<`). The header still does not include
`<string>`, `<string_view>`, `<memory>`, or `<locale>`.

## Wiring

`src/prism/bmc.cpp` and `src/prism/pir/stage.cpp` were not edited.
`CMakeLists.txt` was not edited. No new test file was added, so no
`_prism_test_sources` line was added or dropped.
`tests/cpp/test_cxx_regex.cpp` was already listed. `PRISM_PIR_MODELS`
already lists `src/prism/pir/models/cxx/regex`.
`tests/cpp/test_qa_conformance.cpp` was not edited.

## Tests

`tests/cpp/test_cxx_regex.cpp` reads the header. `basic_regex::swap`
returns on `this == &__o` before `_M_swap`. That body does not delete, does
not call `_M_free` or `_M_own`, and does not index the pattern. `_M_swap`
exchanges the flag and does not delete. `empty()`, `mark_count()`, and
`flags()` are unchanged. The null path in `_M_from_cstr` is still the
`FUNC-CONTRACT` check, and the flagged constructor still stores the flag
only after that check. After comments are stripped, `regex_match`,
`regex_search`, and `regex_replace` are still not identifiers. The only
`::operator delete` is still in `_M_free`. Compiled and run alone with
clang++ 18.1.3 (`-std=c++23 -Wall -Wextra -Werror`, doctest main), 168
assertions passed. `prism_tests` was not run.

`tests/cxx_models/regex_trace.cpp` includes `<regex>`. clang++ 18.1.3 with
the system libstdc++ (GCC 13.3.0 headers),
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
swap_a size=2 empty=0 flags=3 marks=0 data=.*
swap_b size=2 empty=0 flags=4 marks=0 data=ab
swap_exchanged=1
swap_empty_a size=0 empty=1 flags=7 marks=0 data=null
swap_empty_b size=0 empty=1 flags=0 marks=0 data=null
swap_both_empty size=0 empty=1 flags=0 marks=0 data=null
swap_both_empty_b size=0 empty=1 flags=0 marks=0 data=null
swap_self size=2 empty=0 flags=4 marks=0 data=ab
self_same=1
swap_now_empty size=0 empty=1 flags=0 marks=0 data=null
swap_now_full size=2 empty=0 flags=4 marks=0 data=ab
std_swap_a size=2 empty=0 flags=2 marks=0 data=cd
std_swap_b size=2 empty=0 flags=1 marks=0 data=ab
std_exchanged=1
std_self size=2 empty=0 flags=5 marks=0 data=zw
std_self_same=1
```

`swap_a` and `swap_b` exchanged the blocks of `ab` (flags 4) and `.*`
(flags 3). `swap_exchanged=1` is those same pointers, not a copy.
`swap_empty_a` and `swap_empty_b` are two empty patterns whose flags
exchanged (7 and 0). `swap_both_empty` is two default patterns, still
empty, `data=null`. `self_same=1` is a self-swap whose `data()` pointer
did not change. `swap_now_full` is the block a non-empty pattern handed
to an empty one. `std_self_same=1` is `std::swap` of one object with
itself. ASan stayed quiet on that run (exit 0).

A separate driver whose `operator new` and `operator delete` count calls,
run after the patterns were already constructed, reported
`delta new=0 del=0` for member swap, swap of two empty patterns, self-swap,
`std::swap` of an object with itself, and swap with an empty pattern.

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
