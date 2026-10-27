# Task-definition `expected_verdict`

Slice `finish/taskdef-verdict`. `parse_taskdef` (`src/prism/taskdef.cpp`) reads one conformance or SV-COMP task object.

## The check

`properties[].expected_verdict` is optional. When the key is absent, the property's `expected_verdict` stays unset. The loader does not invent `true`.

When the key is present:

- A JSON or YAML boolean stays that boolean. YAML 1.1 still types an unquoted `true` / `false` (and the other YAML 1.1 boolean spellings) before this check runs. `parse_taskdef` keeps the boolean it is given.
- The strings `true` and `false`, compared exactly, become `true` and `false`.
- Any other type or text fails the load: `parse_taskdef` returns `nullopt` and sets the error string to `expected_verdict must be true or false`. The property is not dropped, and the value is not recorded as `true`.

Rejected text includes `TRUE`, `False`, `yes`, `no`, `1`, and `0`. A number (`1`, `0`) is not a boolean and is not that text, so it fails the same way. The old branch treated every non-empty non-boolean as true, so the string `false` and an unknown string were stored as expected-true.

## Left alone

`options.data_model` is still copied as text. An absent data model stays empty on the `TaskDef`. The LP64 default lives on the SV-COMP options elsewhere and was not changed. ILP32 stays opt-in. YAML 1.1 plain-scalar typing was not changed. No file under `testdata/` was edited.

## Tests

`tests/cpp/test_taskdef_verdict.cpp`, one line in `_prism_test_sources` next to `tests/cpp/test_svcomp.cpp`. The cases call `parse_yaml` or `nlohmann::ordered_json::parse`, then `parse_taskdef`. They do not spawn a process.

- Boolean `false` stays `false`. Boolean `true` stays `true`. Both from YAML and from JSON.
- Text `false` is `false`. Text `true` is `true`. Both from YAML and from JSON.
- Text `yes` and `FALSE`, and a number, fail the load. They do not produce a `TaskDef` whose expected verdict is `true`.
- A missing `expected_verdict` key stays unset.

`prism_tests` was not executed in this slice.
