# chrono duration abs

Branch `finish/cxx-chrono-abs`, base `d481207ffc8c452d59399ea19bc752c2f4a2bc26`.

`std::chrono` stays one extensionless file, `src/prism/pir/models/cxx/chrono`.
A directory of that name would make `#include <chrono>` fail with EISDIR.
The include guard is still `_GLIBCXX_CHRONO`. `#pragma GCC system_header`.
Namespace `std::chrono`. The file does not `#include <ratio>` and does not
`#include <compare>`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
This header is not a proof. `std::chrono::abs` of a duration is not PROVED
and not CLEAN.

## What abs does

`abs` is a function template. The argument is
`const duration<_Rep, _Period>&`, so the result is that same duration
type. It reads the stored count as `long long`. A `long long` is not a
duration, so `std::chrono::abs(1LL)` is not an overload.

The most negative signed 64-bit count is `__duration_rep_min`
(`-9223372036854775807LL - 1`). That count does not have a positive
counterpart in the representation. `__ok` is 0 when
`__x == __duration_rep_min`, and 1 otherwise. That comparison does not
negate the stored count. `__prism_check` is called with class
`INT-SIGNED-OVF` and message
`std::chrono::duration abs overflows the representation`, then
`__builtin_trap`. That arm has no `return` and no `-`. It does not return
`duration(0)` and it does not form the negation. The locked overflow return
`if (!__ok) return duration<_Rep, _Period>(0);` is not on this path.

A negative count that is not the minimum takes `else if (__x < 0)` and
returns `duration<_Rep, _Period>(static_cast<_Rep>(-__x))`. That is the
negation. The minimum count is not on this path. A count of -5 yields 5.
A count of -1 yields 1. One more than the minimum count,
`-9223372036854775807`, yields `9223372036854775807`.

Zero and every positive count take the remaining `else` and return
`duration<_Rep, _Period>(static_cast<_Rep>(__x))`, a duration with the
same count. Zero stays zero. A count of 5 stays 5. A count of 1 stays 1.
The maximum count stays `9223372036854775807`.

`__prism_check` runs for a fitting count as well, with `__ok` 1. The
negation and the same-count return are only after that call. The message
occurs once in the header. The negation message is not used here.

## What stayed

`operator+` and binary `operator-` are unchanged. Each overflow path still
calls `__prism_check` and then
`if (!__ok) return duration<_Rep, _Period>(0);`. That return appears twice
in the header, on those two operators.

Unary `operator-` is unchanged. Negating the minimum count is still
`__prism_check` then `__builtin_trap`, and every other count is still
`duration(-count)`.

`operator/` is unchanged. A zero divisor is still `__prism_check` then
`__builtin_trap`, before any quotient. The minimum count divided by -1 is
still `__prism_check` then `__builtin_trap`. Every other quotient is still
the stored count divided by the scalar. `operator/=` is still not declared.

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
- `std::chrono::abs` of a `long long` (`no matching function for call to
  'abs'`)
- a scalar whose type is not the duration's rep (`invalid operands to
  binary expression` for `duration<long long>` and `int`, on `operator/`
  and `operator*`)
- a duration divided by another duration
- a comparison of a different period
- a cast to a different period
- clocks, `time_point`, and the calendar typedefs
- `sleep_for` / `sleep_until` (a sleep that returns immediately would be a
  fake wait; `std::chrono::sleep_for` is not a member)

`abs` is the same template for every period. A same-type period stand-in
with `den` 1000 still compares the counts. The absolute value of a count
of 1000 is 1000, of -1000 is 1000, and of 0 is 0. Dividing a count of 1000
by 2 still yields 500, and negating that count still yields -1000. The
denominator is not applied.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/chrono` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_chrono.cpp` was
already in `_prism_test_sources`. The conformance header-name array was not
edited. No new test source was added, so no `_prism_test_sources` line was
added or removed. `tests/cxx_models/chrono_trace.cpp` is not listed in
`CMakeLists.txt`.

## Tests

`tests/cpp/test_cxx_chrono.cpp` still reads the header as text. The binary
operators are sliced at `operator+(const duration` and
`operator-(const duration`. Each overflow path is still `__prism_check`
and then `if (!__ok) return duration<_Rep, _Period>(0);`. The division
slice now ends at the abs comment, and that region is unchanged. The new
checks require one
`abs(const duration<_Rep, _Period>& __d)`, the abs message,
`__prism_check` before `__builtin_trap`, the negated count only in
`else if (__x < 0)`, and the same count only in the following `else`.
The trap arm has no `return`, no `duration(0)`, and no `-`. `sleep_for(`
and `sleep_until(` stay absent. `<ratio>` and `<compare>` stay absent.
`operator*=(` and `operator/=(` stay absent. The header says this is not
PROVED and not CLEAN. That sentence appears ten times.

Compiled and run with clang++ 18.1.3 (`-std=c++23 -Wall -Wextra
-Wpedantic -Werror`, doctest 2.4.11). The case passed, 370 assertions.
`prism_tests` was not run.

`tests/cxx_models/chrono_trace.cpp` includes `<chrono>`. The happy path
still prints the previous lines, and now also:

```
abs_zero=0
abs_pos=5
abs_neg=5
abs_one=1
abs_m1=1
abs_max=9223372036854775807
abs_min1=9223372036854775807
```

`abs_ovf` takes `std::chrono::abs` of `duration::min()`. Sleep is not
called. A clock is not read.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Wpedantic -Werror`,
at `-O0` and `-O2`. The trace binary against this header and the trace
binary against libstdc++ print the same stdout. With
`-fsanitize=undefined -fno-sanitize-recover=undefined`, that same stdout
is printed and UndefinedBehaviorSanitizer stays quiet on this header at
`-O0` and `-O2`, and on libstdc++ at `-O0`. A copy of the header compiled
without `#pragma GCC system_header` is clean under the same warnings, and
the trace binary against that copy prints the same stdout.

With the trace's aborting `__prism_check`, `abs_ovf` aborts on
`INT-SIGNED-OVF` for abs before `wrapped=` is printed.
`neg_ovf` still aborts on the negation message, `ovf` still aborts on the
addition message, `div_ovf` still aborts on the division overflow message,
and `div_zero` still aborts on `FUNC-CONTRACT` for division by zero. None
of those print `wrapped=`.

A driver whose `__prism_check` returns still reaches `__builtin_trap`
(illegal instruction, exit 132) for `duration::min()`. It does not print a
result after that call. UndefinedBehaviorSanitizer stays quiet on that
trap, at `-O0` and `-O2`. The same driver prints `z=0`, `p=5`, `n=5`,
`one=1`, `m1=1`, `9223372036854775807` for the maximum count, and
`9223372036854775807` for one more than the minimum count. Each of those
calls the abs check once with `ok=1`. The minimum count prints
`CHECK INT-SIGNED-OVF: std::chrono::duration abs overflows the representation ok=0`
and no later line. The negation message is not printed.

`operator*=` does not compile (`no viable overloaded '*='`).
`operator/=` does not compile (`no viable overloaded '/='`).
`sleep_for` is not a member of `std::chrono`. `std::chrono::abs(1LL)` does
not compile (`no matching function for call to 'abs'`). A same-type period
stand-in with `den` 1000 compiles: a count of 1000 is greater than a count
of 1, `abs` of 1000 is 1000, `abs` of -1000 is 1000, `abs` of 0 is 0,
dividing that count by 2 yields 500, and negating it yields -1000.
