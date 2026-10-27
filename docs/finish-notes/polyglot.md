# Polyglot missing python3 and JSON syntax

Slice `finish/polyglot-notrun`. Python source syntax stays an external tool.
JSON and TOML syntax stay in process (`json_syntax`, `toml_syntax`).

## Statuses locked

One `.py` file, with `cfg.tools` empty and `polyglot::ExeWhich` returning
nullopt for every name:

| field | value |
|---|---|
| status | `NOTRUN` |
| strength | `FINDS` |
| message | `python-syntax: python not found for 1 python file(s)` |
| extra.install | `install Python 3 (python3 on PATH)` |
| extra.check | `python-syntax` |

The message names the tool `python`. The install hint names `python3`.
Strength is `FINDS`. Status is `NOTRUN`. `laws::is_proof` is false.
`run_check` writes that row from `resolve` returning nullopt and does not
call `run_process`.

One broken `.json` file (`{"a": 1,}`), through `run_polyglot` and
`json_syntax`:

| field | value |
|---|---|
| status | `FAILED` |
| class | `SYNTAX-ERROR` |
| file / line | `bad.json` / 1 |
| strength | `FINDS` |
| extra.tool | `prism-json` |
| extra.check | `json-syntax` |
| message | prefix `prism-json: JSON: ` |

Status is `FAILED`. It is a syntax defect.

One valid `.json` file (`{"a": [1, 2]}`):

| field | value |
|---|---|
| status | `UNKNOWN` |
| strength | `FINDS` |
| message | `prism-json: 1 json file(s), no diagnostics (not a proof)` |

No finding from that run is `PROVED`, `PROVED-UNBOUNDED`, `PROVED-CERTIFIED`,
or `PROVED-ASSUMING`, and no row has strength `PROVES`. The json row's
strength is `FINDS`. The built-in credential scan is a separate `CLEAN` row
whose message says "(not a proof)".

TOML uses the same native path (`toml_syntax`): a syntax issue is `FAILED`
`SYNTAX-ERROR`, and a quiet file is `UNKNOWN` with "no diagnostics (not a
proof)". This slice's new test locks the JSON strings above.

## Lookup seam

`Config::which` has no override, and on Windows it searches the application
directory, the current directory, and the system directories before `PATH`.
Clearing `PATH` (as `tests/cpp/test_main.cpp` still does for its broader
missing-tool case) does not hide an interpreter in those directories.

`include/prism/polyglot.hpp`:

- `polyglot::ExeWhich` — one executable name to a path, or nullopt.
- `resolve(tool, cfg, which)`. `which == nullptr` keeps today's order:
  `cfg.tools` when the file exists, then the pinned build, then `PATH`.
  A non-null `which` replaces the pinned build and `PATH` and is final.
- `run_polyglot(root, cfg, which)` — the stage with that lookup. The
  two-argument `run_polyglot` passes nullptr. Native JSON and TOML checkers
  do not call `which`.

Production callers pass nullptr, so a missing `python3` is still the same
`NOTRUN` row.

## Tests

`tests/cpp/test_polyglot_missing.cpp`, added to the `prism_tests` source
list in `CMakeLists.txt`.

Not executed in this slice: no cmake, ninja, or `prism_tests`.

## Left unchanged

`src/prism/config.cpp`, `src/prism/cli.cpp`, `src/prism/pipeline.cpp`, the
`NOTRUN` message text, `json_syntax`, and `toml_syntax`. Python syntax is
still `python -I -c` of `PY_COMPILE` when an interpreter is found. No flag
turns a missing interpreter into a pass.
