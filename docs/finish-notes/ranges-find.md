# ranges find

`std::ranges::find`, `std::ranges::find_if`, and `std::ranges::find_if_not`
are declared on `src/prism/pir/models/cxx/ranges`. The header is still a
file. It still does not include `<algorithm>`, `<vector>`, `<iterator>`,
`<memory>`, `<string>`, or `<span>`. `views::take` and `views::drop` are
unchanged. `ranges::sort` is still not declared.

`find` of a `T(&)[N]` with `N >= 1` returns a pointer to the first element
that compares equal with `==`, or one past the last. The subscript stays
below `N`. One past the last adds the bound. `N == 0` is a second overload
constrained with `sizeof` of the array equal to 0. It returns the decayed
pointer and does not load an element. A pointer is not a range: there is
no pointer overload, and the zero-length overload does not match a pointer.

`find_if` and `find_if_not` take `bool (*)(const T&)`. A null predicate is
`__prism_check` with class `FUNC-CONTRACT` (`std::ranges::find_if null
predicate`, `std::ranges::find_if_not null predicate`), then
`__builtin_trap`, before a call and before a load. `N == 0` returns the
decayed pointer and does not call the predicate. A lambda that is not a
function pointer, and an iterator pair, fail to compile against this
header and pir falls back to libstdc++.

This is not a proof that an iterator stays in range. This is not PROVED
and not CLEAN.
