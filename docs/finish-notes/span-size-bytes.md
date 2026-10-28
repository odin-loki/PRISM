# span size_bytes

Branch `finish/cxx-span-size-bytes`, base `76e4f7ad2e546b00a3a532df97f16e90b23bb82b`.
`std::span::size_bytes` is on the existing header file
`src/prism/pir/models/cxx/span`. The header is still a file, not a directory.
`first`, `last`, and both `subspan` overloads are unchanged.

The solver was not run. `prism_tests` was not run. This header is not a proof.

## What size_bytes does

`size_bytes()` is `size() * sizeof(element_type)`.

An empty span returns 0. That path does not read an element. A null pointer
with length 0 is that same result: the call does not dereference the pointer.

`sizeof(element_type) == 0` is not a 0 result. That arm calls

`__prism_check(0, "FUNC-CONTRACT", "std::span::size_bytes element size is zero")`

and then `__builtin_trap`. The arm has no return. It is `if constexpr`, so
the later division is not instantiated for a zero-size element. An empty
span of a zero-size element takes this arm, not the empty success return.

When the product does not fit in `size_t`, `size_bytes` calls

`__prism_check(0, "MEM-OOB-READ", "std::span::size_bytes size times element size does not fit in size_t")`

and then `__builtin_trap`. That arm has no return. It does not return a
wrapped byte count. A product that fits exactly, including
`(static_cast<size_t>(-1) / sizeof(int)) * sizeof(int)`, is returned.

## What stayed

`last(count)` with `count > size()` is still `__prism_check` with class
`MEM-OOB-READ` and still returns `span(_M_ptr, 0)`. That span starts at the
viewed pointer and has length 0, so it does not start before the buffer and
it does not extend past `size()`. `last(0)` is still an empty span and does
not read an element.

`span(pointer, size)` still stores that pointer and that size. A null
pointer with a non-zero size is still

`__prism_check(0, "PTR-NULL-DEREF", "std::span constructed from a null pointer with a non-zero size")`.

The stored length stays the requested size. The destructor is still
defaulted, so the view does not own the array. `first(count)` and both
`subspan` overloads still use `__glibcxx_assert`.

## What is still omitted

Not declared, so a use fails to compile against this header and the pir
stage lowers the translation unit again with libstdc++ (`extra.cxx_models`).
They are not stubbed:

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

`tests/cpp/test_cxx_span.cpp` reads the header. `size_bytes` checks a zero
element size before it returns 0 for an empty span, and the product check
and `__builtin_trap` come before `return size() * sizeof(element_type)`.
Neither failing arm contains `return`. The body does not dereference an
element. The sentence `size_bytes, and the template first/subspan` is gone.
`the template first/subspan` remains. `last` still returns
`span(_M_ptr, 0)` on `count > size()`, and the pointer constructor still
keeps `_M_len(__n)`.

`tests/cxx_models/span_trace.cpp` also prints `size_bytes` of the 4-int
span, of an empty span, and of `last(0)`. `last(0)` still prints a size and
does not read an element.

Checked with Ubuntu clang++ 18.1.3 against libstdc++, `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. The trace compiled twice (`-isystem` the
model directory, and without it). Stdout matched, including

```
last2=2 30 40 1
last0=0 1
none_last=0
bytes=16
none_bytes=0
last0_bytes=0
```

A null pointer with length 0 printed `empty_bytes=0` and exited 0. A span
whose length is `static_cast<size_t>(-1) / sizeof(int)` printed
`fit=18446744073709551612` and did not print `wrapped=`. One past that
length aborted in the check (`MEM-OOB-READ`,
`std::span::size_bytes size times element size does not fit in size_t`) and
printed no `wrapped=` line.

`struct Z { int a[0]; }` has `sizeof(Z) == 0` under this clang. `size_bytes`
of `span<Z>(&z, 0)` and of `span<Z>(&z, 3)` both aborted with
`FUNC-CONTRACT` and `std::span::size_bytes element size is zero`, and
neither printed a byte count. A static two-int span's `size_bytes()` is a
constant expression equal to `2 * sizeof(int)`. An empty span's
`size_bytes()` is a constant 0.
