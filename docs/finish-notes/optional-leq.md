# optional less-or-equal

Branch `port/wip-c`, parent `0a9571725` (`main` at this commit). This slice
ports an uncommitted `operator<=` from base `f3e3ea07c` onto the current
header. The base header and its tests do not apply to `main`, which
re-implemented `operator<` and `operator>` and added `nullopt`.

`operator<=` for two `std::optional<T>` of the same `T` is declared on the
existing header `src/prism/pir/models/cxx/optional`. The header is still a
file, not a directory. `operator==`, `operator<`, `operator>`, `swap`,
`emplace`, `nullopt`, `value()`, `operator*`, and `operator->` are
unchanged. The header does not include `<memory>` or `<functional>`.

PIR was not run. The solver was not run. This is not PROVED and not CLEAN.
Ordering two optionals is not a proof that a caller ordered its values,
and it is not a fuzzer `CLEAN` result.

## What changed from the uncommitted patch

The patch returned `!(__y < __x)`. That calls `operator<` on `T`. libstdc++
calls `operator<=` on `T`. So a `T` with `<` and no `<=` compiled against
the model and failed against libstdc++, and the model accepted code the
real library rejects. A `T` with `<=` and no `<` went the other way. The
engaged lines of its trace also differed from libstdc++ (`lt 2 1` here,
`le 1 2` there).

This port compares the stored values with `operator<=`, in libstdc++'s
order. The trace now matches libstdc++ line for line, and both `T` cases
compile or fail the same way against both headers.

## What less-or-equal does

One function template, the same `T`, declared a friend with the other
comparisons and defined after `operator>`:

```cpp
template <typename _Tp>
bool operator<=(const optional<_Tp>& __x, const optional<_Tp>& __y) {
    if (!__x._M_engaged) return true;
    if (!__y._M_engaged) return false;
    return __x._M_storage._M_value <= __y._M_storage._M_value;
}
```

- A disengaged left is true, so two disengaged optionals compare
  less-or-equal. That return is before `_M_value` is named. It does not
  read the inactive `T` and does not construct `T{}`.
- An engaged left and a disengaged right is false. That return is also
  before `_M_value` is named.
- Two engaged optionals compare the stored values with `operator<=` on
  `T`. The result is a `bool`. It is not `T{}`. `value()` and `operator*`
  are not used, so a disengaged comparison is not a `CXX-OPTIONAL-NULL`
  check.

The body is a function template, so it is not instantiated just because an
optional is stored. A `T` with no `operator<=` can still be held.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `operator>=` and `operator<=>`
- comparison of `optional<T>` with `optional<U>` when `T` and `U` differ
- comparison with a bare `T`
- `hash`, `in_place`, `make_optional`, and assignment from `T`

## Tests

`tests/cpp/test_cxx_optional.cpp` reads the header without clang++. The
sentence `This is not PROVED and not CLEAN.` now appears eight times, and
that lock is 8. A new test checks that `operator<=` is after `operator>`
and before the free `swap`. Its body returns true on a disengaged left,
then false on a disengaged right, then compares `_M_value` with `<=`. The
first `_M_value` is on that last line. The body does not call `<` on `T`,
does not name `value()`, `operator*`, `__prism_check`, `T{}`, `_Tp{}`, or
`_Tp()`. After comments are stripped, `bool operator<=(` appears twice
(the friend and the definition), and `operator>=`, `operator<=>`, and
`hash` are absent.

The same driver compiles `tests/cxx_models/optional_leq_trace.cpp` twice,
against libstdc++ and with `-isystem` on `src/prism/pir/models/cxx`, under
`-std=c++23 -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1 -w`. The dependency dump names the model
header `optional`. `Box` has no default constructor. Its `operator<` prints
`lt`, `operator<=` prints `le`, and `operator>` prints `gt`. Both binaries
print the same 35 lines, from `ee le=1` to `ii le=1`. Two empty `Box`
optionals print `ee le=1` and `ee rev=1` with no `le` line before them.
`Box(1)` against `Box(2)` prints `le 1 2` then `ord le=1`. No line starts
with `lt` or `gt`. The driver checks `lib == want` and `mod == lib`.

A second program compares `optional<int>` with `optional<long>` using
`operator<=`. It compiles against libstdc++ and fails against this header:
`_Tp` cannot be both `int` and `long`.

`OnlyLt` provides `operator<` and not `operator<=`. Comparing two
`optional<OnlyLt>` fails against libstdc++ and against this header.
`OnlyLe` provides `operator<=` and not `operator<`. That program compiles
against both and both binaries return 0.

clang++ 23 against libstdc++ 14 (`__GLIBCXX__ 20240908`) produced those
lines. ASan stayed quiet and both trace binaries returned 0. The trace
also compiles with `-std=c++23 -Wall -Wextra -Werror` against this header.

`tests/cxx_models/optional_trace.cpp` still does not call `operator<=`.
Its comment now says `operator>` and `operator<=` are declared.
