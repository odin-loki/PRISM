# stacktrace empty

Branch `finish/cxx-stacktrace-empty`, base `76e4f7ad2e546b00a3a532df97f16e90b23bb82b`.

`std::stacktrace::empty` is on the existing extensionless file
`src/prism/pir/models/cxx/stacktrace`. A directory of that name would make
`#include <stacktrace>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is still `_GLIBCXX_STACKTRACE`. `#pragma GCC
system_header`. Namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml files
were not edited. `expected` stays a YAML boolean. Nothing here is `PROVED`.
`empty()` being true is the result this header defines. It is not a proof
that a call stack was walked.

## What empty does

`bool empty() const noexcept` returns `size() == 0`. `size()` is still
`_M_size`, which is 0 or 1. A default trace has `_M_size == 0`, so
`empty()` is true. `current()` that stored nothing returns
`basic_stacktrace()`, the same default, so that trace is empty too.
`current()` that stored one address has size 1, so `empty()` is false.

The body is that return. It does not name `_M_frame`, it does not construct
a `stacktrace_entry`, and it does not call `operator[]`. It does not invent
a frame for an empty trace to hand back.

## What stayed

`current()` is unchanged. It stores at most one
`__builtin_return_address(0)` when that builtin exists and the result is
non-null. Otherwise it returns `basic_stacktrace()` and `size()` is 0.
There is still no `__builtin_return_address(1)`.

`operator[]` is unchanged. An index `>= size()` is `__prism_check` with
class `MEM-OOB-READ` and message `stacktrace::operator[] index out of range`,
then `__builtin_trap`. That path does not return a frame. `[0]` on an empty
trace is that same check.

`description()`, `source_file()`, and `source_line()` are not declared. The
header does not contain `main`, `unknown`, or `line 1`. It does not include
`<memory>`, `<string>`, or `<functional>`. It includes no standard header
at all.

## What falls back

`at`, the `current` overloads that take a skip or a max depth, iterators,
and `basic_stacktrace<Allocator>` are not declared. A unit that names one
fails to compile against this header, so pir falls back to libstdc++. They
are not stubbed.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/stacktrace` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_stacktrace.cpp` was
already in `_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` was
not edited. `"stacktrace"` stays in that header-name array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.

## Tests

`tests/cpp/test_cxx_stacktrace.cpp` reads the header. The `empty` body is
`return size() == 0;` and that body does not name `_M_frame`, `_M_size`,
`stacktrace_entry`, `native_handle`, a builtin, or `basic_stacktrace`, and
it has no subscript. `current()` and `operator[]` are still the one-address
store and the check-then-trap. On GCC or Clang the same file includes the
header: a default trace has size 0 and `empty()` is true. `current()` has
`size() <= 1`, and `empty()` is true exactly when that size is 0. When the
builtin exists, size is 1, `empty()` is false, and `[0]` is a non-null
pointer. When it does not, size is 0 and `empty()` is true, with no index.

`tests/cxx_models/stacktrace_trace.cpp` is `#include <stacktrace>`. It
prints the default size, `empty()` of that trace, the size of `current()`,
and `empty()` of that trace. It reads `[0]` only when the size is 1. `argv`
`oob` calls `empty()` on a default trace and, because that is true, indexes
it. `empty()` does not print a frame.

`testdata/conformance/prism/cxx/cxx_stacktrace_true.cpp` calls `current()`.
Size 0 returns only when `empty()` is true, and does not index. Size 1
indexes `[0]` only when `empty()` is false (`no-oob`, `expected: true`).
`cxx_stacktrace_false.cpp` indexes a default trace when the input is 0.
`empty()` true does not skip that index (`expected: false`, witness `[0]`).
`testdata/conformance/libc-models/stacktrace_contracts.cpp` includes
`<stacktrace>`. `stacktrace_empty_true` requires `empty()` on a default
trace. `stacktrace_current_bound_true` requires `empty()` when `current()`
stored nothing, and indexes `[0]` only when `empty()` is false.
`stacktrace_empty_index_false` is still class `MEM-OOB-READ`, witness `[0]`.
The yml `expected` values are unchanged.

clang++ 18.1.3 compiled the trace with `-std=c++23 -Wall -Wextra -Werror`,
this file ahead of libstdc++ (`-isystem`), plus
`-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`. Stdout was

```
empty 0
eflag 1
current 1
cflag 0
frame 1
```

and the process exited 0. `eflag 1` is `empty()` on the default trace.
`cflag 0` is `empty()` on the one stored address. g++ 13.3.0 compiled the
same trace without the sanitizers and printed the same lines, exit 0.

With `__has_builtin` forced false (`-Wno-builtin-macro-redefined`),
`current().size()` was 0, `empty()` was true, and the program printed no
frame:

```
empty 0
eflag 1
current 0
cflag 1
```

Exit 0. `argv` `oob` on clang++ and g++ printed
`CHECK MEM-OOB-READ: stacktrace::operator[] index out of range` and no
frame, then aborted. A `__prism_check` that returns still reaches
`__builtin_trap` (illegal instruction) and does not print a frame.
`empty()` on that trace did not take the skip path.

clang++ 18.1.3 also compiled `tests/cpp/test_cxx_stacktrace.cpp`, both
prism tasks, and the contracts file (`-std=c++23 -Wall -Wextra -Werror`).
The doctest translation unit was syntax-only. `prism_tests` was not run, so
the `TEST_CASE` bodies were not executed. Compiling that file did typecheck
the `empty()` calls.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ stacktrace unencoded` (`is not a stacktrace model`) from the source
text, before the header is inlined. Those files were not edited. Until that
gate is gone, `cxx_stacktrace_true` and `cxx_stacktrace_false` do not reach
this header. `docs/PIR.md` was not edited.
