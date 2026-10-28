# string_view substr

Branch `finish/cxx-string-view-substr`, base `1dd4566ba15e332a21a398049eebbd2c31a2356c`.

`substr` is a member of the existing header
`src/prism/pir/models/cxx/string_view`. That path is a file. A directory of
the same name would make `#include <string_view>` fail with EISDIR. The
include guard is still `_GLIBCXX_STRING_VIEW`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. A slice that stops at `size()` is the length the header
returns. It is not a proof that a caller stayed in range.

## What substr does

`basic_string_view::substr(pos, count)` is `constexpr`. `count` defaults to
`npos`. `pos` defaults to 0. `npos` is

```
static constexpr size_type npos = static_cast<size_type>(-1);
```

When `pos > size()`, the function calls

```
__prism_check(__pos <= _M_len, "MEM-OOB-READ", "std::string_view::substr position past size()");
```

and then `__builtin_trap`. That path has no `return`. It does not return a
view whose pointer starts past the end.

Otherwise the length is `count` when `count < size() - pos`, and
`size() - pos` otherwise. `npos` takes the second arm: it is the maximum
`size_type`, so it is not less than the room that remains. A count whose
sum with `pos` would pass `size()`, including a sum that would wrap, takes
that arm too. The sum is not formed. The length is computed from
`size() - pos`, which is defined because `pos <= size()`. The result does
not extend past `size()`.

The result is `basic_string_view(data + pos, length)`. That constructor
stores the pointer and the length. It does not allocate, and it does not
read a character. `substr(0, 0)` has length 0, so it does not read one.
On an empty view the pointer stays null. On a non-empty view it is the
same pointer the caller passed in. A later position is that pointer plus
`pos`, which is inside the view or one past the last character when
`pos == size()`.

The view still does not own the characters. Destroying the result does
not free them.

## What stayed

The constructors are unchanged. `basic_string_view(const CharT*, count)` is
still `constexpr` and `noexcept`. It stores the pointer and the count. It
does not check null and it does not read the characters. The default
constructor is still an empty view (`size() == 0`, `data() == nullptr`).
The destructor is still defaulted.

`operator[]`, `front`, `back`, `remove_prefix`, and `remove_suffix` are
unchanged.

`starts_with` and `ends_with` are unchanged. An empty needle is still true.
The characters compared are still the overlap, at most `size()`, and a
compared length of 0 does not read the pointer.

The header still does not include `<string>` or `<memory>`. `std::__sv_check`
and `std::__sv_limit` are unchanged.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `compare`, `find`
- `contains`, `at`, iterators, `swap`, `copy`, hash

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited:
`_prism_test_sources` already lists `tests/cpp/test_cxx_string_view.cpp`.
The conformance header array in `tests/cpp/test_qa_conformance.cpp` is
unchanged. `string_view` stays in that list.

## Tests

`tests/cpp/test_cxx_string_view.cpp` reads the header. The pointer-plus-count
constructor line is still the noexcept store. The four prefix bodies are
unchanged. `substr(` is the one new definition. Its body checks
`MEM-OOB-READ` before `__builtin_trap`, and the trap is before the only
`return`. The length is `__count < __room ? __count : __room` with
`__room = _M_len - __pos`. The body does not index `_M_str` and does not
add `pos` and `count`. `compare(` and `find(` are absent from the code.
`string_view_trace.cpp` calls `substr`, `starts_with`, and `ends_with`, and
still does not call `compare` or `find`.

The same driver still compiles that trace twice, against libstdc++ and with
`-isystem` on the model directory, under `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined`. Checked with clang++ 18.1.3 and `-Wall
-Wextra -Werror` as well as those sanitizers. The traces match:

```
sub0=0 1 1
submid=2 qr 1
subnpos=3 3 1
subover=2 rs 1
subend=0 1 1
subz=0 1 1
suball=4 1 1
subempty=0 1 1 0 1
subalive=pqrs
```

`sub0` is `substr(0, 0)`: empty, same pointer, no character printed.
`subnpos` is `substr(1, npos)` and `substr(1)`, both length 3.
`subover` is `substr(2, size_t(-2))`, length 2 (`r` `s`), not a wrapped
length. `subend` is `substr(4, 1)` on a 4-character view: empty, pointer
one past the last character. `subempty` is `substr(0, 0)` and
`substr(0, npos)` on a default view: size 0 and a null pointer.
`subalive` is the caller's buffer after those views are destroyed.

`substr(5, 1)` on that 4-character view aborts both binaries. The model
stderr is `check MEM-OOB-READ: std::string_view::substr position past size()`.
libstdc++ throws `std::out_of_range` and `std::terminate` aborts. The
trace stub aborts inside `__prism_check`, so this run does not reach
`__builtin_trap`. The header still has the trap after the check.

A separate constexpr program static-asserts the in-range lengths and
pointers, including `substr(0, 0)` on an empty view. Without a usable
clang++ the differential case in the doctest is `NOTRUN`. The header read
runs either way.

`testdata/conformance/libc-models/string_view_contracts.cpp` no longer lists
`substr` as omitted. Its harness functions are unchanged. The yml is
unchanged.
