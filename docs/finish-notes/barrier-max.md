# barrier max

Branch `finish/cxx-barrier-max`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.

`std::barrier::max` is declared on the existing header
`src/prism/pir/models/cxx/barrier`. The header is still one extensionless file.
A directory of that name would make `#include <barrier>` fail with EISDIR.
The include guard is `_GLIBCXX_BARRIER`. `#pragma GCC system_header`.
Namespace `std`.

cmake, ninja, `prism_tests`, and pir were not run. The solver was not run.
This header is not a concurrency proof.

## What max does

`static constexpr ptrdiff_t max() noexcept` returns `PTRDIFF_MAX`.
It does not wait. It does not read or write `_M_expected`, so it does not
change any barrier. It does not call `arrive` or `arrive_and_drop`. Nothing
is allocated.

`<cstdint>` is included so `PTRDIFF_MAX` is defined. `<atomic>`, `<latch>`,
`<semaphore>`, and `<thread>` are not included.

## What the constructor stores

The constructor used to initialize `_M_expected` with the argument and then
check. A negative argument stayed in the object. It now initializes
`_M_expected` to 0, calls `__prism_check` with class `FUNC-CONTRACT` and
message `barrier constructed with a negative count`, and assigns the
argument only when that argument is non-negative. On failure the expected
count stays 0. A non-negative argument, including 0, is stored.

## What stayed

`arrive(n)` is unchanged. It subtracts only when `n > 0` and `n` is not
greater than the expected count. Otherwise the same `CXX-BARRIER-ARRIVE`
check fires (`barrier::arrive past the expected count`) and the count is
left unchanged. The count does not wrap.

`arrive_and_drop()` is unchanged. It decreases the expected count by one
and does not wait. If the expected count is already zero, it is
`__prism_check` with class `CXX-BARRIER-DROP`, and the count stays zero.

`wait` and `arrive_and_wait` stay omitted. A blocking wait that returns
immediately would be a fake synchronization success. A unit that names one
fails to compile against this header and pir falls back to libstdc++.
They are not stubbed. The completion-function template parameter and the
arrival token stay omitted.

The earlier barrier note left `max` undeclared. This slice models `max` as
`PTRDIFF_MAX`. The differential trace does not call `max()`, so its stdout
comparison stays the arrive trace.

## Wiring

`CMakeLists.txt` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/barrier`. `_prism_test_sources` already lists
`tests/cpp/test_cxx_barrier.cpp`. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` already names `barrier`. No new test
source was added.

`src/prism/bmc.cpp` still reports `C++ std::latch/barrier unencoded`
(`is not a sync primitive model`) from the function body, before this
header is inlined. That file was not edited. `src/prism/conc/` was not
edited. `src/prism/pir/stage.cpp` was not edited.

## Tests

`tests/cpp/test_cxx_barrier.cpp` reads the header as text. It now requires
the constructor to start from `_M_expected(0)`, to keep the message
`barrier constructed with a negative count`, and to assign
`_M_expected = __expected` only after that check. It requires
`static constexpr ptrdiff_t max() noexcept` with `return PTRDIFF_MAX;`,
placed before `arrive`, and that body does not mention `_M_expected`,
`-=`, `arrive`, or `wait`. The comments say `max() is modelled`,
`does not change any barrier`, and
`wait and arrive_and_wait stay omitted`.

`arrive` past the count is still that `__prism_check`, and that call is
still the `_M_expected -= __n` under `if (__ok)`. `arrive_and_drop` is
still defined. `wait(` and `arrive_and_wait` are still not defined after
comments are stripped.

`tests/cxx_models/barrier_trace.cpp` was not edited. It does not call
`max()`. It still arrives exactly the expected count (`barrier(2)` then
`arrive(2)`) and does not call `wait`.
