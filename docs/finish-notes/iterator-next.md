# iterator next slice

Branch `finish/cxx-iterator-next`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.

`std::next` for a pointer lives in the existing extensionless file
`src/prism/pir/models/cxx/iterator`. A directory of that name would make
`#include <iterator>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_GLIBCXX_ITERATOR`. The file is
still a `#pragma GCC system_header`, and the functions stay in namespace
`std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. This is not a proof that an iterator stays in range, and it
is not a proof that the pointer addresses an array.

## What the model returns

`begin`, `end`, and `distance` are unchanged, including both `N == 0`
overloads. For `N > 0` the parameter is `T (&)[N]`. `begin` returns
`__arr` (array-to-pointer decay, not a load). `end` returns `__arr + _Nm`.
`N == 0` is still the reference constrained with `sizeof(_Arr) == 0`.
Those two pointers compare equal. A pointer argument still does not match
`begin` or `end`. `distance` is still the two-`T*` overload. A count that
does not fit in `ptrdiff_t` is still `__prism_check` with class
`FUNC-CONTRACT` and message
`std::distance subtraction does not fit in ptrdiff_t`, then
`__builtin_trap`.

`std::next` is two overloads, both for `T*` only. There is no default
argument. A class iterator does not match either one.

`next(p)` returns `p + 1`.

`next(p, 0)` returns `p`. That return does not add.

`next(p, n)` for `n > 0` returns `p + n`. The sum is the `else` of the
`n < 0` and `n == 0` tests, so a negative `n` does not form `p + n`.

`n < 0` calls `_Prism_next_negative`, which calls

`__prism_check(0, "FUNC-CONTRACT", "std::next offset is negative")`

and then `__builtin_trap`. That function does not return and does not
form `p + n`. The same sentence is a comment on the header: this is not
a proof that the pointer addresses an array.

`__prism_check` stays declared
`extern "C" void __prism_check(int, const char*, const char*);` and is
not defined in the header. This slice does not declare it again.

A negative offset is not a constant expression. Constant evaluation calls
`_Prism_next_negative`, which is not `constexpr`. The diagnostic names
that call. It does not name `return __p + __n`. `next(p)`, `next(p, 0)`,
and `next(p, n)` for an in-range positive `n` are constant expressions.
`next(end, 0)` is `end`.

## What falls back

Not declared, or not a match, so a unit that names one fails to compile
against this header and pir lowers that translation unit again with
libstdc++ (`extra.cxx_models`). They are not stubbed:

- `begin` and `end` for a pointer
- `std::distance` for anything other than two `T*`
- `std::next` for anything other than `T*`
- `std::prev`, `std::advance`, and `reverse_iterator`
- iterator traits, tags, insert iterators, stream iterators, and the
  container overloads of `begin` and `end`

The header includes `<cstddef>` and `<cstdint>`. It does not include
`<memory>`, `<vector>`, `<string>`, `<algorithm>`, or `<ranges>`. A later
`++` or a dereference of the returned pointer is the caller's expression.
This header does not decide whether that use stays inside an array.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/iterator`. `_prism_test_sources` already lists
`tests/cpp/test_cxx_iterator.cpp`. `tests/cpp/test_qa_conformance.cpp`
already lists `"iterator"` in the public header-name array. That array was
not edited. The skips for `iostream/prism_ostream.hpp` and `deque/deque`
are unchanged. `iostream` and `ostream` stay absent. `CMakeLists.txt` was
not edited.

## Tests

`tests/cpp/test_cxx_iterator.cpp` reads the header as text. It still checks
that `begin` and `end` are the array-reference overload and the
`sizeof == 0` overload, that those bodies do not load the element, and
that a pointer is not a `begin` parameter. The star count is now twelve:
the previous eight, plus two `_Tp*` returns and two `_Tp*` parameters on
the `next` overloads. With line comments removed, the code names `next`
twice and does not name `prev`, `advance`, or `reverse_iterator`. The
negative helper contains the `FUNC-CONTRACT` call above, then
`__builtin_trap`, and it contains no `+`. `next(p)` is `return __p + 1`.
In `next(p, n)` the only `+` is `return __p + __n`, and that return is
after `return __p`. The translation unit was syntax-only. `prism_tests`
was not run, so those `CHECK`s were not executed by the test binary. A
reader using the same comment strip and brace match accepted the header.

`tests/cxx_models/iterator_trace.cpp` is `#include <iterator>`. Checked
with clang++ 18.1.3 and libstdc++ 14 (`-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra
-Werror`). The model trace printed

```
len 3 first 4 last 6
neg -3
empty 1
const 2 8
ptrs 2 1
next 5 1 6 1
next0 1
nextc 9
nextrt 1 6
```

and exited 0. `next 5 1 6 1` is `*next(b)`, `next(b, 0) == b`,
`*next(b, 2)`, and `next(b, 3) == end`. `next0` is `next` of an empty
array's `begin` by zero. `nextc` is `*next` of a const array. `nextrt`
is the same zero and `+ 2` steps with a volatile offset, so those
branches ran. `argv` `neg` aborts with
`CHECK FUNC-CONTRACT: std::next offset is negative` and prints no
`moved=` line. `argv` `huge` still aborts with
`CHECK FUNC-CONTRACT: std::distance subtraction does not fit in ptrdiff_t`
and prints no `wrapped=` line.

A driver whose `__prism_check` returns dies with SIGILL from
`__builtin_trap` and prints no `moved=` line.

An `N > 0` program (distance `3` and `-3` on `int[3]`, distance `2` on a
const array and on an array of pointers, and the same `next` steps)
printed the same lines against libstdc++ and against this header.
`static_assert` of those constexpr `next` results holds both ways.
`std::begin` on `int*` fails here (`no matching function for call to
'begin'`). `std::next` on a class type fails here (`no matching function
for call to 'next'`). `std::prev` fails here (`no member named 'prev'`).
`std::advance` fails here (`no member named 'advance'`).
`std::reverse_iterator` fails here (`no member named 'reverse_iterator'`).
An empty array still does not compile against libstdc++ (`zero-length
arrays are not permitted in C++`). It compiles against this header.

libstdc++ `next(p, -1)` on `int[3]` prints `moved=-1` and exits 0, with
no sanitizer diagnostic.

`testdata/conformance/prism/cxx/cxx_iterator_true.cpp` still reads `*begin`
of a three-element array and compares `begin` and `end` of an empty array
(`no-oob`, `expected: true`). It also requires `next(b) == b + 1`,
`next(b, 0) == b`, `next(b, 2) == b + 2`, and `next` of the empty `begin`
by zero to be that pointer. Those calls do not load the element.
`cxx_iterator_false.cpp` is unchanged: `distance` of two `char*` separated
by `PTRDIFF_MAX + 1` bytes (`expected: false`, witness `[1]`, property
`no-overflow`). Both `expected` values are YAML booleans. The yml files
were not edited.

`testdata/conformance/libc-models/iterator_contracts.cpp` includes
`<iterator>` and calls the same in-range `distance` and `next`. It is the
contract harness the header case searches for. It is not a scored task.

cmake, ninja, `prism_tests`, and pir were not run.
