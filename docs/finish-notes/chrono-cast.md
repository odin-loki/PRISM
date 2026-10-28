# chrono duration_cast

Branch `finish/cxx-chrono-cast`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::chrono` stays one extensionless file, `src/prism/pir/models/cxx/chrono`.
A directory of that name would make `#include <chrono>` fail with EISDIR.
The include guard is still `_GLIBCXX_CHRONO`. `#pragma GCC system_header`.
Namespace `std::chrono`. The file does not `#include <ratio>`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
This header is not a proof.

## What duration_cast does

`duration_cast` is declared only when the source period and the target
period are the same type, this header's `__duration_period` (`num` 1,
`den` 1), and the reps are that same type. The result is
`_ToDur(__d.count())`. The count is copied. There is no ratio multiply,
no division, and no `static_cast`, so the count is not truncated and not
wrapped. A duration of the maximum signed 64-bit count casts to that same
count.

`operator+` and `operator-` are unchanged. Each overflow path still calls
`__prism_check` and then `if (!__ok) return duration<_Rep, _Period>(0);`.

## What still falls back

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++. They are not stubbed:

- a cast to a different period (the constraint fails, or the argument
  period is not `__duration_period`, so there is no matching function)
- a cast to a different rep (a converting cast would truncate or wrap)
- clocks, `time_point`, the calendar typedefs, and compound assignment
- `sleep_for` / `sleep_until` (a sleep that returns immediately would be a
  fake wait)

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/chrono` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_chrono.cpp` was
already in `_prism_test_sources`. The conformance header-name array was
not edited.

## Tests

`tests/cpp/test_cxx_chrono.cpp` still reads the header as text. The
overflow checks are unchanged. It also requires the same-period
`duration_cast` to return `__d.count()`, with no `/`, `%`, `+`, or
`static_cast` in that function, and it requires `sleep_for(` and
`sleep_until(` to stay absent. The comment states that a different period
still falls back.

`tests/cxx_models/chrono_trace.cpp` casts `duration<long long>` of count 2
to the same type and prints `cast=2`. Sleep is not called.

Checked with clang++ 18, `-std=c++23 -Wall -Wextra -Wpedantic -Werror`.
The same-period program compiles against this header and prints the
original count, including the maximum signed 64-bit count. A cast to
another period type does not compile (`no matching function`). The trace
binary against this header and the trace binary against libstdc++ print
the same happy-path lines, including `cast=2`.
