# PRISM SV-COMP verifier package

This directory is the BenchExec-facing SV-COMP entry for PRISM (roadmap 6.3).
See [docs/SVCOMP.md](../../docs/SVCOMP.md) for mapping rules, the pinned subset
score and witness validation.

## Layout

| file | role |
|---|---|
| `prism_svcomp.py` | executable BenchExec runs on one task |
| `witness.py` | SV-COMP witness format 2.0 writer |
| `prism.py` | BenchExec tool-info module (`BaseTool2`) |
| `prism-subset.xml` | local BenchExec benchmark for the pinned subset |
| `run_subset.py` | score the subset without BenchExec |
| `package_archive.py` | build `prism-svcomp.tar.gz` for submission |
| `fm-tools.yml` | fm-tools registration metadata (edit before upload) |

## Dependencies on the competition machine

- `python3` (3.10+; stdlib only in the wrapper)
- `clang` and `opt` on `PATH` for the `pir` stage (tested with LLVM 18)
- `bubblewrap` (`bwrap`) optional but recommended for counterexample replay

The archive contains the PRISM C++ binary only. It does **not** bundle clang.

## Build the archive

From a built tree:

```bash
python tools/svcomp/package_archive.py --prism build/prism --out prism-svcomp.tar.gz
```

Unpack to a BenchExec tools directory (or pass `--tool-directory`):

```text
prism/
  prism
  prism_svcomp.py
  witness.py
  prism.py
  README.md
  LICENSE
  MANIFEST.json
```

## BenchExec

Copy `prism.py` into BenchExec's `benchexec/tools/` (or add this directory to
`PYTHONPATH` and point `--tool-directory` at the unpacked `prism/` folder).

The benchmark must pass `<option name="--allow-exec"/>`: every `false` answer
replays the counterexample (Law 9).

```bash
PYTHONPATH=tools/svcomp python -m benchexec.test_tool_info .prism \
  --tool-directory /path/to/unpacked/prism --no-container
```

## Licence

PRISM is AGPL-3.0-only. The `LICENSE` file in the archive is the repository
root licence.
