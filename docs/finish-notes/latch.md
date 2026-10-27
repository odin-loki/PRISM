# latch slice

Branch `finish/cxx-latch`, base `3e0ebbb46361d4d3c7557657eeeb590d2ae3aa19`.

`std::latch` is one extensionless file, `src/prism/pir/models/cxx/latch`.
A directory of that name would make `#include <latch>` fail with EISDIR.
The include guard is `_GLIBCXX_LATCH`, so libstdc++'s `<latch>` is not parsed
as well. `#pragma GCC system_header`. Namespace `std`.

The solver was not run. The yml records the contract each harness asks for.
`expected` values are the booleans `true` and `false`. This header is not a
concurrency proof.

## What the model covers

One count, stored in the object as a `ptrdiff_t`. Nothing is allocated.
The constructor stores that argument. A negative argument is
`__prism_check` with class `FUNC-CONTRACT` (`latch constructed with a
negative count`), so a latch that passes the check holds a non-negative
count.

`count_down(n)` subtracts `n` only when `n >= 0` and `n` is not greater
than the current count. A larger `n` would pass below zero. That is
`__prism_check` with class `FUNC-CONTRACT` and message
`latch::count_down would pass below zero`, and the count is left unchanged.
The same check rejects a negative update, so the count is not increased
and is not wrapped to a large value. `count_down` does not run waiters.

`try_wait()` is true only when the count is already zero. It does not
decrement. It is false while the count is positive. `count_down(0)` on a
positive count leaves `try_wait` false.

Copy construction and copy assignment are deleted. The destructor is
defaulted and frees nothing. The only include is `<cstddef>`. `<atomic>`,
`<memory>`, and `<thread>` are not included.

## What is omitted

`max`, `wait`, and `arrive_and_wait` are not declared. A unit that names
one fails to compile against this header and `lower_to_ir` lowers that
translation unit again with libstdc++ (`extra.cxx_models`). They are not
stubbed. A blocking `wait` that returns immediately would be a fake
synchronization success.

`max` is omitted on purpose. libstdc++ 14's `max()` is
`__int_traits<__platform_wait_t>::__max` (2147483647 on this machine).
This object stores a `ptrdiff_t`, so publishing `PTRDIFF_MAX` would not
match that header, and publishing 2147483647 would pretend the counter is
the platform wait word.

## Still in front of the model

`src/prism/bmc.cpp` (via `src/prism/bmc_unenc.inc`) still returns
`C++ std::latch/barrier unencoded: bitvector BMC is not a sync primitive model`
from the function body, before any header is inlined. That status is
`NEEDS-HARNESS`. It is not a proof. Those files were not edited.
`src/prism/pir/stage.cpp` and `src/prism/conc/` were not edited. PIR was
not run, so this slice does not claim the tasks are proved.

## Wiring

`PRISM_PIR_MODELS` gained one line,
`src/prism/pir/models/cxx/latch`. The existing embed writes `cxx/latch`.
`_prism_test_sources` gained `tests/cpp/test_cxx_latch.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `latch` in the header-name array.
The skips for `iostream/prism_ostream.hpp` and `deque/deque` are unchanged.

## Tests

`tests/cpp/test_cxx_latch.cpp` reads the header as text. `count_down` past
zero is the `__prism_check` above, and that call is the only `_M_count -=`.
`try_wait` is `return _M_count == 0` and does not decrement. The header
does not define `wait(` or `arrive_and_wait(`.

Checked with clang++ 18, libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-pthread -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`.
`tests/cxx_models/latch_trace.cpp` compiled twice (`-isystem` the model
directory, and without it). Stdout matched:

```
start 0
mid 0
zero 1
empty 1
```

`start 0` and `mid 0` are `try_wait` while the count is still positive.
`zero 1` is `try_wait` after the count reaches zero. `empty 1` is a latch
constructed at zero. `-M` names `src/prism/pir/models/cxx/latch`.

`argv` `over` is `count_down(2)` on a latch of 1. The model aborts with
`CHECK FUNC-CONTRACT: latch::count_down would pass below zero` and does not
print `wrapped`. libstdc++ 14's `count_down` is `fetch_sub` with no
assertion, so the same binary exits 0 and prints `wrapped`. A separate
driver also aborted on a negative constructor (`latch constructed with a
negative count`) and on `count_down(-1)`, and `count_down(0)` on a count of
3 left `try_wait` false until `count_down(3)`.

`g.wait()` and `g.arrive_and_wait()` fail to compile against this header
(`no member named 'wait'` / `no member named 'arrive_and_wait'`).

`testdata/conformance/prism/cxx/cxx_latch_true.cpp` counts down to zero and
asserts `try_wait` is false before that and true after (`memsafety` true).
`cxx_latch_false.cpp` is `count_down` past zero when the input is greater
than 1 (`false`, witness `[2]`). `sanitizer_blind` names that function
because the witness is this model's precondition: under ASan/UBSan,
libstdc++ stays clean. `testdata/conformance/libc-models/latch_contracts.cpp`
is the `#include <latch>` the conformance case searches for. It is not a
scored task.

cmake, `prism_tests`, and pir were not run.
