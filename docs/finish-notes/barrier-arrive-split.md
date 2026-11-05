# barrier arrive

Branch `finish/cxx-barrier-arrive-split`, base `63466ba8cbd95157f6dff251c7c8b0475639c4a8`.

`std::barrier::arrive` is the existing member on
`src/prism/pir/models/cxx/barrier`. The header is still one extensionless file.
A directory of that name would make `#include <barrier>` fail with EISDIR.
The include guard is `_GLIBCXX_BARRIER`. `#pragma GCC system_header`.
Namespace `std`.

cmake, ninja, `prism_tests`, and pir were not run. The solver was not run.
This header is not a concurrency proof. `arrive` is not PROVED and
not CLEAN.

## What arrive does

`void arrive(ptrdiff_t __n = 1)` does not wait. It does not call a blocking
operation, and `wait` is still not declared. `arrive_and_wait` is still not
declared.

A negative `n` calls `__prism_check` with class `FUNC-CONTRACT` and message
`barrier::arrive called with a negative count`, then `__builtin_trap`.
That arm does not write `_M_expected`, so the count does not increase.

`n == 0` returns. That arm does not write `_M_expected` and does not trap.
The count stays where it is.

`n` greater than the remaining expected count calls `__prism_check` with
class `FUNC-CONTRACT` and message
`barrier::arrive count exceeds the expected count`, then `__builtin_trap`.
That arm does not write `_M_expected`, so the count does not wrap.

Otherwise the function subtracts `n` (`_M_expected -= __n`). That is the only
subtraction. A positive `n` that is not greater than the remaining expected
count takes this arm, including the update that reaches zero.

The old combined check is gone. `arrive` no longer uses class
`CXX-BARRIER-ARRIVE` or the message
`barrier::arrive past the expected count`. A zero update is not that failing
check.

## What stayed

The constructor is unchanged. It still initializes `_M_expected` to 0, calls
`__prism_check` with class `FUNC-CONTRACT` and message
`barrier constructed with a negative count`, and assigns
`_M_expected = __expected` only when the argument is non-negative. A negative
constructor count is still not stored. This slice does not add a trap to the
constructor and does not store a new negative count from `arrive`.

`max()` is unchanged. `static constexpr ptrdiff_t max() noexcept` still
returns `PTRDIFF_MAX`. It does not wait and it does not change any barrier.

`arrive_and_drop()` is unchanged. It decreases the expected count by one
and does not wait. If the expected count is already zero, it is
`__prism_check` with class `CXX-BARRIER-DROP`, and the count stays zero.

`wait` and `arrive_and_wait` stay omitted. A blocking wait that returns
immediately would be a fake synchronization success. A unit that names one
fails to compile against this header and pir falls back to libstdc++.
They are not stubbed. The completion-function template parameter and the
arrival token stay omitted.

## Wiring

`CMakeLists.txt` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/barrier`. `_prism_test_sources` already lists
`tests/cpp/test_cxx_barrier.cpp`. No new test source was added, so no CMake
line was added or dropped. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` already names `barrier`.

`src/prism/conc/` was not edited. `src/prism/pir/stage.cpp` was not edited.

## Tests

`tests/cpp/test_cxx_barrier.cpp` reads the header as text. The negative arm
is `__prism_check` then `__builtin_trap` before the zero arm, and that arm
does not mention `_M_expected`. The zero arm returns and does not trap. The
exceeds arm is `__prism_check` then `__builtin_trap` before the subtraction,
and that arm does not write `_M_expected`. The only `_M_expected -=` is the
fitting update. The old message `barrier::arrive past the expected count`
is gone. The constructor text and `max()` are still the previous functions.
`wait` and `arrive_and_wait` are still not defined. The comment on
`arrive` says this is not PROVED and not CLEAN.

`tests/cxx_models/barrier_trace.cpp` still arrives exactly the expected
count (`barrier(2)` then `arrive(2)`) and prints `arrived` when argv is
empty. It does not call `wait`. `argv` `zero` is `arrive(0)` on a barrier
of 2 and prints `held`. `argv` `over` is `arrive(2)` on a barrier of 1.
`argv` `neg` is `arrive(-1)` on a barrier of 3. The model stand-in aborts
from `__prism_check` on those two paths, so it does not print `wrapped` or
`increased`.
