# Sandbox timeout

A sandboxed run that hits its wall-clock deadline keeps the stdout and stderr the child had already written. The deadline stays a timeout.

## Behavior

`detail::run` (`src/prism/proc.cpp`) is the runner behind `sandbox::wrap_argv` and `sandbox::limits_for`. While the child runs it reads both pipes. When the deadline fires it kills the child, sets `timed_out`, and reads what is still in the pipes. Those bytes come back on `out` and `err`.

`rc` is the kill status (not 0). `failed` stays false: the tool did start, so this is not a missing-tool `NOTRUN`. `crashed` stays false: the kill is the deadline. The status shape of that result is `TIMEOUT`.

`sandbox::apply_child_limits` is unchanged. The CPU rlimit stays a second past the wall-clock deadline. This slice does not add a switch that skips the sandbox, and it does not run scanned code without `--allow-exec`.

The runner keeps the whole captured stream. Callers that store a short note (`execute_cex` `note_sandbox_output` takes the last 400 bytes, `sandbox_run` the last 2000) still see that tail, because the bytes are there. `sandbox.cpp` was not changed.

## Tests

`tests/cpp/test_sandbox.cpp` (added to `_prism_test_sources`; the test target lists sources and does not glob):

- A missing executable is `failed`, rc 127, with `timed_out` clear. That shape is `NOTRUN`.
- The child writes 500 filler bytes and then `STDOUT-TAIL` / `STDERR-TAIL`, and the writer exits before the hang (`cat` / `type`, then `sleep` / `ping`). The run is wrapped with `sandbox::wrap_argv` and `sandbox::limits_for`. It comes back `timed_out`, with `rc != 0`, `failed` clear, and `crashed` clear. The last 400 bytes of each stream contain the marker.

## Left as the runner implements it

Output is not truncated to 400 bytes inside the runner. A prefix-only capture would drop the marker; the test fails on that, and on an empty string.
