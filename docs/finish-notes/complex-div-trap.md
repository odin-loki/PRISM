# complex division trap

Branch `finish/cxx-complex-div-trap`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

Division by a zero complex in `src/prism/pir/models/cxx/complex` was
`__prism_check` with class `FUNC-CONTRACT` and message
`complex division by zero`, then `return *this`. That return handed back
the dividend after a failed check. The zero arm is now `__builtin_trap`.
It does not return `*this`, does not divide, does not produce a quotient,
and does not store into the real or imag members.

The header is still a file, not a directory. The include guard is
`_GLIBCXX_COMPLEX`. The zero-divisor path is not `PROVED` and not `CLEAN`.

PIR was not run. The solver was not run. cmake, ninja, and `prism_tests`
were not run.

## What the zero arm does

`operator/=` still computes

`__zero = (real == T() && imag == T())`.

`-0` compares equal to `0`, so `(-0, -0)` is the same divisor. The check
stays unconditional:

```c
__prism_check(!__zero, "FUNC-CONTRACT", "complex division by zero");
```

On the zero arm the next statement is `__builtin_trap()`. The quotient,
the stores of `_M_real` and `_M_imag`, and `return *this` sit in the
`else`. A nonzero divisor still uses the schoolbook quotient
`(ac+bd)/(c²+d²)` and `(bc-ad)/(c²+d²)`. `(1, 2) / (0, 1)` is `2 - i`.

`operator/(complex, complex)` still does `x /= y` and returns `x`. A zero
`y` traps inside `operator/=`, so that return is not reached.

Scalar `operator/(complex, T)` is still `return x / complex(y)`. A zero
`y`, including `-0`, builds `complex(y, 0)` and hits the same check and
the same trap. There is one check text in the header. The scalar function
does not carry its own check or trap. `operator/(T, complex)` is
unchanged and still builds `complex(x)` as the dividend.

`__builtin_trap` is the same stand-in `abs` already uses under
`_MSC_VER && !__clang__`. That function was not edited. The comment above
it now also names a zero divisor.

## What stays

`abs`, `norm`, `operator+`, `operator+=`, `operator-`, `operator-=`,
`operator*`, `operator*=`, `conj`, and `operator==` are unchanged.

Not declared, so a use fails to compile against this header and pir falls
back to libstdc++ (`extra.cxx_models`). They are not stubbed:

- `exp`, `log`, `log10`, `pow`, `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`
- `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`
- `arg`, `polar`, `proj`

## Wiring

`src/prism/pir/stage.cpp` was not edited. `src/prism/bmc.cpp` was not
edited. `CMakeLists.txt` was not edited. `tests/cpp/test_cxx_complex.cpp`
was already one `_prism_test_sources` line. No test source line was added
or removed. `tests/cpp/test_qa_conformance.cpp` was not edited.
`testdata/conformance/libc-models/complex_contracts.cpp` and its yml were
not edited. `complex_div0_false` still divides by `(0, 0)`. The check
still runs before the trap. `tests/cxx_models/complex_trace.cpp` was not
edited and still does not divide by zero.

## Tests

`tests/cpp/test_cxx_complex.cpp` reads the header. The `operator/=` body
has the `FUNC-CONTRACT` check, then `if (__zero)`. The brace arm of that
`if` contains `__builtin_trap` and does not contain `return *this`, a
store of `_M_real` or `_M_imag`, or a quotient. The text from the check
to that `if` does not contain `return *this`. The denominator, the two
stores, the division by `__den`, and the one `return *this` are after
that arm. The scalar `operator/(const complex<T>&, const T&)` body is
`return __x / complex<_Tp>(__y)` and has no check and no trap. The old
`if (__zero) return *this;` text is absent. The message
`complex division by zero` occurs once. The header says the zero-divisor
path is not `PROVED` and not `CLEAN`.

The same case still divides `(1, 2)` by `(0, 1)` and by `2.0`. Those
checks pass (`ok != 0`, class `FUNC-CONTRACT`) and the quotients are
`2 - i` and `0.5 + i`. It does not execute a zero divisor: that path
traps. The abs and norm cases are unchanged, except the abs source case
no longer requires `if (__zero) return *this;`.

Checked with clang++ 18.1.3 (`-std=c++23 -Wall -Wextra -Wpedantic
-Werror`), at `-O0` and `-O2`. No warnings. The doctest translation unit
was compiled on its own, with `PRISM_SOURCE_DIR` pointing at this tree,
and its 403 assertions passed. `prism_tests` was not built.

A by-path include of the header, with a `__prism_check` that prints and
returns, printed no `quot` line. `(1, 2) / (0, 0)`, `(1, 2) / (-0, -0)`,
`(1, 2) / 0.0`, and `(1, 2) / -0.0` each printed
`CHECK FUNC-CONTRACT: complex division by zero` and exited with SIGILL
(`rc=132`). The same holds at `-O2`. A nonzero run printed `q 2 -1`,
`half 0.5 1`, `divassign 2 -1`, `add 1.5 0`, `sub 0 0`, `mul -2 1`,
`abs0 0`, `abs34 5`, `norm0 0`, `norm34 25`, `conj 1 -2`, and `eq 1`.
