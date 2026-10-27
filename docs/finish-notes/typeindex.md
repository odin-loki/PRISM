# typeindex slice

Branch `finish/cxx-typeindex`, base `1278b9dc714ceaee54277a298c50212376942489`.

`std::type_index` is the extensionless file
`src/prism/pir/models/cxx/typeindex`. A directory of that name would make
`#include <typeindex>` fail with EISDIR, and the unit would fall back to
libstdc++. The include guard is `_GLIBCXX_TYPEINDEX`, so libstdc++'s own
header is not also included. The file is a `#pragma GCC system_header` and
the type lives in namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The yml records
the contract the task asks for. `expected` is a YAML boolean. Nothing here
is `PROVED`. Comparing two `type_index` values is not a proof of a
program's behavior.

## What the model stores

A `const std::type_info*`. `type_index(const std::type_info&)` stores that
object's address. It does not copy a name.

`operator==` calls `type_info::operator==` on the two objects
(`*_M_target == *__rhs._M_target`). Two different `type_info` objects are
not treated as equal by a name invented in this header. `operator!=` is
the negation of that same comparison.

`hash_code()` returns `type_info::hash_code()`. Its return type is
`size_t` from `<cstddef>`, the same type `type_info::hash_code()` returns.
`name()` returns `type_info::name()`, that object's pointer. The header
does not contain the literal `unknown` and does not contain a hardcoded
`"main"`.

`operator<`, `operator<=`, `operator>`, and `operator>=` call
`type_info::before`, which is the order libstdc++ uses. `<` on `int`
versus `double` matches `typeid(int).before(typeid(double))`.

## What falls back

`std::hash<std::type_index>` is not declared. A specialization that
returned a constant would hide `type_info::hash_code()`. The header does
not include `<functional>`, so a unit that names `std::hash` fails to
compile against this header and pir falls back to libstdc++.
`operator<=>` is omitted the same way: `<compare>` is not included.
`<string>` and `<memory>` are not included.

`#include <typeinfo>` is libstdc++. This directory has no `typeinfo`
file, so that include is not a PRISM swap.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

```
src/prism/pir/models/cxx/typeindex
```

so the existing embed writes `cxx/typeindex` and the existing `-isystem`
directory is enough for `#include <typeindex>`. `_prism_test_sources`
gained `tests/cpp/test_cxx_typeindex.cpp`.
`tests/cpp/test_qa_conformance.cpp` lists `"typeindex"` in the
header-name array. Every name already in that array stays. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged. `iostream`
and `ostream` stay absent.

## Tests

`tests/cpp/test_cxx_typeindex.cpp` includes the model by path and reads
the header text. The path is a regular file named `typeindex`. The
constructor stores `&__rhs`. `operator==` is `type_info::operator==` on
the two objects. `hash_code()` and `name()` forward. The header text has
no `unknown`, no `"main"`, no `struct hash`, and no `#include` of
`<string>`, `<functional>`, `<memory>`, or `<compare>`. A `type_index` of
`int` equals one of `int`, does not equal one of `double`, and its
`hash_code()` and `name()` match that `type_info`. `<` matches
`type_info::before`.

`tests/cxx_models/typeindex_trace.cpp` is `#include <typeindex>`. It
prints whether two `int` indexes compare equal, whether `int` and
`double` do not, whether hash and name match `type_info`, and whether
`<` matches `before`.

`testdata/conformance/prism/cxx/cxx_typeindex_true.cpp` builds those
indexes and reads `buf[0]` on both branches (`no-oob`, `expected: true`).
The comparison does not choose an index. There is no false task.
`testdata/conformance/libc-models/typeindex_contracts.cpp` includes
`<typeindex>`. It is not a scored task.

clang++ 18 compiled the trace, the true task, and the contracts function
(`-std=c++23 -Wall -Wextra -Werror`). With this directory ahead of
libstdc++ on the include path, and again against libstdc++, the trace
printed the same five lines (`same 1`, `diff 0`, `hash 1`, `name 1`,
`less 0`) and both binaries exited 0. `less` is
`typeid(int).before(typeid(double))` on that libstdc++.

## Still in front of the model

`src/prism/bmc_unenc.inc` still rejects a function body that contains
`typeid(` (`C++ typeid unencoded`, `is not an RTTI model`) before a
header is inlined. `src/prism/bmc.cpp` repeats that message when the
lowered text already says `typeid unencoded`. Those files were not
edited. The task, the harness, and the trace each call `typeid`, so
until that gate is gone `cxx_typeindex_true` does not reach this header.
There is no separate `type_index` gate. `docs/PIR.md` was not edited.
