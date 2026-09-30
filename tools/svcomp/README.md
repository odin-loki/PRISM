# PRISM SV-COMP verifier package

This directory is the BenchExec-facing SV-COMP entry for PRISM (roadmap 6.3).
See [docs/SVCOMP.md](../../docs/SVCOMP.md) for mapping rules, the pinned subset
score and witness validation.

## Layout

| file | role |
|---|---|
| `prism.py` | BenchExec tool-info module (`BaseTool2`); runs `prism svcomp` |
| `prism-subset.xml` | local BenchExec benchmark for the pinned subset |
| `fm-tools.yml` | fm-tools registration metadata (edit before upload) |

The verifier itself is the PRISM binary:

| command | role |
|---|---|
| `prism svcomp --prop P.prp [--data-model ILP32\|LP64] [--allow-exec] TASK.c` | what BenchExec runs on one task: answer, replay, witness 2.0 |
| `prism svcomp score [--property no-overflow\|unreach-call] [--jobs N]` | score the pinned subset without BenchExec |
| `prism svcomp pack --out prism-svcomp.tar.gz` | build the archive for submission |

## Dependencies on the competition machine

- `clang` and `opt` on `PATH` for the `pir` stage (tested with LLVM 18); `clang`
  (or `gcc`) with UBSan/ASan for counterexample replay
- `bubblewrap` (`bwrap`) optional but recommended for counterexample replay
- Python only for BenchExec itself, which loads the tool-info module

The archive contains the PRISM C++ binary only. It does **not** bundle clang.

## Build the archive

From a built tree:

```bash
build/prism svcomp pack --out prism-svcomp.tar.gz
```

It packs the binary it is run as (or `--prism BIN`). Unpack to a BenchExec
tools directory (or pass `--tool-directory`):

```text
prism/
  prism
  prism.py
  README.md
  fm-tools.yml
  LICENSE
  MANIFEST.json
  dependencies.json
```

`dependencies.json` records the clang/opt versions seen when the archive was
built (not bundled) and optional `PRISM_*` environment knobs documented for
competition runs.

## BenchExec

Copy `prism.py` into BenchExec's `benchexec/tools/` (or add this directory to
`PYTHONPATH` and point `--tool-directory` at the unpacked `prism/` folder).
Run BenchExec from a directory that has no `prism/` Python package in it, so
that the module name `prism` means this file.

The benchmark must pass `<option name="--allow-exec"/>`: every `false` answer
replays the counterexample (Law 9).

```bash
PYTHONPATH=tools/svcomp python -m benchexec.test_tool_info .prism \
  --tool-directory /path/to/unpacked/prism --no-container
```

## Licence

PRISM is AGPL-3.0-only. The `LICENSE` file in the archive is the repository
root licence.
