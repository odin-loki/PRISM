# iterator empty for arrays

Ported from `5300b1acc` onto main, where PR #15 had already landed
`size`, `data`, `cdata`, `ssize` (arrays and container lvalue) and
`empty` for a container lvalue. This slice adds only the two array
overloads of `std::empty` to `src/prism/pir/models/cxx/iterator`.

For `N > 0` the parameter is `T (&)[N]`, unnamed, constrained with
`requires true` like the other array overloads so it is preferred over
the libstdc++ `bits/range_access.h` overload:

```cpp
template <typename _Tp, size_t _Nm>
    requires true
constexpr bool empty(_Tp (&)[_Nm]) noexcept {
    return false;
}
```

`N == 0` is the reference constrained with `sizeof(_Arr) == 0`, also
unnamed, and returns true:

```cpp
template <typename _Arr>
constexpr bool empty(_Arr&) noexcept
    requires(sizeof(_Arr) == 0)
{
    return true;
}
```

Neither body names an element, so neither loads. Neither calls size,
begin, or end. No `__prism_check`, no `__builtin_trap`, and no include
was added. A pointer argument does not match either overload, and the
container overload does not match an array (`__t.empty()` is not
well-formed), so there is no ambiguity. A pointer still fails to compile
and pir falls back to libstdc++. This is not PROVED and not CLEAN.

`tests/cpp/test_cxx_iterator.cpp` locks both signatures, `empty(` six
times (two array overloads plus four spellings on the container
overload), `requires(sizeof(_Arr) == 0)` seven times, and bodies with no
star, subscript, `__arr`, begin, end, size, data, `__prism_check` or
`__builtin_trap`. The star count and the count of
`This is not PROVED and not CLEAN.` are unchanged.
