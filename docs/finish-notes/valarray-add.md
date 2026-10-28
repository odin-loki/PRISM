# valarray addition and subtraction

Branch `finish/cxx-valarray-add`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`std::valarray::operator+` and `std::valarray::operator-` for two
`valarray<T>` values are declared on the existing header
`src/prism/pir/models/cxx/valarray`. The header is still a file, not a
directory. The include guard is still `_GLIBCXX_VALARRAY`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
`PROVED`. Nothing here is `CLEAN`. This header is not a proof that a sum
or a difference is in range.

## What operator+ and operator- do

Both take two `const valarray&` arguments. Scalar overloads are not
declared. Unary `+` and unary `-` are not declared.

Different sizes call `__prism_check` with class `FUNC-CONTRACT` and the
message `valarray::operator+ sizes differ` or
`valarray::operator- sizes differ`, then `__builtin_trap`. The check is
before the trap, and the trap is before any load of either buffer and
before `_S_alloc`. That path does not return a partial array. One empty
array and one non-empty array take it. A type whose element `operator+`
prints `loaded` does not print it.

Two empty arrays, including a valarray resized to 0, compare equal sizes
and return an empty valarray. That return is before any `_M_data` load.
A type whose element `operator+` counts calls stays at 0. Under ASan the
empty call does not fault on the null buffers.

Otherwise the result has the same size as the inputs. Each element is the
sum or the difference. The result is a new buffer. Writing the result
does not change the inputs. `v + v` and `v - v` read the same array twice
and store into the new one.

An element that does not fit calls `__prism_check` and then
`__builtin_trap` before that element is formed and before it is stored.
The class is `INT-SIGNED-OVF` when `value_type` is a signed integer, and
`FUNC-CONTRACT` for every other type. The messages are
`valarray::operator+ does not fit in value_type` and
`valarray::operator- does not fit in value_type`. The function does not
return a wrapped element and does not return a value-initialized `T`.

The signed and unsigned tests stay in range, so the test itself does not
wrap. `INT_MAX + -1`, `INT_MAX + INT_MIN`, `INT_MIN - INT_MIN`, and
`-1 - INT_MIN` fit. `INT_MAX + 1` and `INT_MIN - 1` do not. The same
check covers `long long`. An unsigned value that would wrap is
`FUNC-CONTRACT`, including `0u - 1u`. A finite floating pair whose sum or
difference is not finite is `FUNC-CONTRACT`. That non-finite temporary is
not stored. `bool` is not a signed integer; `true + true` is `true`, and
`false - true` is `true`.

A class type with `operator+` and `operator-`, and a deleted default
constructor, is the same shape. Empty `operator+` still compiles. A
two-element sum is one call per element, and the stored values are those
results. The fit class for that type is `FUNC-CONTRACT`.

A driver whose `__prism_check` returns on a failing condition dies with
SIGILL from `__builtin_trap` and does not print `partial=`, `wrapped=`,
or `loaded`. Under UBSan that death is the illegal instruction. UBSan
does not report a signed overflow, an unsigned wrap, or a float overflow
on those paths.

## What stayed

`operator[]`, `resize`, `_S_alloc`, `sum()`, `min()`, and `max()` are
unchanged. An index `>= size()` is still `MEM-OOB-READ` and does not
return a value-initialized `T`. `sum()` of an empty valarray is still
`value_type(0)` before any `_M_data` load. A sum that does not fit is
still `__prism_check` then `__builtin_trap`, and it does not return a
wrapped total. The class is still `INT-SIGNED-OVF` for a signed integer
and `FUNC-CONTRACT` otherwise. The message is still
`valarray::sum does not fit in value_type`. `min()` and `max()` of an
empty valarray are still `FUNC-CONTRACT` then `__builtin_trap` before any
load.

The allocation check is still `__prism_check` with `MEM-OOB-WRITE` and
`valarray allocation byte count overflows`, followed by `__builtin_trap`
when the byte count does not fit. The allocation expression is still
`::operator new(__n * sizeof(value_type))`, once.

Scalar `+` and `-`, the other math operators, the compound forms, the
comparisons, unary `+` `-` `~` `!`, `shift`, `cshift`, `apply`, `slice`,
`gslice`, mask subscript, and indirect subscript are still not declared.
They are not stubbed. The header does not include the vector, memory,
algorithm, or initializer_list headers.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
this header. `CMakeLists.txt` was not edited. `tests/cpp/test_cxx_valarray.cpp`
was already on `_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp`
was not edited. The skips for `iostream/prism_ostream.hpp` and
`deque/deque` are unchanged. The conformance header-name array was not
edited.

## Tests

The sum case and the min/max case used to require the text `operator+` to
be absent. Those two checks now require the scalar signatures
`operator+(const value_type&` and `operator-(const value_type&` to be
absent. A new case requires both two-array operators.

`tests/cpp/test_cxx_valarray.cpp` reads the header. Each operator contains
the `FUNC-CONTRACT` size check, then `__builtin_trap`, then
`return valarray()`, and only then a load of `_M_data`. The trap is
before `_S_alloc`. The only full return is `return __r` after
`__r._M_size` is set to the common size, and the loop does not return.
Neither operator returns `T{}`, `value_type{}`, `value_type()`, or
`value_type(0)`, and neither calls `resize`. `_S_add_elem` and
`_S_sub_elem` contain the `INT-SIGNED-OVF` check and the `FUNC-CONTRACT`
check, then `__builtin_trap`, then `__a + __b` or `__a - __b`. They do
not `return __a + __b` or `return __a - __b`, and they do not placement-new
into the buffer. `_S_diff_fits` does not return the difference it uses to
test a floating value. The allocation expression and the overflow message
are still in `_S_alloc`, and the expression occurs once. `sum()` of an
empty valarray is still `value_type(0)` before any `_M_data` load.
`_S_add_checked` is still the sum message, then `__builtin_trap`, then
`+=`. An `#include` of `<vector>`, `<memory>`, `<algorithm>`, or
`<initializer_list>` is absent. `slice_array`, `gslice_array`, `shift(`,
`cshift(`, and `apply(` are absent. Unary `operator+() const` and
`operator-() const` are absent.

The translation unit was compiled with clang++ 18.1.3, `-std=c++23 -Wall
-Wextra -Werror`, and the doctest main. `__prism_check` is only declared
in the header, so that link used a stub that returns when the condition
holds. 4 test cases, 392 assertions passed. The same binary with
`-fsanitize=address,undefined -fno-sanitize-recover=all -O1` passed the
same 392 assertions. cmake, ninja, and `prism_tests` were not run.

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
add0 0 0
addz 0
add 11 22 34
sub 9 18 26
self 2 4 0 0
edge 2147483646 2147483647 -2147483648 -2147483647 0 2147483647
uadd 4294967295 4294967294
dadd 4.0 -1.0 -1.0 0.0
badd 1 1 0 1
ll 9223372036854775806 -9223372036854775807 0 9223372036854775807
```

`addsz` aborts with `CHECK FUNC-CONTRACT: valarray::operator+ sizes differ`.
`subsz` aborts with `CHECK FUNC-CONTRACT: valarray::operator- sizes differ`.
`addovf` aborts with
`CHECK INT-SIGNED-OVF: valarray::operator+ does not fit in value_type`.
`subovf` aborts with
`CHECK INT-SIGNED-OVF: valarray::operator- does not fit in value_type`.
`uaddovf` and `faddovf` abort with
`CHECK FUNC-CONTRACT: valarray::operator+ does not fit in value_type`.
`usubovf` and `fsubovf` abort with
`CHECK FUNC-CONTRACT: valarray::operator- does not fit in value_type`.
None of them print `partial=` or `wrapped=`. The ASan build does not
report a heap buffer overflow on those paths.

A driver whose `__prism_check` returns on a failing condition dies with
SIGILL on a size mismatch, on `INT_MAX + 1` in the second element, on
`INT_MIN - 1`, on `0u - 1u`, and on `double` max plus max. It does not
print `partial=`, `wrapped=`, or `loaded`. Empty `operator+` and
`operator-` of a type with a deleted default constructor print `empty=0 0`
and do not print `loaded`. UBSan on the SIGILL driver does not report an
overflow before the illegal instruction.

libstdc++ with `_GLIBCXX_ASSERTIONS` aborts on the trace's empty `sum`
(`valarray:954`, `Assertion '_M_size > 0' failed`) before `operator+`
runs. An add-only driver of the same values prints the same `add0`
through `ll` lines against libstdc++. libstdc++ different sizes abort in
`operator+` (`valarray:1196`, `Assertion '__v.size() == __w.size()' failed`).
Without that assertion, libstdc++ prints `partial=2 4`. Without a
sanitizer, libstdc++ prints `partial=2 wrapped=-2147483648` for max plus
one in the second element, `wrapped=2147483647` for `INT_MIN - 1`,
`wrapped=0` for unsigned max plus one, `wrapped=4294967295` for `0u - 1u`,
and `wrapped=inf` for the two floating overflows. The model does not
return those values.

`v + 1`, `1 + v`, `v - 1`, and `1 - v` compile against libstdc++ and do
not compile against this header (`invalid operands to binary expression`).
`std::slice` and `std::gslice` do not compile against this header (`no
member named 'slice'`, `no type named 'gslice'`). Both names compile
against libstdc++.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return
`C++ valarray unencoded: ... is not a valarray model` from the source
text, before a header is inlined. Those files were not edited. Until that
gate is gone, a unit that names `valarray` does not reach this header.
That run is not `PROVED` and not `CLEAN`. `docs/PIR.md` was not edited.
