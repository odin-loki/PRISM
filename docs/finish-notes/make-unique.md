# make_unique slice

Branch `finish/cxx-make-unique`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::make_unique` is declared on the existing file
`src/prism/pir/models/cxx/memory`. The header is still a file, not a
directory. The include guard is `_GLIBCXX_MEMORY`, so libstdc++'s own
`<memory>` is not also included. The file is a `#pragma GCC system_header`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml files
record the contract each harness asks for. `expected` is a YAML boolean.
Nothing here is `PROVED`.

## What the model covers

`std::unique_ptr` is unchanged. `reset` still stores the new pointer and
frees the previous object once (`::operator delete` for one object, one
array delete for `unique_ptr<T[]>`). `release()` still stores nullptr and
does not free. `operator*` on an empty `unique_ptr` is still
`__prism_check` and then `*_M_ptr`. It does not invent an object.

`make_unique<T>(args...)` allocates with `new T(...)` (the arguments are
forwarded with `static_cast<Args&&>`) and returns a `unique_ptr<T>` that
owns that object. There is no allocator argument and no allocator type in
this header.

`make_unique<T[]>(n)` value-initializes `n` elements with `new T[n]()` and
returns a `unique_ptr<T[]>` that frees them with that array delete. `n == 0`
uses that same count. It does not allocate one stand-in element. A count
that does not fit (`n > SIZE_MAX/sizeof(T)`, or `sizeof(T) == 0`) calls
`__prism_check` with class `MEM-OOB-WRITE` and message
`make_unique array allocation byte count overflows`, then `__builtin_trap`,
before `new`. The count passed to `new` is `n`. It is not a wrapped product.

`_Prism_make_uniq` only picks the return type so the two overloads do not
collide. A known bound has no nested type, so `make_unique<T[N]>` does not
match. That does not instantiate `unique_ptr<T[N]>`.

## What falls through to libstdc++

Not declared, so a use fails to compile against this header and the pir
stage lowers that translation unit again with libstdc++. They are not
stubbed:

- `shared_ptr`, `weak_ptr`, `allocator`, `pointer_traits`, `uninitialized_*`
- `make_unique_for_overwrite`
- a deleter other than `default_delete` (static_assert)
- `unique_ptr` of `void`, of a reference, or of a bounded array
- `make_unique<T[N]>` (no matching function)

Checked with clang++ 18.1.3, `-std=c++23 -fsyntax-only`, the model directory
first on the include path. `shared_ptr`, `weak_ptr`, `allocator`,
`pointer_traits`, `uninitialized_copy`, and `make_unique_for_overwrite` are
"no member" / "no template". `make_unique<int[4]>()` is "no matching
function".

## Tests

`tests/cpp/test_cxx_memory.cpp` reads the header. Empty `operator*` is still
a check and `return *_M_ptr`. Object `reset` still has one
`::operator delete(` after the pointer is replaced. Array `reset` still has
one array delete. The header still has each of those frees once.
`make_unique` for one object returns
`unique_ptr<_Tp>(new _Tp(static_cast<_Args&&>(__args)...))` and does not
check or trap. The array overload's `__prism_check` and `__builtin_trap`
are before `new _Elem[__n]()`. The condition is
`sizeof(_Elem) == 0 || __n > (SIZE_MAX / sizeof(_Elem))`. The body does not
allocate `new _Elem[1]` and does not multiply `__n` into the element count.
`make_unique_for_overwrite(` is absent. `class shared_ptr` is absent.
`shared_ptr` remains in the comment, so a unit that names it still fails
here and falls back.

`tests/cxx_models/memory_trace.cpp` is `#include <memory>`. Checked with
clang++ 18.1.3 and libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -O1`. Compiled
twice (`-isystem` the model directory, and without it). Stdout matched:

```
empty 1
own 42
 c1 c2 d1 reset 2
 d2
 c3 move src_empty=1 dst=3
 d3
 c4 c5 d5 move-assign src_empty=1 dst=4
 d4
 c6 release empty=1
 d6
arr 7 8 9
arr-move src_empty=1 dst=7
arr-reset empty=1
mk 11
mk0 0
 c12 mkE 12
 d12
mkarr 0 0 0
mkzero empty=0
 z z mktwo 0 0
 Z Z
```

`mk 11` is `make_unique<int>(11)`. `mk0 0` is `make_unique<int>()`
(value-initialized). `mkarr 0 0 0` is `make_unique<int[]>(3)`.
`mkzero empty=0` is `make_unique<Z[]>(0)`: `Z`'s constructor did not run,
and `get()` is not nullptr, the same as libstdc++ `new Z[0]()`.
`mktwo` is two value-initialized `Z` elements. A move-only `M` constructed
as `make_unique<M>(M(3))` prints `moved 3` against both headers.

`argv` `huge` is `make_unique<int[]>(SIZE_MAX)`. The model aborts with
`MEM-OOB-WRITE: make_unique array allocation byte count overflows` and
prints no `wrapped=` line. libstdc++ under ASan aborts with
`requested allocation size 0xffffffffffffffff` and also prints no
`wrapped=` line. The model does not reach that allocation.

`testdata/conformance/prism/cxx/cxx_make_unique_true.cpp` owns one `int`,
calls `make_unique<int[]>(0)` and does not index it, and reads two
value-initialized `int`s (`memsafety`, `expected: true`).
`cxx_make_unique_false.cpp` calls `make_unique<int[]>(SIZE_MAX)` when the
input is non-zero (`expected: false`, witness `[1]`). Both `expected`
values are YAML booleans. The failing class is `MEM-OOB-WRITE`, which
`memsafety` includes.

## Wiring

`CMakeLists.txt` was not edited. `_prism_test_sources` already lists
`tests/cpp/test_cxx_memory.cpp`. `PRISM_PIR_MODELS` already lists
`src/prism/pir/models/cxx/memory`. `tests/cpp/test_qa_conformance.cpp`
already lists `"memory"`. That header array was not edited. `stage.cpp`
was not edited. It already puts the embedded `cxx/` directory on the
include path.
