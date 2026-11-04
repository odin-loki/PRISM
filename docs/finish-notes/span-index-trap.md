# span index, front, and back traps

`std::span::operator[]` with an index `>= size()` is `__prism_check` with
class `MEM-OOB-READ` and message `std::span::operator[] index is outside
the span`, then `__builtin_trap`, before the element address is formed.
An empty span's `operator[](0)` takes that path and does not load.

`front()` on an empty span is `__prism_check` with class `FUNC-CONTRACT`
and message `std::span::front of an empty span`, then `__builtin_trap`,
before the address is formed. `back()` is the same with message
`std::span::back of an empty span`. Neither returns a fabricated element.

`subspan` and `first` still use `__glibcxx_assert`. This is not PROVED
and not CLEAN.
