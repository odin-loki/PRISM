# regex equality

Branch `finish/cxx-regex-equal`, base `54bc116e1ace138cb8c9461ba314916a6439e353`.

`std::regex` is still the extensionless file `src/prism/pir/models/cxx/regex`.
The include guard is `_GLIBCXX_REGEX`. `#pragma GCC system_header`. Namespace
`std`. The object still owns a copy of the pattern characters. An empty
pattern stays empty. A null `const char*` is still `__prism_check` with
class `FUNC-CONTRACT` and the message
`std::regex constructed from a null pointer`, then `__builtin_trap`.

This header is not a proof that a string matches. `basic_regex` `operator==`
and `operator!=` are not PROVED and not CLEAN. Two empty patterns comparing
equal is the result this header defines. It is not a proof that a string
matches, and it is not a fuzzer `CLEAN` result.

## What equality does

`basic_regex` is this `regex`. The operators are members:

```cpp
inline bool basic_regex::operator==(const basic_regex& __y) const noexcept {
    if (_M_len == 0 && __y._M_len == 0) return true;
    if (_M_len != __y._M_len) return false;
    for (std::size_t __i = 0; __i < _M_len; ++__i) {
        if (_M_str[__i] != __y._M_str[__i]) return false;
    }
    return true;
}

inline bool basic_regex::operator!=(const basic_regex& __y) const noexcept {
    return !(*this == __y);
}
```

Two empty patterns compare equal. Both lengths are 0, and that arm returns
true before `_M_str` is named. No character is read. A null pattern pointer
with length 0 is that empty pattern: the pointer is not loaded. A default
pattern and `regex("")` are both that case, including when their flags
differ. Equality does not read `_M_flags`.

Different lengths compare unequal. That return is the length check. It
happens before a character is read, so the longer pattern's stored
characters are not read and the empty side's null pointer is not loaded.

The same length compares the stored characters, `_M_str[__i]` with
`__y._M_str[__i]`, in index order. A mismatch returns false and does not
read a later index. Length 0 never reaches that loop. The same characters
compare equal when the flags differ. The body does not call `flags()`,
`_M_free`, `_M_own`, `_M_from_cstr`, `__prism_check`, `__builtin_trap`, or
`__prism_obj_size`. It does not allocate, so it has no `::operator delete(`.

`operator!=` is `!(*this == __y)`. That body does not name `_M_str`,
`_M_len`, or `_M_flags`.

The comment above `operator==` says this is not PROVED and not CLEAN.
Equality does not decide a match.

## What stayed

`swap` is unchanged. `basic_regex::swap` still returns on `this == &__o`
before `_M_swap`. Self-swap does not free the pattern. Two empty patterns
stay empty. `std::swap` is still that member.

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
The header still does not include `<string>`, `<string_view>`, `<memory>`,
or `<locale>`.

## Wiring

`CMakeLists.txt` was not edited. No new test file was added, so no
`_prism_test_sources` line was added or dropped.
`tests/cpp/test_cxx_regex.cpp` was already listed. `PRISM_PIR_MODELS`
already lists `src/prism/pir/models/cxx/regex`.
`tests/cpp/test_qa_conformance.cpp` was not edited.

## Tests

`tests/cpp/test_cxx_regex.cpp` reads the header. The empty arm of
`operator==` returns true before `_M_str`, a subscript, or `data()`. The
length mismatch returns false before those loads. The same length compares
`_M_str[__i]` with `__y._M_str[__i]` after that return. `operator!=` is
`!(*this == __y)` and does not name `_M_str`. `empty()`, `mark_count()`,
`flags()`, `size()`, and `basic_regex::swap` are unchanged. The null path
in `_M_from_cstr` is still the `FUNC-CONTRACT` check, and the flagged
constructor still stores the flag only after that check. After comments
are stripped, `regex_match`, `regex_search`, and `regex_replace` are still
not identifiers. The only `::operator delete` is still in `_M_free`.
Compiled and run alone with clang++ 18.1.3 (`-std=c++23 -Wall -Wextra
-Werror`, doctest main), 2 test cases, 243 assertions passed.
`prism_tests` was not run.

`tests/cxx_models/regex_trace.cpp` includes `<regex>`. clang++ 18.1.3 with
the system libstdc++ (GCC 14 headers),
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -O1 -Wall -Wextra -Werror`, and `-I` the model
directory. The command ran from an ext4 copy, not from an OneDrive path.
Stdout:

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
eq_empty=1
ne_empty=0
eq_empty_flag=1
ne_empty_flag=0
eq_ptr_empty=1
eq_ab=1
ne_ab=0
eq_self=1
eq_len=0
ne_len=1
eq_empty_full=0
eq_star=1
ne_first=1
ne_second=1
eq_moved_empty=1
```

`eq_empty=1` / `ne_empty=0` is a default pattern and `regex("")`, both
`data=null`. `eq_empty_flag=1` is that empty pattern against an empty
pattern whose flags are 7. `eq_ptr_empty=1` is three empty patterns,
including a copy of the flagged empty pattern. `eq_ab=1` / `ne_ab=0` is
`ab` from a pointer, an array, a copy, a move, and `regex("ab", 0u)`.
`eq_self=1` is one pattern compared with itself. `eq_len=0` / `ne_len=1`
is `ab` against `(a)`. `eq_empty_full=0` is an empty pattern against `ab`.
`eq_star=1` is `.*` with flags 0 against `.*` with flags 3. `ne_first=1`
is `ab` against `xb`. `ne_second=1` is `ab` against `ax`.
`eq_moved_empty=1` is a moved-from pattern against a default pattern.
ASan stayed quiet on that run (exit 0). g++ 13.3.0 compiled the same trace
without the sanitizers and printed the same lines, exit 0.

A separate driver whose `operator new` and `operator delete` count calls,
run after the patterns were already constructed, reported
`delta new=0 del=0` for two empty patterns (flags 0 and 7), the same
characters with flags 0 and 4, a first-character mismatch, a length
mismatch, and a pattern compared with itself.

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
