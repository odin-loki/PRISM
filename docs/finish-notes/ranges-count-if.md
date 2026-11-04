# ranges count_if

Branch `port/wip-b`, base `main` at `0a9571725`.

`std::ranges::count_if` is declared in the `<algorithm>` model,
`src/prism/pir/models/cxx/algorithm`, inside `namespace std::ranges`. It is
not in the `<ranges>` model. libstdc++ declares it in `<algorithm>`
(`bits/ranges_algo.h`), and its `<ranges>` does not: a program that
includes only `<ranges>` and calls `std::ranges::count_if` fails to compile
against libstdc++. The model does the same, so it does not accept a program
the library rejects.

The solver was not run. Nothing here is `PROVED`. Nothing here is `CLEAN`.
After a failed check the path traps. It does not continue into a call, a
load, or a fabricated count.

## What this slice adds

Two overloads.

`__PTRDIFF_TYPE__ count_if(T (&a)[N], bool (*pred)(const T&))`, for `N >= 1`.
`T` is deduced from the array only; the predicate parameter goes through
`_Prism_count_if_identity`, so a `const` array binds `bool (*)(const int&)`
and a captureless lambda of `const T&` converts. A null predicate calls

`__prism_check(0, "FUNC-CONTRACT", "std::ranges::count_if null predicate")`

and then `__builtin_trap`. That path does not call the predicate and does
not load an element. Otherwise the loop is `for (size_t __i = 0; __i < _Nm;
++__i)` and `++__n` when `__pred(__a[__i])` is true. The subscript is below
`N`. The predicate is called once per element. The result is the count,
not a pointer.

`__PTRDIFF_TYPE__ count_if(_Arr&, bool (*)(const _Elem&)) noexcept`, with
`requires(sizeof(_Arr) == 0 && _Prism_count_if_zero_elem<_Arr, _Elem>)`. A
zero-length array returns 0. It does not load and does not call the
predicate, even when the predicate is null.

The result type is the same type as `std::ptrdiff_t`. The helper names are
`_Prism_count_if_identity` and `_Prism_count_if_zero_elem`, not the
`<ranges>` model's `_Prism_type_identity_t` and `_Prism_zero_array_elem`,
so the two headers can be included together.

## What stays omitted

A pointer, a pointer to an array, an iterator pair, a class with a `size()`
member, a projection, a predicate that takes `T` by value, and a lambda
that takes `T` by value do not match. Those calls compile against
libstdc++ and fail against the model, and pir falls back to libstdc++.
`std::count_if`, the iterator form, is not declared.

## Wiring

No new `tests/cpp` file was added, so `CMakeLists.txt` was not edited. The
`<ranges>` model is unchanged.

## Tests

`tests/cxx_models/ranges_trace.cpp` now includes `<algorithm>` after
`<ranges>`, against libstdc++ and against the model directory. It adds
constant checks, `decltype` checks for `ptrdiff_t`, `requires` checks
(arrays and `const` arrays match; `int*`, `const int*`, `int (*)[4]`, and a
class do not), and printed lines:

```
cfhit 1 4
cftwo 2 4
cffront 1 4
cfmiss 0 4
cfempty 0 0
cftie 2 2
cftiemiss 0 2
cfone 1
cfkeep 1
cfcfirst 1
cfczero 2 4
cfcall 4 4
cfcmiss 0
cfznull 1
cfpnull 2
cfpmiss 0
```

The second number is how many times the predicate was called: once per
element, and 0 for a zero-length array. `argv` `countnull` aborts with
`CHECK FUNC-CONTRACT: std::ranges::count_if null predicate` and does not
print `called`.

`tests/cpp/test_cxx_ranges.cpp` checks that the `<ranges>` model does not
contain `count_if`, and compiles snippets: arrays, `const` arrays and
`int*` elements compile against both; `<ranges>` alone fails against both;
a pointer, a pointer to an array and a class fail against the model; an
iterator pair, a by-value lambda and a by-value function compile against
libstdc++ and fail against the model; a `const int&` lambda compiles
against both; a null predicate in a constant expression is ill-formed at
`__prism_check`.

`tests/cpp/test_cxx_algorithm.cpp` reads the header: `count_if` occurs only
after `namespace ranges {`; the null check and trap come before the loop;
no call, load or return before the loop; one return; the zero-length body
is `return 0;` with no load, no call and no check.

The ranges trace was built with clang++ 18.1.3 (the compiler the doctest
picks), `-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1 -w`. Stdout of the model binary matched
libstdc++ (116 lines).
