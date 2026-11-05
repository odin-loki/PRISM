# system_error value and category

Branch `finish/cxx-system-error-value`, base `ad41422d133a60b9ffa1d90ea9754896d126de23`.

`error_code::value()` and `error_code::category()` live on the existing header
`src/prism/pir/models/cxx/system_error`. That path is a file. A directory of
the same name would make `#include <system_error>` fail with EISDIR. The
include guard is `_GLIBCXX_SYSTEM_ERROR`. `#pragma GCC system_header`.
Namespace `std`.

`value()` was already declared. Its body is still `return _M_value`. That is
the int the constructor stored. It does not build a message. A default
`error_code` stores value 0. `category()` was not declared; this slice adds
it. `category()` is `return _M_cat_ptr`, the pointer stored beside the
category id. It does not build a message.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
This is not PROVED and not CLEAN. Reading the stored int and the stored
category pointer is the result this header defines. It is not a proof that
the category names a system condition, and it is not a fuzzer `CLEAN` result.

## What value() and category() do

`std::error_code` still stores an int value and an int category id.
`operator==` is unchanged:

```
return __lhs._M_value == __rhs._M_value && __lhs._M_cat == __rhs._M_cat;
```

It reads those two fields. It does not read `_M_cat_ptr`.

- `error_code()` stores value 0, category id 0, and a null category pointer.
  `value()` is 0. `category()` is null. `operator bool` is false.
- `error_code(n, generic_category())` stores `n`, category id 1, and the
  address of one stable `__generic_category_tag`. `value()` returns `n`.
  A nonzero `n` stays `n`. `category()` returns that same address for every
  generic code. The constructor argument is a temporary, so the stored
  pointer is not the address of that argument.
- Two `error_code(5, generic_category())` values compare equal. A default
  code compares unequal to `error_code(0, generic_category())`. A zero value
  with a different category id is unequal. `operator!=` is still the
  negation of `operator==`.

`generic_category()` is still an empty tag, not an `error_category` object.
`__generic_category_ptr()` returns the address of a function-local static
tag. `category()` returns the pointer the constructor stored.

The sentence `This is not PROVED and not CLEAN.` appears three times: the
file comment, the comment on `value()`, and the comment on `category()`.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). They are
not stubbed:

- `message()`. The header text does not contain the word Success, and it
  does not declare `message() const`.
- `error_condition`
- the `system_error` exception type
- `system_category`, `errc`, `make_error_code`, `error_category`
- `operator<` and the other orderings

This header does not `#include <string>`, `<string_view>`, or `<stdexcept>`.

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/system_error` was
already in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_system_error.cpp` was
already in `_prism_test_sources`. No test source line was added and none was
removed. The conformance header array in `tests/cpp/test_qa_conformance.cpp`
is unchanged. `system_error` stays in that array.

## Tests

`tests/cpp/test_cxx_system_error.cpp` still reads the header. The value case
is unchanged: the constructor stores `__v` in `_M_value`, `value()` returns
`_M_value`, the default constructor stores 0 and category id 0, and
`operator bool` is `value() != 0`. The header text still has no Success, no
`<string>` or `<stdexcept>` include, and no `class error_condition` or
`class system_error`. The equality case is unchanged: `operator==` compares
`_M_value` and `_M_cat`, and `operator!=` is `!(__lhs == __rhs)`. One case
was added. It requires `category()` to be `return _M_cat_ptr`, the default
constructor to store `_M_cat_ptr(nullptr)`, and the generic constructor to
store `__generic_category_ptr()`. It requires the stable object
`static const __generic_category_tag object{}` and `return &object;`. It
requires `operator==` to stay the same return line, and the header text to
have no `_M_cat_ptr ==`. It requires `This is not PROVED and not CLEAN.`
The header text has no `message() const`, no `return 0`, and no Success.

`tests/cxx_models/system_error_trace.cpp` includes `<system_error>`. It
still checks that `error_code(5, generic_category()).value()` is 5 and that
a default `error_code` is false and has value 0. It also checks the equality
cases from the previous slice. It calls `category()` and compares the
results with `==`: two generic codes of 5, two default codes, a default
code against a generic zero, and a generic 5 against a generic 9. It does
not call `message()`.

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
cat5 1
cat_def 1
cat_def_generic0 0
cat_diff_val 1
```

`value` is 5 and `def` is 0. `cat5` is 1: two generic codes of 5 share a
category. `cat_def` is 1: two default codes share a category.
`cat_def_generic0` is 0: a default code is not a generic zero.
`cat_diff_val` is 1: generic 5 and generic 9 share a category. On libstdc++
`category()` returns a reference and `==` compares those objects. On this
header `category()` returns the stored pointer.

A model-only translation unit, compiled with the same flags and `-isystem`
on the model directory, checked that a default `error_code` has `value()` 0
and `category()` null, that two generic codes share
`__generic_category_ptr()`, and that `operator==` still treats a default
code as unequal to a generic zero. It exited 0. That unit is not in the
tree.

`testdata/conformance/prism/cxx/cxx_system_error_true.cpp` and
`testdata/conformance/libc-models/system_error_contracts.cpp` were not
edited. They still check `value()` 5 and a default code that is false.

## Ported onto main

Main already stored category id 2 (system_category) in a default
`error_code`, as [syserr.errcode.constructors] requires, and had
`system_category()`, `assign()` and `clear()`. The port keeps that. The
null default pointer above is not on main: a default code, a
`system_category()` code, `assign(n, system_category())` and `clear()`
store `__system_category_ptr()`, one stable `__system_category_tag`.
Generic codes store `__generic_category_ptr()`. Both tags derive from the
empty `__error_category_tag`, and `category()` returns
`const __error_category_tag*`. `_M_cat_ptr` is never null. The doctest
requires `_M_cat_ptr(__system_category_ptr())` and no `_M_cat_ptr(nullptr)`.
The trace is unchanged: a default code's category is not a generic zero's.
