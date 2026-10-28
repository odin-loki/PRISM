# system_error equality

Branch `finish/cxx-system-error-equal`, base `fc5655e019dc4f99d49cc78713c3849bf82c2dc1`.

`operator==` and `operator!=` live on the existing header
`src/prism/pir/models/cxx/system_error`. That path is a file. A directory of
the same name would make `#include <system_error>` fail with EISDIR. The
include guard is `_GLIBCXX_SYSTEM_ERROR`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
Nothing here is `PROVED`.

## What equality does

`std::error_code` still stores an int value and an int category id.

`operator==` is true only when both fields are equal:

```
return __lhs._M_value == __rhs._M_value && __lhs._M_cat == __rhs._M_cat;
```

`operator!=` is the negation of that comparison.

- `error_code()` stores value 0 and category id 0.
- `error_code(0, generic_category())` stores value 0 and category id 1.
  Those two codes compare unequal. A zero value is not enough.
- `error_code(5, generic_category())` compares equal to another
  `error_code(5, generic_category())`. Both store 5 and category id 1.
- The comparison reads the stored ids. It does not read a category name.

`generic_category()` is still an empty tag, not an `error_category` object.
`value()` still returns the stored int, so `error_code(5, generic_category()).value()`
is 5. `operator bool` is still `value() != 0`. A default `error_code` is
false. `error_code(0, generic_category())` is false because its value is 0.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). They are
not stubbed:

- `message()`. The header text does not contain the word Success.
- `error_condition`
- the `system_error` exception type
- `category()` as an `error_category` object (the id stays private)
- `system_category`, `errc`, `make_error_code`, `error_category`
- `operator<` and the other orderings

This header does not `#include <string>`, `<string_view>`, or `<stdexcept>`.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/system_error` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_system_error.cpp` was
already in `_prism_test_sources`. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` is unchanged. `system_error` stays in
that array.

## Tests

`tests/cpp/test_cxx_system_error.cpp` still reads the header. The value case
is unchanged: the constructor stores `__v` in `_M_value`, `value()` returns
`_M_value`, the default constructor stores 0 and category id 0, and
`operator bool` is `value() != 0`. The header text still has no Success, no
`<string>` or `<stdexcept>` include, and no `class error_condition` or
`class system_error`. No case required equality to be absent. One case was
added. It requires `operator==` to compare `_M_value` and `_M_cat`, and
`operator!=` to be `!(__lhs == __rhs)`. The header text has no `.name(`,
no `strcmp`, and no `message()` declaration.

`tests/cxx_models/system_error_trace.cpp` includes `<system_error>`. It
still checks that `error_code(5, generic_category()).value()` is 5 and that
a default `error_code` is false. It also checks that two generic codes of 5
compare equal, that a default code compares unequal to
`error_code(0, generic_category())`, that two generic zeros compare equal,
and that two default codes compare equal. It does not call `message()`.

Checked with clang++ 18.1.3, `-std=c++23 -Wall -Wextra -Werror
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. The trace was compiled twice (`-isystem`
the model directory, and against libstdc++). Stdout matched and both
binaries exited 0:

```
value 5
bool 1
def 0
def_bool 0
eq5 1
neq5 0
eq_def_generic0 0
neq_def_generic0 1
eq_generic0 1
eq_def 1
neq_val 0
```

`eq5` is 1: two `error_code(5, generic_category())` values are equal.
`neq5` is 0. `eq_def_generic0` is 0 and `neq_def_generic0` is 1: a default
code is not a generic zero. `eq_generic0` is 1. `eq_def` is 1. `neq_val`
is 0: value 5 is not value 0. `value` is 5 and `def_bool` is 0.

`testdata/conformance/prism/cxx/cxx_system_error_true.cpp` and
`testdata/conformance/libc-models/system_error_contracts.cpp` were not
edited. They still check `value()` 5 and a default code that is false.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still reject source text
that contains `system_error` or `error_code` (`is not a system_error model`,
`is not an error_code model`) before the header is inlined. Those files
were not edited. Until that gate is gone, `cxx_system_error_true` does not
reach this header. `docs/PIR.md` was not edited.
