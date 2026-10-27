# format slice

Branch `finish/cxx-format`, base `d036e09c7c29d24e411c1375a227a69189d819b1`.
`std::format` is one extensionless file, `src/prism/pir/models/cxx/format`.
A directory of that name would make `#include <format>` fail with EISDIR.
The include guard is `_GLIBCXX_FORMAT`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. The yml files record the contract each task asks
for. `expected` values are YAML booleans. This header is not a proof.

## What the model covers

`format_to_n(char* out, size_t n, const char*, int)` writes one `int` in
decimal. The format string it accepts is "{}". It writes at most `n`
characters and does not write past the buffer. `n == 0` writes nothing,
including when `out` is null, and still reports the full length. The
returned `format_to_n_result::size` is that untruncated length even when
`n` is smaller. It is not 0 for a non-zero int (`42` is length 2, so a
one-byte buffer still reports 2). A null `out` with `n > 0` is
`__prism_check` with class `PTR-NULL-DEREF`. The check does not store
through the null pointer.

## What falls through to libstdc++

`std::format` and `format_to` return or allocate `std::string`. Those names
are not declared. They fail to compile and pir falls back to libstdc++
(`extra.cxx_models`). They are not stubbed.

`<ostream>` in C++23 includes `<format>` and, under `__cpp_lib_print`,
names `std::format`, `vformat_to`, and `format_args`. A translation unit
that includes `<ostream>` (and so `<iostream>`) fails against this
stand-in and falls back the same way. The header does not `#include`
`<string>`, `<string_view>`, `<sstream>`, or `<vector>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/format`

so the existing embed writes `cxx/format`. `_prism_test_sources` gained
`tests/cpp/test_cxx_format.cpp`. `tests/cpp/test_qa_conformance.cpp` lists
`format` in the header-name array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.

## Tests

`tests/cxx_models/format_trace.cpp` includes `<format>`. Compiled twice
(`-isystem` the model directory, and without it), clang++ 18, libstdc++
14, `-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. Stdout
matched:

```
full size=2 d0=4 d1=2
tiny size=2 d0=4 rest=x
zeron size=2 untouched=x
null0 size=2
neg size=2 d0=- d1=3
min size=11 text=-2147483648
```

`tiny` is `format_to_n` of `42` into one byte: size 2, not 0, and the byte
after the buffer's first character stays `'x'`. `argv` `null` on the model
binary aborts with `CHECK PTR-NULL-DEREF` and does not print a size.

`tests/cpp/test_cxx_format.cpp` is that comparison, plus a read of the
header: the null check is `__prism_check` under `n > 0`. Without a usable
clang++ it is `NOTRUN`, same as the other C++ model driver.

`testdata/conformance/prism/cxx/cxx_format_true.cpp` formats `42` into
eight bytes and checks the digits (`no-null-deref`, true).
`cxx_format_false.cpp` passes a null output pointer when `n > 0`
(`false`, witness `[1]`). `testdata/conformance/libc-models/format_contracts.cpp`
is the `#include <format>` the embedded-header case searches for. It has
no sidecar. cmake, `prism_tests`, and pir were not run.
