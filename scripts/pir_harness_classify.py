#!/usr/bin/env python3
"""Classify the results of a pir_batch.sh run by why a function is still NEEDS-HARNESS
(docs/HARNESS_GENERATOR.md "Measurement"), and count the verdicts and the results that
still carry an automatic-harness assumption.

    scripts/pir_harness_classify.py OUT_DIR [--top N] [--json]

OUT_DIR is a pir_batch.sh output directory (OUT_DIR/shards/shard-NN.jsonl). The last record of a
unit and function wins, as `pir_batch.sh --summary` does. Classification is by script, from the
verdict message and the assumptions of the record; nothing here decides a verdict.
"""
import collections
import glob
import json
import re
import sys


def load(out):
    rec = {}
    for path in sorted(glob.glob(out + "/shards/shard-*.jsonl")):
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                try:
                    j = json.loads(line)
                except ValueError:
                    continue
                if j.get("record") != "function":
                    continue
                rec[(j.get("unit"), j.get("function"), j.get("start_line"))] = j
    return list(rec.values())


def reason(j):
    """Why a NEEDS-HARNESS function is not a verdict. One of the fixed categories."""
    m = j.get("message") or ""
    asm = " ".join(j.get("assumptions") or [])
    if "container_of" in m:
        return "container_of (negative offset from a parameter)"
    if m.startswith("UNENCODED: call @") or "UNENCODED: call @" in m:
        return "unknown call"
    if re.search(r"indirect call|function pointer|call through", m, re.I):
        return "function-pointer call / field dereference"
    if m.startswith("pointer parameter: violation only under"):
        cls = j.get("class") or ""
        if "unconstrained arbitrary values" in asm and re.search(r"PTR|INVALID|NULL|WILD", cls + m):
            return "depth bound / unbound pointer field dereferenced"
        return "harness violation (field values, indices, sizes)"
    if "automatic pointer harness for parameter" in m:
        return "no harness object (" + m.split(": ")[-1][:60] + ")"
    if "recursive call" in m or "call depth" in m:
        return "recursion / inline depth"
    if m.startswith("UNENCODED"):
        return "other UNENCODED: " + re.sub(r"\s+", " ", m)[:60]
    if "violation needs values of mutable globals" in m or "mutable globals" in m:
        return "global state precondition"
    return "other: " + re.sub(r"\s+", " ", m)[:60]


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    out, top, as_json = argv[1], 12, "--json" in argv
    if "--top" in argv:
        top = int(argv[argv.index("--top") + 1])
    recs = load(out)
    verdicts = collections.Counter(j.get("verdict") for j in recs)
    auto = sum(1 for j in recs if (j.get("harness") or {}).get("auto"))
    discharged = sum(1 for j in recs if any("the automatic harness is discharged" in a for a in (j.get("assumptions") or [])))
    why = collections.Counter(reason(j) for j in recs if j.get("verdict") == "NEEDS-HARNESS")
    callee = collections.Counter()
    for j in recs:
        mm = re.search(r"UNENCODED: call @([\w.$]+)", j.get("message") or "")
        if j.get("verdict") == "NEEDS-HARNESS" and mm:
            callee[mm.group(1)] += 1
    res = {
        "functions": len(recs),
        "verdicts": dict(verdicts.most_common()),
        "results_with_auto_harness_assumption": auto,
        "harness_discharged_results": discharged,
        "needs_harness_by_reason": dict(why.most_common()),
        "top_unknown_callees": dict(callee.most_common(top)),
    }
    if as_json:
        print(json.dumps(res, indent=1))
        return 0
    print("functions:", res["functions"])
    print("verdicts:")
    for k, v in verdicts.most_common():
        print("  %-16s %6d" % (k, v))
    print("results carrying an (auto harness) assumption: %d" % auto)
    print("results whose harness was discharged at the call sites: %d" % discharged)
    print("NEEDS-HARNESS by reason:")
    for k, v in why.most_common():
        print("  %6d  %s" % (v, k))
    print("most frequent unknown callees:")
    for k, v in callee.most_common(top):
        print("  %6d  %s" % (v, k))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
