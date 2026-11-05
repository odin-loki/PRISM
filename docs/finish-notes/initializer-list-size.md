# initializer_list size and empty

Branch `finish/cxx-initializer-list-size`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`std::initializer_list::size` was already declared on the extensionless file
`src/prism/pir/models/cxx/initializer_list`. `empty()` was not. A directory
of that name would make `#include <initializer_list>` fail with EISDIR.
The include guard is still `_GLIBCXX_INITIALIZER_LIST` (and `_INITIALIZER_LIST`).
`#pragma GCC system_header`. Namespace `std`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
`size()` being the stored length, and `empty()` being that length compared
with zero, is the result this header defines. It is not PROVED and not CLEAN.

## What size and empty do

`size()` is still

```cpp
constexpr size_type size() const noexcept { return _M_len; }
```

`_M_len` is the length stored by the default constructor and by the
private constructor the compiler calls. The body does not read `_M_array`
and does not load an element. A zero-length list has size 0. That includes
a default-constructed list (`_M_array` is null and `_M_len` is 0) and a
braced list whose compiler-supplied length is 0.

`empty()` is

```cpp
constexpr bool empty() const noexcept { return size() == 0; }
```

It is `size() == 0`. The body does not read `_M_len` or `_M_array`, and it
does not load an element. A zero-length list is empty. A list of three
elements is not.

The comments above both functions say this is not PROVED and not CLEAN.

## What stayed

`begin()`, `end()`, and the free `begin` / `end` are unchanged. `end()` on
a zero-length list is still the same pointer as `begin()`, with no
arithmetic on null and no fabricated element. The destructor is still
defaulted, so destroying a list does not free the compiler-owned array.
The private constructor still stores the pointer and the length and does
not allocate. The header still includes only `<cstddef>`.

Subscript is not declared. The header does not include `<memory>` or
`<algorithm>`.

## What still falls through to libstdc++

Not declared here, so a use fails to compile against this header and pir
lowers that translation unit again with libstdc++ (`extra.cxx_models`).
They are not stubbed. libstdc++ 13's `<initializer_list>` has `size()` and
does not declare `empty()`, so a call to `empty()` is this model. The
header does not declare `data`, reverse iterators, or a subscript.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/initializer_list`
was already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_initializer_list.cpp`
was already in `_prism_test_sources`. No source line was added and none
was dropped. `tests/cpp/test_qa_conformance.cpp` was not edited.
`initializer_list` stays in that header-name array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.
`src/prism/pir/stage.cpp` was not edited.

## Tests

`tests/cpp/test_cxx_initializer_list.cpp` reads the header. `size()` is
`return _M_len` and that body does not name `_M_array`, `begin`, or `end`,
and it has no indirection and no subscript. `empty()` is `return size() == 0`
and that body does not name `_M_len` or `_M_array`. The default constructor
still stores `_M_len(0)`. The sentence `This is not PROVED and not CLEAN.`
appears three times. The sentence `A zero-length list has size 0.` appears
twice. `operator[]` is absent. `#include <memory` and `#include <algorithm`
are absent.

`tests/cxx_models/initializer_list_size_trace.cpp` is
`#include <initializer_list>`. It is compiled with this file ahead of
libstdc++ (`-isystem`). It is not compiled against libstdc++ alone:
that header has no `empty()`. A constexpr zero-length list has size 0 and
`empty()` true. A constexpr three-element list has size 3 and `empty()`
false. At run time the same holds for a default list, a braced empty
list, and `{4, 5, 6}`. A default list of an element type is size 0 and
empty, and constructing it does not copy or read an element. After a
three-element list is built, `size()` and `empty()` add no copy and no
read. The element type's read counter increments only from `get()`, which
those two calls do not use.

`tests/cxx_models/initializer_list_trace.cpp` was not edited. It still
walks with `size()` and does not call `empty()`. clang++ 18.1.3 compiled
it twice (`-isystem` the model directory, and without it) with
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. Stdout matched:

```
empty size=0 eq=1 hits=0
copy_dtors=0 copy_frees=0
still=6
walk=60
empty_walk=0
```

clang++ 18.1.3 compiled the size trace with `-std=c++23 -Wall -Wextra
-Werror -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`, this file ahead of libstdc++. Stdout was

```
zero size=0 empty=1
braced size=0 empty=1
three size=3 empty=0
none size=0 empty=1 copies=0 reads=0
after size=3 empty=0 copies=0 reads=0
```

and the process exited 0. g++ 13.3.0 compiled the same trace without the
sanitizers (`-Wall -Wextra -Werror`) and printed the same lines, exit 0.
That run is not PROVED and not CLEAN.

`prism_tests` was not run, so the `TEST_CASE` bodies were not executed.
The header text checks were applied to this file separately and passed.
Compiling the size trace typechecked `size()` and `empty()`.
