# iterator distance slice

Branch `finish/cxx-iterator-distance`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::distance` for two pointers lives in the existing extensionless file
`src/prism/pir/models/cxx/iterator`. A directory of that name would make
`#include <iterator>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_GLIBCXX_ITERATOR`. The file is
still a `#pragma GCC system_header`, and the functions stay in namespace
`std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml
records the contract the task asks for. `expected` is a YAML boolean.
Nothing here is `PROVED`. This is not a proof that an iterator stays in
range, and it is not a proof that the two pointers address the same array.

## What the model returns

`begin` and `end` are unchanged. For `N > 0` the parameter is `T (&)[N]`.
`begin` returns `__arr` (array-to-pointer decay, not a load of the
element). `end` returns `__arr + _Nm`. `N == 0` is still the second
overload, a reference constrained with `sizeof(_Arr) == 0`. Those two
pointers compare equal. Neither body loads the pointer. A pointer argument
has a non-zero size, so it still does not match `begin` or `end`.

`std::distance` is one overload, two `T*` arguments. A class iterator does
not match. The result is a `std::ptrdiff_t` element count from the first
pointer to the second, and it may be negative.

The run-time count is an unsigned address gap divided by `sizeof(T)`. It
is not the C++ expression `__last - __first`. A count outside
`[PTRDIFF_MIN, PTRDIFF_MAX]` calls `__prism_check` with class
`FUNC-CONTRACT` and message
`std::distance subtraction does not fit in ptrdiff_t`, then
`__builtin_trap`. That path does not return. `PTRDIFF_MIN` is returned
only when the negative magnitude is exactly that value, which fits. A
larger magnitude is not returned as a wrapped bit pattern.

A byte gap can fail to fit in `ptrdiff_t` while the element count still
fits. Two `int*` that are `PTRDIFF_MAX + 1` bytes apart are `2^61`
elements apart. That count fits, so the model returns
`2305843009213693952`. It does not return the negative quotient of a
wrapping signed division.

Constant evaluation uses `__last - __first`. An unrepresentable difference
is not a constant expression, so that return is not a wrapped value.
`constexpr` distance of a three-element array is `3`, and the reverse is
`-3`, against this header and against libstdc++.

`__prism_check` is declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header.

## What falls back

Not declared, so a unit that names one fails to compile against this header
and pir lowers that translation unit again with libstdc++
(`extra.cxx_models`). They are not stubbed:

- `begin` and `end` for a pointer
- `std::distance` for anything other than two `T*`
- `std::next`, `std::prev`, `std::advance`, and `reverse_iterator`
- iterator traits, tags, insert iterators, stream iterators, and the
  container overloads of `begin` and `end`

The header includes `<cstddef>` and `<cstdint>`. It does not include
`<memory>`, `<vector>`, `<string>`, `<algorithm>`, or `<ranges>`. A later
`++` or a dereference of a pointer is the caller's expression. This header
does not decide whether that use stays inside an array, and it does not
decide that the two pointers address one array.

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
that a pointer is not a `begin` parameter. It used to require the header
to contain no `__prism_check` and exactly two `*` tokens, which is the
shape before `distance`. Those checks now require the single
`FUNC-CONTRACT` call above, followed by `__builtin_trap` in a function
that does not return, and eight `*` tokens: the two `_Tp*` return types,
the four `_Tp*` parameters, and the two `const char*` parameters of the
check declaration. The run-time count does not contain `__last - __first`.
The constant-evaluation return is that subtraction, and it is the only
one. With line comments removed, the code still does not name `next`,
`prev`, `advance`, or `reverse_iterator`.

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
```

and exited 0. `argv` `huge` is two `char*` whose element count is
`PTRDIFF_MAX + 1`. The model aborts with
`CHECK FUNC-CONTRACT: std::distance subtraction does not fit in ptrdiff_t`
and prints no `wrapped=` line. libstdc++ returns
`-9223372036854775808` (`PTRDIFF_MIN`) and exits 0, with no sanitizer
diagnostic.

An `N > 0` program (distance `3` and `-3` on `int[3]`, distance `2` on a
const array and on an array of pointers) printed the same lines against
libstdc++ and against this header. `static_assert` of those constexpr
distances holds both ways. `std::begin` on `int*` fails here. `std::next`
fails here (`no member named 'next'`). `std::distance` on a class type
fails here (`could not match '_Tp *'`). An empty array still does not
compile against libstdc++ (`zero-length arrays are not permitted in C++`).

`testdata/conformance/prism/cxx/cxx_iterator_true.cpp` still reads `*begin`
of a three-element array and compares `begin` and `end` of an empty array
(`no-oob`, `expected: true`). It also requires `distance` of that array to
be `3` and `-3`, and `distance` of the empty array to be `0`. Those calls
do not load the element. `cxx_iterator_false.cpp` calls `distance` on two
`char*` separated by `PTRDIFF_MAX + 1` bytes when the input is non-zero
(`expected: false`, witness `[1]`, property `no-overflow`). Both
`expected` values are YAML booleans. The false task is `sanitizer_blind`:
a UBSan build of that function against libstdc++ returns 0. The
`ptrdiff_t` result is `PTRDIFF_MIN`, and the conversion to `int` is 0,
with no sanitizer diagnostic. The model takes the check and the trap
instead, and does not return that value.

`testdata/conformance/libc-models/iterator_contracts.cpp` includes
`<iterator>` and calls the same in-range `distance`. It is the contract
harness the header case searches for. It is not a scored task.
