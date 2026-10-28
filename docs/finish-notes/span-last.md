# span last

Branch `finish/cxx-span-last`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.
`std::span::last` is on the existing header file
`src/prism/pir/models/cxx/span`. The header is still a file, not a directory.
`first` and both `subspan` overloads are unchanged: they still use
`__glibcxx_assert`.

The solver was not run. `prism_tests` was not run. This header is not a proof.

## What last does

`last(count)` is the trailing `count` elements when `count <= size()`. The
pointer is `_M_ptr + (size() - count)` and the length is `count`. `last(0)`
is that end pointer with length 0. It does not read an element.

When `count > size()`, `last` calls

`__prism_check(count <= size(), "MEM-OOB-READ", "std::span::last count exceeds size()")`

and returns `span(_M_ptr, 0)`. That span starts at the viewed pointer and has
length 0, so it does not start before the buffer and it does not extend past
`size()`. The length is not the caller's `count`, and the pointer is not
`_M_ptr` minus a count.

A null `_M_ptr` is not stepped. `last(0)` on that pointer is an empty span.
If the size is not 0, `last` calls `__prism_check` with class
`PTR-NULL-DEREF` and still returns an empty span, not a null pointer with a
non-zero length.

## Null pointer at construction

`span(pointer, size)` stores that pointer and that size. A null pointer with
a non-zero size is

`__prism_check(0, "PTR-NULL-DEREF", "std::span constructed from a null pointer with a non-zero size")`.

The stored length stays the requested size. It is not rewritten into an empty
view, and the span does not allocate an array to stand in for it. The
destructor is still defaulted, so the view does not own the array.

## What is still omitted

Not declared, so a use fails to compile against this header and the pir
stage lowers the translation unit again with libstdc++ (`extra.cxx_models`).
They are not stubbed:

- `size_bytes`
- the template `first` / `subspan`
- `std::span<T, N>` and `std::dynamic_extent`
- deduction guides
- a constructor from an iterator pair, or from an rvalue range
- `as_bytes` / `as_writable_bytes`
- a converting constructor from `span<U>`

The header does not include `<memory>`, `<vector>`, `<array>`, or `<deque>`.
`__cpp_lib_span` is not defined.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/span` was already
in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_span.cpp` was already in
`_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` already lists
`span` in the header-name array. That array was not edited.

## Tests

`tests/cpp/test_cxx_span.cpp` reads the header. `last` calls `__prism_check`
with `MEM-OOB-READ` before it returns `span(_M_ptr, 0)` on `count > size()`,
and the in-range return is `_M_ptr + (size() - count)`. The pointer
constructor keeps `_M_len(__n)` and checks a null pointer with a non-zero
size. `size_bytes(` is absent. `first` and `subspan` still use
`__glibcxx_assert`.

`tests/cxx_models/span_trace.cpp` prints `last(2)` and `last(0)` of
`{10, 20, 30, 40}`. `last(0)` prints a size and does not read an element.
The host stub aborts only when `__prism_check` fails. libstdc++ does not
call it.

Checked with Ubuntu clang++ 18.1.3 against libstdc++, `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. The trace compiled twice (`-isystem` the
model directory, and without it). Stdout matched, including

```
last2=2 30 40 1
last0=0 1
none_last=0
```

`last(5)` on a 4-element span aborted in the check (`MEM-OOB-READ`,
`std::span::last count exceeds size()`). `span(nullptr, 1)` aborted with
`PTR-NULL-DEREF`. `last(0)` on the 4-element span printed `z=0` and exited
0. Not an ASan heap error.
