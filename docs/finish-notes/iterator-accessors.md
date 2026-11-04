# iterator size, empty, data, cdata, ssize, iter_swap, iter_move

`std::size`, `std::empty`, `std::data`, `std::cdata`, `std::ssize`,
`std::iter_swap`, and `std::iter_move` live in the existing extensionless
file `src/prism/pir/models/cxx/iterator`. A directory of that name would
make `#include <iterator>` fail with EISDIR. The include guard is still
`_GLIBCXX_ITERATOR`. `begin`, `end`, `distance`, `next`, `prev`, and
`advance` are unchanged.

Nothing here is `PROVED`. Nothing here is `CLEAN`. This is not a proof
that an iterator stays in range, and it is not a proof that a pointer
addresses an array.

## What the new overloads return

`size` of a `T (&)[N]` with `N >= 1` returns the bound and does not load
an element. `N == 0` is the `sizeof == 0` overload and returns 0. A
pointer does not match. An lvalue with a `size()` member returns that
member once. The body does not invent 0.

`empty` is only the lvalue with an `empty()` member. The body returns
that member once. It does not call `size` and does not invent `false`.
`std::empty` for an array is not declared, so that call fails to compile
here and pir falls back.

`data` of a `T (&)[N]` returns `__arr` (array-to-pointer decay, not a
load). `N == 0` returns the decayed pointer. An lvalue with a `data()`
member returns that member once and does not dereference it.

`cdata` is the array overloads only. The bound overload returns
`const T*`. `N == 0` returns the decayed pointer. Neither body loads an
element.

`ssize` of an array is the bound as `ptrdiff_t`. A bound greater than
`PTRDIFF_MAX` is `__prism_check` with class `FUNC-CONTRACT` and message
`std::ssize array bound does not fit in ptrdiff_t`, then `__builtin_trap`.
That path does not cast the bound. `N == 0` returns 0. The container
overload reads `size()` once. A value greater than `PTRDIFF_MAX` is
`__prism_check` with class `FUNC-CONTRACT` and message
`std::ssize value does not fit in ptrdiff_t`, then `__builtin_trap`.

`iter_swap` is two pointers of the same type. A null pointer is
`__prism_check` with class `PTR-NULL-DEREF` and message
`std::iter_swap null pointer`, then `__builtin_trap`, before a load or a
store. The same pointer twice returns without inventing a value. Two
different non-null pointers exchange the stored `T` values.

`iter_move` is one pointer. A null pointer is `__prism_check` with class
`PTR-NULL-DEREF` and message `std::iter_move null pointer`, then
`__builtin_trap`, before a load. A non-null pointer returns the `T`
stored through that pointer.

Array overloads that libstdc++ also declares are constrained with
`requires true` so they stay distinct from `bits/range_access.h` if that
header is also visible. `reverse_iterator` stays omitted.
