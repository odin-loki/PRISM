# execution slice

Branch `finish/cxx-execution`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::execution` is the extensionless file `src/prism/pir/models/cxx/execution`.
A directory of that name would make `#include <execution>` fail with EISDIR,
and the unit would fall back to libstdc++. The include guard is
`_GLIBCXX_EXECUTION`, so libstdc++'s own header is not also included. The
file is a `#pragma GCC system_header`. The names live in
`namespace std::execution`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml records
the contract the task asks for. `expected` is a YAML boolean. Nothing here
is `PROVED`. This header is not a parallelism proof.

## What the model covers

`sequenced_policy` and `seq` only.

`sequenced_policy` is an empty class. It has no data members and no member
function. `seq` is `inline constexpr sequenced_policy seq{}`. Copying it
yields another `sequenced_policy`. `decltype(seq)` is
`const sequenced_policy`, because `constexpr` on that variable implies
`const`. The tag does not run an algorithm.

libstdc++ 14's `sequenced_policy` is an empty class as well, so `sizeof`
is 1 against both headers. This header does not declare the
`__allow_unsequenced`, `__allow_vector`, and `__allow_parallel` members
that libstdc++ 13 puts on that class.

## What falls back

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- `par`, `par_unseq`, and `unseq`
- `parallel_policy`, `parallel_unsequenced_policy`, and `unsequenced_policy`
- every algorithm
- `std::is_execution_policy`

A parallel policy that ran on one thread would be a fake parallel
execution. The header does not include `<thread>`, `<atomic>`,
`<algorithm>`, or `<memory>`. It defines no feature-test macro. Defining
`__cpp_lib_parallel_algorithm` here would claim algorithms this file does
not provide.

`std::for_each(std::execution::seq, first, last, f)` still fails when
`<algorithm>` is included beside this header. libstdc++'s policy overload
requires `__pstl::execution::is_execution_policy<sequenced_policy>`, and
that trait is true only for the pstl policy types. Clang reports
`no matching function for call to 'for_each'`. The same call compiles
against libstdc++ 14.

## Still in front of the model

`src/prism/bmc_unenc.inc` still returns
`C++ execution unencoded: bitvector BMC is not an execution model` when the
function body matches `std::execution::`, before a header is inlined.
`src/prism/bmc.cpp` `harness_for_parsefail` returns
`C++ execution unencoded` when the compiler diagnostic contains the
substring `execution`. Those files were not edited. The task and the
harness each name `std::execution::seq`, so the source gate matches them.
Until that gate is gone, `cxx_execution_true` does not reach this header.

The same file still returns `C++ std::task unencoded` for `std::task<` and
`std::execution::task<`, and `C++ sync_wait unencoded` for `sync_wait(`.
The task, the harness, and the trace do not use those names. The scored
function does not use the word `constexpr`, so the `constexpr unencoded`
gate does not match it.

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model and passes that directory as `-isystem`.

## Wiring

`PRISM_PIR_MODELS` gained

```
src/prism/pir/models/cxx/execution
```

so the embedded name is `cxx/execution`. `_prism_test_sources` gained
`tests/cpp/test_cxx_execution.cpp`. `tests/cpp/test_qa_conformance.cpp`
lists `"execution"` next to the other public header names. Every name
already in that array stays. The skips for `iostream/prism_ostream.hpp`
and `deque/deque` are unchanged. `iostream` and `ostream` stay absent.

## Tests

`tests/cpp/test_cxx_execution.cpp` reads the header as text and does not
include it. Defining `_GLIBCXX_EXECUTION` in that translation unit would
hide libstdc++'s `<execution>`. The file is a regular file named
`execution`. With line comments removed, the code has no `(`, no
`#include`, and no `par` or `unseq`, so no function and no parallel policy
is declared. `sequenced_policy` appears twice: the class and the type of
`seq`. The declaration is `inline constexpr sequenced_policy seq{}`. A
comment states that this header is not a parallelism proof, and that a
parallel policy that ran on one thread would be a fake parallel execution.

`tests/cxx_models/execution_trace.cpp` is `#include <execution>`. It copies
`seq` and prints whether `decltype(seq)` and the copy are
`const sequenced_policy`, and the size of that class. It does not name
`par`, `par_unseq`, or `unseq`, and it does not call an algorithm.

Checked with clang++ 18.1.3 and libstdc++ 14 (`_GLIBCXX_RELEASE 14`),
`-std=c++23 -D_GLIBCXX_ASSERTIONS -Wall -Wextra -Werror -O1`. Compiled
twice (`-isystem` the model directory, and without it). Stdout matched,
and both binaries exited 0:

```
same 1
copy 1
size 1
```

The same flags with `-fsanitize=address,undefined
-fno-sanitize-recover=all` printed those lines and exited 0 both ways.

`std::execution::par` fails to compile against this header
(`no member named 'par'`). `par_unseq` and `unseq` fail the same way
(`no member named 'par_unseq'`, `no member named 'unseq'`). All three
compile against libstdc++ 14. `std::is_execution_policy` fails here
(`no member named 'is_execution_policy'`) and compiles against libstdc++.

`testdata/conformance/prism/cxx/cxx_execution_true.cpp` copies `seq` and
reads `buf[0]` on both branches (`no-oob`, `expected: true`). The tag does
not choose an index. There is no false task.
`testdata/conformance/libc-models/execution_contracts.cpp` is the
`#include <execution>` the header case searches for. It is not a scored
task. Both files compile with the flags above, against this header and
against libstdc++.

cmake, ninja, `prism_tests`, and pir were not run.
