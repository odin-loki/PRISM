# tuple arity 3

Branch `finish/cxx-tuple-3`, base `1f3d4de85465663e28412dfcaeabacbc1dc360fe`.
`std::tuple` stays one extensionless file, `src/prism/pir/models/cxx/tuple`.
A directory of that name would make `#include <tuple>` fail with EISDIR.
The include guard is still `_GLIBCXX_TUPLE`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. The yml records the contract the harness asks for.
`expected` is the boolean `true`.

## What changed

Arity 3 is the same shape as arity 1 and 2. `tuple<_T0, _T1, _T2>` stores
three members. `std::get<0>`, `std::get<1>`, and `std::get<2>` return those
members (lvalue, const lvalue, rvalue, and const rvalue). `tuple_size` is 3.
`tuple_element<0>`, `tuple_element<1>`, and `tuple_element<2>` name `_T0`,
`_T1`, and `_T2`.

`get<3>` on a 3-tuple still instantiates `_Prism_tuple_get` and hits
`static_assert`. There is no `get` member for that index, and nothing returns
a value-initialized object in its place. Arity 0, 1, and 2 are unchanged.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). They are
not stubbed:

- `tuple_cat`, `apply`, `forward_as_tuple`, `tie`
- comparison
- any arity above 3 (the primary class template is not defined)

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/tuple` was already
in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_tuple.cpp` was already in
`_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` already lists
`tuple` in the header-name array.

## Tests

`tests/cpp/test_cxx_tuple.cpp` keeps the existing cases. One case was added:
`get<2>` of a 3-tuple is the third stored value, not a default return
(`return {}`, `return T{}`, `return T()`). The header text for that index
returns `__t._M_2`, `tuple_size` is 3, and `tuple_element<2>` names `_T2`.
When clang++ with ASan/UBSan is usable, the case compiles a 3-tuple holding
`(4, 5, 17)` against this header and requires the printed `get<2>` to be 17.
`get<3>` on that tuple is ill-formed, and `tuple<int, int, int, int>` is
still an undefined primary template.

Checked with clang++ 18.1.3 against libstdc++, `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. A 3-tuple driver compiled twice
(`-isystem` the model directory, and without it). Stdout matched:

```
get0=4
get1=5
get2=17
cget2=17
rget2=17
set2=8
size3=3
```

`get<2>` is 17, the value stored in the third member, including through a
const tuple and an rvalue. Assigning through `get<2>` stores 8.
`tuple_element` for `(int, char, long)` names those three types.
`get<3>` fails `static_assert` (`std::get index is not a valid compile-time
index`). A 4-tuple is an implicit instantiation of the undefined primary
template. `tests/cxx_models/tuple_trace.cpp` (arities 0, 1, and 2) still
prints the same lines both ways. cmake, `prism_tests`, and pir were not run.

`testdata/conformance/prism/cxx/cxx_tuple3_true.cpp` is memsafety true: it
reads `get<2>` of a `tuple<int, int, int>` whose third element is the
argument. There is no false task. An index that is not stored does not
compile, which is the fallback.

## Still in front of the model

`src/prism/bmc.cpp` (`low.find("tuple")`) and `src/prism/bmc_unenc.inc`
(`tuple\s*<`) still return "not a tuple model" from the source text, before
the header is inlined. Those files were not edited. Until that gate is gone,
`cxx_tuple3_true` does not reach this header. `docs/PIR.md` was not edited.
