# cmath fma, hypot, remainder, fmod, fdim

`<cmath>` is still one extensionless file, `src/prism/pir/models/cxx/cmath`.
The include guard is `_GLIBCXX_CMATH`. `sqrt`, `fabs`, `abs`, the classifiers,
`copysign`, `fmin`, `fmax`, `floor`, `ceil`, and `trunc` are unchanged.

This header is not a proof of numeric accuracy. `fma`, `hypot`, `remainder`,
`fmod`, and `fdim` are not PROVED and not CLEAN.

## What this slice adds

Each function is declared for `float`, `double`, and `long double`. An
integer argument is not an overload.

`fdim(x, y)` returns `x` or `y` when that argument is a NaN. When `x <= y`
the result is `+0`. Otherwise the result is `x - y`, so an infinite
difference stays infinite.

`fmod` and `remainder` return a NaN argument unchanged. A zero divisor
returns a NaN (`__builtin_nan` / `nanf` / `nanl`), not zero. Any other pair
is the matching builtin. An IEEE infinity stays an infinity when that is
the builtin result, and is not replaced with 0.

`hypot` returns `+infinity` when either argument is an infinity, including
when the other argument is a NaN. A NaN with a finite partner is returned
as that NaN. A pair of zeros is `+0`. Otherwise the result is
`__builtin_hypot` / `hypotf` / `hypotl`.

`fma` returns a NaN argument as that NaN, `x` before `y` before `z`.
Otherwise the result is `__builtin_fma` / `fmaf` / `fmal`. An infinity stays
an infinity when that is the IEEE result. `inf * 0` stays a NaN.

`<math.h>` macros of these names are undef'd next to `floor`, `ceil`, and
`trunc`. The header still does not include `<math.h>`. `sin`, `cos`, `tan`,
`exp`, `log`, and `pow` stay undeclared.
