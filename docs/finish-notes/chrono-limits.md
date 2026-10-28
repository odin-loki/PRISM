# chrono duration limits and multiply

Branch `finish/cxx-chrono-limits`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`std::chrono` stays one extensionless file, `src/prism/pir/models/cxx/chrono`.
A directory of that name would make `#include <chrono>` fail with EISDIR.
The include guard is still `_GLIBCXX_CHRONO`. `#pragma GCC system_header`.
Namespace `std::chrono`. The file does not `#include <ratio>` and does not
`#include <compare>`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
This header is not a proof. `duration::zero`, `duration::min`,
`duration::max`, and a same-rep product are not PROVED and not CLEAN.

## What zero, min, and max do

`zero()`, `min()`, and `max()` are static constexpr members of
`duration<_Rep, _Period>`. They take no argument, so they do not read
another duration. They do not call `count()` on a duration, do not add,
and do not sleep. They do not call `__prism_check`.

`zero()` returns `duration(static_cast<_Rep>(0))`. The count is 0.

`min()` returns `duration(static_cast<_Rep>(__duration_rep_min))`.
`__duration_rep_min` is `-9223372036854775807LL - 1`, the most negative
signed 64-bit count. It is not 0.

`max()` returns `duration(static_cast<_Rep>(__duration_rep_max))`.
`__duration_rep_max` is `9223372036854775807LL`, the most positive signed
64-bit count. It is not 0.

A default duration equals `zero()`. `min()` is less than `zero()`, and
`zero()` is less than `max()`. `min()` is less than `max()`.

## What scalar multiply does

`operator*` is two function templates. One takes
`const duration<_Rep, _Period>&` and a scalar. The other takes the scalar
first. The scalar type must be that same `_Rep`
(`requires __duration_same_type<_Scalar, _Rep>`). Both return
`__duration_mul(__d, __s)`, so the period stays the duration's period.

`__duration_mul` reads the two counts as `long long`. If either count is
0, it returns `duration<_Rep, _Period>(static_cast<_Rep>(0))` before
`__prism_check` and before any multiply. That zero is the product. It is
not a stand-in for a product that does not fit. Zero times the minimum
count, zero times the maximum count, and either order, are 0 and do not
overflow.

Otherwise `__duration_mul_fits` decides whether the product is
representable. The magnitudes are unsigned. The most negative count is not
negated: its magnitude is `-(v + 1) + 1`. The unsigned product is formed
only after it fits in `unsigned long long`, then that magnitude is compared
with the signed limit. A negative product may be one larger than the
maximum count, which is the minimum count. A positive product may not.

When the product fits, `__prism_check` is called with that success and
class `INT-SIGNED-OVF`, and the `else` returns
`__duration_product`. That value is the unsigned magnitude converted to
the signed count. A negative product is `static_cast<long long>(0ull -
product)`, so the minimum count is not a signed negation. The source does
not contain `__x * __y`.

When the product does not fit, `__prism_check` is called with class
`INT-SIGNED-OVF` and message
`std::chrono::duration multiplication overflows the representation`, then
`__builtin_trap`. That arm has no `return`. It does not return a wrapped
count and does not return a zero duration. `LLONG_MAX * 2`, `2 * LLONG_MAX`,
`LLONG_MIN * -1`, `-1 * LLONG_MIN`, `LLONG_MIN * 2`, and `LLONG_MAX * -2`
take that arm. `LLONG_MAX * 1`, `LLONG_MAX * -1`, `LLONG_MIN * 1`, and
`1 * LLONG_MIN` fit. `4611686018427387903 * 2` is `9223372036854775806`.
`-4611686018427387904 * 2` is the minimum count.

## What stayed

`operator+` and `operator-` are unchanged. Each overflow path still calls
`__prism_check` and then `if (!__ok) return duration<_Rep, _Period>(0);`.
That return appears twice in the header.

`operator+=` and `operator-=` are unchanged. Each one still uses the
matching fit test, calls `__prism_check`, and stores 0 when the sum or
difference does not fit.

`duration_cast` is unchanged. It is still declared only when the source
period and the target period are the same type, this header's
`__duration_period` (`num` 1, `den` 1), and the reps are that same type.
The result is still `_ToDur(__d.count())`.

`operator==`, `operator!=`, `operator<`, `operator>`, `operator<=`, and
`operator>=` are unchanged. They still compare the stored counts of one
duration type.

## What still falls back

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++. They are not stubbed:

- a duration multiplied by another duration, including a different period
  (clang++ reports `invalid operands to binary expression`)
- a scalar whose type is not the duration's rep (`int` on either side is
  `invalid operands`)
- `operator*=` (`no viable overloaded '*='`)
- a comparison of a different period
- a cast to a different period
- clocks, `time_point`, and the calendar typedefs
- `sleep_for` / `sleep_until` (a sleep that returns immediately would be a
  fake wait; `std::chrono::sleep_for` is not a member)

A same-type period stand-in with `den` 1000 still compares the counts, and
a `long long` scalar multiplies those counts. The denominator is not
applied. `1000 * 2` is `2000`.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/chrono` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_chrono.cpp` was
already in `_prism_test_sources`. The conformance header-name array was
not edited. No new test source was added. `tests/cxx_models/chrono_trace.cpp`
is not listed in `CMakeLists.txt`. `src/prism/bmc.cpp` and
`src/prism/pir/stage.cpp` were not edited.

## Tests

`tests/cpp/test_cxx_chrono.cpp` still reads the header as text. The binary
operators are sliced at `operator+(const duration` and
`operator-(const duration`. Each overflow path is still `__prism_check`
and then `if (!__ok) return duration<_Rep, _Period>(0);`. The comparison
slice now ends at the scalar-multiply comment, and those six operators are
unchanged. The new checks require `zero()`, `min()`, and `max()` to be
static, with `min()` and `max()` returning the representation limits and
not 0. That region has no `count()`, no addition, and no
`__prism_check`. The multiply region requires both orders, the same-rep
constraint, a zero return before `__prism_check`, the check before
`__builtin_trap`, and the product return only in the `else`. The trap arm
has no `return`. `sleep_for(` and `sleep_until(` stay absent. `<ratio>`
and `<compare>` stay absent. The header says this is not PROVED and not
CLEAN.

Compiled and run with clang++ 18.1.3 (`-std=c++23 -Wall -Wextra
-Wpedantic -Werror`, doctest 2.4.11 main). The case passed, 203
assertions. `prism_tests` was not run.

`tests/cxx_models/chrono_trace.cpp` includes `<chrono>`. The happy path
still prints `zero=0`, `sum=5`, `cast=2`, `diff=4`, `zero_sum=0`,
`plus_eq_zero=0`, `plus_eq=5`, and `minus_eq=4`, and the comparison lines
from the previous slice. It also prints:

```
lim_zero=0
lim_min=-9223372036854775808
lim_max=9223372036854775807
mul=6
mul_left=6
mul_neg=-6
mul_neg2=6
mul_zero_max=0
mul_zero_min=0
mul_max_zero=0
mul_min_zero=0
mul_left_zero=0
mul_max_one=9223372036854775807
mul_max_neg1=-9223372036854775807
mul_min_one=-9223372036854775808
mul_min_left=-9223372036854775808
mul_near=9223372036854775806
mul_edge=-9223372036854775808
mul_edge_left=-9223372036854775808
```

`static_assert` requires `zero()`, `min()`, and `max()` to be those counts,
and that `min()` is not 0 and `max()` is not 0. Sleep is not called. A
clock is not read.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Wpedantic -Werror`.
The trace binary against this header and the trace binary against
libstdc++ print the same stdout. With `-fsanitize=undefined
-fno-sanitize-recover=undefined`, that same stdout is printed and
UndefinedBehaviorSanitizer stays quiet, on this header and on libstdc++.
A copy of the header compiled without `#pragma GCC system_header` is clean
under the same warnings. With the trace's aborting `__prism_check`, `ovf`
and `plus_eq_ovf` abort on `INT-SIGNED-OVF` for addition, `minus_eq_ovf`
aborts on `INT-SIGNED-OVF` for subtraction, and `mul_ovf`, `mul_left_ovf`,
`mul_min_ovf`, and `mul_min_left_ovf` abort on `INT-SIGNED-OVF` for
multiplication, each before `wrapped=` is printed. A driver whose
`__prism_check` returns still reaches `__builtin_trap` (illegal
instruction) on `LLONG_MAX * 2`, `2 * LLONG_MAX`, `LLONG_MIN * -1`,
`-1 * LLONG_MIN`, `LLONG_MIN * 2`, and `LLONG_MAX * -2`, and prints no
`wrapped=` line. UndefinedBehaviorSanitizer stays quiet on that trap.
The same driver prints `z=0`, the minimum count for
`-4611686018427387904 * 2`, and `-9223372036854775807` for
`LLONG_MAX * -1`.

A separate driver compared the fit test and the formed product with a
128-bit product for the corner counts and 20000 random pairs. Every
fitting pair matched, including zero times either limit. Pairs that do
not fit were not returned. That driver is not a proof.

A different period does not compile (`invalid operands` for `*` and for
`==`). A duration times a duration does not compile. An `int` scalar does
not compile on either side. `operator*=` does not compile. `sleep_for` is
not a member of `std::chrono`. A same-type period stand-in with `den` 1000
compiles: a count of 1000 is greater than a count of 1, `zero()` is 0,
`min()` and `max()` are the signed 64-bit limits, and `1000 * 2LL` is
2000.
