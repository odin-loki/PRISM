# The Lean database (`prism leandb`)

PRISM owns the Lean proof data (Odin, 18 Nov 2026: "Lean data is PRISM data"). It is built and searched with the FORGE
libraries only: `forge::db` (vendored SQLite, parallel reads), `forge::task` (thread pool, deterministic merges) and
`forge::ann` (reciprocal-rank fusion; the symbolic metric of `docs/SIM_SEARCH.md`). FORGE ships no Lean tool.
Everything here is opt-in tooling: no verdict path, solver default or certified path reads it.

## Layout

```
data/leandb/PIN.json            the 21 sources: revision / commit / record, licence as recorded, use, SHA-256 of every file
data/leandb/ATTRIBUTION.md      attributions required by the licences of committed sources
data/leandb/shards/             committed: sorted SQLite shards <= 10 MiB each + manifest.json   (redistributable sources only)
data/leandb/local/              gitignored: the same layout for sources that are not redistributable (built on your machine)
scripts/leandb_make_pin.py      regenerates PIN.json from OS-Proving's manifest.tsv and sources.json
scripts/leandb_fetch_local.py   downloads pinned sources from the original hosts, checks SHA-256, normalises, imports
tools/leandb/                   OS-Proving's normaliser (build_lean_db.py, leanparse.py), copied unmodified
src/leandb/, include/prism/leandb.hpp   the tool
```

`prism leandb search` reads `data/leandb/shards` **and** `data/leandb/local` together whenever the local shards exist, with
no flag. BM25 statistics are taken over all present shards, so adding local data changes rankings exactly as if it had been
in one database. `PRISM_LEANDB_DIR` moves the root; `--db DIR` (repeatable) names shard directories explicitly, for example
the private complete copy.

## Commands

```
prism leandb import LEAN_DB.sqlite [--pin PIN.json] [--committed DIR] [--local DIR] [--all DIR] [--sources a,b] [--threads N]
prism leandb build-local LEAN_DB.sqlite [--pin PIN.json] [--out DIR]
prism leandb search "QUERY" [-k N] [--mode bm25|ann|sym|hybrid] [--threads N] [--db DIR]...
prism leandb verify [--db DIR]... [--pin PIN.json]
```

* `import` reads the normalised `lean_db.sqlite` that OS-Proving's `tools/lean_db/build_lean_db.py` builds (schema:
  OS-Proving `data/lean-db/SCHEMA.md`, version 1.0) and writes shard sets. `--committed` takes the sources the pin marks
  `redistributable`, `--local` takes all others, `--all` takes every source into one set (the private complete copy).
  `--sources` restricts to named sources.
* `build-local` is `import --local` with the default output `data/leandb/local`; it is what the fetch script calls.
* `search` modes: `bm25` (FTS5 ranking with global statistics), `ann` (256-bit SimHash of the identifier and statement
  words, Hamming distance), `sym` (the goal is translated to a term and compared with `prism::sim::symbolic_metric` against
  the stored vectors of goals that translated; a query that does not translate returns nothing), `hybrid` (reciprocal-rank
  fusion, k = 60, of bm25, ann and, when the query translates, sym).
* `verify` checks, per shard directory: every file is at most 10 MiB; sizes and SHA-256 equal `manifest.json`; the
  manifest's `pin_sha256` equals the pin file's; every source's licence text equals the pin's; a committed set contains only
  sources the pin marks redistributable; eval shards have no index; search shards have no eval row and no
  eval-overlapping row in the `indexed` table; the `indexed` table equals the searchable flags and the FTS token total
  equals the manifest's. It exits 1 on any failure.

## Shards

Each shard is a SQLite file with a fixed schema (`kSchemaVersion` 1):

* `meta(key,value)`, `srcs(id,name,use,license)`.
* `items(id, src, tbl, orig_id, kind, module, name, statement, proof, norm_hash, eval_overlap, searchable, simhash, symvec)`.
  `id` is the first 60 bits of sha256(source, table, input row id): stable across rebuilds and independent of other shards.
  `tbl` is 0 decls, 1 problems, 2 trace theorems. `statement` is the declaration signature (decls: plus the docstring as
  `/-- ... -/`) or the formal statement; `proof` is the declaration body or formal proof; traces keep the statement only.
  `symvec` is a `prism::sim::sym_vec` blob for statements the Lean adapter translates (BitVec, Nat/Int linear, Bool, Fin).
* Search shards also have `fts` (FTS5 over `name, statement`, external content = `items`, filled only for searchable rows)
  and `indexed(id)`, the list of ids that went into any index.
* Eval shards (`eval-NNNN.sqlite`) have `items` only: no FTS table, no vectors, `searchable = 0`.

**Eval isolation.** A row is searchable only if its source is not `eval`, it is not flagged `eval_overlap` by the
normaliser (exact or near duplicate of an eval problem), and its source is not of unverified provenance. Everything else
is stored but in no index. Eval sources go to their own `eval-*` shards.

**Determinism.** Rows are written in sort-key order, SQLite page size and journal settings are fixed, the FTS index is
optimised before close, nothing carries a timestamp. A rebuild from the same input is byte-identical (tested), so unchanged
shards cause no git churn, and `scripts/leandb_fetch_local.py --all --compare data/leandb/shards` is a reproducibility
check against upstream.

**Sort key and shard boundaries.** Rows are sorted by `(use class, table, source, module, name, id)` and cut into files at
about 9.5 MiB (estimated before writing; a file that comes out over 10 MiB is split in half and rewritten, so the limit is
never exceeded). Why this key:

* `use class` first: library, data and eval never share a file, so eval shards can be dropped or audited whole.
* `table` next: declarations, problems and traces have different shapes and sizes.
* `source` next: a source update touches only its own shards (local git churn), and the licence class is a function of the
  source, so the committed and local sets partition cleanly with no mixed file.
* `module` then `name`: a namespace or module prefix (`Mathlib.Algebra.Group.`) is one contiguous run of rows, so a prefix
  lookup touches few shards, and declarations that import each other sit together, which is what premise retrieval wants.
  `manifest.json` lists each shard's first and last key, so a lookup by key is a binary search over the manifest followed by
  one shard.
* `id` last: a total order, so the layout is deterministic.

Alternatives rejected: sorting by `norm_hash` (good for dedup, destroys module locality and spreads every source over every
shard), sorting by id (random, no locality). A separate name-to-shard index and a `norm_hash` index were considered and
not built: dedup uses the stored `norm_hash` column inside the shards, and no command needs a second key yet.

**Parallel search.** Each query runs one task per shard on a `forge::task::pool`: each task opens its shard read-only
(`immutable=1`, no locks) and returns a top-k run; `forge::task::merge_runs` merges the runs with ties broken by id, so the
result does not depend on the thread count. Vector search is an exact scan of each shard's blobs: a shard holds at most
about 20 000 vectors, so a per-shard HNSW graph would cost more than it saves; the global top-k is the merge of the
per-shard exact top-k (equal to a flat search over everything).

**BM25 equals the unsharded database.** `prism_bm25` is FTS5's `bm25()` reimplemented as an FTS5 auxiliary function with
the same formula (k1 = 1.2, b = 0.75, `idf = max(log((N - n + 0.5)/(n + 0.5)), 1e-6)`) but with N, the average document
length and each query term's document frequency summed over all present shards (document counts and token totals are in the
manifest; frequencies are counted per query). The test `leandb: sharded BM25 equals the unsharded FTS5 bm25` builds the
same rows into one FTS5 table and compares the ten best ids and scores for several queries.

## Licences

`PIN.json` keeps each source's `license` exactly as OS-Proving recorded it. `redistributable` and `evidence` are PRISM's
reading of the dataset card or LICENSE file. A source is committed only when `redistributable` is true; the rest stay
pin-only. What was found in the dataset cards (18 Nov 2026):

| source | recorded licence | what the card / LICENSE says | committed |
|---|---|---|---|
| lean4, batteries, mathlib4, lean-smt | Apache-2.0 | GitHub API licence field (checked by OS-Proving) | yes |
| LeanDojo | MIT | GitHub API licence field | yes |
| leandojo-benchmark-4-v10 | CC-BY-2.0 | Zenodo record 12740403; attribution required | yes, with attribution |
| leandojo-benchmark-4-2026 | CC-BY-4.0 | newer third-party upload, provenance unverified | no (local) |
| Lean-Workbook, Lean-Github, Herald_proofs, MathOlympiadBench | Apache-2.0 | card front matter `license: apache-2.0` | yes |
| NuminaMath-LEAN | see dataset card | card: `license: apache-2.0`, "available under the Apache License, Version 2.0" | yes |
| Goedel-Lean-workbook-proofs, Goedel-Pset-v1 | see dataset card | dataset LICENSE file: "This dataset is licensed under the MIT License." | yes |
| DeepSeek-Prover-V1 | other (see dataset card) | card: `license: other`, `deepseek-license`; the dataset's LICENSE file is empty; code MIT, models under a Model Agreement | no (local) |
| FormalMATH-All, miniF2F-lean4, minif2f-lean4-hf, ProofNet-lean4, proofnet-hf | MIT | card / API | yes (eval shards) |
| PutnamBench | none stated (check repo) | README: "Apache 2.0 for Lean 4 and Isabelle, MIT for Coq"; `lean4/LICENSE` is Apache-2.0; only `lean4/` is ingested | yes (eval shards) |

The recorded licence strings were not changed even where the card gives more (NuminaMath-LEAN, Goedel, PutnamBench): the
card text is in `evidence`. If a card is wrong, the pin is a one-line fix and the shards are rebuilt.

## Fetching the pin-only sources

```
scripts/leandb_fetch_local.py            # the sources marked redistributable=false, then build-local
scripts/leandb_fetch_local.py --sources DeepSeek-Prover-V1,leandojo-benchmark-4-2026 --yes
scripts/leandb_fetch_local.py --all --compare data/leandb/shards    # rebuild everything, compare with the committed shards
```

It prints each source's name, upstream URL and licence as pinned and asks you to confirm (`--yes` for scripts): you are
responsible for each dataset's terms. Downloads are exactly the pinned revisions (Hugging Face dataset revision, Zenodo
record, git commit); every file's SHA-256 is checked against the pin, a mismatch removes the file and exits 2. It resumes
partial files (HTTP Range into `.part`), retries with backoff, checks free space, and draws `rich` progress bars when
`rich` is installed (plain progress lines otherwise). Downloads go to `~/.prism/leandb/downloads` by default
(`--dest`). The normaliser needs `pip install pyarrow ijson` for the parquet and LeanDojo sources.

## The complete private copy (OS-Proving)

OS-Proving holds every source, including the pin-only and eval ones, with licence fields intact, as the same sorted shards:

```
prism leandb import /mnt/d/ParanoidBSD/lean-db/lean_db.sqlite --pin data/leandb/PIN.json \
    --all /mnt/c/ParanoidBSD/OS-Proving/data/lean-db/shards
prism leandb verify --db /mnt/c/ParanoidBSD/OS-Proving/data/lean-db/shards --pin data/leandb/PIN.json
prism leandb search "..." --db /mnt/c/ParanoidBSD/OS-Proving/data/lean-db/shards
```

The `--all` set has its own `manifest.json` (`class: all`), keeps eval sources in `eval-*` shards, and never puts them in an
index.

## Checks in the test suite

`prism_tests` (`tests/cpp/test_leandb.cpp`) covers: import and sorted, non-overlapping shards within 10 MiB; rebuild is
byte-identical; every search mode; local shards joining by default; no eval or eval-overlapping row returned in any mode;
sharded BM25 equal to unsharded; tamper, pin-mismatch detection; the committed shards in the repository verify (SHA-256
equal to the manifest, size limit, no eval in an index); and the fetch script end to end against a local fake source
(wrong hash fails, right hash downloads, imports, and search finds the row).

## Premise retrieval evaluation

`tools/leandb/eval_premises.py LEAN_DB.sqlite --prism build/prism --db data/leandb/shards [--db data/leandb/local]` measures
recall@k of bm25, ann, sym and hybrid on the LeanDojo benchmark test split (`split_random = 'test'` of
leandojo-benchmark-4-v10): the query is the theorem's statement, the gold set is the premises its tactic steps use that resolve
to a library declaration, the theorem itself is removed from the results. It also runs the leakage test (no result from an
eval source, none flagged `eval_overlap`) and exits 1 if a result leaks. See "Measured" below for the numbers of the run.

## Relation to forge::db shards

FORGE gained a generic `forge::db::shard_writer` / `shard_set` (support/include/forge/db/shards.hpp) while this tool was being
written. `prism leandb` uses `forge::db` (connection, statements, transactions, FTS5 helpers) and `forge::task` directly, and
its own shard writer and manifest (`manifest.json`, class `committed`/`local`/`all`), because it needs two shard kinds (search
and eval) with three tables and a BM25 over several shard directories at once (committed plus local). Moving the writer and
the single-directory search onto `shard_writer` / `shard_set` is mechanical and is the follow-up; the on-disk shard schema
above would not change.

## Measured

**Not yet measured on the real database.** When this was written OS-Proving's `lean_db.sqlite` was still being built
(`lean_db.sqlite.new` was 6.5 GB and growing), so `prism leandb import` was not run on it, no real shards are committed
(`data/leandb/shards` does not exist yet and the repository test skips), and the premise-retrieval recall of bm25, sym and
hybrid on the LeanDojo test split (`tools/leandb/eval_premises.py`) was not run. What was run is the fixture suite
(`tests/cpp/test_leandb.cpp`, 9 cases, all passing): import, sorted shards, byte-identical rebuild, identical shards for the
same rows under different input row ids, a one-source rebuild reproducing exactly that source's shards, every search mode,
local shards joining by default, no eval leakage, sharded BM25 equal to the unsharded FTS5 bm25, tamper detection, and the
fetch script end to end against a local fake source.

To produce the real numbers once the database is complete:

```
prism leandb import /mnt/d/ParanoidBSD/lean-db/lean_db.sqlite --pin data/leandb/PIN.json \
    --committed data/leandb/shards --local data/leandb/local
prism leandb verify
tools/leandb/eval_premises.py /mnt/d/ParanoidBSD/lean-db/lean_db.sqlite --prism build/prism \
    --db data/leandb/shards --db data/leandb/local --out premise_recall.json
du -sh data/leandb/shards; ls data/leandb/shards | wc -l      # stop before committing if the total is over about 1.5 GB
```
