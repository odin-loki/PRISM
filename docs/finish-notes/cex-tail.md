# Counterexample tail

Interpreter and sandbox output keep the end of the stream. A timed-out run is not `CLEAN`.

## Behavior

- `tail` (`llm.cpp`) keeps the last N bytes, stepping forward off a split UTF-8 character. `sandbox_run` stores the last 2000 bytes of stdout and stderr. `interpreter_loop` stores the last 400 of stdout on `CLEAN` and of stderr on `CRASH`.
- A run timeout still returns the partial stdout and stderr the child had already written. `code` is null and `error` is `timeout`. `sandbox_verdict` maps `timeout` and `compile-timeout` to `FAILED`, including a result that also says `ok`. It does not become `CLEAN`.
- `exec-disabled` and a missing compiler stay `NOTRUN` (Law 9). Without `--allow-exec`, `sandbox_run` does not compile or run, and `interpreter_loop` does not ask the model.
- `CLEAN` and `CRASH` from `interpreter_loop` copy `extra.sandbox` when the run result has that string. `CRASH` also copies `extra.code` when the result has `code`. Concrete replay rows keep `extra.oracle = concrete-replay`; that path interprets in process and does not start a sandbox.

## Tests

`tests/cpp/test_agent.cpp`

- Verdict: timeout and compile-timeout are `FAILED`, never `CLEAN`; exec disabled stays `NOTRUN`.
- `tail` at 400 and 2000 keeps a marker at the end. The same-length prefix does not.
- When gcc or clang is on `PATH`, `sandbox_run` keeps `USEFUL-TAIL` inside the last 2000 bytes, and a spinning child that printed `PARTIAL-OUT` before the deadline comes back as `timeout` with that text, not `CLEAN`.
- The interpreter `CLEAN` row keeps `MARK` inside the last 400 bytes of stdout and sets `extra.sandbox`, not `extra.code`.
