# tuple arity 6

Branch `finish/cxx-tuple-6`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.
`std::tuple` stays one extensionless file, `src/prism/pir/models/cxx/tuple`.
A directory of that name would make `#include <tuple>` fail with EISDIR.
The include guard is still `_GLIBCXX_TUPLE`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. The yml records the contract the harness asks for.
`expected` is the boolean `true`.

## What changed

Arity 6 is the same shape as arity 1, 2, 3, 4, and 5.
`tuple<_T0, _T1, _T2, _T3, _T4, _T5>` stores six members. `std::get<0>`,
`std::get<1>`, `std::get<2>`, `std::get<3>`, `std::get<4>`, and `std::get<5>`
return those members (lvalue, const lvalue, rvalue, and const rvalue).
`tuple_size` is 6. `tuple_element<0>`, `tuple_element<1>`, `tuple_element<2>`,
`tuple_element<3>`, `tuple_element<4>`, and `tuple_element<5>` name `_T0`,
`_T1`, `_T2`, `_T3`, `_T4`, and `_T5`.

`get<6>` on a 6-tuple still instantiates `_Prism_tuple_get` and hits
`static_assert`. There is no `get` member for that index, and nothing returns
a value-initialized object in its place. Arity 0, 1, 2, 3, 4, and 5 are unchanged.
`get<5>` on a 5-tuple stays ill-formed through that same primary `static_assert`.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). They are
not stubbed:

- `tuple_cat`, `apply`, `forward_as_tuple`, `tie`
- comparison
- any arity above 6 (the primary class template is not defined)

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/tuple` was already
in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_tuple.cpp` was already in
`_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` already lists
`tuple` in the header-name array.

## Tests

`tests/cpp/test_cxx_tuple.cpp` keeps the existing cases. The cases that
required `tuple<int, int, int, int, int, int>` to be an undefined primary now
require that of a 7-tuple, because arity 6 is in the model. One case was
added: `get<5>` of a 6-tuple is the sixth stored value, not a default return
(`return {}`, `return T{}`, `return T()`). The header text for that index
returns `__t._M_5`, `tuple_size` is 6, and `tuple_element<5>` names `_T5`.
When clang++ with ASan/UBSan is usable, the case compiles a 6-tuple holding
`(4, 5, 6, 7, 8, 17)` against this header and requires the printed `get<5>` to
be 17. `get<6>` on that tuple is ill-formed, and
`tuple<int, int, int, int, int, int, int>` is still an undefined primary template.

Checked with clang++ 18.1.3 against libstdc++, `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. A 6-tuple driver compiled twice
(`-isystem` the model directory, and without it). Stdout matched:

```
get0=4
get1=5
get2=6
get3=7
get4=8
get5=17
cget5=17
rget5=17
crget5=17
set5=8
size6=6
```

`get<5>` is 17, the value stored in the sixth member, including through a
const tuple, an rvalue, and a const rvalue. Assigning through `get<5>` stores
8. `get<0>`, `get<1>`, `get<2>`, `get<3>`, and `get<4>` are 4, 5, 6, 7, and 8.
`tuple_element` for `(int, char, long, short, bool, unsigned)` names those six
types. `get<6>` fails `static_assert` (`std::get index is not a valid
compile-time index`). A 7-tuple is an implicit instantiation of the undefined
primary template. `get<5>` on a 5-tuple is still ill-formed, and `get<4>` on a
4-tuple is still ill-formed.
`tests/cxx_models/tuple_trace.cpp` (arities 0, 1, and 2) still prints the
same lines both ways. cmake, `prism_tests`, and pir were not run.

`testdata/conformance/prism/cxx/cxx_tuple6_true.cpp` is memsafety true: it
reads `get<5>` of a `tuple<int, int, int, int, int, int>` whose sixth element
is the argument. There is no false task. An index that is not stored does not
compile, which is the fallback.

## Still in front of the model

`src/prism/bmc.cpp` (`low.find("tuple")`) and `src/prism/bmc_unenc.inc`
(`tuple\s*<`) still return "not a tuple model" from the source text, before
the header is inlined. Those files were not edited. Until that gate is gone,
`cxx_tuple6_true` does not reach this header. `docs/PIR.md` was not edited.
