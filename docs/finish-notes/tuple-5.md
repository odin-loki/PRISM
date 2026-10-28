# tuple arity 5

Branch `finish/cxx-tuple-5`, base `1dd4566ba15e332a21a398049eebbd2c31a2356c`.
`std::tuple` stays one extensionless file, `src/prism/pir/models/cxx/tuple`.
A directory of that name would make `#include <tuple>` fail with EISDIR.
The include guard is still `_GLIBCXX_TUPLE`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. The yml records the contract the harness asks for.
`expected` is the boolean `true`.

## What changed

Arity 5 is the same shape as arity 1, 2, 3, and 4. `tuple<_T0, _T1, _T2, _T3, _T4>`
stores five members. `std::get<0>`, `std::get<1>`, `std::get<2>`, `std::get<3>`,
and `std::get<4>` return those members (lvalue, const lvalue, rvalue, and const
rvalue). `tuple_size` is 5. `tuple_element<0>`, `tuple_element<1>`,
`tuple_element<2>`, `tuple_element<3>`, and `tuple_element<4>` name `_T0`,
`_T1`, `_T2`, `_T3`, and `_T4`.

`get<5>` on a 5-tuple still instantiates `_Prism_tuple_get` and hits
`static_assert`. There is no `get` member for that index, and nothing returns
a value-initialized object in its place. Arity 0, 1, 2, 3, and 4 are unchanged.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). They are
not stubbed:

- `tuple_cat`, `apply`, `forward_as_tuple`, `tie`
- comparison
- any arity above 5 (the primary class template is not defined)

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/tuple` was already
in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_tuple.cpp` was already in
`_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` already lists
`tuple` in the header-name array.

## Tests

`tests/cpp/test_cxx_tuple.cpp` keeps the existing cases. The cases that
required `tuple<int, int, int, int, int>` to be an undefined primary now
require that of a 6-tuple, because arity 5 is in the model. One case was
added: `get<4>` of a 5-tuple is the fifth stored value, not a default return
(`return {}`, `return T{}`, `return T()`). The header text for that index
returns `__t._M_4`, `tuple_size` is 5, and `tuple_element<4>` names `_T4`.
When clang++ with ASan/UBSan is usable, the case compiles a 5-tuple holding
`(4, 5, 6, 7, 17)` against this header and requires the printed `get<4>` to
be 17. `get<5>` on that tuple is ill-formed, and
`tuple<int, int, int, int, int, int>` is still an undefined primary template.

Checked with clang++ 18.1.3 against libstdc++, `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. A 5-tuple driver compiled twice
(`-isystem` the model directory, and without it). Stdout matched:

```
get0=4
get1=5
get2=6
get3=7
get4=17
cget4=17
rget4=17
crget4=17
set4=8
size5=5
```

`get<4>` is 17, the value stored in the fifth member, including through a
const tuple, an rvalue, and a const rvalue. Assigning through `get<4>` stores
8. `get<0>`, `get<1>`, `get<2>`, and `get<3>` are 4, 5, 6, and 7.
`tuple_element` for `(int, char, long, short, bool)` names those five types.
`get<5>` fails `static_assert` (`std::get index is not a valid compile-time
index`). A 6-tuple is an implicit instantiation of the undefined primary
template. `get<4>` on a 4-tuple is still ill-formed.
`tests/cxx_models/tuple_trace.cpp` (arities 0, 1, and 2) still prints the
same lines both ways. cmake, `prism_tests`, and pir were not run.

`testdata/conformance/prism/cxx/cxx_tuple5_true.cpp` is memsafety true: it
reads `get<4>` of a `tuple<int, int, int, int, int>` whose fifth element is
the argument. There is no false task. An index that is not stored does not
compile, which is the fallback.

## Still in front of the model

`src/prism/bmc.cpp` (`low.find("tuple")`) and `src/prism/bmc_unenc.inc`
(`tuple\s*<`) still return "not a tuple model" from the source text, before
the header is inlined. Those files were not edited. Until that gate is gone,
`cxx_tuple5_true` does not reach this header. `docs/PIR.md` was not edited.
