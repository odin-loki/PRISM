# tuple arity 7

Branch `finish/cxx-tuple-7`, base `ed3874ee424575195b01c034a0ca5cfe60b7ade3`.
`std::tuple` stays one extensionless file, `src/prism/pir/models/cxx/tuple`.
A directory of that name would make `#include <tuple>` fail with EISDIR.
The include guard is still `_GLIBCXX_TUPLE`. `#pragma GCC system_header`.
Namespace `std`.

The solver was not run. The yml records the contract the harness asks for.
`expected` is the boolean `true`. That contract is not PROVED and not CLEAN.

## What changed

Arity 7 is the same shape as arity 1, 2, 3, 4, 5, and 6.
`tuple<_T0, _T1, _T2, _T3, _T4, _T5, _T6>` stores seven members. `std::get<0>`,
`std::get<1>`, `std::get<2>`, `std::get<3>`, `std::get<4>`, `std::get<5>`, and
`std::get<6>` return those members (lvalue, const lvalue, rvalue, and const
rvalue). `tuple_size` is 7. `tuple_element<0>`, `tuple_element<1>`,
`tuple_element<2>`, `tuple_element<3>`, `tuple_element<4>`, `tuple_element<5>`,
and `tuple_element<6>` name `_T0`, `_T1`, `_T2`, `_T3`, `_T4`, `_T5`, and
`_T6`.

`get<7>` on a 7-tuple still instantiates `_Prism_tuple_get` and hits
`static_assert`. There is no `get` member for that index, and nothing returns
a value-initialized object in its place. Arity 0, 1, 2, 3, 4, 5, and 6 are
unchanged. `get<6>` on a 6-tuple stays ill-formed through that same primary
`static_assert`.

## What falls through to libstdc++

Not declared, so the unit fails to compile against this header and
`lower_to_ir` lowers it again with libstdc++ (`extra.cxx_models`). They are
not stubbed:

- `tuple_cat`, `apply`, `forward_as_tuple`, `tie`
- comparison
- any arity above 7 (the primary class template is not defined)

## Wiring

`CMakeLists.txt` was not edited. `src/prism/pir/models/cxx/tuple` was already
in `PRISM_PIR_MODELS`, and `tests/cpp/test_cxx_tuple.cpp` was already in
`_prism_test_sources`. `tests/cpp/test_qa_conformance.cpp` already lists
`tuple` in the header-name array.

## Tests

`tests/cpp/test_cxx_tuple.cpp` keeps the existing cases. The cases that
required `tuple<int, int, int, int, int, int, int>` to be an undefined primary
now require that of an 8-tuple, because arity 7 is in the model. One case was
added: `get<6>` of a 7-tuple is the seventh stored value, not a default return
(`return {}`, `return T{}`, `return T()`). The header text for that index
returns `__t._M_6`, `tuple_size` is 7, and `tuple_element<6>` names `_T6`.
When clang++ with ASan/UBSan is usable, the case compiles a 7-tuple holding
`(4, 5, 6, 7, 8, 9, 17)` against this header and requires the printed `get<6>`
to be 17. `get<7>` on that tuple is ill-formed, and
`tuple<int, int, int, int, int, int, int, int>` is still an undefined primary
template.

Checked with clang++ 18.1.3 against libstdc++, `-std=c++23
-D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined
-fno-sanitize-recover=all -g -O1`. A 7-tuple driver compiled twice
(`-isystem` the model directory, and without it). Stdout matched:

```
get0=4
get1=5
get2=6
get3=7
get4=8
get5=9
get6=17
cget6=17
rget6=17
crget6=17
set6=8
size7=7
```

`get<6>` is 17, the value stored in the seventh member, including through a
const tuple, an rvalue, and a const rvalue. Assigning through `get<6>` stores
8. `get<0>`, `get<1>`, `get<2>`, `get<3>`, `get<4>`, and `get<5>` are 4, 5, 6,
7, 8, and 9. `tuple_element` for `(int, char, long, short, bool, unsigned,
double)` names those seven types. `get<7>` fails `static_assert` (`std::get
index is not a valid compile-time index`). An 8-tuple is an implicit
instantiation of the undefined primary template. `get<6>` on a 6-tuple is
still ill-formed, `get<5>` on a 5-tuple is still ill-formed, and `get<4>` on a
4-tuple is still ill-formed.
`tests/cxx_models/tuple_trace.cpp` (arities 0, 1, and 2) still prints the
same lines both ways. cmake, `prism_tests`, and pir were not run.

`testdata/conformance/prism/cxx/cxx_tuple7_true.cpp` is memsafety true: it
reads `get<6>` of a `tuple<int, int, int, int, int, int, int>` whose seventh
element is the argument. There is no false task. An index that is not stored
does not compile, which is the fallback. The task is not PROVED and not CLEAN.

## Still in front of the model

`src/prism/bmc.cpp` (`low.find("tuple")`) and `src/prism/bmc_unenc.inc`
(`tuple\s*<`) still return "not a tuple model" from the source text, before
the header is inlined. Those files were not edited. Until that gate is gone,
`cxx_tuple7_true` does not reach this header, so the result stays not PROVED
and not CLEAN. `docs/PIR.md` was not edited.
