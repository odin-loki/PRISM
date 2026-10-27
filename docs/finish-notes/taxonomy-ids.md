# Taxonomy class ids

`taxonomy_classes()` (`include/prism/taxonomy.hpp`) returns the static table in
`src/prism/taxonomy.cpp` (the initializer from `MEM-OOB-READ` through
`CTRL-LONGJMP-INVALID`). Inspection of that table: 641 classes.

- Every `id` is non-empty.
- Every `id` is unique. There is no duplicate id, so there is no pair to merge
  and no checker to drop.
- Every `name` is non-empty.
- No `id` is the status string `PROVED`, `BOUNDED`, `CLEAN`, or `NOTRUN`.

The table was left unchanged. `tests/cpp/test_taxonomy_ids.cpp` walks
`taxonomy_classes()` and checks the same four properties (a duplicate id fails
the test). It is registered in `CMakeLists.txt`. `tests/cpp/test_taxonomy.cpp`
was not edited.
