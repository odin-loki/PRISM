# chrono duration division

Branch `finish/cxx-chrono-div`, base `ad41422d133a60b9ffa1d90ea9754896d126de23`.

`std::chrono` stays one extensionless file, `src/prism/pir/models/cxx/chrono`.
A directory of that name would make `#include <chrono>` fail with EISDIR.
The include guard is still `_GLIBCXX_CHRONO`. `#pragma GCC system_header`.
Namespace `std::chrono`. The file does not `#include <ratio>` and does not
`#include <compare>`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
This header is not a proof. Dividing a duration by a scalar is not PROVED
and not CLEAN.

## What operator/ does

`operator/` is a function template. The duration argument is
`const duration<_Rep, _Period>&` and the scalar is `const _Scalar&` with
`_Scalar` the same type as `_Rep`, so the result is that same duration
type. A different scalar type is not an overload. It reads the stored
count and the scalar as `long long`.

A zero divisor sets `__nz` to 0 and calls `__prism_check` with class
`FUNC-CONTRACT` and message
`std::chrono::duration division by zero`, then `__builtin_trap`. That arm
has no `return`, no `/`, and no quotient. It does not return
`duration(0)`. A count of 5 divided by 0, a count of 0 divided by 0, and
the minimum count divided by 0 take this arm. The overflow check is not
reached.

A nonzero divisor calls that same check with `__nz` 1. Dividing the most
negative signed 64-bit count by -1 then sets `__ok` to 0. The comparison
is `__x == __duration_rep_min && __y == -1`. It does not negate the stored
count. `__prism_check` is called with class `INT-SIGNED-OVF` and message
`std::chrono::duration division overflows the representation`, then
`__builtin_trap`. That arm has no `return`, no `-`, and no `/`. It does
not form the negation and does not form a quotient.

Every other quotient is the stored count divided by the scalar,
`duration<_Rep, _Period>(static_cast<_Rep>(__x / __y))`, and only in that
`else`. For this representation every other pair fits. The division
truncates toward zero. A count of 10 divided by 2 is 5. A count of 5
divided by 2 is 2. A count of -5 divided by 2 is -2. A count of 5 divided
by -2 is -2. A count of -5 divided by -2 is 2. A count of 1 divided by 2
is 0, and a count of -1 divided by 2 is 0. A count of 0 divided by 5 is 0,
and a count of 0 divided by -1 is 0. The maximum count divided by 1 is the
maximum count. The maximum count divided by -1 is
`-9223372036854775807`. The minimum count divided by 1 is the minimum
count. The minimum count divided by 2 is `-4611686018427387904`. The
minimum count divided by -2 is `4611686018427387904`. The maximum count
divided by itself is 1. The minimum count divided by itself is 1. The
maximum divided by the minimum is 0. The minimum divided by the maximum
is -1.

`__prism_check` runs for a fitting quotient as well: the zero check with
`__nz` 1, then the overflow check with `__ok` 1. The quotient is only in
the final `else`, after both calls. Each message occurs once in the header.

## What stayed

`operator+` and binary `operator-` are unchanged. Each overflow path still
calls `__prism_check` and then
`if (!__ok) return duration<_Rep, _Period>(0);`. That return appears twice
in the header, on those two operators.

Unary `operator-` is unchanged. Negating the minimum count is still
`__prism_check` then `__builtin_trap`, and every other count is still
`duration(-count)`.

`operator+=` and `operator-=` are unchanged. Each one still uses the
matching fit test, calls `__prism_check`, and stores 0 when the sum or
difference does not fit.

`operator*` is unchanged. A product that does not fit is still
`__prism_check` then `__builtin_trap`. Zero times any count is still zero,
returned before that check. `operator*=` is still not declared.

`operator==`, `operator!=`, `operator<`, `operator>`, `operator<=`, and
`operator>=` are unchanged. They still compare the stored counts of one
duration type.

`duration_cast` is unchanged. It is still declared only when the source
period and the target period are the same type, this header's
`__duration_period` (`num` 1, `den` 1), and the reps are that same type.
The result is still `_ToDur(__d.count())`.

`duration::zero`, `duration::min`, and `duration::max` are unchanged.
`min()` is still the most negative count and `max()` is still the most
positive count.

## What still falls back

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++. They are not stubbed:

- `operator/=` (`no viable overloaded '/='`)
- `operator*=` (`no viable overloaded '*='`)
- a scalar whose type is not the duration's rep (`invalid operands to
  binary expression` for `duration<long long>` and `int`)
- a duration divided by another duration (same diagnostic, both operands
  durations)
- a scalar divided by a duration (`long long` and `duration<long long>`)
- a comparison of a different period
- a cast to a different period
- clocks, `time_point`, and the calendar typedefs
- `sleep_for` / `sleep_until` (a sleep that returns immediately would be a
  fake wait; `std::chrono::sleep_for` is not a member)

`operator/` is the same template for every period. A same-type period
stand-in with `den` 1000 still compares the counts, and dividing a count
of 1000 by 2 yields 500. The denominator is not applied.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/chrono` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_chrono.cpp` was
already in `_prism_test_sources`. The conformance header-name array was
not edited. No new test source was added, so no `_prism_test_sources` line
was added or removed. `tests/cxx_models/chrono_trace.cpp` is not listed in
`CMakeLists.txt`. `src/prism/bmc.cpp` and `src/prism/pir/stage.cpp` were
not edited.

## Tests

`tests/cpp/test_cxx_chrono.cpp` still reads the header as text. The binary
operators are sliced at `operator+(const duration` and
`operator-(const duration`. Each overflow path is still `__prism_check`
and then `if (!__ok) return duration<_Rep, _Period>(0);`. The unary
negation slice now ends at the division comment, and that region is
unchanged. The new checks require one
`operator/(const duration<_Rep, _Period>& __d, const _Scalar& __s)`,
both messages, the zero check before the zero trap, the overflow check
before the overflow trap, and `__x / __y` only after both traps. The zero
arm has no `return`, no `duration(0)`, and no `/`. The overflow arm has
no `return`, no `-`, and no `/`. `sleep_for(` and `sleep_until(` stay
absent. `<ratio>` and `<compare>` stay absent. `operator*=(` and
`operator/=(` stay absent. The header says this is not PROVED and not
CLEAN. That sentence appears eight times.

Compiled and run with clang++ 18.1.3 (`-std=c++23 -Wall -Wextra
-Wpedantic -Werror`, doctest 2.4.11). The case passed, 309 assertions.
`prism_tests` was not run.

`tests/cxx_models/chrono_trace.cpp` includes `<chrono>`. The happy path
still prints the previous lines, and now also:

```
div=5
div_trunc=2
div_neg=-2
div_neg_s=-2
div_neg2=2
div_zero_count=0
div_zero_neg1=0
div_max_one=9223372036854775807
div_max_neg1=-9223372036854775807
div_min_one=-9223372036854775808
div_min_two=-4611686018427387904
div_min_neg2=4611686018427387904
div_one_two=0
div_m1_two=0
div_max_max=1
div_min_min=1
div_max_min=0
div_min_max=-1
```

`div_zero` divides a count of 5 by 0. `div_min_zero` divides the minimum
count by 0. `div_ovf` divides the minimum count by -1. Sleep is not
called. A clock is not read.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Wpedantic -Werror`,
at `-O0` and `-O2`. The trace binary against this header and the trace
binary against libstdc++ print the same stdout. With
`-fsanitize=undefined -fno-sanitize-recover=undefined`, that same stdout
is printed and UndefinedBehaviorSanitizer stays quiet, on this header and
on libstdc++. A copy of the header compiled without
`#pragma GCC system_header` is clean under the same warnings, and the
trace binary against that copy prints the same stdout.

With the trace's aborting `__prism_check`, `div_zero` and `div_min_zero`
abort on `FUNC-CONTRACT` for division by zero before `wrapped=` is
printed. `div_ovf` aborts on `INT-SIGNED-OVF` for division before
`wrapped=` is printed. `neg_ovf` still aborts on the negation message, and
`ovf` still aborts on the addition message.

A driver whose `__prism_check` returns still reaches `__builtin_trap`
(illegal instruction, exit 132) for a count of 5 divided by 0, a count of
0 divided by 0, `duration::min()` divided by 0, and `duration::min()`
divided by -1. None of those print a `quot=` line. The zero cases print
only `CHECK FUNC-CONTRACT: std::chrono::duration division by zero ok=0`.
The minimum count divided by -1 prints that check with `ok=1` and then
`CHECK INT-SIGNED-OVF: std::chrono::duration division overflows the representation ok=0`.
UndefinedBehaviorSanitizer stays quiet on those traps, at `-O0` and
`-O2`. The same driver prints `q=5`, `neg=-2`,
`-9223372036854775807` for the maximum count divided by -1,
`-9223372036854775808` for the minimum count divided by 1,
`4611686018427387904` for the minimum count divided by -2, and `z=0`
for a count of 0 divided by -1. Each of those calls both checks with
`ok=1`.

`operator*=` does not compile (`no viable overloaded '*='`).
`operator/=` does not compile (`no viable overloaded '/='`).
`sleep_for` is not a member of `std::chrono`. A scalar of type `int`
does not compile (`invalid operands to binary expression`). A duration
divided by a duration does not compile. A scalar divided by a duration
does not compile. A same-type period stand-in with `den` 1000 compiles:
a count of 1000 is greater than a count of 1, and dividing that count by
2 yields 500. A count of 5 divided by 2 yields 2. The minimum count
divided by -2 yields `4611686018427387904`. The maximum count divided by
-1 yields `-9223372036854775807`.
