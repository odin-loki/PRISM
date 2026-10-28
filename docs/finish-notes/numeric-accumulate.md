# numeric accumulate

Branch `finish/cxx-numeric-accumulate`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`std::accumulate` is declared on the existing header
`src/prism/pir/models/cxx/numeric`. The header is still one file, not a
directory. The include guard is `_GLIBCXX_NUMERIC`. `#pragma GCC system_header`.
Namespace `std`.

`std::midpoint`, `std::gcd`, `std::lcm`, and `std::iota` are unchanged.
`gcd(0, 0)` is 0. Midpoint still takes the distance in the corresponding
unsigned type and applies half of that distance to one argument. It does
not form `(a+b)/2`. `lcm` of a product that does not fit is still
`__prism_check` with `FUNC-CONTRACT` and `__builtin_trap`. That path does
not return a wrapped product and does not return 0 for a nonzero pair.
`iota` still stores `value`, `value + 1`, and so on, and still traps before
storing a wrapped next value.

The solver was not run. This header is not a proof. The result is not
`PROVED` and not `CLEAN`.

## What accumulate does

`std::accumulate` is one overload: two pointers of the same type, and an
initial value of that type. A class iterator does not match. An initial
value of a different type does not match. Those calls fail to compile
against this header, and pir falls back to libstdc++.

`last < first` calls

`__prism_check(0, "FUNC-CONTRACT", "std::accumulate last is before first")`

and then `__builtin_trap`. That path does not read and does not return
init.

`first == last` returns init before any read.

Otherwise the result is init plus each element of `[first, last)`, in
order. The next sum is formed only when another element remains. If that
sum does not fit in `T`, the function calls `__prism_check` and then
`__builtin_trap`. The class is `INT-SIGNED-OVF` when `T` is a signed
integer, and `FUNC-CONTRACT` otherwise. The message is
`std::accumulate sum does not fit in T`. The sum sits in the `else` of the
fit test, so a wrapped sum is not returned.

The signed maximum is `static_cast<make_unsigned_t<T>>(-1) >> 1`, the same
shift `iota` uses. The minimum is `-1` minus that maximum, which is
representable, so the test itself does not overflow. A positive element is
compared with `max - element` only when the element is positive. A negative
element is compared with `min - element` only when the element is negative.
Zero always fits, and the sum is still formed in the `else`. The unsigned
limit is the maximum value of `T`. `init > max - element` is the failing
test, and that subtraction stays in range.

`0 + INT_MAX` and `INT_MAX + 0` fit and return `INT_MAX`. `INT_MAX + 1` and
`INT_MIN + -1` take `INT-SIGNED-OVF`. The same split covers `long long` and
`signed char`. `signed char` maximum is 127. A range `{INT_MAX, 1}` with
init 0 forms `INT_MAX` for the first element, then traps on the second and
does not return a wrapped total. `unsigned` maximum plus one is
`FUNC-CONTRACT`, and the result is not 0.

A non-integer, including `float`, adds with `+` and has no separate range
check. `bool` is not on the integral fit path. `false` plus `{true, true}`
is `true`, matching libstdc++ after the usual arithmetic conversions.

`__prism_check` was already declared
`extern "C" void __prism_check(int, const char*, const char*);`. This slice
does not declare it again and does not define it.

## What is still not declared

Naming one of these fails to compile, and the pir stage lowers that
translation unit again with libstdc++ (`extra.cxx_models`). A partial header
is intentional:

- `reduce`
- `transform_reduce`
- `inner_product`
- `adjacent_difference`
- `partial_sum`
- `exclusive_scan`

Floating-point and pointer `midpoint` are not declared. The header does not
include `<iterator>`, `<memory>`, `<vector>`, or `<algorithm>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/numeric`, and `_prism_test_sources` already lists
`tests/cpp/test_cxx_numeric.cpp`. `CMakeLists.txt` was not edited.
`tests/cpp/test_qa_conformance.cpp` was not edited.

No test required `accumulate` to be absent. The header comment used to list
it with the omitted algorithms. That sentence no longer names `accumulate`.
The new assertions require the overload.

`src/prism/bmc.cpp`, `src/prism/bmc_unenc.inc`, and
`src/prism/bmc_encoder.inc` were not edited. They do not name
`std::accumulate`. `docs/PIR.md` was not edited.

## Tests

`tests/cpp/test_cxx_numeric.cpp` reads the header. The inverted-range check
and its trap are before the empty return, and that return is before any
`*__first`. The failing arm does not read, does not add, and does not
return. `INT-SIGNED-OVF` and the unsigned `FUNC-CONTRACT` each trap before
`init + element`, that sum is in the `else`, and it is not on the failing
arm. The pointer advances only after that sum. The midpoint text still
rejects `a + b` and `(a+b)/2`. `gcd(0, 0)` is still the unentered loop.
`lcm` still traps before returning the product. `iota` is still declared.
`reduce`, `transform_reduce`, `inner_product`, `adjacent_difference`,
`partial_sum`, and `exclusive_scan` are still not declared. The header is
still a regular file. `__prism_check` is not defined. The translation unit
was not built with `prism_tests`.

`tests/cxx_models/numeric_trace.cpp` is `#include <numeric>`. Checked with
clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra
-Werror`. Compiled twice (`-isystem` the model directory, and without it).
Stdout matched:

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
acc=10
acc_empty=3,7
acc_neg=0
acc_min=-2147483646
acc_near=2147483647
acc_hi=2147483647
acc_imin=-2147483648
acc_u=6
acc_unear=4294967295
acc_umax=4294967295
acc_uzero=4294967295
acc_one=41
acc_sc=127
acc_uc=255
acc_f=2
acc_b=1
```

`argv` `acc-max1` on the model printed `acc_hi=2147483647`.
`acc-ovf`, `acc-llovf`, `acc-minovf`, `acc-sc`, and `acc-part` aborted with
`CHECK INT-SIGNED-OVF: std::accumulate sum does not fit in T`.
`acc-uovf` aborted with
`CHECK FUNC-CONTRACT: std::accumulate sum does not fit in T`.
`acc-inv` aborted with
`CHECK FUNC-CONTRACT: std::accumulate last is before first`.
None of those runs printed `wrapped=`.

A driver whose `__prism_check` returns dies with SIGILL from
`__builtin_trap` and prints no `wrapped=` line. `INT_MAX + 1` took that
path.

`constexpr` evaluation against this header accepts `1+2+3+4`, an empty
range, one `INT_MAX` added to 0, the pair that reaches `INT_MAX` from
`INT_MAX - 2`, and `0 + INT_MIN`. `INT_MAX + 1`, `INT_MIN + -1`,
`UINT_MAX + 1`, and an inverted pair are not constant expressions: those
calls evaluate `__prism_check`. In-range values matched libstdc++.

cmake, ninja, `prism_tests`, and pir were not run.
