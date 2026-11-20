# Attribution for the committed Lean data shards

`data/leandb/shards/` contains statements, names, docstrings and (for some sources) proofs taken from the sources below, under
the licences recorded in `PIN.json` (the licence strings there are exactly as OS-Proving recorded them; `evidence` says what
the dataset card or LICENSE file states). Each shard row carries its source name; `srcs.license` in every shard repeats the
licence text. Licence texts: Apache-2.0 https://www.apache.org/licenses/LICENSE-2.0, MIT https://opensource.org/license/mit,
CC-BY-2.0 https://creativecommons.org/licenses/by/2.0/.

| source | licence | upstream | revision pinned |
|---|---|---|---|
| lean4 | Apache-2.0 | https://github.com/leanprover/lean4 | see PIN.json |
| batteries | Apache-2.0 | https://github.com/leanprover-community/batteries | see PIN.json |
| mathlib4 | Apache-2.0 | https://github.com/leanprover-community/mathlib4 (Mathlib contributors) | see PIN.json |
| lean-smt | Apache-2.0 | https://github.com/ufmg-smite/lean-smt | see PIN.json |
| LeanDojo | MIT | https://github.com/lean-dojo/LeanDojo | see PIN.json |
| leandojo-benchmark-4-v10 | CC-BY-2.0 | https://zenodo.org/records/12740403, Kaiyu Yang et al., "LeanDojo: Theorem Proving with Retrieval-Augmented Language Models" (NeurIPS 2023). Rows are unmodified extracts; PRISM stores them normalised and sorted. | Zenodo record 12740403 |
| Lean-Workbook, Lean-Github | Apache-2.0 | https://huggingface.co/datasets/internlm/Lean-Workbook, https://huggingface.co/datasets/internlm/Lean-Github | see PIN.json |
| Herald_proofs | Apache-2.0 | https://huggingface.co/datasets/FrenzyMath/Herald_proofs | see PIN.json |
| NuminaMath-LEAN | Apache-2.0 (card) | https://huggingface.co/datasets/AI-MO/NuminaMath-LEAN | see PIN.json |
| Goedel-Lean-workbook-proofs, Goedel-Pset-v1 | MIT (dataset LICENSE) | https://huggingface.co/datasets/Goedel-LM/Lean-workbook-proofs, https://huggingface.co/datasets/Goedel-LM/Goedel-Pset-v1 | see PIN.json |
| MathOlympiadBench | Apache-2.0 | evaluation shards only | see PIN.json |
| FormalMATH-All, miniF2F-lean4, minif2f-lean4-hf, ProofNet-lean4, proofnet-hf | MIT | evaluation shards only | see PIN.json |
| PutnamBench (lean4/) | Apache-2.0 | https://github.com/trishullab/PutnamBench, evaluation shards only | see PIN.json |

Sources whose licence is not clearly redistributable (DeepSeek-Prover-V1, leandojo-benchmark-4-2026) are not in the repository:
see `scripts/leandb_fetch_local.py`.
