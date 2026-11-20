# tools/leandb

`build_lean_db.py` and `leanparse.py` are copied unmodified from OS-Proving `tools/lean_db/` (same author). They
normalise the raw Lean downloads into one SQLite file (`lean_db.sqlite`), which `prism leandb import` /
`prism leandb build-local` turns into sorted shards (docs/LEANDB.md).

They expect OS-Proving's layout (`tools/lean_db/sources.json`, `data/lean-db/manifest.tsv` next to the script).
`scripts/leandb_fetch_local.py` writes those two files from `data/leandb/PIN.json` into a scratch tree, copies these
scripts there and runs them, so nothing here is edited. They need `pyarrow` and `ijson` for the parquet and
LeanDojo-benchmark sources (`pip install pyarrow ijson`).
