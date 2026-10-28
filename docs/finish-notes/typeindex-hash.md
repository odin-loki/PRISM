# typeindex hash slice

Branch `finish/cxx-typeindex-hash`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::hash<std::type_index>` is declared in the existing extensionless file
`src/prism/pir/models/cxx/typeindex`. A directory of that name would make
`#include <typeindex>` fail with EISDIR. The include guard is still
`_GLIBCXX_TYPEINDEX`. The file is still a `#pragma GCC system_header` and
the specialization lives in namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
`PROVED`. Comparing two `type_index` values is not a proof of a program's
behavior.

## What the specialization does

`std::hash<std::type_index>::operator()` returns `type_index::hash_code()`.
That member is unchanged: it still returns `type_info::hash_code()`. The
hash is not a constant. It is not computed from `name()`.

Two `type_index` objects that compare equal have the same hash because both
calls forward that `hash_code`. `operator==` is unchanged. It still calls
`type_info::operator==` on the two objects
(`*_M_target == *__rhs._M_target`). It does not compare names, and it is
not a constant.

`operator<`, `name()`, and `hash_code()` are unchanged. `operator<` still
calls `type_info::before`.

The primary `std::hash` template is only forward-declared
(`template<typename _Tp> struct hash`). The header does not include
`<functional>`, `<string>`, or `<memory>`. `<compare>` stays not included,
so `operator<=>` stays omitted.

## Wiring left as it was

`CMakeLists.txt` was not edited. `tests/cpp/test_qa_conformance.cpp` was
not edited, so the conformance header-name array is unchanged.
`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` were not edited. A
function body that contains `typeid(` is still `C++ typeid unencoded` /
`is not an RTTI model` before a header is inlined.
`testdata/conformance/prism/cxx/cxx_typeindex_true.cpp` was not edited, so
that task still calls `typeid` and still stops at that gate.

## Tests

`tests/cpp/test_cxx_typeindex.cpp` reads the header text. It requires
`struct hash<type_index>` and `return __ti.hash_code()`. It still requires
`operator==` to be `type_info::operator==`, and it still rejects an include
of `<functional>`, `<string>`, or `<memory>`, a `return 0`, and a stand-in
name. It calls `std::hash<std::type_index>` on an `int` index and a
`double` index. The two `int` indexes compare equal, and each hash matches
that `type_index::hash_code()`.

`tests/cxx_models/typeindex_trace.cpp` includes `<typeindex>` and prints
that same check as `stdhash`.

clang++ 18.1.3 compiled the trace (`-std=c++23 -Wall -Wextra -Werror`)
with this directory ahead of libstdc++ and again against libstdc++. Both
binaries printed the same six lines (`same 1`, `diff 0`, `hash 1`,
`name 1`, `less 0`, `stdhash 1`) and exited 0. `less` is
`typeid(int).before(typeid(double))` on that libstdc++. A second
translation unit included `<string>` and `<unordered_set>` before this
header and then called the same hash. It compiled, and the hash matched
`type_index::hash_code()`.
