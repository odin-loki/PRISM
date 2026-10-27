# iterator slice

Branch `finish/cxx-iterator`, base `1278b9dc714ceaee54277a298c50212376942489`.

`std::begin` and `std::end` for arrays live in the extensionless file
`src/prism/pir/models/cxx/iterator`. A directory of that name would make
`#include <iterator>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is `_GLIBCXX_ITERATOR`, so libstdc++'s own
header is not also included. The file is a `#pragma GCC system_header` and
the functions live in namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml records
the contract the task asks for. `expected` is a YAML boolean. Nothing here
is `PROVED`. This is not a proof that an iterator stays in range.

## What the model returns

`begin` and `end` for an array, and nothing else.

For `N > 0` the parameter is `T (&)[N]`. `begin` returns `__arr`. That is
array-to-pointer decay: a pointer to the first element, not a load of it.
`end` returns `__arr + _Nm`, a pointer one past the last element. A const
array uses the same templates, because `_Tp` is deduced as `const U`.

`N == 0` is valid, and it is not that parameter. Clang rejects
`T (&)[0]` while substituting the bound ("zero-length arrays are not
permitted in C++"), so libstdc++'s one overload does not accept an empty
array either. The empty array is a second overload: a reference constrained
with `sizeof(_Arr) == 0`. `begin` returns the decayed pointer. `end` returns
that pointer plus zero. The two pointers compare equal. Neither body loads
the pointer. A pointer argument has a non-zero size, so it does not match
this overload.

## What falls back

Not declared, so a unit that names one fails to compile against this header
and pir lowers that translation unit again with libstdc++
(`extra.cxx_models`). They are not stubbed:

- `begin` and `end` for a pointer. A pointer does not match `T (&)[N]`, and
  `sizeof` of a pointer is not 0, so the empty-array overload does not take
  it either. Declaring one would accept the call and the unit would not
  fall back.
- `std::next`, `std::prev`, `std::advance`, and `reverse_iterator`
- iterator traits, tags, insert iterators, stream iterators, and the
  container overloads of `begin` and `end`

The header includes `<cstddef>` and no other standard header. It does not
include `<memory>`, `<vector>`, `<string>`, `<algorithm>`, or `<ranges>`.
There is no `__prism_check`. A later `++` or a dereference of the returned
pointer is the caller's expression. This header does not decide whether
that use stays inside the array.

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model and passes that directory as `-isystem`. `src/prism/bmc.cpp`
still returns early for `basic_const_iterator` and `counted_iterator`. Those
gates were not edited. The task source does not name them.

## Wiring

`PRISM_PIR_MODELS` is an explicit list. One line was added:

```
src/prism/pir/models/cxx/iterator
```

`_prism_test_sources` gained `tests/cpp/test_cxx_iterator.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"iterator"` next to the other
public header names. The skips for `iostream/prism_ostream.hpp` and
`deque/deque` are unchanged. `iostream` and `ostream` stay absent.

## Tests

`tests/cpp/test_cxx_iterator.cpp` reads the header as text and does not
include it. Doctest already includes the platform library; defining
`_GLIBCXX_ITERATOR` in that translation unit would hide libstdc++'s
`<iterator>` or define `std::begin` twice. The test checks that the file is
not a directory, that the only include is `<cstddef>`, and that `begin` and
`end` each have the array-reference declaration and the `sizeof == 0`
declaration. The array `begin` returns `__arr`. The array `end` returns
`__arr + _Nm`. The empty `begin` returns `__arr`. The empty `end` returns
`__arr + 0`. Those bodies contain no `*` and no subscript. With line
comments removed, the code has two `*` tokens, both the `_Tp*` return type,
and it does not name `next`, `prev`, `advance`, `reverse_iterator`, or
`__prism_check`. A comment states that this is not a proof that an iterator
stays in range.

`tests/cxx_models/iterator_trace.cpp` is `#include <iterator>`. It prints the
distance from `begin` to `end` on a three-element array, the first and last
elements, `1` when an empty array's `begin` and `end` compare equal, the
same facts for a const array, and the first element of an array of pointers.
It does not dereference the empty pointers. `static_assert` rejects
`std::begin` on `int*` and `const int*`. It does not call `std::next`.
An empty array does not compile against libstdc++: that library's
`T (&)[N]` parameter is the substitution failure above.

Checked with clang++ 18.1.3 (`-std=c++23 -D_GLIBCXX_ASSERTIONS -O1`, and
again with `-Wall -Wextra -Werror`). The trace against this header printed
`len 3 first 4 last 6`, `empty 1`, `const 2 8`, `ptrs 2 1` and exited 0.
The same run with `-fsanitize=address,undefined -fno-sanitize-recover=all`
exited 0. A program that only uses `N > 0` printed the same lines against
libstdc++ and against this header. `std::begin` on an `int*` failed to
compile both ways. `std::next` failed here (`no member named 'next'`) and
compiled against libstdc++. cmake, ninja, `prism_tests`, and pir were not
run.

`testdata/conformance/prism/cxx/cxx_iterator_true.cpp` reads `*begin` of a
three-element array and compares `begin` and `end` of an empty array
(`no-oob`, `expected: true`). There is no false task.
`testdata/conformance/libc-models/iterator_contracts.cpp` includes
`<iterator>`. It is the contract harness that case searches for. It is not
a scored task.
