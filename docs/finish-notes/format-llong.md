# format long long

Branch `finish/cxx-format-llong`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`format_to_n` for `long long` is on the existing header
`src/prism/pir/models/cxx/format`. The header is still one file, not a
directory. The include guard is `_GLIBCXX_FORMAT`. `#pragma GCC system_header`.
Namespace `std`. The only include is `<cstddef>`.

The `int` overload still builds the magnitude in `unsigned`, so `INT_MIN`
is not a signed negation, and it still checks a null output pointer under
`n > 0` with class `PTR-NULL-DEREF` and the message
`format_to_n called with a null output pointer`. A format other than `"{}"`
is still `__prism_check` with class `FUNC-CONTRACT` and the message
`format_to_n model accepts only {}`, then `__builtin_trap`. The characters
of a successful `int` are stored by the same writer the `long long`
overload calls. The returned size is still the full length.

The solver was not run. This header is not a proof. This is not PROVED and
not CLEAN.

## What the long long overload does

`format_to_n(char*, size_t, const char*, long long)` writes one `long long`
in decimal. The format string it accepts is `"{}"`.

The magnitude is an `unsigned long long` subtraction from zero when the
value is negative, so `LLONG_MIN` is not a signed negation. A negative
value writes `'-'` and then the digits. The digit loop is the same shape
as `int`: reversed digits, then the sign in front.

The digit count plus that sign is a `size_t`. When `__nd` is greater than
`static_cast<std::size_t>(-1) - __sign`, the function calls

`__prism_check(0, "FUNC-CONTRACT", "format_to_n length does not fit in size_t")`

and then `__builtin_trap`. The sum `__len = __nd + __sign` is in the
`else`. That arm does not add, does not return, and does not call the
writer. A decimal `long long` is at most 20 characters, so the trap is the
guard around the addition. The caller's buffer is not written on that path.

A format string other than `"{}"` takes the same `FUNC-CONTRACT` check and
`__builtin_trap` as `int`. That trap is before the writer, so nothing is
written.

`n == 0` returns before the output pointer is named. The arm does not call
the writer and does not index the pointer. The returned size is the full
length. The buffer is not written. The returned iterator is null because
the pointer was not read.

A null output pointer with `n > 0` calls

`__prism_check(__out != nullptr ? 1 : 0, "FUNC-CONTRACT", "format_to_n called with a null output pointer")`

and then `if (__out == nullptr) __builtin_trap()`. The writer is after that
trap. Nothing is written.

Otherwise both overloads call `__prism_format_write`. The store count is
`__len < __n ? __len : __n`. The loop is `__k < __put`, so the function
writes at most `n` characters and does not write past that prefix. The
unwritten tail is not stored. The returned `size` is `__len`, the full
untruncated length, not `__put` and not 0. A non-zero value formats as at
least one digit, so that size is not 0.

## What is still not declared

`std::format` and `format_to` are not declared. Those names fail to compile
and pir falls back to libstdc++. They are not stubbed. The header does not
include `<string>` or `<memory>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/format`. `_prism_test_sources` already lists
`tests/cpp/test_cxx_format.cpp`. That CMake line was not deleted.
`CMakeLists.txt` was not edited. `tests/cpp/test_qa_conformance.cpp` was
not edited.

`src/prism/bmc.cpp` was not edited. `docs/ENGINEERING_BACKLOG.md`,
`docs/ROADMAP_STATUS.md`, `README.md`, and `docs/CPP_PORT_PLAN.md` were not
edited.

## Tests

`tests/cpp/test_cxx_format.cpp` reads the header. The `int` null check is
still `PTR-NULL-DEREF` under `__n > 0`. The `long long` length check and
its trap are before the `else` that adds `__nd + __sign`, and that add is
before `n == 0`. The `n == 0` arm does not name the output pointer and does
not call the writer. The format trap is before that arm. The null
`FUNC-CONTRACT` check and its trap are before `__prism_format_write`. The
writer loop is bounded by `__put`. The header is still a regular file.
`__prism_check` is not defined.

`tests/cxx_models/format_trace.cpp` is `#include <format>`. The happy path
still prints the `int` lines and adds `long long` lines for `42`, a
one-byte truncation, `n == 0`, a null pointer with `n == 0`, `-3`, `7`,
a four-byte prefix of `LLONG_MIN`, and the full `LLONG_MIN` and
`LLONG_MAX` text. `argv` `nullll` and `bad` are model-only. clang++ was
not run: WSL did not start (`HCS_E_CONNECTION_TIMEOUT`). cmake, ninja,
`prism_tests`, and pir were not run.
