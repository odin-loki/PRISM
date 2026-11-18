#!/usr/bin/env python3
"""before/after counts per verdict label and the transition matrix, matched by (unit, function, start_line).
usage: compare.py BEFORE.jsonl.gz AFTER_SHARDS_DIR_OR_FILE [more AFTER...] > out.md"""
import gzip, json, sys, glob, collections, os, re


def load(paths):
    recs = {}
    for p in paths:
        if os.path.isdir(p):
            files = sorted(glob.glob(os.path.join(p, "shard-*.jsonl")))
        else:
            files = [p]
        for f in files:
            op = gzip.open if f.endswith(".gz") else open
            with op(f, "rt") as fh:
                for line in fh:
                    try:
                        r = json.loads(line)
                    except Exception:
                        continue
                    if r.get("record") != "function":
                        continue
                    recs[(r["unit"], r["function"], r.get("start_line"))] = r
    return recs


def main():
    before = load([sys.argv[1]])
    after = load(sys.argv[2:])
    units_after = {k[0] for k in after}
    before = {k: v for k, v in before.items() if k[0] in units_after}
    common = set(before) & set(after)
    labels = ["PROVED", "PROVED-UNBOUNDED", "PROVED-ASSUMING", "BOUNDED", "FAILED", "NEEDS-HARNESS", "UNKNOWN", "TIMEOUT", "ERROR"]
    cb = collections.Counter(r["verdict"] for r in before.values())
    ca = collections.Counter(r["verdict"] for r in after.values())
    cbc = collections.Counter(before[k]["verdict"] for k in common)
    cac = collections.Counter(after[k]["verdict"] for k in common)
    print(f"units in the rerun: {len(units_after)}; functions before (same units): {len(before)}; after: {len(after)}; matched: {len(common)}\n")
    print("| label | before (all) | after (all) | before (matched) | after (matched) | delta (matched) |")
    print("|---|---:|---:|---:|---:|---:|")
    for l in labels + sorted((set(cb) | set(ca)) - set(labels)):
        if l not in cb and l not in ca:
            continue
        print(f"| {l} | {cb.get(l,0)} | {ca.get(l,0)} | {cbc.get(l,0)} | {cac.get(l,0)} | {cac.get(l,0)-cbc.get(l,0):+d} |")
    print(f"| total | {sum(cb.values())} | {sum(ca.values())} | {sum(cbc.values())} | {sum(cac.values())} | |")
    trans = collections.Counter((before[k]["verdict"], after[k]["verdict"]) for k in common if before[k]["verdict"] != after[k]["verdict"])
    print("\nTransitions (matched functions whose label changed):\n")
    print("| before | after | functions |")
    print("|---|---|---:|")
    for (b, a), n in trans.most_common():
        print(f"| {b} | {a} | {n} |")
    # what the new options contributed (assumption lines of the after records)
    feat = collections.Counter()
    for r in after.values():
        asm = r.get("assumptions", [])
        if any(a.startswith("callee-contract:") for a in asm): feat["uses a contract stub"] += 1
        if any("(function-pointer field " in a for a in asm): feat["binds a function-pointer field"] += 1
        if any(a.startswith("addr-values:") for a in asm): feat["uses the address-value model"] += 1
        h = r.get("harness", {})
        if h.get("depth_reached", 0) > 2: feat["reaches harness depth > 2"] += 1
    print("\nFunctions whose result uses a new mechanism (after):\n")
    for k, n in feat.most_common():
        print(f"- {k}: {n}")
    # NEEDS-HARNESS by message class, before and after
    def cls(m):
        m = re.sub(r"@[\w.$]+", "@F", m)
        m = re.sub(r"\d+", "N", m)
        return m[:90]
    nb = collections.Counter(cls(r["message"]) for k, r in before.items() if r["verdict"] == "NEEDS-HARNESS" and k in common)
    na = collections.Counter(cls(r["message"]) for k, r in after.items() if r["verdict"] == "NEEDS-HARNESS" and k in common)
    print("\nNEEDS-HARNESS by message (matched functions), top 14 before:\n")
    print("| message | before | after |")
    print("|---|---:|---:|")
    for m, n in nb.most_common(14):
        print(f"| {m} | {n} | {na.get(m,0)} |")
    new = [(m, n) for m, n in na.most_common() if m not in nb][:8]
    if new:
        print("\nNEEDS-HARNESS messages that are new after:\n")
        for m, n in new:
            print(f"- {n}: {m}")


main()
