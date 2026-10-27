# ratio slice

Branch `finish/cxx-ratio`, base `b58ed8da4042896363fbbe540d29692207d1a1ea`.
`std::ratio` is a header file, `src/prism/pir/models/cxx/ratio`. A directory
of that name would make `#include <ratio>` fail and every such unit would
fall back to libstdc++.

The solver was not run. `cxx_ratio_true.yml` records the contract the task
asks for. `expected` is the YAML boolean `true`. This header is not a proof
of a program that merely names a ratio.

## What the model covers

`std::ratio<Num, Den>` stores the reduced compile-time fraction. The sign
is on the numerator and the denominator is positive. `ratio<2,4>::num` is
1 and `den` is 2. `Den == 0` is ill-formed: `static_assert(_Den != 0,
"denominator cannot be zero")`. That specialization has no `num` and no
`den`, so it is not `0/1` and not a runtime zero. An absolute value that
does not fit in `intmax_t` (`INTMAX_MIN`) is ill-formed the same way.

`ratio_add`, `ratio_subtract`, `ratio_multiply`, and `ratio_divide` are
aliases of the reduced result. Common factors are cancelled before the
multiply. A product or a sum that does not fit in `intmax_t` instantiates
a specialization with a `static_assert` and no `value` member. The header
does not call `__builtin_mul_overflow` or `__builtin_add_overflow`.

`nano`, `micro`, `milli`, `centi`, `deci`, `deca`, `hecto`, `kilo`, `mega`,
and `giga` are aliases of `ratio<...>`.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). They
are not stubbed:

- `ratio_equal`, `ratio_not_equal`, `ratio_less`, `ratio_less_equal`,
  `ratio_greater`, `ratio_greater_equal`
- `atto`, `femto`, `pico`, `tera`, `peta`, `exa`

The header does not `#include <chrono>`. A translation unit that needs one
of those names, including through `<chrono>`, fails against this stand-in
and falls back. A zero denominator and an overflowing product fail to
compile. That failure is the fallback, not a planted defect and not a
wrapped value.

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` lists
`src/prism/pir/models/cxx/ratio`, so the embedded name is `cxx/ratio`.
`tests/cpp/test_qa_conformance.cpp` names `ratio` in the header array. The
iostream checks and the `deque/deque` skip in that test are unchanged.

## Tests

`tests/cpp/test_cxx_ratio.cpp` includes the model by path, the same way
`test_cxx_iostream.cpp` includes its header. `ratio<2,4>::num` is 1 and
`den` is 2. The header text has the zero-denominator `static_assert` and
the overflow specialization has no stored value.

`tests/cxx_models/ratio_trace.cpp` is `#include <ratio>` and prints `num`
and `den` for a few small ratios, including `ratio<1,2>`.

`testdata/conformance/prism/cxx/cxx_ratio_true.cpp` returns
`ratio<1,2>::num + ratio<1,2>::den` (`no-overflow`, `true`). There is no
`cxx_ratio_false`: a zero denominator or an overflowing product does not
compile, so it is not a defect finding. The in-house pair check
(`n_true == n_false`, and a `_true` stem needs a `_false` stem) therefore
sees this task alone.

`testdata/conformance/libc-models/ratio_contracts.cpp` is the text the
embedded-header check looks for (`#include <ratio>`). It only reads `num`
and `den` of `ratio<1,2>` and `ratio<2,4>`. It has no sidecar: a
libc-models task must list both a true and a false function, and a false
ratio result here would be a compile failure, not a planted defect.

Checked with clang++ 18, `-std=c++23 -Wall -Wextra -Wpedantic -Werror`.
The model translation unit and `tests/cpp/test_cxx_ratio.cpp` compile.
`ratio<1,0>` as a complete type fails the zero-denominator `static_assert`.
`ratio_multiply` of two `ratio<10^12,1>` fails `product does not fit in
intmax_t`. `ratio_add` of `ratio<INTMAX_MAX,1>` and `ratio<1,1>` fails
`sum does not fit in intmax_t`. `tests/cxx_models/ratio_trace.cpp` compiled
twice (`-isystem` the model directory, and without it) and the stdout
matched, including `ratio<1,2>` as `1/2`. No engine configure, no
`prism_tests`, no conformance run.
