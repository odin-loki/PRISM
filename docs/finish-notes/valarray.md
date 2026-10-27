# valarray slice

Branch `finish/cxx-valarray`, base `2d487e2a3effd894f0ea923089a7d130db99a10c`.

`std::valarray` is the extensionless file `src/prism/pir/models/cxx/valarray`.
A directory of that name would make `#include <valarray>` fail with EISDIR,
and the unit would fall back to libstdc++. The include guard is
`_GLIBCXX_VALARRAY`, so libstdc++'s own header is not also included. The file
is a `#pragma GCC system_header` and the type lives in namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml files
record the contract each harness asks for. `expected` is a YAML boolean.
Nothing here is `PROVED`.

## What the model stores

One heap buffer of `T`. The block comes from `::operator new` and is released
with `::operator delete`. There is no second buffer and no container. `size()`
returns that live count. A default valarray has `_M_size == 0` and a null
pointer, so `size()` is 0.

`operator[]` calls `__prism_check` with `__n < this->size()` (`MEM-OOB-READ`)
before it returns `_M_data[__n]`. An index that is not in range does not
return a value-initialized `T`. `[0]` on an empty valarray is that same
check: the size is 0, so the index fails it.

`resize` to a different length allocates the new block and constructs copies
of the fill value, then `_M_release` destroys the old elements and
`::operator delete`s the old pointer. The old pointer is written out of
`_M_data` before that delete, and it is not placed in the new buffer. The
new pointer is stored only after the old block is released. A shrink and a
grow both take this path. The same length returns without allocating, so it
does not leak a second block. `size()` after `resize(n)` is `n`.

Copy and move keep that single owner. Move leaves the source empty
(nullptr, size 0) so the source destructor does not free. Copy assignment
releases this object's previous block after the copy exists.

## What falls back

Omitted math operators (`+` `-` `*` `/` `%` `^` `&` `|` `<<` `>>`, the
compound forms, the comparisons, and unary `+` `-` `~` `!`), and `sum`,
`min`, `max`, `shift`, `cshift`, and `apply`, are not declared. A unit that
names one fails to compile against this header, so pir falls back to
libstdc++. They are not stubbed. `slice`, `gslice`, mask, and indirect
subscript are omitted the same way. Construction from a pointer or an
`initializer_list` is omitted. The header does not include the vector,
memory, or initializer_list headers, or any other container.

`src/prism/pir/stage.cpp` was not edited. It already writes every embedded
`cxx/` model and passes that directory as `-isystem`. `src/prism/bmc.cpp`
still returns "is not a valarray model" from the source text before a header
is inlined. That exit is unchanged.

## Wiring

`PRISM_PIR_MODELS` is an explicit list. One line was added:

```
src/prism/pir/models/cxx/valarray
```

`_prism_test_sources` gained `tests/cpp/test_cxx_valarray.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"valarray"` next to the other
public header names. The skips for `iostream/prism_ostream.hpp` and
`deque/deque` are unchanged.

## Tests

`tests/cpp/test_cxx_valarray.cpp` includes the model by path and reads the
header text. Both `operator[]` overloads contain `__prism_check(__n <
this->size())` before `return _M_data[__n]`, and neither returns a
value-initialized `T`. `size()` returns `_M_size`, which starts at 0. A
default `valarray<int>` has `size() == 0`. `resize` calls `_M_release`
before it stores the new pointer. `_M_release` nulls `_M_data` and then
`::operator delete`s the old pointer, after destroying the elements.

`tests/cxx_models/valarray_trace.cpp` is `#include <valarray>`. It prints the
default size, stores one value and reads `[0]`, then grows, shrinks, and
resizes to 0. It does not index an empty valarray.

`testdata/conformance/prism/cxx/cxx_valarray_true.cpp` stores one value and
reads it at `[0]` (`no-oob`, `expected: true`). `cxx_valarray_false.cpp`
indexes an empty valarray when the input is 0 (`expected: false`, witness
`[0]`). `testdata/conformance/libc-models/valarray_contracts.cpp` includes
`<valarray>`. Its false function is `valarray_empty_index_false`, class
`MEM-OOB-READ`.
