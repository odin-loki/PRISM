# stacktrace slice

Branch `finish/cxx-stacktrace`, base `1278b9dc714ceaee54277a298c50212376942489`.

`std::stacktrace` is the extensionless file `src/prism/pir/models/cxx/stacktrace`.
A directory of that name would make `#include <stacktrace>` fail with EISDIR,
and the unit would fall back to libstdc++. The include guard is
`_GLIBCXX_STACKTRACE`, so libstdc++'s own header is not also included. The file
is a `#pragma GCC system_header` and the types live in namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml files
record the contract each harness asks for. `expected` is a YAML boolean.
Nothing here is `PROVED`.

## What the model stores

Partial `basic_stacktrace`. It is not a template. The allocator argument is
omitted, and `using stacktrace = basic_stacktrace` is the name call sites
already use. The header does not include `<memory>`, `<string>`, or
`<functional>`. It includes no standard header at all.

`current()` records at most one instruction pointer, and only through
`__builtin_return_address(0)` when that builtin exists. A null result is not
stored. When the builtin does not exist, `current()` is empty. `size()` is 0
or 1. A default trace has `_M_size == 0`, so `size()` is 0. Empty `current()`
has size 0.

The entry stores that pointer. `description()` is not declared. `source_file()`
and `source_line()` are not declared either. The header does not contain the
string `main`, the string `unknown`, or `line 1`, so it cannot invent a
function name or a source line.

`operator[]` calls `__prism_check` with `__n < this->size()` (`MEM-OOB-READ`)
before it returns the stored entry. An index that is not in range then calls
`__builtin_trap` and does not return a frame. `[0]` on an empty trace is that
same check: the size is 0, so the index fails it. `__prism_check` is declared
`extern "C" void __prism_check(int, const char*, const char*)` and is not
defined in this file.

## What falls back

`at`, the `current` overloads that take a skip or a max depth, iterators, and
`basic_stacktrace<Allocator>` are not declared. A unit that names one fails to
compile against this header, so pir falls back to libstdc++. They are not
stubbed.

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model and passes that directory as `-isystem`.

## Wiring

`PRISM_PIR_MODELS` is an explicit list. One line was added:

```
src/prism/pir/models/cxx/stacktrace
```

`_prism_test_sources` gained `tests/cpp/test_cxx_stacktrace.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"stacktrace"` next to the other
public header names. Every name that was already in that array stays. The
skips for `iostream/prism_ostream.hpp` and `deque/deque` are unchanged.
`iostream` and `ostream` stay absent.

## Tests

`tests/cpp/test_cxx_stacktrace.cpp` reads the header text. `current()` contains
`__builtin_return_address(0)` and does not contain `__builtin_return_address(1)`.
The empty returns are `basic_stacktrace()` with the default size 0. The one
recorded frame is `basic_stacktrace(__ip, 1)`. `operator[]` contains
`__prism_check` before `__builtin_trap` before `return _M_frame`, and it does
not return a fresh entry. On GCC or Clang the same test includes the header:
a default trace has size 0, and `current()` has size 1 with a non-null pointer
when the builtin exists.

`tests/cxx_models/stacktrace_trace.cpp` is `#include <stacktrace>`. It prints
the default size and the size of `current()`. It reads `[0]` only when that
size is 1. `argv` `oob` indexes an empty trace.

`testdata/conformance/prism/cxx/cxx_stacktrace_true.cpp` calls `current()` and
reads `[0]` only when `size()` is 1 (`no-oob`, `expected: true`).
`cxx_stacktrace_false.cpp` indexes an empty trace when the input is 0
(`expected: false`, witness `[0]`).
`testdata/conformance/libc-models/stacktrace_contracts.cpp` includes
`<stacktrace>`. Its false function is `stacktrace_empty_index_false`, class
`MEM-OOB-READ`.

clang++ 18.1.3 compiled the trace, both prism tasks, and the contracts file
(`-std=c++23 -Wall -Wextra -Werror`, this file ahead of libstdc++ on the
include path). The trace printed `empty 0`, `current 1`, `frame 1` and exited
0. With `__has_builtin` forced false, `current().size()` was 0. Indexing an
empty trace with a `__prism_check` that returns still did not print a frame:
the process died with SIGILL from `__builtin_trap`. With the trace's aborting
stand-in, the same index printed `CHECK MEM-OOB-READ: stacktrace::operator[]
index out of range` and did not print a frame.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ stacktrace unencoded` (`is not a stacktrace model`) from the source text,
before the header is inlined. Those files were not edited. Until that gate is
gone, `cxx_stacktrace_true` and `cxx_stacktrace_false` do not reach this
header. `docs/PIR.md` was not edited.
