# chrono duration comparison

Branch `finish/cxx-chrono-cmp`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.

`std::chrono` stays one extensionless file, `src/prism/pir/models/cxx/chrono`.
A directory of that name would make `#include <chrono>` fail with EISDIR.
The include guard is still `_GLIBCXX_CHRONO`. `#pragma GCC system_header`.
Namespace `std::chrono`. The file does not `#include <ratio>` and does not
`#include <compare>`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
This header is not a proof. Comparing two durations is not PROVED and not
CLEAN.

## What the six operators do

`operator==`, `operator!=`, `operator<`, `operator>`, `operator<=`, and
`operator>=` are function templates. Each one takes two
`const duration<_Rep, _Period>&` arguments, so both durations are the same
type. Each is constexpr and noexcept. The result is that comparison of the
two stored counts, `count()` against `count()`.

A count of 2 against a count of 3 is unequal, less, and less-or-equal. It
is not equal, not greater, and not greater-or-equal. A count compared with
itself is equal, less-or-equal, and greater-or-equal, and it is not less
and not greater. A default duration equals a duration of count 0. A count
of -1 is less than 0 and is not equal to 0. The maximum signed 64-bit count
is greater than that count minus one, equal to itself, and not less than
itself. The minimum signed 64-bit count is less than 0 and less than the
maximum. Those comparisons do not call `__prism_check`. Ordering two stored
counts does not add, so it does not overflow the representation.

The bodies do not `static_cast`, do not divide, and do not name
`common_type`. A period is not converted. A same-type duration whose period
stand-in has `den` 1000 still compares the counts: a count of 1000 is
greater than a count of 1, and those two counts are not equal. The
denominator is not applied.

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

## What still falls back

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++. They are not stubbed:

- a comparison of a different period (each operator is one template of one
  duration type; clang++ reports `invalid operands to binary expression`
  for `==`, `!=`, `<`, `>`, `<=`, and `>=`)
- `operator<=>` (`<compare>` is not included)
- a cast to a different period
- clocks, `time_point`, and the calendar typedefs
- `sleep_for` / `sleep_until` (a sleep that returns immediately would be a
  fake wait)

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/chrono` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_chrono.cpp` was
already in `_prism_test_sources`. The conformance header-name array was
not edited. No new test source was added. `src/prism/bmc.cpp` and
`src/prism/pir/stage.cpp` were not edited.

## Tests

`tests/cpp/test_cxx_chrono.cpp` still reads the header as text. The binary
operators are sliced at `operator+(const duration` and
`operator-(const duration`. Each overflow path is still `__prism_check`
and then `if (!__ok) return duration<_Rep, _Period>(0);`. The compound
assignment slice now ends at the count-comparison comment, and those
bodies are unchanged. The new checks require six constexpr operators on
one `duration<_Rep, _Period>`, each returning the matching comparison of
`__a.count()` and `__b.count()`. That region has no second period, no
`__prism_check`, no addition, no `duration_cast`, and no `static_cast`.
`sleep_for(` and `sleep_until(` stay absent. `<ratio>` and `<compare>`
stay absent. The header says this is not PROVED and not CLEAN.

Compiled and run with clang++ 18.1.3 (`-std=c++23 -Wall -Wextra
-Wpedantic -Werror`, doctest 2.4.11 main). The case passed, 138
assertions. `prism_tests` was not run.

`tests/cxx_models/chrono_trace.cpp` includes `<chrono>`. The happy path
still prints `zero=0`, `sum=5`, `cast=2`, `diff=4`, `zero_sum=0`,
`plus_eq_zero=0`, `plus_eq=5`, and `minus_eq=4`. Before `a` is updated it
also prints `eq=0`, `ne=1`, `lt=1`, `gt=0`, `le=1`, `ge=0`, `eq_self=1`,
`eq_same=1`, `lt_same=0`, `le_same=1`, `ge_same=1`, `zero_eq=1`,
`zero_lt=1`, `neg_lt_zero=1`, `neg_eq_zero=0`, `max_gt=1`, `max_eq=1`,
`max_lt_self=0`, `min_lt_zero=1`, and `min_lt_max=1`. `static_assert`
requires those same relations, including the minimum and maximum counts.
Sleep is not called. A clock is not read. `ovf`, `plus_eq_ovf`, and
`minus_eq_ovf` are unchanged.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Wpedantic -Werror`.
The trace binary against this header and the trace binary against
libstdc++ print the same stdout. With `-fsanitize=undefined
-fno-sanitize-recover=undefined`, that same stdout is printed and
UndefinedBehaviorSanitizer stays quiet, including the minimum and maximum
comparisons. With the trace's aborting `__prism_check`, `ovf` and
`plus_eq_ovf` abort on `INT-SIGNED-OVF` for addition, and `minus_eq_ovf`
aborts on `INT-SIGNED-OVF` for subtraction, each before `wrapped=` is
printed. A same-type period stand-in with `den` 1000 compiles, and a
count of 1000 is not equal to a count of 1. A different period does not
compile (`invalid operands` on all six operators). `sleep_for` does not
compile.
