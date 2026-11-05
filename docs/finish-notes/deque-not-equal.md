# deque not-equal

Branch `finish/cxx-deque-ne`, base `c5e0da54bd8c00255c28edf702717dbbd64006ba`.

`operator!=` is a non-member on the existing header
`src/prism/pir/models/cxx/deque`. On main that path is an extensionless
regular file (the original branch had it at `deque/deque`; the port follows
main). A directory of that name would make `#include <deque>` fail with
EISDIR. The include guard is still
`_PRISM_MODEL_DEQUE`. The file still defines `_GLIBCXX_DEQUE` and is still a
`#pragma GCC system_header`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
Nothing here is `PROVED`. Comparing two deques for inequality is not a
fuzzer `CLEAN` result. The header says this is not PROVED and not CLEAN.

## What not-equal does

One function template, the same `T` and the same `Alloc`:

```
[[nodiscard]] bool operator!=(const deque<_Tp, _Alloc>& __x,
                              const deque<_Tp, _Alloc>& __y)
```

The body is `return !(__x == __y)`. It calls the existing container
`operator==`. It does not walk elements, it does not call `begin()`,
`end()`, or `size()`, and it does not return `T{}`.

Two empty deques compare not-equal as false. `operator==` is still

```
return __x.size() == __y.size() && std::equal(__x.begin(), __x.end(), __y.begin());
```

`size()` returns `_M_c ? _M_c->size : 0`. A default deque has `_M_c == nullptr`,
so that read is 0 and does not allocate. Sizes are compared first. When both
sizes are 0 the `&&` still calls `std::equal`. That call is libstdc++'s
`std::equal` from `<bits/stl_algobase.h>`, which this header already includes.

The model iterator is `_Iter`, not libstdc++ `_Deque_iterator`, so this call
does not use `__equal_dit` in `bits/deque.tcc`. It uses the generic loop:

```
for (; __first1 != __last1; ++__first1, (void) ++__first2)
    if (!(*__first1 == *__first2))
        return false;
return true;
```

`begin()` on an empty deque is `_M_begin`. That calls `_M_bind()` and, when
`head` is null, returns `_M_end()`. `end()` is that same past-the-end iterator.
Iterator `operator==` compares the control pointer, the end flag, and the node
pointer. It loads the anchor's `_M_live` through `_M_off()`, and when either
iterator is an end iterator it also loads that end cookie's `_M_live`. It does
not call `operator*`. Iterator `operator!=` is not declared, so the loop
condition is the rewritten negation of iterator `operator==`. An empty range
makes that condition false before `operator*` and before an element
`operator==`, so no element is loaded. `empty()` is still `size() == 0`.

A size mismatch makes `operator==` false before `std::equal` is evaluated, so
`begin()` is not called from that comparison and no element is loaded. A
mismatch of equal length stops at the first difference inside that
`std::equal`. `operator!=` is the negation of that result and does not start a
second walk. Equal deques make `operator==` true, so `operator!=` is false.

A different `T` or a different `Alloc` is not an overload. There is no
second template.

`operator<=>` is still the lexicographical three-way compare. Iterator
`operator!=` is not declared. Iterator `operator==` is still the control,
end-flag, and node comparison.

## What stayed

`operator==`, `operator<=>`, `swap`, `clear`, `front`, `back`, `pop_front`,
`pop_back`, `push_front`, `push_back`, `insert`, `erase`, and `operator[]`
are unchanged. Member `swap` still swaps `_M_c`. `clear` still returns
immediately when `_M_c` is null, and otherwise destroys the elements and
replaces the anchor and the end cookie. The non-member `swap` still calls
the member. `front` and `back` are still `__glibcxx_assert(!this->empty())`
and then the head or tail element. `pop_front` and `pop_back` are still
those same bodies. `push_front` and `push_back` still forward to `emplace`.
`operator[]` is still `__n < this->size()` and then `_M_nth`. Not-equal does
not call `__prism_check` or `__builtin_trap`. The header still has neither.

## What still falls through to libstdc++

Unchanged from `docs/finish-notes/deque.md`. Not declared as a definition, so
a unit that names one fails to compile against this header and the pir stage,
once the header is wired, lowers it again with libstdc++ (`extra.cxx_models`).
They are not stubbed:

- allocators other than `std::allocator<T>` (`static_assert`, including
  `std::pmr::deque`)
- over-aligned `T`
- `shrink_to_fit`
- C++23 `from_range`, `append_range`, `prepend_range`, `insert_range`,
  `assign_range`

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` was not edited.
`CMakeLists.txt` gained one
`_prism_test_sources` line:

```text
tests/cpp/test_cxx_deque_ne.cpp
```

No existing line was removed. `tests/cpp/test_cxx_deque.cpp` is still listed.
`tests/cpp/test_qa_conformance.cpp` was not edited.
`src/prism/pir/models/cxx/algorithm` was not edited. `std::equal` is the one
already included from `<bits/stl_algobase.h>`.

## Tests

`tests/cpp/test_cxx_deque_ne.cpp` reads the header. The header is a regular
file, not a directory. `operator!=` is declared
once, after `operator==` and before `operator<=>`, as `return !(__x == __y)`.
That body does not contain `!=`, a loop, `begin`, `end`, `size`,
`std::equal`, a dereference, `_M_`, `T{}`, `__prism_check`, or
`__builtin_trap`. After comments are stripped, `operator!=` occurs once and
iterator `operator!=` does not. `operator==` is still the previous return:
`size()` is compared before `std::equal`. `operator<=>`, `operator[]`,
`front`, `back`, `pop_front`, `pop_back`, `push_front`, `push_back`,
`insert`, `erase`, `clear`, member `swap`, and the non-member `swap` are
still the previous bodies. The comment above `operator!=` says this is not
PROVED and not CLEAN. The same case checks that `tests/cpp/test_cxx_deque.cpp`,
`tests/cpp/test_cxx_list_ne.cpp`, `tests/cpp/test_cxx_vector_ne.cpp`, and
`tests/cpp/test_cxx_forward_list_ne.cpp` are still listed in `CMakeLists.txt`.

`tests/cxx_models/deque_trace.cpp` includes `<deque>`. The previous lines
stay. New lines cover two empty deques, two equal deques, a mismatch at the
first, middle, and last element, a shorter deque, a longer deque, a deque
compared with itself, two deques compared after `clear`, a const deque, and
a `NoDef` element type with no default constructor. A `Load` element counts
`operator==` calls, counts a read of a negative value, and counts
constructors. Those counts are the reads inside `operator==`. A negative
value sits past a mismatch and must not be read. A size mismatch must not
call `operator==` on an element.

Checked with clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -Wall -Wextra
-Werror -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. The trace was compiled twice: once with
this header ahead of libstdc++ as `<deque>`, and once against libstdc++.
Sibling model headers were not on that include path. The dependency dump
names this header. Stdout matched, 41 lines. Not-equal lines:

```
ne empty=0 same=0 head=1 mid=1 tail=1 short=1 long=1 self=0 clear=0 csame=0 nodef=0
nel empty=0/0 same=0/3 head=1/1 mid=1/2 tail=1/3 len=1/0 lreads=0 poison=1/2 preads=0 eqneg=0/2/1 ctors=0
```

`empty=0` is two empty `deque<int>` values: not-equal is false.
`nel` `empty=0/0` is two empty `deque<Load>` values: not-equal is false
and `Load::operator==` was not called. `ctors=0` is the sum of `Load`
constructors during those comparisons. `same=0/3` is two equal deques of
three elements: three element comparisons and a false result. `head=1/1`
is a mismatch at the first element. `mid=1/2` is a mismatch at the second
element. `tail=1/3` is a mismatch at the last element. `len=1/0` with
`lreads=0` is `{1, -1} != {1}`: true because the sizes differ, with no
element comparison and no read of the negative value. `poison=1/2` with
`preads=0` is `{1, 2, -1} != {1, 9, -1}`: true after two comparisons, and
the negative element is not read. `eqneg=0/2/1` is `{1, -1} != {1, -1}`:
false after two comparisons, and the matching negative element is read.
`nodef=0` is two empty `deque<NoDef>` values. `NoDef` has no default
constructor. Both binaries exited 0. `argv` `uaf` still reports
`heap-use-after-free` in `_M_off` on the model.

The doctest translation unit was compiled on its own with
`-DDOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` and run. The not-equal case passed
(98 assertions). `prism_tests` was not run.

libstdc++ 14 `<deque>` in this mode still declares container `operator==`
as the size check and then `std::equal`. Container `operator!=` is compiled
only when `__cpp_lib_three_way_comparison` is not set. Iterator `operator!=`
is compiled only in that same `#else`. In C++23, `!=` is the rewritten
negation of `operator==`. This header declares the container overload and
does not declare iterator `operator!=`.
