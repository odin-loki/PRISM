# span first and last

`std::span::first(count)` and `std::span::last(count)` are on the existing
header file `src/prism/pir/models/cxx/span`. The header is still a file, not a
directory. Both `subspan` overloads are unchanged: they still use
`__glibcxx_assert`.

This header is not a proof. A sub-span from `first` or `last` is not PROVED
and not CLEAN.

## What first and last do

A count greater than `size()` is a precondition failure. `first` calls

`__prism_check(0, "FUNC-CONTRACT", "std::span::first count is outside the span")`

and `last` calls

`__prism_check(0, "FUNC-CONTRACT", "std::span::last count is outside the span")`.

Each then calls `__builtin_trap`, before an element address is formed. That
path does not load an element and does not return a span.

This replaces the earlier `last` path in `span-last.md`. That path was
`MEM-OOB-READ` and then returned `span(_M_ptr, 0)`, so the program went on
with a view the caller did not ask for. `first` used `__glibcxx_assert`. Both
now stop at the check. The class is `FUNC-CONTRACT`, like `front`, `back`,
and `size_bytes`, because no element is read on that path.

A count of 0 returns an empty span and does not load an element. `first(0)`
is `span(_M_ptr, 0)`. `last(0)` is `span(_M_ptr + size(), 0)`, the end
pointer. An in-range count returns that sub-span: `first` starts at
`_M_ptr`, and `last` starts at `_M_ptr + (size() - count)`.

`last` on a null pointer is still not stepped. A non-zero size on that
pointer is still `PTR-NULL-DEREF`, and the result is still an empty span.
The count check comes first, so `last(1)` on an empty span traps.

## What is still omitted

The template `first<N>()` / `last<N>()` / `subspan<O, N>()`,
`std::span<T, N>`, and `std::dynamic_extent` are still not declared. A use
fails to compile against this header and pir falls back to libstdc++.

## Tests

`tests/cpp/test_cxx_span.cpp` reads both bodies as text. Each has the count
test, then the check, then the trap, then the empty case, then the in-range
return. The over-count arm has no `return` and no `_M_ptr`. Neither body
has `__glibcxx_assert` or `MEM-OOB-READ`. The header no longer contains
`__glibcxx_assert(__count <= this->size())`.

The same file builds a driver against the model whose `__prism_check`
aborts. `last(5)` on a 4-element span, `last(1)` on an empty span, and
`first(5)` on a 4-element span abort on `FUNC-CONTRACT` before a size is
printed. `last(0)`, `first(0)`, and `first(4)` print their sizes, and
`first(4)[3]` is 40.

`tests/cxx_models/span_trace.cpp` now also prints `first(2)`, `first(0)`,
`first(4)`, `first(0)` of an empty span, and `last(4)`. The trace binary
against the model and the trace binary against libstdc++ print the same
lines. A count past `size()` is not in the trace, because libstdc++ does not
define it.
