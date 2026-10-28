# optional compare

Branch `finish/cxx-optional-compare`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.
`operator==` and `operator<` for two `std::optional<T>` of the same `T` are
declared on the existing header `src/prism/pir/models/cxx/optional`. The
header is still a file, not a directory. `value()`, `value_or`, `and_then`,
`or_else`, `transform`, and `reset` are unchanged. `emplace` and `hash` stay
omitted. `_Prism_optional_kind<optional<_T>>` stays after the class. The
header does not include `<memory>` or `<functional>`.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run. This is not PROVED and not CLEAN. Ordering two optionals is
not a proof that a caller ordered its values, and it is not a fuzzer
`CLEAN` result.

## What the operators do

Both are function templates on `const optional<T>&`. They are friends so
the body can read the engaged flag and, only when both optionals are
engaged, the stored `T`. The bodies are not instantiated just because an
optional is stored, so a `T` with no `operator==` can still be held.

- Two disengaged optionals compare equal. Neither is less than the other.
  Those paths return before the stored `T` is named. They do not construct
  `T{}` and they do not invent a value.
- A disengaged optional is less than an engaged one. An engaged optional is
  not less than a disengaged one. Those paths do not read the inactive `T`
  and do not read the engaged `T` either.
- Two engaged optionals compare the stored values with `operator==` and
  `operator<`. The comparison does not copy `T` and does not build a
  different value.
- `value()` and `operator*` are not used. A disengaged comparison is not a
  `CXX-OPTIONAL-NULL` check. `value()` on a disengaged optional is still
  `__prism_check` with that class, and that path still does not return a
  value.

`operator!=` is not a separate declaration. It is the rewritten negation of
`operator==`, so it uses the same rules. `operator>`, `operator<=`,
`operator>=`, and `operator<=>` are not declared.

## What still falls through to libstdc++

Not declared, so a use fails to compile against this header and the unit
falls back to libstdc++. They are not stubbed:

- `emplace`
- `hash`
- `operator<=>`, `operator>`, `operator<=`, and `operator>=`
- comparison of `optional<T>` with `optional<U>` when `T` and `U` differ
- comparison with a bare `T`
- `nullopt`, `in_place`, `swap`, `make_optional`, and assignment from `T`

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`_prism_test_sources` already lists `tests/cpp/test_cxx_optional.cpp`. The
conformance header array was not edited.

## Tests

`tests/cpp/test_cxx_optional.cpp` reads the header without clang++. The
header is a file. It does not include `<memory>` or `<functional>`. The
sentence `This is not PROVED and not CLEAN.` appears three times. The
`_Prism_optional_kind` specialization is after `class optional` and before
the two operator definitions. `operator==` returns on a disengaged optional
before it names `_M_value`. `operator<` returns when the right-hand optional
is disengaged, and when the left-hand one is, before either names
`_M_value`. Neither body calls `value()`, `operator*`, or `__prism_check`,
and neither builds `T{}`. After comments are stripped, `emplace`, `hash`,
and `operator<=>` are absent. `reset`, `value_or`, `and_then`, `or_else`,
`transform`, and `CXX-OPTIONAL-NULL` remain.

The same driver writes a program and compiles it twice, against libstdc++
and with `-isystem` on `src/prism/pir/models/cxx`, under `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined`. The dependency dump
names the model header `optional`. `Box` has no default constructor.
`operator==` and `operator<` print the two stored ints, so a read of an
inactive `Box` would print before any engaged pair.

Two empty `Box` optionals print `ee eq=1`, `ee lt=0`, `ee rev=0` and do not
print `eq` or `lt`. An empty `Box` compared with `Box(1)` prints `de eq=0`,
`de lt=1`, `de rev=0` and still does not print a call. `Box(1)` compared
with `Box(1)` prints `eq 1 1` then `same eq=1`, then `lt 1 1` twice around
`same lt=0` and `same rev=0`. `Box(1)` compared with `Box(2)` prints
`eq 1 2`, `ord eq=0`, `lt 1 2`, `ord lt=1`, `lt 2 1`, `ord rev=0`. A const
empty `Box` against a const `Box(2)` prints `cde eq=0`, `cde lt=1`,
`cde rev=0`. Two const `Box(2)` optionals print `eq 2 2`, `c22 eq=1`,
`lt 2 2`, `c22 lt=0`, `lt 2 2`, `c22 rev=0`. An empty `optional<int>`
against one that holds 0 prints `iz eq=0`, `iz lt=1`, `iz rev=0`. Two zeros
print `zz eq=1`, `zz lt=0`, `zz rev=0`. Zero against 1 prints `01 eq=0`,
`01 lt=1`, `01 rev=0`. `-1` against 0 prints `neg eq=0`, `neg lt=1`,
`neg rev=0`. An empty `optional<int>` compared with itself prints
`ii eq=1`, `ii lt=0`. `operator!=` then prints `ee ne=0`, `de ne=1`,
`eq 1 1`, `same ne=0`, `eq 1 2`, `ord ne=1`, `ii ne=0`, `iz ne=1`,
`zz ne=0`. No line is `eq 0 0` or `lt 0 0`.

A second program compares `optional<int>` with `optional<long>`. It
compiles against libstdc++ and fails against this header: the candidate is
ignored because `_Tp` cannot be both `int` and `long`.

clang++ 18.1.3 produced the same compare lines from libstdc++ and from this
header. ASan stayed quiet and both binaries returned 0. The older optional
trace, the `and_then` trace, the `or_else` trace, and the `transform` trace
still match. `argv` `empty` still aborts with `CHECK CXX-OPTIONAL-NULL` and
prints no value. A `P` with no `operator==` still compiles: an empty
optional prints `empty 0`, and an engaged one prints `arrow 7 8`. Without a
usable clang++ the differential cases are `NOTRUN`. The header read runs
either way. `CMakeLists.txt` is unchanged. `prism_tests` was not run.

`tests/cxx_models/optional_trace.cpp` still does not call `operator==` or
`operator<`. Its comment now says those operators are declared, and that
`emplace` and `hash` are not.
