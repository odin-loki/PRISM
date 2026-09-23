"""Lean proof search over PRISM's own proofs (roadmap 9.2, milestone M6).

Finds every theorem whose proof is a `sorry` under proofs/, proofs/semantics
and proofs/techniques (or the roots given) and runs `prism prove` on each:
the prover model proposes, the Lean kernel checks, `#print axioms` must stay
within propext / Classical.choice / Quot.sound, and `lake build` gates
acceptance. Accepted proofs are written back only with --write, and then go
into the lemma library (proofs/lemmas.jsonl) that later prompts include.

    python tools/prism_prove.py --list
    python tools/prism_prove.py --allow-exec [--write] [--budget 24] [--bin build/prism]

Prover: PRISM_PROVER_SERVER (llama-server URL) or PRISM_PROVER_GGUF (a
DeepSeek-Prover / Goedel-Prover / Kimina-Prover GGUF served by llama-server).
Without one every theorem is NOTRUN (exit 3), never "proved".
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ROOTS = ("proofs", "proofs/semantics", "proofs/techniques")
_COMMENT = re.compile(r"--[^\n]*|/-.*?-/", re.S)
_DECL = re.compile(r"^(?:[ \t]*(?:private|protected|noncomputable)\s+)*[ \t]*(?:theorem|lemma)\s+([^\s(:{\[]+)", re.M)
_TOP = re.compile(r"^(?:theorem|lemma|def|instance|example|structure|inductive|class|namespace|end|section|"
                  r"open|abbrev|axiom|noncomputable|private|protected|variable|attribute|set_option|@\[|#|/--)",
                  re.M)


def _mask(text: str) -> str:
    """Comments blanked, same length (so offsets stay valid)."""
    return _COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def sorry_theorems(path: Path) -> list[str]:
    text = _mask(path.read_text(encoding="utf-8"))
    out = []
    for m in _DECL.finditer(text):
        nxt = _TOP.search(text, m.end())
        body = text[m.end(): nxt.start() if nxt else len(text)]
        if re.search(r"(?<![\w.'])sorry(?![\w'])", body):
            out.append(m.group(1))
    return out


def lean_files(roots: list[str]) -> list[Path]:
    files: set[Path] = set()
    for r in roots:
        base = (ROOT / r).resolve()
        if not base.is_dir():
            continue
        for p in base.rglob("*.lean"):
            if ".lake" in p.parts:
                continue
            files.add(p)
    return sorted(files)


def _prism_bin(explicit: str | None) -> Path | None:
    cands = [Path(explicit)] if explicit else []
    if os.environ.get("PRISM_BIN"):
        cands.append(Path(os.environ["PRISM_BIN"]))
    cands += [ROOT / "build" / "prism"]
    for c in cands:
        if c.is_file() and os.access(c, os.X_OK):
            return c
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("roots", nargs="*", default=list(DEFAULT_ROOTS))
    ap.add_argument("--list", action="store_true", help="only list theorems with a sorry")
    ap.add_argument("--bin", help="prism binary (default PRISM_BIN or build/prism)")
    ap.add_argument("--allow-exec", action="store_true", help="Law 9: Lean elaboration runs code")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--budget", type=int, default=24)
    ap.add_argument("--out", default="prism-out")
    args = ap.parse_args(argv)

    targets = [(p, t) for p in lean_files(args.roots) for t in sorry_theorems(p)]
    if args.list:
        for p, t in targets:
            print(f"{p.relative_to(ROOT)}\t{t}")
        return 0
    if not targets:
        print("no theorem with a sorry under " + ", ".join(args.roots))
        return 0
    exe = _prism_bin(args.bin)
    if exe is None:
        print("NOTRUN: C++ prism binary not found (build it, or pass --bin / PRISM_BIN)", file=sys.stderr)
        return 3
    results = []
    for p, t in targets:
        cmd = [str(exe), "prove", str(p), t, "--json", "--budget", str(args.budget), "--out", args.out]
        if args.allow_exec:
            cmd.append("--allow-exec")
        if args.write:
            cmd.append("--write")
        r = subprocess.run(cmd, capture_output=True, text=True, check=False)
        try:
            j = json.loads(r.stdout)
        except json.JSONDecodeError:
            j = {"theorem": t, "status": "ERROR", "reason": (r.stderr or r.stdout).strip()[:500]}
        j["file"] = str(p.relative_to(ROOT))
        results.append(j)
        print(f"{j['status']:10} {j['file']}:{t}  {j.get('reason', '')[:120]}")
    proved = sum(1 for j in results if j["status"] == "PROVED")
    print(f"proved {proved}/{len(results)} (metric 9.7: proofs completed by the prover model)")
    summary = Path(args.out) / "prove" / "summary.json"
    summary.parent.mkdir(parents=True, exist_ok=True)
    summary.write_text(json.dumps(results, indent=2), encoding="utf-8")
    if proved == len(results):
        return 0
    return 3 if all(j["status"] == "NOTRUN" for j in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
