# chrono duration modulo

Branch `finish/cxx-chrono-mod`, base `c164ba71c67bf51330687627679f49368a361928`.

`std::chrono` stays one extensionless file, `src/prism/pir/models/cxx/chrono`.
A directory of that name would make `#include <chrono>` fail with EISDIR.
The include guard is still `_GLIBCXX_CHRONO`. `#pragma GCC system_header`.
Namespace `std::chrono`. The file does not `#include <ratio>` and does not
`#include <compare>`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
This header is not a proof. The remainder of a duration modulo a scalar is
not PROVED and not CLEAN.

## What operator% does

`operator%` is a function template. The duration argument is
`const duration<_Rep, _Period>&` and the scalar is `const _Scalar&` with
`_Scalar` the same type as `_Rep`, so the result is that same duration
type. A different scalar type is not an overload. It reads the stored
count and the scalar as `long long`. `operator%=` is not declared.

A zero divisor sets `__nz` to 0 and calls `__prism_check` with class
`FUNC-CONTRACT` and message
`std::chrono::duration modulo by zero`, then `__builtin_trap`. That arm
has no `return`, no `%`, and no remainder. It does not return
`duration(0)`. A count of 5 modulo 0, a count of 0 modulo 0, and the
minimum count modulo 0 take this arm. The overflow check is not reached.

A nonzero divisor calls that same check with `__nz` 1. The most negative
signed 64-bit count modulo -1 then sets `__ok` to 0. The comparison is
`__x == __duration_rep_min && __y == -1`. It does not form the remainder.
`__prism_check` is called with class `INT-SIGNED-OVF` and message
`std::chrono::duration modulo overflows the representation`, then
`__builtin_trap`. That arm has no `return`, no `-`, and no `%`. It does
not form the remainder.

Every other result is the stored count modulo the scalar,
`duration<_Rep, _Period>(static_cast<_Rep>(__x % __y))`, and only in that
`else`. The remainder is the C++ remainder: the quotient truncates toward
zero, and the remainder has the sign of the dividend. A count of 10
modulo 2 is 0. A count of 10 modulo 3 is 1. A count of 5 modulo 2 is 1.
A count of -5 modulo 2 is -1. A count of 5 modulo -2 is 1. A count of -5
modulo -2 is -1. A count of 1 modulo 2 is 1, and a count of -1 modulo 2
is -1. A count of 0 modulo 5 is 0, and a count of 0 modulo -1 is 0. The
maximum count modulo 1 is 0. The maximum count modulo -1 is 0. The
minimum count modulo 1 is 0. The minimum count modulo 2 is 0. The minimum
count modulo -2 is 0. The maximum count modulo itself is 0. The minimum
count modulo itself is 0. The maximum modulo the minimum is
`9223372036854775807`. The minimum modulo the maximum is -1.

`__prism_check` runs for a fitting remainder as well: the zero check with
`__nz` 1, then the overflow check with `__ok` 1. The remainder is only in
the final `else`, after both calls. Each message occurs once in the header.

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

`std::chrono::abs` is unchanged. The minimum count is still
`__prism_check` then `__builtin_trap`. A negative count that is not the
minimum is still the negation. Zero stays zero. A positive count is still
the same count.

`duration::zero`, `duration::min`, and `duration::max` are unchanged.
`min()` is still the most negative count and `max()` is still the most
positive count.

## What still falls back

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++. They are not stubbed:

- `operator%=` (`no viable overloaded '%='`)
- `operator/=` (`no viable overloaded '/='`)
- `operator*=` (`no viable overloaded '*='`)
- `std::chrono::abs` of a `long long` (`no matching function for call to
  'abs'`)
- a scalar whose type is not the duration's rep (`invalid operands to
  binary expression` for `duration<long long>` and `int`, on `operator%`,
  `operator/`, and `operator*`)
- a duration modulo another duration (same diagnostic, both operands
  durations)
- a scalar modulo a duration (`long long` and `duration<long long>`)
- a duration divided by another duration
- a comparison of a different period
- a cast to a different period
- clocks, `time_point`, and the calendar typedefs
- `sleep_for` / `sleep_until` (a sleep that returns immediately would be a
  fake wait; `std::chrono::sleep_for` is not a member)

`operator%` is the same template for every period. A same-type period
stand-in with `den` 1000 still compares the counts. A count of 1000 is
greater than a count of 1. That count modulo 2 is 0. A count of 5 modulo
2 is 1, a count of -5 modulo 2 is -1, and a count of 5 modulo -2 is 1.
Dividing a count of 1000 by 2 still yields 500, and negating that count
still yields -1000. The minimum count modulo -2 is 0. The maximum count
modulo -1 is 0. The denominator is not applied.

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
and then `if (!__ok) return duration<_Rep, _Period>(0);`. The abs slice
now ends at the modulo comment, and that region is unchanged. The new
checks require one
`operator%(const duration<_Rep, _Period>& __d, const _Scalar& __s)`,
both messages, the zero check before the zero trap, the overflow check
before the overflow trap, and `__x % __y` only after both traps. The zero
arm has no `return`, no `duration(0)`, and no `%`. The overflow arm has
no `return`, no `-`, and no `%`. `sleep_for(` and `sleep_until(` stay
absent. `<ratio>` and `<compare>` stay absent. `operator*=(`,
`operator/=(`, and `operator%=(` stay absent. The header says this is not
PROVED and not CLEAN. That sentence appears twelve times.

Compiled and run with clang++ 18.1.3 (`-std=c++23 -Wall -Wextra
-Wpedantic -Werror`, doctest 2.4.11). The case passed, 441 assertions.
`prism_tests` was not run.

`tests/cxx_models/chrono_trace.cpp` includes `<chrono>`. The happy path
still prints the previous lines, and now also:

```
mod=0
mod_rem=1
mod_trunc=1
mod_neg=-1
mod_neg_s=1
mod_neg2=-1
mod_zero_count=0
mod_zero_neg1=0
mod_max_one=0
mod_max_neg1=0
mod_min_one=0
mod_min_two=0
mod_min_neg2=0
mod_one_two=1
mod_m1_two=-1
mod_max_max=0
mod_min_min=0
mod_max_min=9223372036854775807
mod_min_max=-1
```

`mod_zero` takes a count of 5 modulo 0. `mod_min_zero` takes the minimum
count modulo 0. `mod_ovf` takes the minimum count modulo -1. Sleep is not
called. A clock is not read.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Wpedantic -Werror`,
at `-O0` and `-O2`. The trace binary against this header and the trace
binary against libstdc++ print the same stdout. With
`-fsanitize=undefined -fno-sanitize-recover=undefined`, that same stdout
is printed and UndefinedBehaviorSanitizer stays quiet, on this header and
on libstdc++, at `-O0` and `-O2`. A copy of the header compiled without
`#pragma GCC system_header` is clean under the same warnings, and the
trace binary against that copy prints the same stdout.

With the trace's aborting `__prism_check`, `mod_zero` and `mod_min_zero`
abort on `FUNC-CONTRACT` for modulo by zero before `wrapped=` is printed.
`mod_ovf` aborts on `INT-SIGNED-OVF` for modulo before `wrapped=` is
printed. `div_zero` still aborts on the division-by-zero message,
`div_ovf` still aborts on the division overflow message, `abs_ovf` still
aborts on the abs message, `neg_ovf` still aborts on the negation message,
and `ovf` still aborts on the addition message. None of those print
`wrapped=`.

A driver whose `__prism_check` returns still reaches `__builtin_trap`
(illegal instruction, exit 132) for a count of 5 modulo 0, a count of 0
modulo 0, `duration::min()` modulo 0, and `duration::min()` modulo -1.
None of those print a `rem=` line. The zero cases print only
`CHECK FUNC-CONTRACT: std::chrono::duration modulo by zero ok=0`.
The minimum count modulo -1 prints that check with `ok=1` and then
`CHECK INT-SIGNED-OVF: std::chrono::duration modulo overflows the representation ok=0`.
UndefinedBehaviorSanitizer stays quiet on those traps, at `-O0` and
`-O2`. The same driver prints `1` for a count of 10 modulo 3, `0` for a
count of 10 modulo 2, `-1` for a count of -5 modulo 2, `1` for a count of
5 modulo -2, `0` for the maximum count modulo -1, `0` for the minimum
count modulo -2, `9223372036854775807` for the maximum count modulo the
minimum count, and `-1` for the minimum count modulo the maximum count.
Each of those calls both checks with `ok=1`.

`operator%=` does not compile (`no viable overloaded '%='`).
`operator*=` does not compile (`no viable overloaded '*='`).
`operator/=` does not compile (`no viable overloaded '/='`).
`sleep_for` is not a member of `std::chrono`. A scalar of type `int`
does not compile (`invalid operands to binary expression`). A duration
modulo a duration does not compile. A scalar modulo a duration does not
compile. `std::chrono::abs(1LL)` does not compile. A same-type period
stand-in with `den` 1000 compiles: a count of 1000 is greater than a
count of 1, that count modulo 2 is 0, a count of 5 modulo 2 is 1, a count
of -5 modulo 2 is -1, a count of 5 modulo -2 is 1, dividing that count by
2 yields 500, and negating it yields -1000. The minimum count modulo -2
is 0. The maximum count modulo -1 is 0.
