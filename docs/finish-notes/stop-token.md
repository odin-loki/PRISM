# stop_token slice

Branch `finish/cxx-stop-token`, base `587f8ec0cf0abb6c2af57ee19b5d537c86c17865`.

`<stop_token>` is a file, `src/prism/pir/models/cxx/stop_token`, the same shape
as `vector` and `memory`. A directory of that name would make
`#include <stop_token>` fail with EISDIR. The include guard is
`_GLIBCXX_STOP_TOKEN`. `#pragma GCC system_header`. Namespace `std`. The
header is partial on purpose.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
The yml records the contract the harness asks for. `expected` is a YAML
boolean.

## What the model covers

`stop_source` owns one flag, a `bool` member. `stop_token` observes it
through a copied pointer. Nothing is allocated. The destructor does not
free. There is no thread.

- `request_stop()` on a source that is not yet stopped sets the flag and
  returns true. A second call returns false and leaves the flag set. It
  does not clear the flag.
- `stop_requested()` is true only after that successful `request_stop`. On
  the source it reads the member. On the token it reads through the
  pointer, and only when the pointer is non-null.
- A default token is not associated with a source. `stop_requested()` is
  false.
- `stop_possible()` on a `stop_source` is true, including a
  default-constructed source, because the flag is a member of that object.
- Copying a token copies the pointer. Copying a `stop_source` copies the
  current flag value into the other object. Later `request_stop` calls do
  not cross that copy.
- Move is not declared. A move falls back to the copy. The flag is not left
  behind as a pointer a moved-from source would still treat as
  `stop_possible()`. `stop_possible()` is true because the object still
  contains its flag.

The token's pointer is valid while that `stop_source` is alive. The task
keeps the source alive for the token it hands out.

## What falls through to libstdc++

Not declared, so a use fails to compile against this header and the pir
stage lowers that translation unit again with libstdc++
(`extra.cxx_models`):

- a callback type (none is declared)
- `nostopstate` / `nostopstate_t`
- comparison and `swap`

A callback that ran inline from `request_stop` would pretend a concurrent
waiter ran. This header is not a concurrency proof. It does not include
`<atomic>`, `<thread>`, or `<functional>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/stop_token`

so the existing embed writes `cxx/stop_token` and the existing `-isystem`
directory is enough for `#include <stop_token>`. `_prism_test_sources`
gained `tests/cpp/test_cxx_stop_token.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `stop_token` in the public header
array. The skips for `iostream/prism_ostream.hpp` and `deque/deque` are
unchanged.

## Tests

`tests/cpp/test_cxx_stop_token.cpp` reads the header. The second
`request_stop` returns false and does not clear the flag. A default token's
`stop_requested` is the null-pointer case, so it is false. The header text
does not define `stop_callback`.

`tests/cxx_models/stop_token_trace.cpp` includes `<stop_token>`. It prints
the default token, one successful `request_stop`, a second `request_stop`
that is false, and `stop_requested` staying true. It was not compiled in
this slice.

`testdata/conformance/libc-models/stop_token_contracts.cpp` includes
`<stop_token>` and asserts that same contract. It is the harness the
conformance case searches for. There is no false task: a compile error is
not what this header checks.

`testdata/conformance/prism/cxx/cxx_stop_token_true.cpp` is memsafety true.
It calls `request_stop` once and again, and reads `stop_requested` after
that. The yml `expected` value is the YAML boolean `true`. PIR was not run.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still report
`C++ stop_token unencoded` (`is not a stop_token model`) from the source
text, before the header is inlined. Those files were not edited. Until that
gate is gone, `cxx_stop_token_true` does not reach this header.
`docs/PIR.md` was not edited.
