# list not-equal

Branch `finish/cxx-list-ne`, base `07c9062eab54990f97bef5991c549b138ce70893`.

`operator!=` is a non-member on the existing header
`src/prism/pir/models/cxx/list`. That path is a file. A directory of the
same name would make `#include <list>` fail with EISDIR. The include guard
is still `_PRISM_MODEL_LIST`. The file still defines `_GLIBCXX_LIST` and is
still a `#pragma GCC system_header`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
Nothing here is `PROVED`. Comparing two lists for inequality is not a
fuzzer `CLEAN` result. The header says this is not PROVED and not CLEAN.

## What not-equal does

One function template, the same `T` and the same `Alloc`:

```
[[nodiscard]] inline bool operator!=(const list<_Tp, _Alloc>& __x,
                                     const list<_Tp, _Alloc>& __y)
```

The body is `return !(__x == __y)`. It calls the existing container
`operator==`. It does not walk elements, it does not call `begin()`,
`end()`, or `size()`, and it does not return `T{}`.

Two empty lists compare not-equal as false. `operator==` is still

```
if (__x.size() != __y.size()) return false;
auto __i1 = __x.begin();
auto __i2 = __y.begin();
for (auto __e = __x.end(); __i1 != __e; ++__i1, (void)++__i2)
    if (!(*__i1 == *__i2)) return false;
return true;
```

`size()` returns the stored `_M_size`. Sizes are compared first. A size
mismatch returns false before `begin()`. No element is loaded.

When both sizes are 0 the size check does not return. `begin()` is
`iterator(_M_head._M_next)` and `end()` is `iterator(&_M_head)`. `empty()`
is still `_M_head._M_next == &_M_head`, so those two iterators are the same
sentinel. Iterator `operator==` compares the node pointers. The loop
condition `__i1 != __e` is false, so the body `*__i1 == *__i2` is not
evaluated and no element is loaded. `operator!=` is the negation of that
true result, so two empty lists compare not-equal as false.

A mismatch of equal length returns false from `operator==` at the first
element that compares not equal. `operator!=` is true and does not start a
second walk. Equal lists make `operator==` true, so `operator!=` is false.

A different `T` or a different `Alloc` is not an overload. There is no
second template.

`operator<=>` is still the lexicographical three-way compare. Iterator
`operator!=` is not declared. Iterator `operator==` is still the two node
pointers.

## What stayed

`operator==`, `operator<=>`, `size`, `empty`, `front`, `back`,
`pop_front`, `pop_back`, `push_front`, `push_back`, `insert`, `erase`,
`clear`, and `swap` are unchanged. Member `swap` still swaps the sentinel
and `_M_size`. `clear` still drops the owned nodes and sets `_M_size` to 0.
The non-member `swap` still calls the member. Not-equal does not call
`__prism_check` or `__builtin_trap`. The header still has neither; empty
`front`, `back`, `pop_front`, and `pop_back` are still `__glibcxx_assert`.

## What still falls through to libstdc++

Not declared as a definition, so a unit that names one fails to compile
against this header and the pir stage lowers it again with libstdc++
(`extra.cxx_models`). They are not stubbed:

- every `splice` overload
- every `merge` overload
- allocators other than `std::allocator<T>`
- `std::pmr::list` (the alias is declared and the class `static_assert`s
  on `polymorphic_allocator`)

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/list`. `CMakeLists.txt` gained one
`_prism_test_sources` line:

```text
tests/cpp/test_cxx_list_ne.cpp
```

No existing line was removed. `tests/cpp/test_qa_conformance.cpp` was not
edited. `testdata/conformance/libc-models/list_contracts.cpp` and its yml
were not edited. `tests/cpp/test_cxx_list.cpp` was not edited.

## Tests

`tests/cpp/test_cxx_list_ne.cpp` reads the header. `operator!=` is
declared once, after `operator==` and before `operator<=>`, as
`return !(__x == __y)`. That body does not contain `!=`, a loop, `begin`,
`end`, `size`, a dereference, `_M_`, `T{}`, `__prism_check`, or
`__builtin_trap`. After comments are stripped, `operator!=` occurs once and
iterator `operator!=` does not. `operator==` is still the previous loop:
`size()` is compared before `begin()`, and the element load is inside the
loop, after `begin` and `end` have been compared. `operator<=>`, `size`,
`empty`, `front`, `back`, `pop_front`, `pop_back`, `push_front`,
`push_back`, `insert`, `erase`, `clear`, member `swap`, and the non-member
`swap` are still the previous bodies. The comment above `operator!=` says
this is not PROVED and not CLEAN. The same case checks that
`tests/cpp/test_cxx_list.cpp`, `tests/cpp/test_cxx_vector_ne.cpp`, and
`tests/cpp/test_cxx_forward_list_ne.cpp` are still listed in
`CMakeLists.txt`.

`tests/cxx_models/list_trace.cpp` includes `<list>`. The previous lines
stay. New lines cover two empty lists, two equal lists, a mismatch at the
first, middle, and last element, a shorter list, a longer list, a list
compared with itself, two lists compared after `clear`, a const list, and
a `NoDef` element type with no default constructor. A `Load` element counts
`operator==` calls, counts a read of a negative value, and counts
constructors. Those counts are the reads inside `operator==`. A negative
value sits past a mismatch and must not be read. A size mismatch must not
call `operator==` on an element.

Checked with clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -Wall -Wextra
-Werror -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. The trace was compiled twice: once with
this header ahead of libstdc++ as `<list>`, and once against libstdc++.
Sibling model headers were not on that include path. The dependency dump
names this header. Stdout matched, 40 lines. Not-equal lines:

```
ne empty=0 same=0 head=1 mid=1 tail=1 short=1 long=1 self=0 clear=0 csame=0 nodef=0
nel empty=0/0 same=0/3 head=1/1 mid=1/2 tail=1/3 len=1/0 lreads=0 poison=1/2 preads=0 eqneg=0/2/1 ctors=0
```

`empty=0` is two empty `list<int>` values: not-equal is false.
`nel` `empty=0/0` is two empty `list<Load>` values: not-equal is false
and `Load::operator==` was not called. `ctors=0` is the sum of `Load`
constructors during those comparisons. `same=0/3` is two equal lists of
three elements: three element comparisons and a false result. `head=1/1`
is a mismatch at the first element. `mid=1/2` is a mismatch at the second
element. `tail=1/3` is a mismatch at the last element. `len=1/0` with
`lreads=0` is `{1, -1} != {1}`: true because the sizes differ, with no
element comparison and no read of the negative value. `poison=1/2` with
`preads=0` is `{1, 2, -1} != {1, 9, -1}`: true after two comparisons, and
the negative element is not read. `eqneg=0/2/1` is `{1, -1} != {1, -1}`:
false after two comparisons, and the matching negative element is read.
`nodef=0` is two empty `list<NoDef>` values. `NoDef` has no default
constructor. Both binaries exited 0. `argv` `uaf` still aborts with
`heap-use-after-free` on both.

The doctest translation unit was compiled on its own with
`-DDOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` and run. The not-equal case passed
(97 assertions). `prism_tests` was not run.

libstdc++ 14 `<list>` in this mode has no separate container `operator!=`.
That overload is compiled only when `__cpp_lib_three_way_comparison` is not
set. Iterator `operator!=` is compiled only when
`__cpp_impl_three_way_comparison < 201907L`. In C++23, `!=` is the rewritten
negation of `operator==`. This header declares the container overload and
does not declare iterator `operator!=`.
