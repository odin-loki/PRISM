# stdatomic compile shim

Slice `finish/stdatomic-note`. `src/prism/pir/models/libc/shim/stdatomic.h`
is a Windows PIR compile shim. It lowers C11 atomics to plain stores and
ignores `memory_order`. It is not a concurrency proof and it is not
sequentially consistent.

`atomic_store_explicit`, `atomic_load_explicit`, `atomic_fetch_add`, and
the `atomic_load` macro stay as they are. Changing them would break the
Windows lowerings and could look like a proof.

## What the comment says

The block next to the existing one-line comment states:

- `memory_order` is ignored.
- Relaxed, acquire, and release are not modeled.
- The conc stage is what refuses those orders as `NEEDS-HARNESS`.
- This header must not be read as `PROVED` or as `CLEAN`.
- It is not a proof.

`src/prism/conc/` is left as it is. That stage is the refusal.

## Test

`tests/cpp/test_stdatomic_shim.cpp` is listed in `_prism_test_sources`
(the test target lists sources and does not glob). It reads the header
from `PRISM_SOURCE_DIR` as text and checks the comment before `#ifndef`
contains `NEEDS-HARNESS` and `not a proof`. It does not include the
header, so the C11 shim is not compiled as C++.

`prism_tests` was not executed in this worktree: this slice does not
configure or build.
