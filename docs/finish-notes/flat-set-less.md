# flat_set less

Branch `port/flat`, parent `fa63548bb` (`origin/main` at this commit). This slice
reimplements the October `operator<` on the current header. It does not
cherry-pick.

`operator<` for two `std::flat_set` objects of the same type lives on the
existing header `src/prism/pir/models/cxx/flat_set`. That path is a file. A
directory of the same name would make `#include <flat_set>` fail with
EISDIR. The include guard is `_GLIBCXX_FLAT_SET`.

The current header already had `insert`, `erase`, `find`, `contains`,
`count`, `empty`, `size`, `clear`, `swap`, `operator==`, and `operator!=`.
This slice adds `operator<` only. `swap` already exchanges `_M_head`,
`_M_tail`, `_M_size`, and `_M_comp` with `std::swap`. Two empty sets swap
null pointers and do not load a node. Self-swap does not free nodes. That
function already matches the empty and self-swap contract, so it was not
rewritten.

This slice did not run cmake, ninja, `prism_tests`, or pir. Nothing here is
PROVED. Nothing here is CLEAN. Two empty sets comparing not-less is the
result this header defines. It is not a proof that two containers this
model does not cover are ordered, and it is not a fuzzer CLEAN result.

## What less does

`operator<` is a friend on `flat_set`. It is container order, not iterator
order. Iterator `operator==` is still the two node pointers. There is no
iterator `operator<`.

Two empty sets compare not-less. An empty set has no node (`_M_head` is
nullptr). That path returns false before any `->`, `_M_key`, or `_M_next`,
so it does not load a node.

An empty set is less than a non-empty set. That return is true, and it is
also before any node field is read, so it does not load the empty set. It
does not load the other set's node either.

A non-empty set is not less than an empty set. That return is false before
any node is loaded, so the longer set is not read.

Otherwise the stored keys are compared lexicographically, in link order.
`insert` links a new key in compare order, so that chain is sorted order.
The comparison is key `operator<`. It does not call the comparator object,
and it does not call `operator==`.

- `__a->_M_key < __b->_M_key` returns true. A later node is not loaded.
- Otherwise `__b->_M_key < __a->_M_key` returns false. The left key is
  greater, and a later node is not loaded.
- Otherwise the two keys are equivalent and the walk continues.

The loop condition is `__a != nullptr && __b != nullptr`, so the walk does
not read past the shorter set. When the left set runs out first, the result
is true. When the right set runs out first, or both run out together, the
result is false. That final return is `__a == nullptr && __b != nullptr`.
It does not read `_M_key`. The body does not construct a key and it does
not return `T{}`. It does not use `_M_size`.

`operator>`, `operator<=`, `operator>=`, and `operator<=>` are not declared.
The header does not call `std::lexicographical_compare`. The steps above
are the same steps that algorithm takes, so a libstdc++ `operator<` on the
sorted keys prints the same booleans and the same `<` counts.

## What stayed

`operator==` is still container equality: two empty sets compare equal
before a node is loaded, different sizes compare unequal before the walk,
and the walk does not read past the shorter set. `operator!=` is still
`!(__x == __y)`. `insert` of an equivalent key still returns the existing
node before `new _Node` and before `++_M_size`, and that prefix does not
assign `_M_key`. `erase` of a missing key still returns 0 before `delete`.
`find` is still `_M_find_node`. `contains` still returns false for a
missing key and true for a present key, and that body does not allocate.
`count` is still `contains` ? 1 : 0. `empty()` is still
`_M_head == nullptr`. `size()` is still `return _M_size`. `clear` still
erases the head until no node remains. `swap` is still the member that
swaps the head, tail, size, and comparator. `end()` is still

`iterator end() const noexcept { return iterator(nullptr); }`.

`operator*` and `operator->` still call `__prism_check` on a null node,
class `PTR-NULL-DEREF`, message `flat_set iterator dereference at end`.

The header does not include `<set>`, `<vector>`, `<memory>`, `<algorithm>`,
`<flat_map>`, or `<initializer_list>`.

## Tests

`tests/cpp/test_cxx_flat_set.cpp` reads the header. The new case checks
container `operator<`: the empty return is before any `->`, `_M_key`, or
`_M_next`; an empty set is less than a non-empty set before those loads;
a non-empty set is not less than an empty set before those loads; the walk
requires both nodes and compares `_M_key` with `<`; a deciding key returns
before `_M_next`; the shorter set running out is the only true result after
the walk. The body does not name `key_type` or `value_type`, does not use
`_M_comp` or `_M_size`, and has no `new`. The comment above `operator<`
says this is not PROVED and not CLEAN. There is one `operator<(const flat_set&`
and no iterator `operator<`. The earlier cases still check the
duplicate-insert prefix, `contains`, the missing-key `erase` return of 0,
and container `operator==`.

`CMakeLists.txt` was not edited. It already lists
`tests/cpp/test_cxx_flat_set.cpp`. The conformance header array in
`tests/cpp/test_qa_conformance.cpp` is unchanged. `flat_set` stays in that
list.

`tests/cxx_models/flat_set_trace.cpp` includes `<flat_set>`. The lines
through `contains1` / `contains2` are unchanged. The new lines are
`operator<`. clang++ 18.1.3,
`-std=c++23 -O1 -Wall -Wextra -Werror -fsanitize=address,undefined
-fno-sanitize-recover=all -g`, with this header ahead of the system
includes, printed

```text
empty_eq 1
empty_size 0
dup_inserted 0 size 2
contains1 1 contains3 0
find3_end 1
order 1 2
erase_missing 0 size 2
erase_present 1 size 1
contains1 1 contains2 0
lt empty 0 short 1 rev 0 same 0 mid 1 gt 0 prefix 1 longer 0 one 1 self 0 const 0
klt empty 0 calls 0 ctors 0 poison 0 short 1 scalls 0 sctors 0 rev 0 rcalls 0 rctors 0 prefix 1 pcalls 4 ppoison 0 pctors 0 longer 0 lcalls 4 lpoison 0 lctors 0 miss 0 mcalls 4 mpoison 0 head 1 hcalls 1 hpoison 0 tail 0 tcalls 2 tpoison 0
```

`lt empty 0` is two empty `flat_set<int>` values: not less. `short 1` is
an empty set less than `{1}`. `rev 0` is `{1}` not less than empty.
`same 0` and `self 0` are `{1, 2}` compared with itself. `mid 1` is
`{1, 2}` less than `{1, 3}`. `prefix 1` is `{1, 2}` less than `{1, 2, 3}`
and `longer 0` is the other direction. `const 0` is a copy of `{1}`
compared with `{1}`.

The `klt` line uses a key with no default constructor. `operator<` counts
calls, and a read of the sentinel 100 counts as poison. A constructor
counts too. Two empty sets compare with zero calls and zero constructors.
An empty set is less than `{1}` with zero calls. `{1, 2}` against
`{1, 2, 100}` takes four `<` calls and does not read 100. `{1, 9, 100}`
against `{1, 2, 100}` stops at the second key (four calls, poison 0).
`{1, 100}` less than `{2, 100}` is one call and does not read 100. The
other direction is two calls and does not read 100. Nothing is constructed
during the comparisons. That run is not PROVED and not CLEAN.

libstdc++ 13.3 on this machine has no `<flat_set>` header, so the same
driver was not built against it. The new lines are lexicographical order
of the sorted keys, which is what that header's `operator<` prints. The
lines above the less block are the previous stdout.

## Still in front of the model

`src/prism/syntax_gates.cpp` still reports `C++ flat_set unencoded`. This
slice does not edit that file, `stage.cpp`, or `docs/PIR.md`. Until that
gate is gone, a flat_set task does not reach this header.
