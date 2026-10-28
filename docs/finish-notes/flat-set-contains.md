# flat_set contains

Branch `finish/cxx-flat-set-contains`, base `f830d4ef8b8857b9fd78c95346fbfe1d6def669a`.

`std::flat_set::contains` lives on the existing header
`src/prism/pir/models/cxx/flat_set`. That path is a file. A directory of
the same name would make `#include <flat_set>` fail with EISDIR. The
include guard is `_GLIBCXX_FLAT_SET`.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here
is `PROVED`. A missing key's `false` is the result this header defines.
It is not a proof that the key is absent from a container this model does
not cover.

## What contains does

`bool contains(const key_type& __x) const` calls `_M_find_node`. That
helper walks the nodes already linked and returns the node or nullptr. Its
body has no new-expression.

A missing key, including every key when the container is empty, is a null
node. `contains` returns false. That return does not allocate a node, does
not insert a key, and does not build an end iterator.

A key that is already present (equivalent under the comparator) is that
key's node. `contains` returns true. That return does not allocate a node
and does not assign over the stored key.

`count` is still `contains` ? 1 : 0, so a missing key counts as 0 through
the same lookup.

## What stayed

`insert` of an equivalent key still returns the existing node before
`new _Node` and before `++_M_size`, and that prefix does not assign
`_M_key`. `erase` of a missing key still returns 0 before `delete`.
`end()` is still

`iterator end() const noexcept { return iterator(nullptr); }`.

`operator*` and `operator->` still call `__prism_check` on a null node,
class `PTR-NULL-DEREF`, message `flat_set iterator dereference at end`.

The header does not include `<set>`, `<vector>`, `<memory>`, `<flat_map>`,
or `<initializer_list>`.

## Tests

`tests/cpp/test_cxx_flat_set.cpp` reads the header. The `contains` body
calls `_M_find_node`, returns false, then returns true, and has no `new`,
no `delete`, and no `_M_key` assignment. The same case still checks the
duplicate-insert prefix and the missing-key `erase` return of 0. It also
checks that the end-iterator dereference message is still on both
operators. The old check required the one-line body
`return find(__x) != end()`. That line was the previous body. The case
now checks the lookup above.

`CMakeLists.txt` already lists `tests/cpp/test_cxx_flat_set.cpp`, so this
slice adds no test source line. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` is unchanged. `flat_set` stays in that
list.

`tests/cxx_models/flat_set_trace.cpp` and the flat_set contract harnesses
already call `contains`. A present key is true and a missing key is false.
Those files are unchanged.
