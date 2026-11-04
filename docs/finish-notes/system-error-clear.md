# system_error clear

Branch `finish/cxx-system-error-clear`, base `72a1cc5a5402874a154e9266b9807cf1d70f0db6`.

`error_code::clear()` lives on the existing header
`src/prism/pir/models/cxx/system_error`. That path is a file. A directory of
the same name would make `#include <system_error>` fail with EISDIR. The
include guard is `_GLIBCXX_SYSTEM_ERROR`. `#pragma GCC system_header`.
Namespace `std`.

`clear()` was not declared. This slice adds it. `value()`, `category()`,
`operator bool`, `operator==`, and `operator!=` are unchanged.

The solver was not run. cmake, ninja, `prism_tests`, and pir were not run.
This is not PROVED and not CLEAN. Writing 0, category id 0, and a null
category pointer is the result this header defines. It is not a proof that
the code names no system condition, and it is not a fuzzer `CLEAN` result.

## What clear() does

`void clear() noexcept` writes the three fields the default constructor
stores:

```cpp
void clear() noexcept {
    _M_value = 0;
    _M_cat = 0;
    _M_cat_ptr = nullptr;
}
```

It sets the stored value to 0 and the stored category pointer to null. It
stores category id 0, the same id `error_code()` stores. It does not build
a message. After `clear()`, `value()` is 0 and `operator bool` is false.

`operator==` is unchanged. It still compares the stored int and the category
id. It does not read the category pointer and it does not read a category
name:

```
return __lhs._M_value == __rhs._M_value && __lhs._M_cat == __rhs._M_cat;
```

A cleared generic code stores the same three fields as a default
`error_code`, so those two compare equal. `error_code(0, generic_category())`
still stores category id 1, so a cleared code compares unequal to that
generic zero. The category id is what the comparison reads. The pointer is
not.

`value()` is still `return _M_value`. `category()` is still
`return _M_cat_ptr`. A default code and a cleared code both return a null
pointer. A generic code that has not been cleared still returns
`__generic_category_ptr()`. `clear()` does not call `generic_category()` and
does not replace that stable object.

`generic_category()` is still an empty tag, not an `error_category` object.
Two `error_code(5, generic_category())` values that have not been cleared
still compare equal. A default code still compares unequal to a generic
zero. `operator!=` is still the negation of `operator==`.

The sentence `This is not PROVED and not CLEAN.` is on the file comment for
`clear()`, on the comment above `clear()`, and still on the comments for
`value()` and `category()`.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). They are
not stubbed:

- `message()`. The header text does not contain the word Success, and it
  does not declare `message() const`. `clear()` does not build a message.
- `error_condition`
- the `system_error` exception type
- `system_category`, `errc`, `make_error_code`, `error_category`
- `operator<` and the other orderings

This header does not `#include <string>`, `<string_view>`, or `<stdexcept>`.

## Wiring

`CMakeLists.txt` was not edited. No `_prism_test_sources` line was added
and none was dropped. `src/prism/pir/models/cxx/system_error` was already
in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_system_error.cpp` was
already in `_prism_test_sources`. No new test file was added. The
conformance header array in `tests/cpp/test_qa_conformance.cpp` is
unchanged. `system_error` stays in that array.

## Tests

`tests/cpp/test_cxx_system_error.cpp` still reads the header. The value
case is unchanged: the constructor stores `__v` in `_M_value`, `value()`
returns `_M_value`, the default constructor stores 0 and category id 0, and
`operator bool` is `value() != 0`. The header text still has no Success, no
`<string>` or `<stdexcept>` include, and no `class error_condition` or
`class system_error`. The equality case is unchanged: `operator==` compares
`_M_value` and `_M_cat`, and `operator!=` is `!(__lhs == __rhs)`. The
category case is unchanged: `category()` is `return _M_cat_ptr`, the
default constructor stores `_M_cat_ptr(nullptr)`, and the generic
constructor stores `__generic_category_ptr()`. One case was added. It
requires `clear()` to assign `_M_value = 0`, `_M_cat = 0`, and
`_M_cat_ptr = nullptr`. That function text has no `message`, no `return`,
no `.name(`, and no Success. It requires `value()`, `category()`,
`operator bool`, and `operator==` to stay the same lines, and the header
text to have no `_M_cat_ptr ==`. It requires `This is not PROVED and not CLEAN.`
The header text has no `message() const`, no `return 0`, and no Success.

`tests/cxx_models/system_error_trace.cpp` includes `<system_error>`. It
still checks that `error_code(5, generic_category()).value()` is 5 and that
a default `error_code` is false and has value 0. It also checks the equality
and category cases from the previous slice. It calls `clear()` on generic
codes of 5 and 9. After `clear()`, `value()` is 0 and `operator bool` is
false. The cleared code compares equal to a default code and unequal to a
generic zero. Its `category()` compares equal to a default code and to the
other cleared code, and unequal to `generic_category()`. The uncleared
generic 5 still has `value()` 5. It does not call `message()`.

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
clr_value 0
clr_bool 0
clr_eq_def 1
clr_eq_generic0 0
clr_cat_def 1
clr_cat_generic 0
clr_cat_pair 1
```

`clr_value` is 0 and `clr_bool` is 0. `clr_eq_def` is 1: a cleared generic
code compares equal to a default code. `clr_eq_generic0` is 0: it is not a
generic zero. `clr_cat_def` is 1 and `clr_cat_pair` is 1: cleared codes
share a category with each other and with a default code. `clr_cat_generic`
is 0: that category is not `generic_category()`. On libstdc++ `clear()`
assigns `system_category()` and `category()` returns a reference. On this
header `clear()` stores a null pointer and category id 0, and `category()`
returns that pointer.

A model-only translation unit, compiled with the same flags and `-isystem`
on the model directory, checked that `error_code(5, generic_category()).clear()`
has `value()` 0, `operator bool` false, and `category()` null, that it
compares equal to a default code and unequal to a generic zero, and that an
uncleared generic 5 still has `value()` 5. It printed `null_ok` and exited
0. That unit is not in the tree.

`testdata/conformance/prism/cxx/cxx_system_error_true.cpp` and
`testdata/conformance/libc-models/system_error_contracts.cpp` were not
edited. They still check `value()` 5 and a default code that is false.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still reject source text
that contains `system_error` or `error_code` (`is not a system_error model`,
`is not an error_code model`) before the header is inlined. Those files
were not edited. Until that gate is gone, `cxx_system_error_true` does not
reach this header. `docs/PIR.md` was not edited. `src/prism/pir/stage.cpp`
was not edited.

## Ported onto main

Main already had `clear()`, storing value 0 and `system_category()` (id 2),
as [syserr.errcode.modifiers] requires, and the category-pointer port stores
the stable `__system_category_ptr()` for it. The null pointer and category
id 0 described above are not on main: a second `clear()` would redeclare the
member. The port keeps main's `clear()`, gives it the comment and three-line
body, and the doctest requires `_M_cat = 2;`,
`_M_cat_ptr = __system_category_ptr();`, no `nullptr`, and one `clear()`.
The trace lines are kept; libstdc++ and the model agree on every one.
