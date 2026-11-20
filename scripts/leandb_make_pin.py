#!/usr/bin/env python3
"""leandb_make_pin.py OS_PROVING_DIR [--out data/leandb/PIN.json]

Builds data/leandb/PIN.json: the 21 Lean data sources with revision / commit / record, the licence exactly as
OS-Proving recorded it (never altered), the licence evidence found in each dataset card or LICENSE file, whether
the source may be committed (`redistributable`), and the SHA-256 of every downloaded file.
Inputs (OS-Proving, private): tools/lean_db/sources.json and data/lean-db/manifest.tsv.

`redistributable` is true only where a licence text or the card's own statement clearly allows redistribution.
Everything else stays pin-only: prism leandb builds it into local, gitignored shards.
"""
import csv
import json
import os
import sys

# name -> (redistributable, evidence). Evidence quotes what was read in the dataset card / LICENSE file on 18 Nov 2026.
EVIDENCE = {
    "lean4": (True, "Apache-2.0 (GitHub API licence field, checked by OS-Proving 18 Nov 2026; LICENSE file not re-read here)."),
    "batteries": (True, "Apache-2.0 (GitHub API licence field, checked by OS-Proving 18 Nov 2026; LICENSE file not re-read here)."),
    "mathlib4": (True, "Apache-2.0 (GitHub API licence field, checked by OS-Proving 18 Nov 2026; LICENSE file not re-read here)."),
    "lean-smt": (True, "Apache-2.0 (GitHub API licence field, checked by OS-Proving 18 Nov 2026; LICENSE file not re-read here)."),
    "LeanDojo": (True, "MIT (GitHub API licence field, checked by OS-Proving 18 Nov 2026; LICENSE file not re-read here)."),
    "leandojo-benchmark-4-v10": (True, "Zenodo record 12740403 states CC-BY-2.0; redistribution needs attribution (data/leandb/ATTRIBUTION.md)."),
    "leandojo-benchmark-4-2026": (False, "Zenodo record 18815372 states CC-BY-4.0, but this is a newer upload whose provenance is unverified (not the official LeanDojo "
                                         "release): kept out of git until checked."),
    "Lean-Workbook": (True, "Dataset card front matter: license: apache-2.0."),
    "Lean-Github": (True, "Dataset card front matter: license: apache-2.0."),
    "NuminaMath-LEAN": (True, "Dataset card front matter: license: apache-2.0; text: 'The dataset is available under the Apache License, Version 2.0.'"),
    "DeepSeek-Prover-V1": (False, "Dataset card front matter: license: other, license_name: deepseek-license, license_link: LICENSE; the LICENSE file in the dataset is "
                                  "empty (0 bytes); the card says the code is MIT and use of the models is under a separate Model License. No clear grant for the "
                                  "data: not redistributable until checked."),
    "Goedel-Lean-workbook-proofs": (True, "Dataset LICENSE file: 'This dataset is licensed under the MIT License.'"),
    "Goedel-Pset-v1": (True, "Dataset LICENSE file: 'This dataset is licensed under the MIT License.' (the card's badges name MIT for code and a model agreement "
                             "for the models only)."),
    "Herald_proofs": (True, "Dataset card front matter: license: apache-2.0."),
    "FormalMATH-All": (True, "Dataset card front matter: license: mit."),
    "miniF2F-lean4": (True, "MIT (GitHub API licence field, checked by OS-Proving 18 Nov 2026; LICENSE file not re-read here)."),
    "minif2f-lean4-hf": (True, "MIT (HF API licence field, checked by OS-Proving 18 Nov 2026)."),
    "ProofNet-lean4": (True, "MIT (GitHub API licence field, checked by OS-Proving 18 Nov 2026; LICENSE file not re-read here)."),
    "proofnet-hf": (True, "MIT (HF API licence field, checked by OS-Proving 18 Nov 2026)."),
    "PutnamBench": (True, "README: 'released under permissive licenses (Apache 2.0 for Lean 4 and Isabelle, MIT for Coq)'; lean4/LICENSE is Apache-2.0. The "
                          "database ingests lean4/ only; the informal statements (used with MAA permission) are not ingested."),
    "MathOlympiadBench": (True, "Dataset card front matter: license: apache-2.0."),
}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    base = sys.argv[1]
    out = "data/leandb/PIN.json"
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
    sources = json.load(open(os.path.join(base, "tools/lean_db/sources.json")))["sources"]
    rows = list(csv.DictReader(open(os.path.join(base, "data/lean-db/manifest.tsv"), newline=""), delimiter="\t"))
    files = {}
    for r in rows:
        files.setdefault(r["name"], []).append(r)
    pin = {"schema": 1, "note": "Lean data pin. `license` is exactly as recorded by OS-Proving; `redistributable` and `evidence` are PRISM's reading of the dataset "
                                "card / LICENSE. Sources with redistributable=false are never committed: scripts/leandb_fetch_local.py fetches them from the "
                                "original hosts.", "sources": []}
    for s in sources:
        n = s["name"]
        fl = files.get(n, [])
        red, ev = EVIDENCE.get(n, (False, "no evidence recorded"))
        e = {"name": n, "kind": s["kind"], "use": s["use"], "license": s["license"]}
        if s["kind"] == "git":
            e["url"] = s["url"]
        elif s["kind"] == "hf":
            e["repo"] = s["repo"]
            e["url"] = "https://huggingface.co/datasets/" + s["repo"]
        else:
            e["record"] = s["record"]
            e["url"] = "https://zenodo.org/records/" + s["record"]
        e["revision"] = fl[0]["revision"] if fl else ""
        if "note" in s:
            e["note"] = s["note"]
        e["redistributable"] = red
        e["evidence"] = ev
        e["files"] = [{"path": f["path"], "bytes": int(f["bytes"]), "sha256": f["sha256"]} for f in sorted(fl, key=lambda x: x["path"])]
        pin["sources"].append(e)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(pin, f, indent=1, ensure_ascii=False)
        f.write("\n")
    red = [s["name"] for s in pin["sources"] if s["redistributable"]]
    print(f"{len(pin['sources'])} sources, {len(red)} redistributable, {len(pin['sources']) - len(red)} pin-only -> {out}")


if __name__ == "__main__":
    main()
