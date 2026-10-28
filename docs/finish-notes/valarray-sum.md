# valarray sum

Branch `finish/cxx-valarray-sum`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::valarray::sum` is declared on the existing header
`src/prism/pir/models/cxx/valarray`. The header is still a file, not a
directory. The include guard is still `_GLIBCXX_VALARRAY`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
`PROVED`. This header is not a proof that a sum is in range.

## What sum does

`sum()` of an empty valarray returns `value_type(0)` before any load of the
buffer. A default valarray and a valarray resized to 0 both take that path.
Under ASan that call does not fault on the null buffer.

One element is that element. There is no addition, so a single stored value
cannot overflow.

A later add that does not fit in `value_type` is `__prism_check` and then
`__builtin_trap`. The class is `INT-SIGNED-OVF` when `value_type` is a
signed integer, and `FUNC-CONTRACT` for every other type. The message is
`valarray::sum does not fit in value_type`. `+=` is after the trap, so a
wrapped total is not returned.

The signed and unsigned tests subtract from the limit only on the side
where that subtraction stays in range, so the test itself does not wrap.
`0 + INT_MIN` and `0 + INT_MAX` fit. `INT_MAX + 1` and `INT_MIN + -1` do
not. The same check covers `long long`.

An unsigned sum that would wrap is `FUNC-CONTRACT`. A finite floating pair
whose sum is not finite is `FUNC-CONTRACT`. That non-finite temporary is
not the value `sum` returns. A non-finite floating element is already a
value of the type, and that pair uses `+=`.

`bool` is not a signed integer and has no separate range, so the fit test
passes and `+=` is the sum. `true + true` is `true`. A class type with
`+=` is the same shape: the check class on a failing fit test is
`FUNC-CONTRACT`. Empty `sum` still needs `value_type(0)`. A type that
cannot be constructed from `0` fails to compile on that path, and pir
falls back to libstdc++.

## What stayed

`operator[]`, `resize`, and `_S_alloc` are unchanged. An index `>= size()`
is still `MEM-OOB-READ` and does not return a value-initialized `T`. The
allocation check is still `__prism_check` with `MEM-OOB-WRITE` and
`valarray allocation byte count overflows`, followed by `__builtin_trap`
when the byte count does not fit. The allocation expression is still
`::operator new(__n * sizeof(value_type))`.

Math operators, `min`, `max`, `shift`, `cshift`, `apply`, `slice`,
`gslice`, mask subscript, and indirect subscript are still not declared.
They are not stubbed. The header does not include `<vector>` or
`<memory>`. It includes `<limits>` and `<type_traits>` for the fit test.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
this header. `CMakeLists.txt` was not edited. `tests/cpp/test_qa_conformance.cpp`
was not edited. The skips for `iostream/prism_ostream.hpp` and
`deque/deque` are unchanged.

No existing test required `sum` to be absent. The new assertions require
it.

## Tests

`tests/cpp/test_cxx_valarray.cpp` reads the header. The empty return is
`value_type(0)` before any `_M_data` load. `_S_add_checked` contains the
`INT-SIGNED-OVF` check and the `FUNC-CONTRACT` check, then
`__builtin_trap`, then `+=`. It does not `return __a + __b`. Both
`operator[]` overloads are still `MEM-OOB-READ` and do not return a
value-initialized `T`. The allocation expression and the overflow message
are still in the file. The translation unit compiled with clang++ 18.1.3,
`-std=c++23 -Wall -Wextra -Werror`. `prism_tests` was not run, so those
`CHECK`s were not executed.

`tests/cxx_models/valarray_trace.cpp` is `#include <valarray>`. Checked
with clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1`. Compiled
twice (`-isystem` the model directory, and without it). The dependency
dump names the model header. Model stdout:

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
```

`ovf` aborts with `CHECK INT-SIGNED-OVF: valarray::sum does not fit in value_type`
and does not print `wrapped=`. `uovf` and `fovf` abort with
`CHECK FUNC-CONTRACT: valarray::sum does not fit in value_type`.
`INT_MIN + -1` and `long long` max plus one are `INT-SIGNED-OVF`. `float`
max plus max is `FUNC-CONTRACT`.

A driver whose `__prism_check` returns on a failing condition dies with
SIGILL from `__builtin_trap` on those sums and does not print `wrapped=`.
Empty `int`, `unsigned`, `double`, and `float` sums are 0 under ASan.

libstdc++ with `_GLIBCXX_ASSERTIONS` aborts on an empty `sum`
(`Assertion '_M_size > 0' failed`). Without a sanitizer, libstdc++ prints
`signed -2147483648` and `unsigned 0` for max plus one. The model does not
return those values.

`operator+` compiles against libstdc++ and does not compile against this
header. `std::slice` and `std::gslice` do not compile against this header.
`std::slice` compiles against libstdc++.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return
`C++ valarray unencoded: ... is not a valarray model` from the source
text, before a header is inlined. Those files were not edited. Until that
gate is gone, a unit that names `valarray` does not reach this header.
`docs/PIR.md` was not edited.
