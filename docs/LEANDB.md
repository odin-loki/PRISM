# The Lean database (`prism leandb`)

PRISM owns the Lean proof data (Odin, 18 Nov 2026: "Lean data is PRISM data"). It is built and searched with the FORGE
libraries only: `forge::db` (vendored SQLite, parallel reads), `forge::task` (thread pool, deterministic merges) and
`forge::ann` (reciprocal-rank fusion; the symbolic metric of `docs/SIM_SEARCH.md`). FORGE ships no Lean tool.
Everything here is opt-in tooling: no verdict path, solver default or certified path reads it.

## Layout

```
data/leandb/PIN.json            the 21 sources: revision / commit / record, licence as recorded, use, SHA-256 of every file
data/leandb/ATTRIBUTION.md      attributions required by the licences of committed sources
data/leandb/shards/             committed (redistributable sources only):
    leandb.json                   class, pin and input hashes, the sources with their licences, totals
    search/manifest.json + shard-NNNNNN.fdb   forge::db shard set: sorted SQLite shards <= 10 MiB (FTS5, per-shard HNSW)
    eval/manifest.json + shard-NNNNNN.fdb     forge::db shard set of the eval sources: rows only, no index of any kind
data/leandb/local/              gitignored: the same layout for sources that are not redistributable (built on your machine)
scripts/leandb_make_pin.py      regenerates PIN.json from OS-Proving's manifest.tsv and sources.json
scripts/leandb_fetch_local.py   downloads pinned sources from the original hosts, checks SHA-256, normalises, imports
tools/leandb/                   OS-Proving's normaliser (build_lean_db.py, leanparse.py), copied unmodified
src/leandb/, include/prism/leandb.hpp   the tool
```

The shard machinery is FORGE's (`forge::db::shard_writer`, `shard_set`, `shard_bm25_topk`, `forge::ann::shard_ann`;
docs/db.md and docs/ann.md in FORGE). `prism leandb search` reads `data/leandb/shards` **and** `data/leandb/local` together whenever the local shards exist, with
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
* `verify` checks, per shard directory: every file is at most 10 MiB; sizes and SHA-256 equal the `manifest.json` of its
  set (`shard_set::verify_all`); `leandb.json`'s `pin_sha256` equals the pin file's; every source's licence text equals the pin's; a committed set contains only
  sources the pin marks redistributable; eval shards have no index; search shards have no eval row and no
  eval-overlapping row in the `indexed` table and no vector for an excluded row; the `indexed` table equals the rows of
  `items`, and the FTS row and token counts equal the manifest's. It exits 1 on any failure.

## Shards

Each shard is a SQLite file written by `forge::db::shard_writer` (presorted waves, byte-reproducible, `VACUUM`ed, budget
10 MiB checked on the final file; a file that comes out over is split and rewritten):

* `meta(key,value)`, `srcs(id,name,use,license)` (the one source of the shard's group).
* `items(key, id, src, tbl, kind, module, name, statement, proof, norm_hash, eval_overlap, searchable, simhash, symvec)`.
  `key` is the sort key (BLOB, memcmp order). `id` is the first 60 bits of sha256(source, table, natural key), where the
  natural key is file:line:kind:name for declarations, the dataset id and statement hash for problems, file:name:line for
  traced theorems: never the input database's row id, which depends on which sources were normalised before it.
  `tbl` is 0 decls, 1 problems, 2 trace theorems. `statement` is the declaration signature (decls: plus the docstring as
  `/-- ... -/`) or the formal statement; `proof` is the body or formal proof; traces keep the statement only. `simhash` is
  the codec bytes of a 256-bit SimHash (`vector<uint64_t>`), `symvec` a `prism::sim::sym_vec` for statements the Lean
  adapter translates.
* Search shards also have `fts` (FTS5 over `name, statement`, external content = `items`), `indexed(id)`, `excluded`
  (rows stored but never indexed, see below), and the per-shard HNSW tables `ann_text` (Hamming over the SimHash) and
  `ann_sym` (`symbolic_metric` over the sym_vecs), written by the writer's finalize hook with `forge::ann::ann_finalize`.
  FORGE also stores per-term document counts (`forge_df`) there.
* Eval shards have `items`, `srcs`, `meta` only: no FTS table, no vectors, `searchable = 0`.

**Eval isolation.** A row is indexed only if its source is not `eval`, it is not flagged `eval_overlap` by the normaliser
(exact or near duplicate of an eval problem), and its source is not of unverified provenance. Every other row is kept in
the `excluded` table of its shard (so `items` holds exactly the indexed rows, which is also what an external-content FTS5
table counts) and in no index. Eval sources go to their own `eval/` shard set.

**Determinism.** A rebuild from the same input is byte-identical (tested), so unchanged shards cause no git churn.
Each (use class, table, source) group is written by its own `shard_writer` and the groups' shards are then renumbered into
one set, so a rebuild of a single source from upstream reproduces exactly that source's shards (tested; shards are matched
by their first key). `scripts/leandb_fetch_local.py --all --compare data/leandb/shards` is that check against upstream.

**Sort key and shard boundaries.** Rows are sorted by `(use class, table, source, module, name, id)`; the writer cuts
files at 90% of the budget by a measured size ratio, and a shard never spans two groups. Why this key:

* `use class` first: library, data and eval never share a file, so eval shards can be dropped or audited whole.
* `table` next: declarations, problems and traces have different shapes and sizes.
* `source` next: a source update touches only its own shards (local git churn), and the licence class is a function of the
  source, so the committed and local sets partition cleanly with no mixed file.
* `module` then `name`: a namespace or module prefix (`Mathlib.Algebra.Group.`) is one contiguous run of rows, so
  `shard_set::shards_for_prefix` touches few shards, and declarations that import each other sit together, which is what
  premise retrieval wants. The manifest lists each shard's first and last key, so a lookup by key is a binary search.
* `id` last: a total order, so the layout is deterministic.

Alternatives rejected: sorting by `norm_hash` (good for dedup, destroys module locality and spreads every source over every
shard), sorting by id (random, no locality). A separate name-to-shard index and a `norm_hash` index were considered and
not built: dedup uses the stored `norm_hash` column inside the shards, and no command needs a second key yet.

**Search** (`src/leandb/search.cpp`). Every present directory's `search/` shard set (committed, local, a private complete
copy) is merged into one `shard_set` by a small manifest written to a temporary directory whose shard files are absolute
paths, sorted by first key (the sets hold disjoint sources, so the ranges do not overlap). That makes BM25 and the vector
search see all of them:

* `bm25`: `db::shard_bm25_topk(set, pool, "fts", terms, k)`, FORGE's exact global BM25 (N, average length and each term's
  document frequency summed over every shard; column weights name 2, statement 1), with early stop. The test
  `leandb: sharded BM25 equals the unsharded FTS5 bm25` compares the ten best ids and scores with FTS5's own `bm25()` over
  one table holding the same rows.
* `ann` / `sym`: `ann::shard_ann` over the per-shard HNSW graphs (`ef = 128`; FORGE measured recall@10 0.86 at ef 64 on hard
  data), the per-shard top-k merged deterministically.
* `hybrid`: `ann::reciprocal_rank_fusion` of the three id lists.

`prism leandb search --batch` and `leandb::searcher` keep the merged set and the loaded graphs between queries.

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

`prism leandb` is built on FORGE's `shard_writer`, `shard_set`, `shard_bm25_topk` and `shard_ann` as they are in FORGE PR #74
(head 86a08bd); PRISM builds against that. FORGE lacked nothing the tool needed, with two workarounds on the PRISM side:
(1) a `shard_writer` cuts shards by bytes only, so each source group gets its own writer and the groups' manifests are merged
by renumbering the files (this is what makes a one-source rebuild reproduce that source's shards); (2) a `shard_set` is one
directory, so committed plus local shards are searched through an in-memory merged manifest with absolute shard paths. If FORGE
later adds a sidecar to the set (a term filter file is in progress there), the merged manifest must carry or ignore it; that is
a follow-up, not something this PR depends on.

## Measured (19 Nov 2026, on the real database)

Input: OS-Proving `lean_db.sqlite` (8.97 GB, schema 1.0, verified there: 50 raw-file hashes match, no dropped rows). The
file is read from a copy on the WSL ext4 disk; reading it through the `/mnt/d` mount ran at 1 to 5 MB/s under load.

**Size of the committed set: over the 1.5 GB limit, so nothing was committed.** `data/leandb/shards` does not exist.
Measured on the library sources (lean4, batteries, mathlib4, lean-smt): 450,144 items became 52 shards, 457,445,376 bytes
(437 MiB), in 2 min 41 s with 4 threads and 477 MB peak memory; that is about 3.9 bytes of shard per byte of statement and
proof text. The redistributable sources hold 2,862,413 rows and 1,014 MB of text, so the committed set would be about
3.5 to 4 GB. I stopped a full import of it when this ratio was clear (it had written 270 MB in 25 minutes) and did not
measure the exact total. The bulk, by statement and proof text:

| source | rows | text |
|---|---:|---:|
| Goedel-Pset-v1 | 1,732,594 | 698 MB (69% of the text) |
| NuminaMath-LEAN | 104,155 | 84 MB |
| mathlib4 | 273,587 | 76 MB |
| Goedel-Lean-workbook-proofs | 29,750 | 38 MB |
| lean4 | 172,666 | 36 MB |
| Lean-Workbook | 165,428 | 30 MB |
| leandojo-benchmark-4-v10 | 303,490 | 29 MB |
| Herald_proofs | 44,553 | 19 MB |
| the 10 smaller sources (eval and libraries) | 11,800 | about 4 MB |

Without Goedel-Pset-v1 the estimate is about 1.2 GB; with it about 3.8 GB. The choices (Odin's): drop the per-shard vector
tables for the large problem sources, keep Goedel-Pset-v1 pin-only/local, or raise the limit.

**Premise retrieval, LeanDojo benchmark test split** (`tools/leandb/eval_premises.py`, 706 of the 2,000 test theorems that
have a statement in mathlib4 and at least one resolved premise; the index is the library shards above, 450,144 declarations;
query = the theorem's statement; gold = premises its tactic steps use that resolve to a library declaration; the theorem
itself removed):

| mode | R@1 | R@5 | R@10 | R@50 |
|---|---:|---:|---:|---:|
| bm25 | 0.053 | 0.141 | **0.192** | 0.328 |
| ann (SimHash) | 0.039 | 0.090 | 0.112 | 0.169 |
| hybrid (RRF) | 0.049 | 0.135 | 0.180 | 0.299 |
| sym | n/a | n/a | n/a | n/a |

BM25 R@10 of 0.19 is above the 0.13 to 0.15 in OS-Proving's SEARCH_TEST.md, but the protocol differs (gold set, query
construction), so it is not a like-for-like comparison. Hybrid is slightly below BM25 here (the text-ANN list adds noise).
**sym answered no query**: the 706 queries are Mathlib signatures with type-class binders (`{α : Type*} [Group α]`), none of
which translates to the decidable fragment; over the whole library 4,656 of 450,144 statements translate (1.0%), so the
symbolic vectors exist for a small slice and matter for BitVec/Nat/Int goals, not for these queries.

**Leakage test passed**: no result from an eval source and none flagged `eval_overlap` (the check treats a name as flagged
only if every declaration of that name is flagged; the first run counted 20 name collisions and was corrected). `prism leandb
verify` on the library shards: 52 shards, OK.

To reproduce:

```
prism leandb import /path/to/lean_db.sqlite --pin data/leandb/PIN.json --committed OUT --sources lean4,batteries,mathlib4,lean-smt
tools/leandb/eval_premises.py /path/to/lean_db.sqlite --prism build/prism --db OUT --n 2000
```
