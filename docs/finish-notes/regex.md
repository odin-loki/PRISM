# regex slice

Branch `finish/cxx-regex`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::regex` is the extensionless file `src/prism/pir/models/cxx/regex`.
A directory of that name would make `#include <regex>` fail with EISDIR,
and the unit would fall back to libstdc++. The include guard is
`_GLIBCXX_REGEX`, so libstdc++'s own header is not also included. The file
is a `#pragma GCC system_header`. The type is `class regex` in
`namespace std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml records
the contract the harness asks for. `expected` is a YAML boolean. Nothing
here is `PROVED`. This header is not a proof that a string matches.

## What the model stores

A partial `std::regex` only. It owns a copy of the pattern characters the
caller wrote. The bytes sit in a block from `::operator new`. `_M_free` is
the one `::operator delete` in the header. The destructor is that call, so
the owned copy is freed once. A move stores `nullptr` and length 0 in the
source and does not free; the source destructor then sees no block. A copy
allocates its own block. Move-assign from a different object frees the
destination's previous block once, then takes the source and leaves the
source empty. Self-move-assign keeps the single block (`this != &__o`).

An empty pattern does not allocate. `size()` is 0, `empty()` is true, and
`data()` is `nullptr`. It is not rewritten to the two characters of a
match-all pattern. `size()` is the character count of the owned copy. This
header does not include `<string>`, so it does not return a `std::string`.
It includes `<utility>` so `std::move` can bind the move constructor.

`regex(const char*)` reads only inside `__prism_obj_size` of that pointer.
A null pointer is `__prism_check(0, "FUNC-CONTRACT", "std::regex constructed from a null pointer")`
then `__builtin_trap`. That path does not return, and it does not store
`.*` or an empty match-all. A missing terminator inside the object is
`__prism_check(0, "MEM-OOB-READ", "std::regex C string is not terminated within its object")`
then `__builtin_trap`. The scan stops at the object bound, and `_M_own` is
not called, so the bytes in range are not copied either. A terminated empty
C string stays empty.

`regex(const char (&)[N])` copies at most `N` characters and stops at the
first NUL. It does not read past the array. With clang++ 18 a string
literal selects the non-template `regex(const char*)` instead, so `"ab"`
and `""` in the scored task go through `__prism_obj_size`. A `char[N]`
that is not a literal selects the array constructor.

`__prism_check` is declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header. `__prism_obj_size` is declared the same way and is
not defined here.

## What falls back

`regex_match`, `regex_search`, and `regex_replace` are not declared. A
true return from any of them would be a fake match. A use fails to compile
against this header, and the pir stage lowers that translation unit again
with libstdc++ (`extra.cxx_models`). They are not stubbed. `basic_regex`,
`wregex`, `regex_traits`, `match_results`, and the iterators are omitted
the same way.

The header does not include `<string>`, `<string_view>`, `<memory>`, or
`<locale>`.

`src/prism/bmc.cpp` still returns "is not a regex model" when the source
text contains `std::regex`, before any header is inlined. This slice does
not edit `bmc.cpp` or `src/prism/pir/stage.cpp`. `stage.cpp` already writes
every embedded `cxx/` model and passes that directory as `-isystem`.

## Wiring

`PRISM_PIR_MODELS` gained one line:

```
src/prism/pir/models/cxx/regex
```

`_prism_test_sources` gained `tests/cpp/test_cxx_regex.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"regex"` with the other public
model headers. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged. `iostream` and `ostream` stay absent.

## Tests

`tests/cpp/test_cxx_regex.cpp` reads the header. It is a regular file. The
guard, the pragma, and `class regex` are present. The four includes above
are absent. After comments are stripped, `regex_match`, `regex_search`,
`regex_replace`, `basic_regex`, `wregex`, and `match_results` are not
identifiers, and the code has no string literal `.*`. The only
`::operator delete` is in `_M_free`, behind a null pointer. The move
constructor clears the source and does not free. The null and missing
terminator paths call `_M_fail` before `_M_own`. `_M_fail` is
`__prism_check` then `__builtin_trap` and does not return. Compiled alone
with clang++ 18.1.3 (`-std=c++23`, doctest main), 75 assertions passed.
`prism_tests` was not run.

`tests/cxx_models/regex_trace.cpp` includes `<regex>`. clang++ 18.1.3 with
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -O1` compiled it with `-isystem` the model
directory. libstdc++'s `std::regex` has no `size`, `empty`, or `data`, so
the same file does not compile without the model. Stdout of the model
binary:

```
empty size=0 empty=1 data=null
ptr size=2 empty=0 data=ab
ptr_empty size=0 empty=1 data=null
arr size=2 empty=0 data=ab
distinct=1
src_after_move size=0 empty=1 data=null
moved size=2 empty=0 data=ab
copy_after size=2 empty=0 data=ab
assign_distinct=1
sink size=2 empty=0 data=ab
assign_src size=0 empty=1 data=null
written size=2 empty=0 data=.*
```

`written` is the caller-written pattern `.*` (two characters). An empty
pattern is `data=null`, not those two characters. `distinct=1` is a copy
with its own block. ASan stayed quiet on that run (exit 0), including
after the move and the move-assign.

`argv` `null` aborts with
`CHECK FUNC-CONTRACT: std::regex constructed from a null pointer` and
prints no pattern line. `argv` `bare` is four bytes with no NUL and
`__prism_obj_size` equal to that object. It aborts with
`CHECK MEM-OOB-READ: std::regex C string is not terminated within its object`
and prints no pattern line. A separate driver with the same sanitizers
constructed from a three-byte `ab\0` whose object size was 3
(`tight size=2 data=ab`) and then from the same buffer with object size 2.
The second call aborted with the same `MEM-OOB-READ` check and did not
print a size.

`std::regex_match`, `std::regex_search`, and `std::regex_replace` fail to
compile against this header (`no member named` in namespace `std`).
Construct, copy, move, and destroy of `regex("ab")` and `regex("")` exit 0
under the same sanitizers against this header (host stubs for the two
intrinsics) and against libstdc++.

`testdata/conformance/prism/cxx/cxx_regex_true.cpp` checks an owned `"ab"`,
an empty pattern, a distinct copy, and a move that leaves the source empty
(`memsafety`, `expected: true`). A host link of that file with the same
stubs returned 0 under ASan/UBSan. There is no false task that calls
`regex_match`. `testdata/conformance/libc-models/regex_contracts.cpp` is
the `#include <regex>` the header case searches for.
