# latch max

Branch `finish/cxx-latch-max`, base `079b279295e732b434335ce5791f4ba54a4b8f9e`.

`std::latch::max` is declared on the existing header
`src/prism/pir/models/cxx/latch`. The header is still one extensionless file.
A directory of that name would make `#include <latch>` fail with EISDIR.
The include guard is `_GLIBCXX_LATCH`. `#pragma GCC system_header`.
Namespace `std`.

cmake, ninja, `prism_tests`, and pir were not run. The solver was not run.
This header is not a concurrency proof.

## What max does

`static constexpr ptrdiff_t max() noexcept` returns `PTRDIFF_MAX`.
It does not wait. It does not read or write `_M_count`, so it does not
change a latch. It does not call `count_down`. Nothing is allocated.

`<cstdint>` is included so `PTRDIFF_MAX` is defined. `<atomic>`,
`<memory>`, and `<thread>` are not included.

## What stayed

The constructor is unchanged. A negative argument is still `__prism_check`
with class `FUNC-CONTRACT` and message
`latch constructed with a negative count`.

`count_down(n)` is unchanged. It subtracts only when `n >= 0` and `n` is
not greater than the count. Otherwise the same `FUNC-CONTRACT` check fires
(`latch::count_down would pass below zero`) and the count is left
unchanged. The count does not wrap.

`try_wait` is unchanged. It is true only when the count is already zero.

`wait` and `arrive_and_wait` stay omitted. A blocking wait that returns
immediately would be a fake synchronization success. A unit that names one
fails to compile against this header and pir falls back to libstdc++.
They are not stubbed.

The earlier latch note left `max` undeclared. libstdc++ 14's `max()` is
the platform wait-word maximum, and this object's counter is a
`ptrdiff_t`. This slice models `max` as `PTRDIFF_MAX` anyway. The
differential trace does not print `max()`, so its stdout comparison stays
the count-down trace. This header and libstdc++ do not agree on `max()`.

## Wiring

`CMakeLists.txt` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/latch`. `_prism_test_sources` already lists
`tests/cpp/test_cxx_latch.cpp`. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` already names `latch`. No new test
source was added.

`src/prism/bmc.cpp` still reports `C++ std::latch/barrier unencoded`
(`is not a sync primitive model`) from the function body, before this
header is inlined. That file was not edited. `src/prism/conc/` was not
edited.

## Tests

`tests/cpp/test_cxx_latch.cpp` reads the header as text. It did not
require `max` to be absent. It now requires
`static constexpr ptrdiff_t max() noexcept` with `return PTRDIFF_MAX;`,
placed before `count_down`, and that body does not mention `_M_count`,
`-=`, `count_down`, or `wait`. The comments say `max() is modelled` and
`wait and arrive_and_wait stay omitted`. The sentence
`max, wait, and arrive_and_wait are omitted` is gone.

`count_down` past zero is still that `__prism_check`, and that call is
still the only `_M_count -=`. `try_wait` is still `return _M_count == 0`.
`wait` and `arrive_and_wait` are still not defined.

`tests/cxx_models/latch_trace.cpp` was not edited. It does not call `max()`.
