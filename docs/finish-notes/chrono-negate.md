# chrono duration unary negation

Branch `finish/cxx-chrono-negate`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`std::chrono` stays one extensionless file, `src/prism/pir/models/cxx/chrono`.
A directory of that name would make `#include <chrono>` fail with EISDIR.
The include guard is still `_GLIBCXX_CHRONO`. `#pragma GCC system_header`.
Namespace `std::chrono`. The file does not `#include <ratio>` and does not
`#include <compare>`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
This header is not a proof. Unary negation of a duration is not PROVED and
not CLEAN.

## What unary operator- does

`operator-` for one duration is a function template. The argument is
`const duration<_Rep, _Period>&`, so the result is that same duration type.
It reads the stored count as `long long`.

The most negative signed 64-bit count is `__duration_rep_min`
(`-9223372036854775807LL - 1`). That count does not have a negation in the
representation. `__ok` is 0, and `__prism_check` is called with class
`INT-SIGNED-OVF` and message
`std::chrono::duration negation overflows the representation`, then
`__builtin_trap`. That arm has no `return` and no `-`. It does not return
`duration(0)` and it does not form the negation. The locked overflow return
`if (!__ok) return duration<_Rep, _Period>(0);` is not on this path.

Every other count takes the `else` and returns
`duration<_Rep, _Period>(static_cast<_Rep>(-__x))`. That is
`duration(-count)`. Zero stays zero: the negation of 0 is 0. Negating the
maximum count yields `-9223372036854775807`. Negating one more than the
minimum count yields the maximum count. A count of 5 yields -5, and a
count of -5 yields 5.

`__prism_check` runs for a fitting count as well, with `__ok` 1. The
negation is only in the `else`, after that call.

## What stayed

`operator+` and binary `operator-` are unchanged. Each overflow path still
calls `__prism_check` and then
`if (!__ok) return duration<_Rep, _Period>(0);`. That return appears twice
in the header.

`operator+=` and `operator-=` are unchanged. Each one still uses the
matching fit test, calls `__prism_check`, and stores 0 when the sum or
difference does not fit.

`operator*` is unchanged. A product that does not fit is still
`__prism_check` then `__builtin_trap`. Zero times any count is still zero,
returned before that check. `operator*=` is still not declared.

`duration_cast` is unchanged. It is still declared only when the source
period and the target period are the same type, this header's
`__duration_period` (`num` 1, `den` 1), and the reps are that same type.
The result is still `_ToDur(__d.count())`.

`operator==`, `operator!=`, `operator<`, `operator>`, `operator<=`, and
`operator>=` are unchanged. They still compare the stored counts of one
duration type.

`duration::zero`, `duration::min`, and `duration::max` are unchanged.
`min()` is still the most negative count and `max()` is still the most
positive count.

## What still falls back

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++. They are not stubbed:

- `operator*=` (`no viable overloaded '*='`)
- a duration multiplied by another duration
- a scalar whose type is not the duration's rep
- a comparison of a different period
- a cast to a different period
- clocks, `time_point`, and the calendar typedefs
- `sleep_for` / `sleep_until` (a sleep that returns immediately would be a
  fake wait; `std::chrono::sleep_for` is not a member)

Unary `operator-` is the same template for every period. A same-type period
stand-in with `den` 1000 still compares the counts, and negating a count of
1000 yields -1000. The denominator is not applied.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/chrono` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_chrono.cpp` was
already in `_prism_test_sources`. The conformance header-name array was not
edited. No new test source was added. `tests/cxx_models/chrono_trace.cpp` is
not listed in `CMakeLists.txt`.

## Tests

`tests/cpp/test_cxx_chrono.cpp` still reads the header as text. The binary
operators are sliced at `operator+(const duration` and
`operator-(const duration`. Each overflow path is still `__prism_check`
and then `if (!__ok) return duration<_Rep, _Period>(0);`. The multiply
slice now ends at the unary-negation comment, and that region is unchanged.
The new checks require one unary `operator-(const duration<_Rep, _Period>& __d)`,
the negation message, `__prism_check` before `__builtin_trap`, and the
negated count only in the `else`. The trap arm has no `return`, no
`duration(0)`, and no `-`. `sleep_for(` and `sleep_until(` stay absent.
`<ratio>` and `<compare>` stay absent. `operator*=` stays absent. The
header says this is not PROVED and not CLEAN.

Compiled and run with clang++ 18.1.3 (`-std=c++23 -Wall -Wextra
-Wpedantic -Werror`, doctest 2.4.11). The case passed, 242 assertions.
`prism_tests` was not run.

`tests/cxx_models/chrono_trace.cpp` includes `<chrono>`. The happy path
still prints the previous lines, and now also:

```
neg_zero=0
neg_pos=-5
neg_neg=5
neg_max=-9223372036854775807
neg_min1=9223372036854775807
```

`neg_ovf` negates `duration::min()`. Sleep is not called. A clock is not
read.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Wpedantic -Werror`.
The trace binary against this header and the trace binary against
libstdc++ print the same stdout. With `-fsanitize=undefined
-fno-sanitize-recover=undefined`, that same stdout is printed and
UndefinedBehaviorSanitizer stays quiet, on this header and on libstdc++.
A copy of the header compiled without `#pragma GCC system_header` is clean
under the same warnings. With the trace's aborting `__prism_check`,
`neg_ovf` aborts on `INT-SIGNED-OVF` for negation before `wrapped=` is
printed. A driver whose `__prism_check` returns still reaches
`__builtin_trap` (illegal instruction) on `duration::min()` and prints no
`wrapped=` line. UndefinedBehaviorSanitizer stays quiet on that trap.
The same driver, and libstdc++, print `z=0`, `-5`, `5`,
`-9223372036854775807` for the maximum count, and
`9223372036854775807` for one more than the minimum count.

`operator*=` does not compile. `sleep_for` is not a member of
`std::chrono`. A same-type period stand-in with `den` 1000 compiles: a
count of 1000 is greater than a count of 1, and negating that count yields
-1000.
