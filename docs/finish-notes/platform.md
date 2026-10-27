# Platform process runner

Slice `finish/platform-notrun`. `src/prism/stages/platform.cpp` is the stages'
process runner (`run_argv` / `run_spec`) and the plain-HTTP client. It is not
a row in `STAGE_ORDER`. `run_pipeline` (`src/prism/pipeline.cpp`) does not
call it. An empty finding list there becomes a stage status of `ok` with
`records == 0`, which is a clean-looking stage. That no-op does not apply
here: there is no platform stage row to record.

## Missing executable

`detail::run` already marks a program that cannot start (`failed`, `rc` 127,
`err` naming the reason). `run_spec` used to drop `failed`, so the caller saw
only an exit code. A missing program is now still that same `rc` and `err`,
and `ProcRun::failed` is kept.

`notrun_if_unstarted` writes one finding when `failed` is set:

| field | value |
|---|---|
| status | `NOTRUN` |
| strength | `READS` |
| message | the runner's `err`, or `tool failed to start` |
| `extra["install"]` | install the missing program, or pass the path of an executable |

It does not return an empty list, `CLEAN`, or `PROVED`. A child that started
(`failed == false`), including an exit of 127, is `nullopt`. That exit stays
the caller's verdict.

`http_request` / `http_ok` are unchanged. A missing server is still an empty
optional, and the LLM stage already turns an unreachable model into `NOTRUN`.

## Tests

`tests/cpp/test_platform.cpp`

- `platform: a missing executable is NOTRUN, never CLEAN or PROVED` — default
  `Config`, empty stdin, a path that is not a file. `failed`, `rc` 127, one
  `NOTRUN` row with an install hint.
- `platform: a started process stays unclassified` — exit 127 and exit 0
  with `failed == false` stay unclassified.

Not executed here: no `prism_tests` binary for this worktree, and the build
stays off OneDrive.
