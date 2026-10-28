# valarray min and max

Branch `finish/cxx-valarray-minmax`, base `020c4b6ffaf889df75f6fe7beb103349d38fbfaf`.

`std::valarray::min` and `std::valarray::max` are declared on the existing
header `src/prism/pir/models/cxx/valarray`. The header is still a file, not a
directory. The include guard is still `_GLIBCXX_VALARRAY`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
`PROVED`. This header is not a proof that an element is the minimum.

## What min and max do

`min()` of an empty valarray calls `__prism_check` with class
`FUNC-CONTRACT` and the message `valarray::min of an empty valarray`, then
`__builtin_trap`. `max()` is the same shape, with the message
`valarray::max of an empty valarray`. The check is before the trap, and the
trap is before any load of the buffer. A default valarray and a valarray
resized to 0 both take that path. The failing path does not return a
value-initialized `T`.

A non-empty valarray returns a stored element: the smallest from `min()`,
the largest from `max()`. The comparison is `operator<`. One element is
that element. There is no addition, so the extremes of `int` and
`long long` are returned as stored. Equal elements may be either
occurrence. This loop keeps the earlier one: a later element replaces the
current one only when it compares strictly less (`min`) or strictly
greater (`max`).

That covers `int`, `unsigned`, `double`, `bool`, and `long long`. `false`
is less than `true`. A class type with `operator<` and a deleted default
constructor is the same shape: the result is a copy of a stored element.
An empty `min` of that type still compiles, and the trap runs with no
`T` constructed.

A driver whose `__prism_check` returns on a failing condition dies with
SIGILL from `__builtin_trap` and does not print `value=`. Under ASan that
empty call does not report a load of the null buffer. Empty `sum` on that
driver is still 0.

## What stayed

`operator[]`, `resize`, `_S_alloc`, and `sum()` are unchanged. An index
`>= size()` is still `MEM-OOB-READ` and does not return a value-initialized
`T`. `sum()` of an empty valarray is still `value_type(0)` before any
`_M_data` load. A sum that does not fit is still `__prism_check` then
`__builtin_trap`, and it does not return a wrapped total. The class is
still `INT-SIGNED-OVF` for a signed integer and `FUNC-CONTRACT` otherwise.
The message is still `valarray::sum does not fit in value_type`.

The allocation check is still `__prism_check` with `MEM-OOB-WRITE` and
`valarray allocation byte count overflows`, followed by `__builtin_trap`
when the byte count does not fit. The allocation expression is still
`::operator new(__n * sizeof(value_type))`.

Math operators, `shift`, `cshift`, `apply`, `slice`, `gslice`, mask
subscript, and indirect subscript are still not declared. They are not
stubbed. The header does not include the vector, memory, or algorithm
headers. It does not call `min_element` or `max_element`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
this header. `CMakeLists.txt` was not edited. `tests/cpp/test_cxx_valarray.cpp`
was already on `_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp`
was not edited. The skips for `iostream/prism_ostream.hpp` and
`deque/deque` are unchanged. The conformance header-name array was not
edited.

## Tests

The sum case used to require `value_type min(` and `value_type max(` to be
absent. Those two checks are gone. A new case requires both functions.

`tests/cpp/test_cxx_valarray.cpp` reads the header. Each of `min` and `max`
contains the `FUNC-CONTRACT` check, then `__builtin_trap`, then a load of
`_M_data`, then `return __best`. Neither body returns `T{}`,
`value_type{}`, `value_type()`, or `value_type(0)`. `sum()` of an empty
valarray is still `value_type(0)` before any `_M_data` load. Both
`operator[]` overloads are still `MEM-OOB-READ` and do not return a
value-initialized `T`. The allocation expression and the overflow message
are still in the file. An `#include` of `<algorithm>`, and the names
`min_element` and `max_element`, are absent. `operator+`, `slice_array`,
`gslice_array`, `shift(`, `cshift(`, and `apply(` are absent.

The translation unit was compiled with clang++ 18.1.3, `-std=c++23 -Wall
-Wextra -Werror`, and the doctest main. 3 test cases, 189 assertions
passed. cmake, ninja, and `prism_tests` were not run.

`tests/cxx_models/valarray_trace.cpp` is `#include <valarray>`. Checked
with clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1`. Compiled
twice (`-isystem` the model directory, and without it). `-H` names the
model header when that directory is first. Model stdout:

```
empty 0 0
one 1 7 7
grow 3 4 12
shrink 1 9 9
zero 0 0
mix 7
unsigned 9
double0 0.0
double 4.0
min0 -2147483648
min1 -2147483647
maxm 2147483646
max0 2147483647
maxb 2147483647
llmin -9223372036854775808
llmax 9223372036854775806
span -2 9
alone 7 7
ties 1 5
ends -4 2
uspan 0 9
dspan -0.25 1.50
bspan 0 1
wide -9223372036854775808 9223372036854775807
tag 0 3 2 1
```

`min0` aborts with `CHECK FUNC-CONTRACT: valarray::min of an empty valarray`.
`max0` and `maxz` abort with `CHECK FUNC-CONTRACT: valarray::max of an empty valarray`.
`minz` is the min message after `resize(0)`. None of those print `value=`.
`ovf` aborts with `CHECK INT-SIGNED-OVF: valarray::sum does not fit in value_type`.
`uovf` and `fovf` abort with
`CHECK FUNC-CONTRACT: valarray::sum does not fit in value_type`.
None of them print `wrapped=`.

`tag 0 3 2 1` is a type with a deleted default constructor. The minimum is
the element whose key is 0. The two keys of 2 compare equal, and `max`
returns the earlier id.

A driver whose `__prism_check` returns on a failing condition dies with
SIGILL on empty `min`, on `max` after `resize(0)`, and on empty `min` of
a type with a deleted default constructor. It does not print `value=`.
Empty `sum` prints `sum=0`. The ASan build of that empty `min` dies with
SIGILL and reports no load.

libstdc++ with `_GLIBCXX_ASSERTIONS` aborts on the trace's empty `sum`
(`Assertion '_M_size > 0' failed`) before `min` runs. A non-empty driver
of the same values prints the same `span` through `tag` lines against
libstdc++. libstdc++ empty `min` aborts in `valarray<int>::min`
(`valarray:1061`, `Assertion '_M_size > 0' failed`). Empty `max` aborts
in `valarray<int>::max` (`valarray:1069`, the same assertion). Neither
prints a value.

`operator+` compiles against libstdc++ and does not compile against this
header (`invalid operands to binary expression`). `std::slice` and
`std::gslice` do not compile against this header (`no member named
'slice'`, `no type named 'gslice'`). Both compile against libstdc++.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return
`C++ valarray unencoded: ... is not a valarray model` from the source
text, before a header is inlined. Those files were not edited. Until that
gate is gone, a unit that names `valarray` does not reach this header.
`docs/PIR.md` was not edited.
