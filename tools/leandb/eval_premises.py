#!/usr/bin/env python3
"""eval_premises.py LEAN_DB.sqlite --prism PATH --db DIR [--db DIR ...] [--n 500] [--ks 1,5,10,50] [--out FILE]

Premise retrieval on the LeanDojo benchmark's test split (split_random = 'test' of leandojo-benchmark-4-v10), through
`prism leandb search --batch`: for each test theorem the query is its statement (the signature of the mathlib4 declaration
of the same name), the gold set is the premises its tactic steps use that resolve to a library declaration, and the metric
is recall@k = |top-k intersect gold| / |gold|, averaged over theorems. The theorem itself is removed from the results.
Modes: bm25, ann, sym, hybrid. sym only answers for statements the Lean adapter translates, so it is also reported against
bm25 on the same subset. Leakage test: no result may come from an eval source or be a declaration flagged eval_overlap.
Run it against the sqlite the shards were imported from (the gold labels are there; the shards hold the retrieval data).
"""
import argparse
import json
import os
import sqlite3
import subprocess
import sys
import tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sqlite")
    ap.add_argument("--prism", required=True)
    ap.add_argument("--db", dest="dirs", action="append", default=[], help="shard directory (repeatable)")
    ap.add_argument("--n", type=int, default=500)
    ap.add_argument("--ks", default="1,5,10,50")
    ap.add_argument("--out", default="")
    ap.add_argument("--modes", default="bm25,ann,sym,hybrid")
    ap.add_argument("--threads", type=int, default=8)
    a = ap.parse_args()
    ks = [int(x) for x in a.ks.split(",")]
    K = max(ks) + 3
    con = sqlite3.connect(f"file:{a.sqlite}?mode=ro", uri=True)
    ev_sources = {r[0] for r in con.execute("SELECT name FROM sources WHERE use='eval'")}
    thms = con.execute("SELECT id, full_name FROM trace_theorems WHERE source='leandojo-benchmark-4-v10' AND split_random='test' ORDER BY id").fetchall()
    step = max(1, len(thms) // a.n)
    queries = []  # (key, name, text, gold)
    for tid, name in thms[::step]:
        row = con.execute("SELECT signature FROM decls WHERE source='mathlib4' AND full_name=? LIMIT 1", (name,)).fetchone()
        if not row or not row[0]:
            continue
        gold = {r[0] for r in con.execute(
            "SELECT DISTINCT tp.full_name FROM traces tr JOIN trace_premises tp ON tp.trace_id=tr.id WHERE tr.theorem_id=? AND tp.lib_decl_id IS NOT NULL", (tid,))}
        gold.discard(name)
        if not gold:
            continue
        queries.append((str(tid), name, " ".join(row[0].split()), gold))
        if len(queries) >= a.n:
            break
    print(f"{len(thms)} test theorems; {len(queries)} usable queries (statement found, at least one resolved premise)", file=sys.stderr)
    with tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False, encoding="utf-8") as f:
        for key, _, text, _ in queries:
            f.write(f"{key}\t{text}\n")
        qfile = f.name
    result = {"queries": len(queries), "test_theorems": len(thms), "ks": ks, "modes": {}}
    answers = {}
    leaks = 0
    flagged = 0
    for mode in a.modes.split(","):
        cmd = [a.prism, "leandb", "search", "--batch", qfile, "--mode", mode, "-k", str(K), "--threads", str(a.threads)]
        for d in a.dirs:
            cmd += ["--db", d]
        out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
        got = {}
        for line in out.splitlines():
            parts = line.split("\t")
            got[parts[0]] = [p.split(":", 1) for p in parts[1:]]
        answers[mode] = got
        for lst in got.values():
            for src, nm in lst:
                if src in ev_sources:
                    leaks += 1
                r = con.execute("SELECT eval_overlap FROM decls WHERE source=? AND full_name=? LIMIT 1", (src, nm)).fetchone()
                if r and r[0]:
                    flagged += 1
    def recall(mode, subset=None):
        tot = {k: 0.0 for k in ks}
        n = 0
        for key, name, _, gold in queries:
            if subset is not None and key not in subset:
                continue
            res = [nm for _, nm in answers[mode].get(key, []) if nm != name]
            n += 1
            for k in ks:
                tot[k] += len(gold & set(res[:k])) / len(gold)
        return {str(k): (tot[k] / n if n else None) for k in ks}, n
    for mode in answers:
        r, n = recall(mode)
        result["modes"][mode] = {"recall": r, "queries_answered": sum(1 for v in answers[mode].values() if v)}
    if "sym" in answers:
        sub = {k for k, v in answers["sym"].items() if v}
        result["sym_translatable_queries"] = len(sub)
        for mode in answers:
            r, n = recall(mode, sub)
            result["modes"][mode]["recall_on_translatable_subset"] = r
    result["leakage"] = {"results_from_eval_sources": leaks, "results_flagged_eval_overlap": flagged, "pass": leaks == 0 and flagged == 0}
    os.unlink(qfile)
    text = json.dumps(result, indent=1)
    print(text)
    if a.out:
        open(a.out, "w").write(text + "\n")
    sys.exit(0 if result["leakage"]["pass"] else 1)


if __name__ == "__main__":
    main()
