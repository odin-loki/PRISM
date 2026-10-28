# chrono duration compound assignment

Branch `finish/cxx-chrono-plus-eq`, base `76e4f7ad2e546b00a3a532df97f16e90b23bb82b`.

`std::chrono` stays one extensionless file, `src/prism/pir/models/cxx/chrono`.
A directory of that name would make `#include <chrono>` fail with EISDIR.
The include guard is still `_GLIBCXX_CHRONO`. `#pragma GCC system_header`.
Namespace `std::chrono`. The file does not `#include <ratio>`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
This header is not a proof.

## What operator+= and operator-= do

Both are members of `duration<_Rep, _Period>`. The parameter is
`const duration&`, so the other operand is the same duration type. Each
one reads the two counts, then calls the same fit test as the matching
binary operator (`__duration_add_fits` or `__duration_sub_fits`) and the
same `__prism_check` (`INT-SIGNED-OVF`, the addition or subtraction
message already used by `operator+` / `operator-`).

When the sum or difference fits, the stored count becomes that value and
the operator returns `*this`. When it does not fit, the check runs and the
stored count becomes 0, the same count `duration<_Rep, _Period>(0)` has.
The fitting addition or subtraction is not evaluated on that path, so a
wrapped negative is not stored. `LLONG_MAX += 1`, `LLONG_MIN += -1`,
`LLONG_MIN -= 1`, and `LLONG_MAX -= -1` each take that path. A check that
returns leaves the count at 0.

Zero stays zero (`zero += zero`, `zero -= zero`). A count added to itself
is twice that count when the double fits. Chaining works:
`(d += a) += b` updates the same object. No clock is read.

## What stayed

`operator+` and `operator-` are unchanged. Each overflow path still calls
`__prism_check` and then `if (!__ok) return duration<_Rep, _Period>(0);`.

`duration_cast` is unchanged. It is still declared only when the source
period and the target period are the same type, this header's
`__duration_period` (`num` 1, `den` 1), and the reps are that same type.
The result is still `_ToDur(__d.count())`.

## What still falls back

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++. They are not stubbed:

- compound assignment of a different period (there is no second-period
  overload; clang++ reports `no viable overloaded '+='`)
- a cast to a different period
- clocks, `time_point`, and the calendar typedefs
- `sleep_for` / `sleep_until` (a sleep that returns immediately would be a
  fake wait)

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/chrono` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_chrono.cpp` was
already in `_prism_test_sources`. The conformance header-name array was
not edited. No new test source was added.

## Tests

`tests/cpp/test_cxx_chrono.cpp` still reads the header as text. The binary
operators are sliced at `operator+(const duration` and
`operator-(const duration`, because `operator+=` contains the characters
`operator+`. Each overflow path is still `__prism_check` and then
`if (!__ok) return duration<_Rep, _Period>(0);`. The new checks require
the same-type member declarations and require each compound operator to
call the matching fit test and `__prism_check`, then
`_M_count = static_cast<_Rep>(0);`, then the fitting store. Those bodies
do not contain the binary overflow return. The comment states that a
different period is not declared. `sleep_for(` and `sleep_until(` stay
absent. `<ratio>` stays absent.

Compiled and run with clang++ 18.1.3 (`-std=c++23 -Wall -Wextra
-Wpedantic -Werror`, doctest main). The case passed, 96 assertions.
`prism_tests` was not run.

`tests/cxx_models/chrono_trace.cpp` includes `<chrono>`. The happy path
still prints `zero=0`, `sum=5`, `cast=2`, `diff=4`, and `zero_sum=0`, and
now also `plus_eq_zero=0`, `plus_eq=5`, and `minus_eq=4`. `ovf` is
unchanged. `plus_eq_ovf` adds 1 to the maximum count through `+=`.
`minus_eq_ovf` subtracts 1 from the minimum count through `-=`. Sleep is
not called. A clock is not read.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Wpedantic -Werror`.
The trace binary against this header and the trace binary against
libstdc++ print the same happy-path lines. With the trace's aborting
`__prism_check`, `ovf`, `plus_eq_ovf`, and `minus_eq_ovf` abort on
`INT-SIGNED-OVF` before `wrapped=` is printed. A separate program whose
check returns, built with `-fsanitize=undefined
-fno-sanitize-recover=undefined`, keeps the fitting edges (`LLONG_MAX + 0`,
`LLONG_MAX + -1`, `LLONG_MIN - 0`, `LLONG_MIN - -1`) and stores 0 for the
four overflowing compound assignments above. UndefinedBehaviorSanitizer
stays quiet on those stores. A different period does not compile
(`no viable overloaded '+='`). `sleep_for` does not compile. A cast to
another period type does not compile.
