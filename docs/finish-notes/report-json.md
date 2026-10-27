# report json

`RunReport::load` keeps a report whose message holds a byte that is not UTF-8. `dump_json` / `RunReport::dumps` already pass `error_handler_t::replace`, and `as_int` already reads an integer string (`"line":"3"` is 3 in `tests/cpp/test_cparse.cpp`). Those were left as they are.

## Load

nlohmann's parser throws on an ill-formed UTF-8 byte inside a string, and `RunReport::load` turned that into `nullopt`. A source byte in a message is still a report. On that parse failure the bytes are replaced with U+FFFD (the same replacement `dump_json` writes) and parsed once more. A file that is already valid UTF-8 but not JSON, or not a report shape, stays `nullopt`. The outer `catch` still returns `nullopt` and does not throw.

A JSON number in a string field is its JSON text (`7` becomes `"7"`). A string in an int field is parsed when it is an integer (`"12"` is 12, `"1"` on `records` stays 1 rather than the finding count). A non-numeric string leaves a finding's optional line empty. One bad field does not drop the record.

## Parsers that stay strict

`finding_from_json_object`, `stage_from_json_object` and `function_from_json_object` still throw through `parse_object` when the text is not a JSON object, including when a message byte is not UTF-8. Inside an object, fields stay tolerant. `journal_read_stages` still skips a line that throws. `journal.cpp` was not changed: `journal_read_functions` still rejects a wrong-typed entry and, if any entry is bad, returns an empty list so `--resume` reruns classify (Law 7). A partial function list is not a successful classify.

## Tests

`tests/cpp/test_report_load.cpp` (added to `_prism_test_sources`; the test target lists sources and does not glob):

- load of `"line":"12"` plus `0xFF` in a message returns a report, does not throw, keeps line 12, keeps a valid `é`, and stores U+FFFD for the bad byte
- `records":"1"` with two findings stays 1; `"file":7` is `"7"`; `"line":"nope"` is no line; `"message":1` is `"1"`
- `dumps()` of a message that contains `0xFF` does not throw and writes U+FFFD
- the three object parsers accept `"line":"12"` and throw on a non-object and on `0xFF`
