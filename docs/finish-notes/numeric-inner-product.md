# numeric inner_product

Branch `finish/cxx-numeric-inner`, base `fe245b58e5fb36a4f551345e3f66ef6d8a0a7d9f`.

`std::inner_product` is declared on the existing header
`src/prism/pir/models/cxx/numeric`. The header is still one file, not a
directory. The include guard is `_GLIBCXX_NUMERIC`. `#pragma GCC system_header`.
Namespace `std`.

`std::midpoint`, `std::gcd`, `std::lcm`, `std::iota`, `std::accumulate`,
`std::adjacent_difference`, and `std::partial_sum` are unchanged.
`gcd(0, 0)` is 0. Midpoint still takes the distance in the corresponding
unsigned type and applies half of that distance to one argument. It does
not form `(a+b)/2`. `lcm` of a product that does not fit is still
`__prism_check` with `FUNC-CONTRACT` and `__builtin_trap`. That path does
not return a wrapped product and does not return 0 for a nonzero pair.
`iota` still stores `value`, `value + 1`, and so on, and still traps before
storing a wrapped next value. `accumulate` still returns init plus each
element, and still traps before returning a wrapped sum.
`adjacent_difference` still copies the first element and stores each later
difference, and still traps before storing a wrapped difference.
`partial_sum` still copies the first element and stores each later prefix
sum, and still traps before storing a wrapped sum.

The solver was not run. This header is not a proof. The result is not
`PROVED` and not `CLEAN`.

## What inner_product does

`std::inner_product` is one overload: two pointers of the same type, a
second pointer of that type, and an initial value of that type. A class
iterator does not match. Pointers of different types do not match. An
initial value of a different type does not match. Those calls fail to
compile against this header, and pir falls back to libstdc++. There is no
binary-operation overload.

`last1 < first1` calls

`__prism_check(0, "FUNC-CONTRACT", "std::inner_product last is before first")`

and then `__builtin_trap`. That path does not read.

`first1 == last1` returns init before any read. An empty range whose second
pointer is null takes that return.

A non-empty range with a null second pointer calls

`__prism_check(0, "PTR-NULL-DEREF", "std::inner_product null second range")`

and then `__builtin_trap`, before any read.

Otherwise each step multiplies one pair and adds that product to the
accumulator. A product or a sum that does not fit calls `__prism_check`
and then `__builtin_trap`, before a wrapped value is stored. The class is
`INT-SIGNED-OVF` when `T` is a signed integer, and `FUNC-CONTRACT` when
`T` is an unsigned integer. The message is
`std::inner_product result does not fit in T`. The product sits in the
`else` of the product test, and the sum sits in the `else` of the sum
test.

The signed maximum is `static_cast<make_unsigned_t<T>>(-1) >> 1`, the same
shift `iota` uses. The minimum is `-1` minus that maximum, which is
representable, so the test itself does not overflow. The product test
divides only by a nonzero value, and it does not divide the signed minimum
by `-1`. `INT_MIN * -1` and `INT_MAX * 2` take `INT-SIGNED-OVF`.
`INT_MAX * 1` with init 0 fits and returns `INT_MAX`. `INT_MAX * 1` with
init `INT_MAX` fits as a product and then takes `INT-SIGNED-OVF` on the
sum. `INT_MIN * 1` returns `INT_MIN`. `46340 * 46340` fits and is
`2147395600`. The same split covers `long long` and `signed char`.
`signed char` `16 * 8` is `INT-SIGNED-OVF`. A range `{2, INT_MAX}` times
`{3, 2}` forms the first product `6`, then traps on the second product,
and does not return a wrapped total.

`unsigned` `UINT_MAX * 2` and `UINT_MAX + 1` are `FUNC-CONTRACT`. The
result is not a wrapped value. `unsigned char` `16 * 16` is the same
class. `UINT_MAX * 1` and init `UINT_MAX` plus a zero product fit.

`bool` and a non-integer, including `float`, use the operators and have
no separate range check. `bool` is not on the integral fit path.
`{true, true, false}` times `{true, false, true}` with init `false` is
`true`. `{false, false}` times `{true, true}` with init `false` is
`false`. `true` times `true` added to init `true` is `true`.

`__prism_check` was already declared
`extern "C" void __prism_check(int, const char*, const char*);`. This slice
does not declare it again and does not define it.

## What is still not declared

Naming one of these fails to compile, and the pir stage lowers that
translation unit again with libstdc++ (`extra.cxx_models`). A partial header
is intentional:

- `reduce`
- `transform_reduce`
- `exclusive_scan`

Floating-point and pointer `midpoint` are not declared. There is no
binary-operation overload. The header does not include `<iterator>`,
`<memory>`, `<vector>`, or `<algorithm>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/numeric`. `CMakeLists.txt` gained one line in
`_prism_test_sources`: `tests/cpp/test_cxx_numeric_inner_product.cpp`, next
to `tests/cpp/test_cxx_numeric_partial_sum.cpp`. No existing line was
removed. `tests/cxx_models/numeric_trace.cpp` is not listed in
`CMakeLists.txt`. `tests/cpp/test_qa_conformance.cpp` was not edited.

The accumulate test, the adjacent_difference test, and the partial_sum
test used to require that the header text not declare `inner_product`.
Those assertions are gone. The new test requires the overload. `reduce`,
`transform_reduce`, and `exclusive_scan` are still not declared.

`src/prism/bmc.cpp`, `src/prism/bmc_unenc.inc`, and
`src/prism/bmc_encoder.inc` were not edited. They do not name
`std::inner_product`. `docs/PIR.md` was not edited.

## Tests

`tests/cpp/test_cxx_numeric_inner_product.cpp` reads the header. The
inverted-range check and its trap are before the empty return, and that
return is before the null check. The null check and its trap are before
any `*__first1` or `*__first2`. The failing arms do not read, do not
multiply, do not add, and do not return. Each `INT-SIGNED-OVF` traps
before the matching product or sum, that operation is in the `else`, and
it is not on the failing arm. Each unsigned `FUNC-CONTRACT` does the same.
A third product and sum, with no fit test, is the `bool` and non-integer
path. The midpoint text still rejects `a + b` and `(a+b)/2`. `gcd(0, 0)`
is still the unentered loop. `iota`, `accumulate`, `adjacent_difference`,
and `partial_sum` are still declared. The header is still a regular file.
`__prism_check` is not defined. The translation unit was not built with
`prism_tests`.

`tests/cxx_models/numeric_trace.cpp` is `#include <numeric>`. Checked with
clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -Wall -Wextra
-Werror`. Compiled twice (`-isystem` the model directory, and without it).
The dependency dump names the model header. Stdout matched. The new lines
were:

```
ip=10
ip_dot=14
ip_init=33
ip_empty=3,7
ip_empty_null=3
ip_neg=-32
ip_hi=2147483647
ip_zero=2147483647
ip_imin=-2147483648
ip_neg1=-2147483647
ip_min0=-2147483648
ip_near=2147483647
ip_big=2147395600
ip_sc=55
ip_scmin=-128
ip_scneg=-127
ip_uc=55
ip_ucmax=255
ip_u=32
ip_unear=4294967295
ip_umax=4294967295
ip_uzero=4294967295
ip_f=20
ip_b=1
ip_bf=0
ip_bt=1
ip_ll=32
```

`argv` `ip-max1` on the model printed `ip_hi=2147483647`.
`ip-ovf`, `ip-sum`, `ip-llovf`, `ip-minovf`, `ip-minsum`, `ip-sc`, and
`ip-part` aborted with
`CHECK INT-SIGNED-OVF: std::inner_product result does not fit in T`.
`ip-uovf`, `ip-usum`, and `ip-ucovf` aborted with
`CHECK FUNC-CONTRACT: std::inner_product result does not fit in T`.
`ip-inv` aborted with
`CHECK FUNC-CONTRACT: std::inner_product last is before first`.
`ip-null` aborted with
`CHECK PTR-NULL-DEREF: std::inner_product null second range`.
None of those runs printed `wrapped=`.

A driver whose `__prism_check` prints the inputs and then aborts reported
`mem=2147483647,5,2,6` for `INT_MAX * 2`, `mem=2,2147483647,3,2` for
`{2, INT_MAX}` times `{3, 2}`, `mem=1,5,1,6` for init `INT_MAX` plus
`1 * 1`, and `mem=4,5,6,7` for an inverted range. The inputs stay the
caller's values. `max1` printed `ip_hi=2147483647`. An empty range with a
null second pointer printed `empty=3,7`. A driver whose `__prism_check`
returns dies with SIGILL from `__builtin_trap` and prints no `wrapped=`
line.

`constexpr` evaluation against this header accepts `1,2,3,4` times
`1,1,1,1` as `10`, an empty range whose second pointer is null, one
`INT_MAX`, `INT_MIN * 1`, `{false, false}` times `{true, true}`, and
`46340 * 46340`. The same values are constant expressions under libstdc++.
`INT_MAX * 2` is not a constant expression on this header or in libstdc++.
`INT_MAX + 1`, `{UINT_MAX, 2u}`, an inverted pair, and a null second
pointer on a non-empty range are not constant expressions on this header:
those calls evaluate `__prism_check`. `int*` times `long*`, and an `int`
range with a `long` init, do not match this overload. libstdc++ accepts
those two calls. In-range values, including `bool` and `float`, matched
libstdc++.

cmake, ninja, `prism_tests`, and pir were not run.
