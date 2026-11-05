# PRISM SV-COMP verifier package

This directory holds the BenchExec-facing files of PRISM's SV-COMP entry.
[docs/SVCOMP.md](../../docs/SVCOMP.md) describes how answers are decided,
the witnesses, the data models and what is still missing for an entry.

## Layout

| file | role |
|---|---|
| `prism.py` | BenchExec tool-info module (`BaseTool2`); runs `prism svcomp` |
| `prism-subset.xml` | BenchExec benchmark definition for the pinned subset |
| `fm-tools.yml` | fm-tools registration draft (edit before upload) |

The verifier itself is the PRISM binary:

| command | role |
|---|---|
| `prism svcomp --prop P.prp [--data-model ILP32\|LP64] [--allow-exec] TASK.c` | what BenchExec runs on one task: answer, replay, witness 2.0 |
| `prism svcomp score [--property no-overflow\|unreach-call] [--jobs N]` | score the pinned subset without BenchExec |
| `prism svcomp pack --out prism-svcomp.tar.gz` | build the archive for submission |

## Dependencies on the competition machine

- The pinned LLVM toolchain (`third_party/llvm/PIN.toml`) for the `pir`
  stage: `PRISM_LLVM_BIN`, or the pinned build baked in at build time.
- `clang`, `gcc` or `cc` on `PATH` with UBSan/ASan for counterexample replay.
- `bubblewrap` (`bwrap`) to jail the replay; without it the replay runs under
  resource limits only.
- Python only for BenchExec itself, which loads the tool-info module.

The archive contains the PRISM binary. It does **not** bundle clang or opt;
`dependencies.json` records the versions found on `PATH` when it was built.

## Build the archive

From a built tree:

```bash
build/prism svcomp pack --out prism-svcomp.tar.gz
```

It packs the binary it is run as (or `--prism BIN`). The archive unpacks to:

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

## BenchExec

Copy `prism.py` into BenchExec's `benchexec/tools/`, or add this directory to
`PYTHONPATH` and point `--tool-directory` at the directory holding `prism`.
Run BenchExec from a directory with no `prism/` Python package in it, so
that the module name `prism` means this file.

The benchmark must pass `<option name="--allow-exec"/>`: every `false` answer
replays the counterexample, which compiles and runs the task (Law 9).

```bash
PYTHONPATH=tools/svcomp python -m benchexec.test_tool_info .prism \
  --tool-directory /path/to/unpacked/prism --no-container
```

## Licence

PRISM is AGPL-3.0-only. The `LICENSE` file in the archive is the repository
root licence.
