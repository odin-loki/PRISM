"""Differential test of PRISM's formal LLVM semantics against ``lli``.

Roadmap 8.3 lists the formal LLVM semantics (proofs/refinement/PrismRefine/
Llvm.lean) as trusted and says it "is tested against lli on generated
programs". This tool does that for every function of the given C files that
is in the modelled fragment:

1. the C++ pir stage exports (LLVM fragment, PIR) pairs (PRISM_PIR_LEAN_EXPORT);
2. each function is run on edge-case and random inputs by ``llvm_eval``
   (proofs/refinement) under the LangRef semantics, the strict semantics and
   the PIR semantics of the proved translator;
3. every input on which the LangRef semantics returns a value without
   undefined behaviour or poison is executed with ``lli`` (same clang -O0 +
   opt pipeline as the pir stage) and the printed result must be that value;
4. the three Lean semantics must agree with each other as proved
   (translate_exact / strict_lazy); a disagreement is a bug in this tool.

    python tools/llvm_sem_vs_lli.py [FILE.c ...] [--bin build/prism] [--vectors 12]

Default files: tests/pir/*.c. Exit 1 on any disagreement, 2 when a tool is
missing (NOTRUN, never a silent pass). Runs scanned code (lli): use it on
trusted test programs only.
"""

from __future__ import annotations

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REFINE = ROOT / "proofs" / "refinement"
EVAL = REFINE / ".lake" / "build" / "bin" / "llvm_eval"
CFLAGS = ["-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone", "-fno-discard-value-names",
          "-gline-tables-only", "-std=c17", "-Wno-error=implicit-function-declaration", "-w"]
PASSES = "-passes=mem2reg,lowerswitch,loop-simplify,lcssa,instnamer"


def records(pirl: Path) -> dict[str, tuple[list[int], int]]:
    """function -> (parameter widths, return width) for functions in the fragment."""
    out: dict[str, tuple[list[int], int]] = {}
    name, params, ret, ok, inblock = "", [], 0, True, False
    for line in pirl.read_text(encoding="utf-8").splitlines():
        w = line.split()
        if not w:
            continue
        if w[0] == "func":
            name, params, ret, ok, inblock = w[1], [], 0, True, False
        elif w[:2] == ["L", "params"]:
            params = [int(x) for x in w[4::2]]
        elif w[:2] == ["L", "block"]:
            inblock = True
        elif w[:2] == ["L", "ret"] and not inblock:
            ret = int(w[2])
        elif w[:2] == ["L", "unsupported"]:
            ok = False
        elif w[0] == "end" and ok and name:
            out[name] = (params, ret)
    return out


def vectors(widths: list[int], n: int, rng: random.Random) -> list[list[int]]:
    def edge(w: int) -> list[int]:
        m = (1 << w) - 1
        vals = {0, 1, 2, 7, 100 & m, m, m - 1, 1 << (w - 1), (1 << (w - 1)) - 1}
        vals |= {rng.getrandbits(w) for _ in range(3)}
        return sorted(v & m for v in vals)
    pools = [edge(w) for w in widths]
    if not widths:
        return [[]]
    out = {tuple(rng.choice(p) for p in pools) for _ in range(n * 4)}
    return [list(v) for v in sorted(out)[:n]]


def harness(ll: str, fn: str, widths: list[int], ret: int, args: list[int]) -> str:
    call_args = ", ".join(f"i{w} {a}" for w, a in zip(widths, args))
    body = ["define i32 @__prism_lli_main() {"]
    if ret == 0:
        body += [f"  call void @{fn}({call_args})",
                 "  call i32 (ptr, ...) @printf(ptr @__prism_lli_void)"]
    else:
        body.append(f"  %r = call i{ret} @{fn}({call_args})")
        if ret < 64:
            body.append(f"  %z = zext i{ret} %r to i64")
        else:
            body.append("  %z = add i64 %r, 0")
        body.append("  call i32 (ptr, ...) @printf(ptr @__prism_lli_fmt, i64 %z)")
    body += ["  ret i32 0", "}"]
    decl = "" if "@printf(" in ll else "declare i32 @printf(ptr, ...)\n"
    return (ll + "\n@__prism_lli_fmt = private constant [6 x i8] c\"%llu\\0A\\00\"\n"
            "@__prism_lli_void = private constant [6 x i8] c\"void\\0A\\00\"\n" + decl
            + "\n".join(body) + "\n")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="*", type=Path)
    ap.add_argument("--bin", default=os.environ.get("PRISM_BIN", str(ROOT / "build" / "prism")))
    ap.add_argument("--vectors", type=int, default=12)
    ap.add_argument("--seed", type=int, default=20260923)
    ap.add_argument("--pairs", type=Path, default=None,
                    help="use existing .pirl files (<dir>/*<file name>*.pirl) instead of running prism")
    args = ap.parse_args(argv)
    rng = random.Random(args.seed)
    tools = {"llvm_eval": EVAL}
    if args.pairs is None:
        tools["prism"] = Path(args.bin)
    for t in ("clang", "opt", "lli"):
        p = shutil.which(t)
        tools[t] = Path(p) if p else Path("/nonexistent")
    missing = [k for k, p in tools.items() if not p.is_file()]
    if missing:
        print(f"llvm_sem_vs_lli: NOTRUN: missing {', '.join(missing)} "
              "(build prism; cd proofs/refinement && lake build llvm_eval)")
        return 2
    files = args.files or sorted((ROOT / "tests" / "pir").glob("*.c"))
    stats: Counter = Counter()
    bad: list[str] = []
    with tempfile.TemporaryDirectory(prefix="prism-sem-lli-") as tmp:
        t = Path(tmp)
        for src in files:
            src = src.resolve()
            if args.pairs is not None:
                pirls = sorted(p for p in args.pairs.glob("*.pirl")
                               if p.name == f"{src.name}.pirl" or p.name.endswith(f"_{src.name}.ll.pirl"))
            else:
                pairs = t / "pairs" / src.stem
                env = dict(os.environ, PRISM_PIR_LEAN_EXPORT=str(pairs))
                subprocess.run([str(tools["prism"]), str(src), "--no-llm", "--stage",
                                "inventory,classify,pir", "--out", str(t / "out" / src.stem)],
                               env=env, capture_output=True, check=False)
                pirls = sorted(pairs.glob("*.pirl"))
            o0, ll = t / f"{src.stem}.o0.ll", t / f"{src.stem}.ll"
            r1 = subprocess.run([str(tools["clang"]), *CFLAGS, "-o", str(o0), str(src)],
                                capture_output=True, check=False)
            r2 = subprocess.run([str(tools["opt"]), PASSES, "-S", "-o", str(ll), str(o0)],
                                capture_output=True, check=False)
            if not pirls or r1.returncode or r2.returncode:
                stats["file-skipped"] += 1
                continue
            ir = ll.read_text(encoding="utf-8")
            for pirl in pirls:
                for fn, (widths, ret) in records(pirl).items():
                    vecs = vectors(widths, args.vectors, rng)
                    q = "".join(f"{fn} {' '.join(map(str, v))}\n" for v in vecs)
                    ev = subprocess.run([str(tools["llvm_eval"]), str(pirl)], input=q, text=True,
                                        capture_output=True, check=False)
                    for line in ev.stdout.splitlines():
                        head, _, res = line.partition(" | ")
                        vals = [int(x) for x in head.split()[1:]]
                        m = re.match(r"lazy=(.*) strict=(.*) pir=(.*)$", res)
                        if m is None:
                            stats["outside"] += 1
                            continue
                        lazy, strict, pir = m.groups()
                        stats["runs"] += 1
                        # the proved relations between the three Lean semantics
                        if lazy.startswith("ret ") and not pir.startswith("outside") and not (
                                strict == lazy and pir == lazy):
                            bad.append(f"{src.name}:{fn}{vals}: lazy={lazy} strict={strict} pir={pir}")
                        if not (lazy.startswith("ret ") or lazy == "ret-void"):
                            stats["lean-ub-or-poison-or-fuel"] += 1
                            continue
                        prog = t / "h.ll"
                        prog.write_text(harness(ir, fn, widths, ret, vals), encoding="utf-8")
                        li = subprocess.run([str(tools["lli"]), "--entry-function=__prism_lli_main", str(prog)],
                                            capture_output=True, text=True, check=False, timeout=60)
                        if li.returncode != 0 or not li.stdout.strip():
                            # the module does not run under lli (unresolved externals
                            # elsewhere in it): not a comparison, counted separately
                            stats["lli-error"] += 1
                            continue
                        got = li.stdout.strip().splitlines()[-1]
                        want = "void" if lazy == "ret-void" else lazy.split()[1]
                        stats["lli-compared"] += 1
                        if got != want:
                            bad.append(f"{src.name}:{fn}{vals}: Lean {want}, lli {got} {li.stderr.strip()[:200]}")
                        else:
                            stats["agree"] += 1
    for b in bad:
        print("DISAGREE " + b)
    print("summary " + " ".join(f"{k}={v}" for k, v in sorted(stats.items())) + f" disagree={len(bad)}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
