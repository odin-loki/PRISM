# numeric adjacent

Branch `finish/cxx-numeric-adjacent`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`std::adjacent_difference` is declared on the existing header
`src/prism/pir/models/cxx/numeric`. The header is still one file, not a
directory. The include guard is `_GLIBCXX_NUMERIC`. `#pragma GCC system_header`.
Namespace `std`.

`std::midpoint`, `std::gcd`, `std::lcm`, `std::iota`, and `std::accumulate`
are unchanged. `gcd(0, 0)` is 0. Midpoint still takes the distance in the
corresponding unsigned type and applies half of that distance to one
argument. It does not form `(a+b)/2`. `lcm` of a product that does not fit
is still `__prism_check` with `FUNC-CONTRACT` and `__builtin_trap`. That
path does not return a wrapped product and does not return 0 for a nonzero
pair. `iota` still stores `value`, `value + 1`, and so on, and still traps
before storing a wrapped next value. `accumulate` still returns init plus
each element, and still traps before returning a wrapped sum.

The solver was not run. This header is not a proof. The result is not
`PROVED` and not `CLEAN`.

## What adjacent_difference does

`std::adjacent_difference` is one overload: two pointers of the same type,
and a destination pointer of that type. A class iterator does not match.
Pointers of different types do not match. Those calls fail to compile
against this header, and pir falls back to libstdc++.

`last < first` calls

`__prism_check(0, "FUNC-CONTRACT", "std::adjacent_difference last is before first")`

and then `__builtin_trap`. That path does not read and does not write.

`first == last` returns dest before any read or write. An empty range whose
dest is null takes that return.

A non-empty range with a null dest calls

`__prism_check(0, "PTR-NULL-DEREF", "std::adjacent_difference null destination")`

and then `__builtin_trap`. That path does not write.

If dest lies in `[first, last)`, the function calls

`__prism_check(0, "FUNC-CONTRACT", "std::adjacent_difference dest lies in [first, last)")`

and then `__builtin_trap`, before any write. The scan uses pointer equality.
A destination in another array compares unequal, so the compare is
specified, including in a constant expression. An overlapping write is not
modelled. `dest == last` is outside `[first, last)`. That call writes
forward from last.

Otherwise the first element is copied to dest. Each later element stores
the difference from the previous input element. The returned pointer is
dest plus the number of elements. An empty range returns the original dest.

A difference that does not fit calls `__prism_check` and then
`__builtin_trap`. The class is `INT-SIGNED-OVF` when `T` is a signed
integer, and `FUNC-CONTRACT` when `T` is an unsigned integer. The message
is `std::adjacent_difference difference does not fit in T`. The subtraction
sits in the `else` of the fit test, so a wrapped difference is not stored.
Elements that already fit stay stored. The failing slot stays the caller's
value.

The signed maximum is `static_cast<make_unsigned_t<T>>(-1) >> 1`, the same
shift `iota` uses. The minimum is `-1` minus that maximum, which is
representable, so the test itself does not overflow. Maximum plus a
negative previous value, and minimum plus a positive previous value, are
the only additions in the test. A previous value of zero always fits, and
the difference is still formed in the `else`. The unsigned test is
`current < previous`. That comparison is not the subtraction.

`INT_MAX - (-1)` and `INT_MIN - 1` take `INT-SIGNED-OVF`. The same split
covers `long long` and `signed char`. `signed char` maximum is 127. A range
`{5, 8, INT_MIN}` stores `5` and then `3`, then traps, and the third slot
stays the caller's value. `unsigned` `0 - 1` is `FUNC-CONTRACT`, and the
second slot is not the wrapped maximum.

`bool` and a non-integer, including `float`, subtract with `operator-` and
have no separate range check. `bool` is not on the integral fit path.
`{false, true, true}` stores `{false, true, false}`.

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
- `partial_sum`
- `exclusive_scan`

Floating-point and pointer `midpoint` are not declared. There is no
binary-operation overload. The header does not include `<iterator>`,
`<memory>`, `<vector>`, or `<algorithm>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/numeric`, and `_prism_test_sources` already lists
`tests/cpp/test_cxx_numeric.cpp`. No new `tests/cpp` file was added, so
`CMakeLists.txt` was not edited. `tests/cxx_models/numeric_trace.cpp` is not
listed in `CMakeLists.txt`. `tests/cpp/test_qa_conformance.cpp` was not
edited.

The accumulate test used to require that the header text not declare
`adjacent_difference`. That assertion is gone. The new assertions require
the overload. `reduce`, `transform_reduce`, `inner_product`, `partial_sum`,
and `exclusive_scan` are still not declared.

`src/prism/bmc.cpp`, `src/prism/bmc_unenc.inc`, and
`src/prism/bmc_encoder.inc` were not edited. They do not name
`std::adjacent_difference`. `docs/PIR.md` was not edited.

## Tests

`tests/cpp/test_cxx_numeric.cpp` reads the header. The inverted-range check
and its trap are before the empty return, and that return is before the
null check. The null check and its trap are before the overlap check. The
overlap trap is before any `*__first` or `*__dest`. The failing arms do not
read, do not write, and do not return. `INT-SIGNED-OVF` and the unsigned
`FUNC-CONTRACT` each trap before `__elem - __prev`, that subtraction is in
the `else`, and it is not on the failing arm. A third subtraction, with no
fit test, is the `bool` and non-integer path. The midpoint text still
rejects `a + b` and `(a+b)/2`. `gcd(0, 0)` is still the unentered loop.
`lcm` still traps before returning the product. `iota` and `accumulate` are
still declared. The header is still a regular file. `__prism_check` is not
defined. The translation unit was not built with `prism_tests`.

`tests/cxx_models/numeric_trace.cpp` is `#include <numeric>`. Checked with
clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1 -Wall -Wextra
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
adj=3,2,4,3 end=4
adj_empty=7 end=1
adj_neg=-1,1,1
adj_min=-2147483648,1,1
adj_near=2147483645,1,1
adj_imax=0,2147483647
adj_imin=0,-2147483648
adj_same=2147483647,0
adj_minsame=-2147483648,0
adj_eq=4,0,0
adj_u=1,1,2
adj_unear=4294967293,1,1
adj_umax=4294967295
adj_usame=4294967295,0
adj_one=41
adj_hi=2147483647
adj_sc=126,1
adj_scn=2,-3
adj_uc=254,1
adj_f=0,1,2
adj_b=0,1,0
adj_tail=8,3,8,-5 end=2
adj_empty_null=1
adj_ll=1,2
```

`argv` `adj-max1` on the model printed `adj_hi=2147483647`.
`adj-ovf`, `adj-llovf`, `adj-minovf`, `adj-sc`, and `adj-part` aborted with
`CHECK INT-SIGNED-OVF: std::adjacent_difference difference does not fit in T`.
`adj-uovf` aborted with
`CHECK FUNC-CONTRACT: std::adjacent_difference difference does not fit in T`.
`adj-inv` aborted with
`CHECK FUNC-CONTRACT: std::adjacent_difference last is before first`.
`adj-null` aborted with
`CHECK PTR-NULL-DEREF: std::adjacent_difference null destination`.
`adj-over` aborted with
`CHECK FUNC-CONTRACT: std::adjacent_difference dest lies in [first, last)`.
None of those runs printed `wrapped=`.

A driver whose `__prism_check` prints the destination and then aborts
reported `mem=-1,5,6` for `INT_MAX - (-1)`, `mem=1,5` for unsigned `0 - 1`,
`mem=-1,5` for `signed char`, `mem=5,3,6` for `{5, 8, INT_MIN}`, `mem=4,5,6`
for an inverted range, and `mem=3,5,9` for a destination at `first + 1`.
The failing slot is the caller's value. `max1` printed
`adj_hi=2147483647 end=1`. An empty range left `7` in place and returned
the original dest. A driver whose `__prism_check` returns dies with SIGILL
from `__builtin_trap` and prints no `wrapped=` line.

`constexpr` evaluation against this header accepts `3,5,9,12` stored as
`3,2,4,3`, an empty range, one `INT_MAX`, and `0` followed by `INT_MIN`.
The same four are constant expressions under libstdc++. `INT_MAX - (-1)`,
`{1u, 0u}`, an inverted pair, a null dest, and a dest inside the input
range are not constant expressions on this header: those calls evaluate
`__prism_check`. libstdc++ also rejects the signed overflow in a constant
expression (`2147483648` is outside `int`). `{1u, 0u}` is a constant
expression in libstdc++: the difference `0u - 1u` wraps. In-range values,
including `bool` and `0.0f`, matched libstdc++.

cmake, ninja, `prism_tests`, and pir were not run.
