# forward_list not-equal

Branch `finish/cxx-forward-list-ne`, base `917518e7f37f06d51d3b305cd0784091a1035e98`.

`operator!=` is a non-member on the existing header
`src/prism/pir/models/cxx/forward_list`. That path is a file. A directory of
the same name would make `#include <forward_list>` fail with EISDIR. The
include guard is still `_PRISM_MODEL_FORWARD_LIST`. The file still defines
`_GLIBCXX_FORWARD_LIST` and is still a `#pragma GCC system_header`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
Nothing here is `PROVED`. Comparing two forward lists for inequality is not
a fuzzer `CLEAN` result. The header says this is not PROVED and not CLEAN.

## What not-equal does

One function template, the same `T` and the same `Alloc`:

```
[[nodiscard]] inline bool operator!=(const forward_list<_Tp, _Alloc>& __x,
                                     const forward_list<_Tp, _Alloc>& __y)
```

The body is `return !(__x == __y)`. It calls the existing container
`operator==`. It does not walk elements itself, and it does not return
`T{}`.

Two empty lists compare not-equal as false. `operator==` takes `begin()`
and `end()` and does not enter its loop when those iterators compare
equal, so no element node is loaded. `begin()` on an empty list is
`iterator(_M_head._M_next)` and `end()` is `iterator(nullptr)`. `empty()`
is still `_M_head._M_next == nullptr`, so those two iterators are the same
null node. The element expression inside the loop is not evaluated.

For a non-empty list the result is the negation of that walk. The walk
stays inside `operator==`. A mismatch returns false from `operator==` at
the first element that compares not equal, so `operator!=` is true and
stops at that same element. Equal lists make `operator==` true, so
`operator!=` is false. A shorter list that matches a prefix of a longer
list compares not-equal as true after the shared elements, still inside
`operator==`.

A different `T` or a different `Alloc` is not an overload. There is no
second template.

`operator<=>` is still the lexicographical three-way compare. Iterator
`operator!=` is not declared. Iterator `operator==` is still the two node
pointers.

## What stayed

`operator==`, `operator<=>`, `front`, `pop_front`, `push_front`,
`emplace_front`, `clear`, `empty`, `swap`, and the iterator operators are
unchanged. `front` and `pop_front` on an empty list are still the
`FUNC-CONTRACT` check, then `__builtin_trap`, before a node load or a
destroy. `__prism_check` and `__builtin_trap()` still occur three times in
the header. The header still includes `<memory>` once. Not-equal does not
call `__prism_check` or `__builtin_trap`.

`splice_after` and `merge` are still not declared. They are not stubbed.

## What still falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and the pir stage lowers it again with libstdc++ (`extra.cxx_models`).
They are not stubbed:

- `splice_after`
- `merge`
- `std::pmr::forward_list` (the alias is declared and the class
  `static_assert`s on `polymorphic_allocator`)

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/forward_list`. `CMakeLists.txt` gained one
`_prism_test_sources` line:

```text
tests/cpp/test_cxx_forward_list_ne.cpp
```

No existing line was removed. `tests/cpp/test_qa_conformance.cpp` was not
edited. `testdata/conformance/libc-models/forward_list_contracts.cpp` and
its yml were not edited.

## Tests

`tests/cpp/test_cxx_forward_list_pop.cpp` still locks
`__prism_check(!this->empty() ? 1 : 0, "FUNC-CONTRACT",` and
`__builtin_trap()` at 3 for the whole header, and `#include <memory>` at 1.
`tests/cpp/test_cxx_forward_list_ne.cpp` reads the header. `operator!=` is
declared once, after `operator==` and before `operator<=>`, as
`return !(__x == __y)`. That body does not contain `!=`, a loop, `begin`,
`end`, a dereference, `_M_`, `T{}`, `__prism_check`, or `__builtin_trap`.
After comments are stripped, `operator!=` occurs once and iterator
`operator!=` does not. `operator==` is still the previous loop: it returns
false at the first element mismatch, and the element load is inside the
loop, after `begin` and `end` have been compared. `front`, `pop_front`,
`push_front`, `emplace_front`, `clear`, `empty`, `swap`, and `operator<=>`
are still the previous bodies. The comment above `operator!=` says this is
not PROVED and not CLEAN.

`tests/cxx_models/forward_list_trace.cpp` includes `<forward_list>`. The
previous lines stay. The trace defines `__prism_check` so a passing check
returns without printing. `front` and `pop_front` in the normal run are on
a non-empty list. New lines cover two empty lists, two equal lists, a
mismatch at the first, middle, and last element, a list compared with
itself, and a `Load` element whose `operator==` counts calls and counts a
read of a negative value. Those counts are the reads inside `operator==`.
A negative value sits past a mismatch and must not be read.

Checked with clang++ 18.1.3 and libstdc++ 14, `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. The trace was compiled twice: once with
this header ahead of libstdc++ as `<forward_list>`, and once against
libstdc++. The dependency dump names this header. Stdout matched, 42
lines. Not-equal lines:

```
ne empty=0 same=0 head=1 mid=1 tail=1 self=0
nel empty=0 calls=0 same=0/3 head=1/1 mid=1/2 tail=1/3 poison=1/2 reads=0
```

`empty=0` is two empty `forward_list<int>` values: not-equal is false.
`nel` `empty=0` with `calls=0` is two empty `forward_list<Load>` values:
`operator==` was not called on an element. `same=0/3` is two equal lists
of three elements: three element comparisons and a false result.
`head=1/1` is a mismatch at the first element. `mid=1/2` is a mismatch at
the second element. `tail=1/3` is a mismatch at the last element.
`poison=1/2` with `reads=0` is `{1, 2, -1} != {1, 9, -1}`: true after two
comparisons, and the negative element is not read. Both binaries exited 0.
`argv` `uaf` and `pop` still abort with `heap-use-after-free`.

The doctest translation unit was compiled on its own with
`-DDOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` and run. The not-equal case passed
(69 assertions). `prism_tests` was not run.

libstdc++ 14 `<forward_list>` in this mode has no separate container
`operator!=`. That overload, and iterator `operator!=`, are compiled only
when `__cpp_impl_three_way_comparison < 201907L`. In C++23, `!=` is the
rewritten negation of `operator==`. This header declares the container
overload and does not declare iterator `operator!=`.
