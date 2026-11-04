# algorithm copy_backward

Branch `port/wip-b`, base `main` at `0a9571725`.

`<algorithm>` stays one extensionless file, `src/prism/pir/models/cxx/algorithm`.
The include guard is `_GLIBCXX_ALGORITHM`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. Nothing here is `PROVED`. Nothing here is `CLEAN`.
After a failed check the path traps. It does not continue into a read, a
write, or a fabricated success.

## What this slice adds

The earlier algorithms are unchanged.

`std::copy_backward` is one overload:

`_Tp* copy_backward(_Tp* first, _Tp* last, _Tp* dest)`

There is no second overload. `T` is deduced from the three pointers. A
`const int*` source and an `int*` destination do not match. That call fails
to compile against this header and the pir stage falls back to libstdc++.
The element type is not required to be equality-comparable or
less-than-comparable.

When `last < first`, the function calls

`__prism_check(0, "FUNC-CONTRACT", "std::copy_backward last is before first")`

and then `__builtin_trap`. That path does not read and does not write.

When `first == last`, the function returns `dest`. That path does not read
and does not write, even when `dest` or the source is null. The empty check
is before the null-destination check.

A null `dest` on a non-empty range calls

`__prism_check(0, "PTR-NULL-DEREF", "std::copy_backward null destination")`

and then `__builtin_trap`. That path does not write.

[alg.copy] requires that `dest` is not in `(first, last]`. A `dest` in that
range calls

`__prism_check(0, "FUNC-CONTRACT", "std::copy_backward dest lies in (first, last]")`

and then `__builtin_trap`. That path does not write. The scan starts at
`last` and steps back to just after `first`. It compares pointers with `==`
and does not read an element. `dest == last` is caught. `dest == last + 1`,
a shift right by one inside one array, is valid and is copied.

Otherwise the loop is `--__src`, `--__out`, `*__out = *__src` until
`__src == first`. The element at `last` is not read. `dest` itself is not
written. The result is the start of the written range, `dest - (last -
first)`. The function does not allocate.

## What stays omitted

`sort`, `for_each`, `fill_n`, `count_if`, and every other algorithm are not
declared. Naming one fails to compile and the pir stage falls back to
libstdc++ (`extra.cxx_models`).

## Wiring

No new `tests/cpp` file was added, so `CMakeLists.txt` was not edited.
`src/prism/pir/stage.cpp` and `tests/cpp/test_qa_conformance.cpp` were not
edited.

## Tests

`tests/cxx_models/algorithm_trace.cpp` adds constant checks and printed lines
for a full copy into a separate buffer, a copy that leaves the element at
`last` of the destination unchanged, an empty range, empty ranges with a
null source, a null destination and both null, one element, `PredOnly`
(no default constructor, no `operator==`), an adjacent copy onto the array
right after the source, and a shift right by one inside one array.

`argv` `invert-copy-backward`, `null-copy-backward`, `overlap-copy-backward`
(`dest` in the middle of the range) and `overlap-copy-backward-last`
(`dest == last`) abort with the matching `CHECK` line and do not print
`wrote`, `read`, or `value=`.

`tests/cpp/test_cxx_algorithm.cpp` reads the header: inverted range, then
empty, then null destination, then the overlap scan, then the copy loop. No
`*__` before the copy loop. No `++`. The name occurs once. In a constant
expression an inverted range, a null destination and `dest == last` are
ill-formed at `__prism_check`. An empty range is a constant expression.
`copy_backward(int*, int*, int*)` compiles against libstdc++ and this
header. `copy_backward(const int*, const int*, int*)` compiles against
libstdc++ and fails against this header.

Checked with clang++ 23 (`~/opt/llvm23`), `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1 -Wall -Wextra
-Werror`. Stdout of the model binary matched libstdc++ (117 lines). The
`copy_backward` lines are:

```
copy_backward_all=9,4,1,4 end=1 src=9,4,1,4
copy_backward_span=9,4,1,8 end=1
copy_backward_empty=8,7,6,5 end=1
copy_backward_one=8 end=1
copy_backward_null=1
copy_backward_null_src=8,7,6,5 end=1
copy_backward_null_dst=1 src=9,4,1,4
copy_backward_pred=3,1,3,5 copy_backward_pred_empty=4,5
copy_backward_adj=1,2,3,1,2,3 end=1
copy_backward_shift=1,1,2,3 end=1
```
