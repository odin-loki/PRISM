# numeric iota

Branch `finish/cxx-numeric-iota`, base `020c4b6ffaf889df75f6fe7beb103349d38fbfaf`.

`std::iota` is declared on the existing header
`src/prism/pir/models/cxx/numeric`. The header is still one file, not a
directory. The include guard is `_GLIBCXX_NUMERIC`. `#pragma GCC system_header`.
Namespace `std`.

`std::midpoint`, `std::gcd`, and `std::lcm` are unchanged. `gcd(0, 0)` is 0.
Midpoint still takes the distance in the corresponding unsigned type and
applies half of that distance to one argument. It does not form `(a+b)/2`.
`lcm` of a product that does not fit is still `__prism_check` with
`FUNC-CONTRACT` and `__builtin_trap`. That path does not return a wrapped
product and does not return 0 for a nonzero pair.

The solver was not run. This header is not a proof.

## What iota does

`std::iota` is one overload: two pointers of the same type, and a starting
value of that type. A class iterator does not match. A starting value of a
different type does not match. Those calls fail to compile against this
header, and pir falls back to libstdc++.

`last < first` calls

`__prism_check(0, "FUNC-CONTRACT", "std::iota last is before first")`

and then `__builtin_trap`. That path does not store.

`first == last` returns before any store.

Otherwise the range receives `value`, then one more than that value, and so
on, through `[first, last)`. The next value is formed only when another
element remains. If that next value does not fit in `T`, the function calls
`__prism_check` and then `__builtin_trap`. The class is `INT-SIGNED-OVF`
when `T` is a signed integer, and `FUNC-CONTRACT` otherwise. The message is
`std::iota next value does not fit in T`. The sum sits in the `else` of the
fit test, so a wrapped value is not stored.

A one-element range whose value is the maximum of `T` stores that value and
returns. It does not form a further value. `iota` of two `int`s starting at
`INT_MAX` stores `INT_MAX` in the first slot, then takes `INT-SIGNED-OVF`
while the second slot is still the caller's value. `unsigned` maximum is
`FUNC-CONTRACT`, and the second slot is not 0. The same split covers
`long long` and `signed char`. `signed char` maximum is 127.

The limit is the maximum of `T`, formed as an unsigned shift
(`static_cast<make_unsigned_t<T>>(-1) >> 1` for a signed type) so the test
itself does not overflow. The increment after a passing test is
`value + 1` in `T`. A negative signed value is below that limit, so
`INT_MIN`, `INT_MIN + 1`, `INT_MIN + 2` is a normal store.

A non-integer, including `float`, uses `++` and has no separate range check.
`bool` is not on the integral fit path. `++` of `bool` is ill-formed, so
that call fails to compile and pir falls back to libstdc++.

`__prism_check` was already declared
`extern "C" void __prism_check(int, const char*, const char*);`. This slice
does not declare it again and does not define it.

## What is still not declared

Naming one of these fails to compile, and the pir stage lowers that
translation unit again with libstdc++ (`extra.cxx_models`). A partial header
is intentional:

- `accumulate`
- `reduce`
- `transform_reduce`
- `inner_product`
- `adjacent_difference`
- `partial_sum`
- `exclusive_scan`

Floating-point and pointer `midpoint` are not declared. The header does not
include `<iterator>`, `<memory>`, or `<vector>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/numeric`, and `_prism_test_sources` already lists
`tests/cpp/test_cxx_numeric.cpp`. `CMakeLists.txt` was not edited.
`tests/cpp/test_qa_conformance.cpp` was not edited.

No test required `iota` to be absent. The header comment used to list it
with the omitted algorithms. That sentence no longer names `iota`. The new
assertions require the overload.

`src/prism/bmc.cpp`, `src/prism/bmc_unenc.inc`, and
`src/prism/bmc_encoder.inc` were not edited. They still report
`views::iota` / `iota_view` as unencoded. That gate does not name
`std::iota`. `std::gcd`, `std::lcm`, and `std::midpoint` are still
unencoded from the source text, before a header is inlined. `docs/PIR.md`
was not edited.

## Tests

`tests/cpp/test_cxx_numeric.cpp` reads the header. The inverted-range check
and its trap are before the empty return, and that return is before any
store. `INT-SIGNED-OVF` and the unsigned `FUNC-CONTRACT` each trap before
`value + 1`, and that sum is not on the failing arm. The midpoint text
still rejects `a + b` and `(a+b)/2`. `gcd(0, 0)` is still the unentered
loop. `lcm` still traps before returning the product. The header is still
a regular file. `__prism_check` is not defined. The translation unit was
not built with `prism_tests`.

`tests/cxx_models/numeric_trace.cpp` is `#include <numeric>`. Checked with
clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra
-Werror`. Compiled twice (`-isystem` the model directory, and without it).
The dependency dump names the model header. Stdout matched:

```
hi_neg=0 hi_zero=0 hi_eq=1
near_neg=0 near_zero=0 near=2147483646
small=3
gcd00=0
gcd=6
gcd0=15
gcd_neg=2
lcm00=0
lcm0=0
lcm_x0=0
lcm=36
lcm_neg=12
lcm_both=12
lcm_max=2147483647
lcm_ll=4294967294
lcm_u=36
lcm_mix=24
iota=0,1,2,3
iota_empty=7
iota_neg=-1,0,1
iota_min=-2147483648,-2147483647,-2147483646
iota_near=2147483645,2147483646
iota_u=1,2,3
iota_unear=4294967293,4294967294
iota_umax=4294967295
iota_one=41
iota_sc=125,126
iota_uc=254,255
iota_f=0,1,2
```

`argv` `iota-max1` on the model printed `iota_hi=2147483647`.
`iota-ovf` and `iota-llovf` aborted with
`CHECK INT-SIGNED-OVF: std::iota next value does not fit in T`.
`iota-uovf` aborted with
`CHECK FUNC-CONTRACT: std::iota next value does not fit in T`.
`iota-inv` aborted with
`CHECK FUNC-CONTRACT: std::iota last is before first`.
None of those runs printed `wrapped=`.

A driver whose check prints the buffer and then aborts reported
`mem=2147483647,5`, `mem=4294967295,5`, `mem=9223372036854775807,5`,
`mem=127,1` for the signed, unsigned, `long long`, and `signed char`
overflows, and `mem=4,5` for an inverted range. The second slot is the
caller's value. `max1` printed `iota_hi=2147483647`, an empty range left
`7` in place, and `signed char` `126,127` was stored. A driver whose
`__prism_check` returns dies with SIGILL from `__builtin_trap` and prints
no `wrapped=` line.

`constexpr` evaluation against this header accepts one `INT_MAX`, the pair
`INT_MAX - 1` and `INT_MAX`, and an empty range. Two elements starting at
`INT_MAX` or at `UINT_MAX`, and an inverted pair, are not constant
expressions: those calls evaluate `__prism_check`. libstdc++ rejects the
one-element `INT_MAX` constant expression because it increments after the
last store (`2147483648` is outside `int`). In-range values, including
`iota` of `INT_MIN` and of `0.0f`, matched libstdc++.

cmake, ninja, `prism_tests`, and pir were not run.
