# SARIF rule name and CWE (D34)

`to_sarif` looks up the result class id in `taxonomy_classes()` (`TaxonomyClass::id`,
`name`, `cwe`). That accessor already exists; taxonomy sources were not changed
and no second table was added.

When the id is a taxonomy class:

- `name` and `shortDescription.text` are the taxonomy short name, when `name` is
  non-empty. The rule `id` stays the class id.
- `properties.cwe` is the taxonomy CWE list as a JSON array of integers, in
  taxonomy order (the same numbers `taxonomy.json` writes).

When `cwe` is empty, `properties.cwe` is omitted. An id that is not a class
(for example `fuzz/CRASH`) keeps `shortDescription.text` equal to the id and
gets neither `name` nor `cwe`.

Result `level` and `exit_code` / `--fail-on` are unchanged.

`tests/cpp/test_sarif.cpp` covers `MEM-UAF` (`use after free`, CWE-416) and
`CTRL-FALLTHROUGH` (name set, no CWE).
