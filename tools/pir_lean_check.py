"""Check the C++ LLVM->PIR translator against the proved Lean translator.

Roadmap 8.2 ("LLVM IR to PIR translation", "Property instrumentation") and
2.4 (translation validation). Runs the C++ binary's pir stage over each tree
with ``PRISM_PIR_LEAN_EXPORT`` set, so every translated function is written as
an (LLVM fragment, PIR) pair (src/prism/pir/export_lean.cpp), then runs
``pir_lean_check`` (proofs/refinement) on the pairs. For every function in the
modelled LLVM fragment the checker demands that the C++ PIR be exactly the
output of the Lean translator the refinement theorems are proved about
(proofs/refinement/PrismRefine/Sound.lean: ``pir_sound`` and friends), so for
those functions the theorems hold of the PIR PRISM actually verified.

    python tools/pir_lean_check.py [TREE ...] [--bin build/prism] [--checker EXE]

Default trees: tests/pir and testdata. Exit status 1 when any function is a
MISMATCH, 2 when a tool is missing (reported, never a silent pass).
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LEAN_PROJECT = ROOT / "proofs" / "refinement"
DEFAULT_TREES = [ROOT / "tests" / "pir", ROOT / "testdata"]


def find_checker(explicit: str | None) -> Path | None:
    """The pir_lean_check executable (built with `lake build` if needed)."""
    if explicit:
        p = Path(explicit)
        return p if p.is_file() else None
    exe = LEAN_PROJECT / ".lake" / "build" / "bin" / "pir_lean_check"
    if exe.is_file():
        return exe
    lake = shutil.which("lake") or str(Path.home() / ".elan" / "bin" / "lake")
    if not Path(lake).is_file():
        return None
    r = subprocess.run([lake, "build", "pir_lean_check"], cwd=LEAN_PROJECT, check=False,
                       capture_output=True, text=True)
    return exe if r.returncode == 0 and exe.is_file() else None


def export_pairs(tree: Path, exe: Path, out: Path, jobs: int) -> tuple[list[Path], str]:
    """Run the pir stage over `tree`; return the written .pirl files and the stage log."""
    pairs = out / "pairs"
    env = dict(os.environ, PRISM_PIR_LEAN_EXPORT=str(pairs))
    cmd = [str(exe), str(tree), "--no-llm", "--stage", "inventory,classify,pir",
           "--out", str(out / "prism-out"), "--jobs", str(jobs)]
    r = subprocess.run(cmd, cwd=ROOT, env=env, check=False, capture_output=True, text=True)
    return sorted(pairs.glob("*.pirl")), r.stdout + r.stderr


def parse(lines: list[str]) -> tuple[Counter, list[list[str]]]:
    counts: Counter = Counter()
    rows: list[list[str]] = []
    for line in lines:
        cols = line.split("\t")
        if len(cols) >= 3 and cols[0] in {"agree", "agree-ext", "agree-reject", "outside", "MISMATCH"}:
            counts[cols[0]] += 1
            rows.append(cols)
    return counts, rows


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("trees", nargs="*", type=Path)
    ap.add_argument("--bin", default=os.environ.get("PRISM_BIN", str(ROOT / "build" / "prism")))
    ap.add_argument("--checker", default=None, help="pir_lean_check executable")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--json", type=Path, default=None, help="write the summary here")
    ap.add_argument("--keep", type=Path, default=None, help="keep the .pirl pairs in this directory")
    args = ap.parse_args(argv)

    exe = Path(args.bin)
    if not exe.is_file():
        print(f"pir_lean_check: NOTRUN: no PRISM binary at {exe} (build it or pass --bin)")
        return 2
    checker = find_checker(args.checker)
    if checker is None:
        print("pir_lean_check: NOTRUN: pir_lean_check not built and lake not found "
              "(cd proofs/refinement && lake build)")
        return 2

    trees = args.trees or DEFAULT_TREES
    summary: dict[str, dict] = {}
    total: Counter = Counter()
    mismatches: list[list[str]] = []
    with tempfile.TemporaryDirectory(prefix="prism-pir-lean-") as tmp:
        for tree in trees:
            out = Path(tmp) / tree.name
            files, log = export_pairs(tree.resolve(), exe, out, args.jobs)
            if not files:
                print(f"{tree}: no pairs exported (pir stage did not run?)\n{log[-2000:]}")
                summary[str(tree)] = {"error": "no pairs exported"}
                continue
            r = subprocess.run([str(checker), *map(str, files)], check=False,
                               capture_output=True, text=True)
            counts, rows = parse(r.stdout.splitlines())
            reasons = Counter(" ".join(row[3].split()[:2]) for row in rows
                              if row[0] == "outside" and len(row) > 3)
            mismatches += [row for row in rows if row[0] == "MISMATCH"]
            total.update(counts)
            summary[str(tree)] = {"counts": dict(counts), "outside_reasons": dict(reasons.most_common(12))}
            print(f"{tree}: " + ", ".join(f"{k}={counts[k]}" for k in
                                         ("agree", "agree-ext", "agree-reject", "outside", "MISMATCH")))
            if args.keep:
                dst = args.keep / tree.name
                dst.mkdir(parents=True, exist_ok=True)
                for f in files:
                    shutil.copy2(f, dst / f.name)
    for row in mismatches:
        print("MISMATCH " + " ".join(row[1:]))
    in_fragment = total["agree"] + total["agree-ext"] + total["MISMATCH"]
    print(f"total: agree={total['agree']} agree-ext={total['agree-ext']} agree-reject={total['agree-reject']} "
          f"outside={total['outside']} mismatch={total['MISMATCH']} "
          f"(functions in the proved fragment: {in_fragment})")
    if args.json:
        args.json.write_text(json.dumps({"trees": summary, "total": dict(total)}, indent=2) + "\n",
                             encoding="utf-8")
    return 1 if total["MISMATCH"] else 0


if __name__ == "__main__":
    sys.exit(main())
