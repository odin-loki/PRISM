# algorithm min_element / max_element

Branch `finish/cxx-algorithm-minmax`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`<algorithm>` stays one extensionless file, `src/prism/pir/models/cxx/algorithm`.
The include guard is `_GLIBCXX_ALGORITHM`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. This header is not a proof that a range is sorted.
Nothing here is `PROVED`.

## What this slice adds

`std::min`, `std::max`, and `std::clamp` are unchanged.

`clamp(v, lo, hi)` when `hi < lo` still calls `__prism_check` with class
`FUNC-CONTRACT` and the message `clamp: hi < lo`, then `__builtin_trap`.
That path does not return `v`. In a constant expression the call is
ill-formed.

`std::min_element` and `std::max_element` each take two pointers, `_Tp*
__first` and `_Tp* __last`. There is no comparator argument. The type must
be comparable with `operator<`, the same constraint as `min`.

When `last < first`, the function calls `__prism_check` with class
`FUNC-CONTRACT` and the message `min_element: last < first` or
`max_element: last < first`, then `__builtin_trap`. That path does not
return a pointer. In a constant expression the call is ill-formed.

When `first == last`, the result is `last`. That path does not read an
element and does not invent a `T`. An empty range of null pointers returns
null. A type with a deleted default constructor is a valid element type.

Otherwise the result is a pointer to the minimum (`min_element`) or maximum
(`max_element`) element in `[first, last)`. The comparison is strict `<`.
Equal elements keep the first pointer.

`__prism_check` stays declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header.

## What stays omitted

Naming one of these fails to compile, and the pir stage lowers that
translation unit again with libstdc++ (`extra.cxx_models`). They are not
stubbed:

- `sort`
- `find`
- `copy`
- `fill`
- `for_each`
- the comparator overloads of `min_element` and `max_element`
- every other algorithm

No `<memory>`, `<functional>`, `<string>`, `<iterator>`, `<ranges>`,
`<vector>`, or `<compare>` header is included.

## Wiring

`src/prism/pir/stage.cpp`, `CMakeLists.txt`, and
`tests/cpp/test_qa_conformance.cpp` were not edited. The model path and
`tests/cpp/test_cxx_algorithm.cpp` were already on those lists. The
conformance header-name array was not edited. `src/prism/bmc.cpp`,
`src/prism/bmc_unenc.inc`, and `src/prism/bmc_encoder.inc` were not edited.
The existing `std::clamp` unencoded gate is unchanged.

## Tests

`tests/cxx_models/algorithm_trace.cpp` includes `<algorithm>`. It is compiled
twice (model directory as `-isystem`, and without it). Constant checks cover
the previous `min` / `max` / `clamp` cases, a pointer range `{9, 4, 1, 4}`,
equal elements, an empty range, a one-element range, a null empty range, and
a type with no default constructor.

`tests/cpp/test_cxx_algorithm.cpp` reads the header. The inverted-clamp path
still has `__prism_check` then `__builtin_trap` and no `return`. Each
element algorithm checks `last < first` the same way, returns `__last` on an
empty range before any dereference, and keeps the earlier pointer when
elements compare equal. `sort(`, `find(`, `copy(`, `fill(`, and `for_each(`
stay absent from the header.

Checked with clang++ 18.1.3, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1`. Stdout matched:

```
min_eq=1
max_eq=1
min_lo=2
max_hi=9
clamp_mid=5 id=1
clamp_lo=2 id=1
clamp_hi=9 id=1
wide_min_is0=0 wide_min_is_min=1
wide_max_is0=0 wide_max_is_max=1
clamp_inside_zero=1
ord_eq_first=1 ord_max=2
ptr_eq_first=1
el_min=1 el_min_i=2
el_max=9 el_max_i=0
el_empty=1
el_tie=1
el_one=1
el_null=1
el_ord_min=1 el_ord_max_i=0
el_nodef_min=1 el_nodef_max_i=0 el_nodef_empty=1
```

`el_one=1` is the one-element range whose value is 8: both algorithms
return that element. `el_tie=1` is two equal values, and both algorithms
return the first. `el_null=1` is an empty null range; ASan reported no
read. `el_nodef` is a type with a deleted default constructor: the minimum
is 1, the maximum is the first element, and the empty range returns the
start pointer.

`argv` `invert` aborts with `CHECK FUNC-CONTRACT: clamp: hi < lo` and does
not print `value=`. `invert-min` aborts with
`CHECK FUNC-CONTRACT: min_element: last < first`. `invert-max` aborts with
`CHECK FUNC-CONTRACT: max_element: last < first`. Neither prints `value=`.

`static_assert(std::clamp(0, 2, 1) == 0)` is not a constant expression.
`static_assert(std::min_element(a + 2, a) == a)` is not a constant
expression: the call reaches `__prism_check`. `std::sort`, `std::find`,
`std::copy`, `std::fill`, and `std::for_each` compile against libstdc++ and
fail against this header (`no member named` each of those names). A
three-argument `min_element` / `max_element` compiles against libstdc++ and
fails against this header (`requires 2 arguments, but 3 were provided`).

`#include <algorithm>` with `-isystem` on the model directory resolved to
`models/cxx/algorithm`. The header assertions in
`tests/cpp/test_cxx_algorithm.cpp` passed under this clang++ (242
assertions). cmake, ninja, `prism_tests`, and pir were not run.
