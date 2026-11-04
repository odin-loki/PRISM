# chrono duration operator%=

`duration::operator%=` is declared on the existing header
`src/prism/pir/models/cxx/chrono`. The header is still one extensionless
file. The include guard is still `_GLIBCXX_CHRONO`.

This header is not a proof. A remainder assigned through `%=` is not PROVED
and not CLEAN.

## What operator%= does

`operator%=` is a member template of `duration`. The scalar is
`const _Scalar&` with `_Scalar` the same type as `_Rep`, the same constraint
as the free `operator%`. A different scalar type is not an overload, so
that use fails to compile and pir falls back to libstdc++.

The body is `*this = *this % __s;` then `return *this;`. It does not form a
second remainder. The zero divisor check and the check on the most negative
count modulo -1 stay inside `operator%`. Each one is `__prism_check` then
`__builtin_trap`, so `%=` does not store a duration on those paths and does
not store 0. Every other result is the stored count modulo the scalar.

`__duration_same_type` now sits before `duration`, so the member constraint
can name it. Its meaning is unchanged.

## What stayed

`operator%`, `operator/`, `operator*`, `operator+=`, `operator-=`,
`duration_cast`, `abs`, the comparisons, the SI typedefs, and the clocks are
unchanged. `operator*=` and `operator/=` are still not declared. A duration
modulo a duration is still not declared, for `%` and for `%=`.

## Tests

`tests/cpp/test_cxx_chrono.cpp` slices the member. It requires the same-rep
constraint, `*this = *this % __s;`, and `return *this;`. The member has no
`__prism_check`, no `__builtin_trap`, no `_M_count`, and no `__x % __y`.
The header has one `__x % __y`, in `operator%`. The sentence "This is not
PROVED and not CLEAN." now appears fourteen times.

`tests/cxx_models/chrono_trace.cpp` assigns remainders with `%=`: 10 %= 3 is
1, -5 %= 2 is -1, 5 %= -2 is 1, 0 %= 5 is 0, the maximum %= -1 is 0, the
minimum %= -2 is 0, and (10 %= 3) %= 2 is 1. The `mod_eq_zero`,
`mod_eq_min_zero`, and `mod_eq_ovf` arguments take the trapping paths
through `%=`. Against the model they stop at the check before `wrapped=` is
printed. They are not compared with libstdc++, where those paths are
undefined behaviour.
