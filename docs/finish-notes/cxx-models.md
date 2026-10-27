# cxx-models slice

Branch `finish/cxx-models`, base `e59a8b3df966e33c78ffbd9ced0a8fc90465b16d`.
Landed from `origin/cursor/libc-cxx-models-6af2` (`da04b3371` sstream,
`211fc5f2d` unordered containers and Houdini). No `std::list`. No iostream
model. These tasks are not `PROVED`: the yml files record the contract the
harness asks for, and the solver was not run.

## What landed

PIR C++ models, embedded from `PRISM_PIR_MODELS` (not a glob):

- `src/prism/pir/models/cxx/sstream` — `basic_stringbuf` and
  `basic_ostringstream` for `char` / `char_traits<char>` / `allocator<char>`.
  Not a `basic_streambuf` or `basic_ostream`. No input, no seek.
- `src/prism/pir/models/cxx/unordered_map`, `unordered_set`, and
  `prism_hash_table.h` — unique and multi keys, `std::allocator` only.
  Node handles, `extract` / `merge`, and `pmr` are not declared, so those
  units fall back to libstdc++.

Contracts and traces:

- `testdata/conformance/libc-models/sstream_contracts.{cpp,yml}` (timeout 300).
  `sstream_empty_false` expects `FUNC-CONTRACT`.
- `testdata/conformance/libc-models/unordered_map_contracts.{cpp,yml}` (timeout 900).
  `umap_erase_uaf_false` expects `MEM-UAF`. `umap_end_deref_false` expects
  `FUNC-CONTRACT` (`find` returns end; `_GLIBCXX_ASSERTIONS`).
- `testdata/conformance/prism/cxx/cxx_sstream_*.{cpp,yml}` and
  `cxx_unordered_*.{cpp,yml}` — no-uncaught true/false twins. False yml
  witness line is `[6]`. No extra timeout.
- `tests/cxx_models/sstream_trace.cpp`, `unordered_map_set_trace.cpp`.
- `tests/cpp/test_cxx_models.cpp` runs those traces.
- `tests/cpp/test_qa_conformance.cpp` expects the three new headers in the
  embedded model list. That is the only hunk in that file.

## Bugs fixed against the vector/map/string models

Hash table (`prism_hash_table.h`):

- Erasing the insertion-order tail no longer points `_M_iter_tail` at the
  successor. The next link after erasing the last-inserted node was a null
  dereference.
- `swap` does not exchange `_M_buckets` when a table has one bucket. That
  slot lives in the object (`_M_single_bucket_head`). Copy assignment uses
  copy-and-swap, so the same bug corrupted copies.
- Move construction, move assignment, and swap rewrite each node's owner
  cookie (`_M_adopt`). Move assignment refuses a self-move.
- Equivalent multi keys stay contiguous in iteration order, so `equal_range`
  is one span. `find` is still the newest match (bucket front).
- A duplicate unique `insert(const value_type&)` looks the key up and does
  not copy a node (libstdc++ `_M_insert`). `insert(value_type&&)` always
  emplaces: libstdc++ builds the node and drops it when the key exists.
  Skipping that move left the differential trace one `T` move short
  (`move=5` vs libstdc++ `move=6` after `insert({3, T(33)})` of an existing
  key).
- Rehash keeps the same nodes. Iterators are not invalidated. A harness must
  not claim rehash frees them.

`sstream`:

- `str()` is the high-water buffer, not a prefix of length `_M_out`. A short
  overwrite of `"seed"` stays `"Xeed"`.
- `ate` / `app` start at the end; a later `app` write also starts at the end.
  A default `out` overwrite keeps the tail.
- `sputn` is public, so `sstream_strbuf_true` is a real call.
- `operator<<(char)` is declared before `operator<<(const char*)`, so
  `os << a << ':' << b` inserts `':'` and not the decimal of the promoted
  `int`.

`unordered_map` / `unordered_set`:

- Include guards also set `_GLIBCXX_UNORDERED_MAP` and `_GLIBCXX_UNORDERED_SET`.
- `std::pmr` aliases and `<bits/memory_resource.h>` are gone. They contradicted
  the fallback comment and the hash table's `std::allocator` static check.
- `operator[]` finds an existing key before emplacing, and does not move an
  rvalue key that is already present.
- `unordered_multiset` redeclares `insert` with iterator return type, plus
  the range and initializer-list overloads that declaration would hide.
  `using _Base::insert` next to a different return type is ill-formed.

## Houdini

`src/prism/pir/houdini.inc` only. `strcat_any_true` and `strncat_any_true`
are not closed. `docs/PIR.md` already says the templates do not contain the
invariant those loops need, and that a 100s run reaches a fixpoint whose
optimistic query is SAT. This slice did not run the solver and did not weaken
a candidate to force a proof.

Local changes, all dropped unless the base and the step prove them:

- `MemScopeAt` with `off < 0` means "no displacement" (`x < size(p)`). It is
  not a lookup of variable `-1`. That lookup is `def_block.at(size_t(-1))`
  and throws `std::out_of_range`, which is not `EncodeFail`, so a read-only
  base (the `strcpy` / `strcat` source) could abort candidate generation.
- Write-side `MemScopeAt` is also emitted with `sum` (`x <= i`), for the
  byte about to be written. The strict form still contains the literal
  `+ x < size(` that `tests/cpp/test_repolint.cpp` locks.
- After parameters are added to the instantiation set `T` (cap 24), each
  `MemEq` / `MemFrame` with `off >= 0` also instantiates `zext(param) - off`
  at the cut header. `strcat` / `strncat` read `d[j]` as the instance
  `x = j - k` of `d[k + x] == s[x]`. One more index of a proved universal
  does not weaken it; the quantifier guard still drops an out-of-range `x`.
  `EncodeFail` on the offset lookup is ignored. This does not by itself
  prove those two contracts.

## CMake

`PRISM_PIR_MODELS` is an explicit list. The new headers are not picked up by
a glob, so `CMakeLists.txt` gained only:

```
  # cxx-models: explicit list, not a glob
  src/prism/pir/models/cxx/unordered_map
  src/prism/pir/models/cxx/unordered_set
  src/prism/pir/models/cxx/prism_hash_table.h
  src/prism/pir/models/cxx/sstream
```

`prism_tree.h` and `bits/basic_string.tcc` were already listed.
`tests/cpp/test_cxx_models.cpp` was already in the doctest source list.
`tests/cxx_models/*.cpp` are compiled by that driver, not by CMake.

## Checked here

clang++ 18.1.3 against libstdc++ 13, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`, model
directory as `-isystem`. `sstream_trace` and `unordered_map_set_trace` print
the same stdout as libstdc++. No engine configure, no `prism_tests`, no
conformance run, no Houdini query.

## Parent must still measure

- `sstream_contracts` at timeout 300 and `unordered_map_contracts` at timeout
  900, including the expect classes above. A timeout is `NOTRUN`, not a pass.
- The prism cxx sstream and unordered true/false tasks (default timeout,
  witness line 6 on the false twins).
- Whether `strcat_any_true` / `strncat_any_true` move at all under the new
  `MemEq` / `MemFrame` index. Do not record them as `PROVED` from this note.
- Rehash does not invalidate iterators in this model. A task that expects
  the standard's invalidation will disagree with the model and with this
  trace.
