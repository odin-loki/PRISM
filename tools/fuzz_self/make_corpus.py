#!/usr/bin/env python3
"""Seed corpus for fuzzing PRISM's own input handling (roadmap 6.2).

    python tools/fuzz_self/make_corpus.py OUT_DIR [--prism BIN]

Writes, from files already in this repository:

  OUT_DIR/c/         C and C++ sources (testdata/, tests/conformance/)
  OUT_DIR/ll/        LLVM IR text: the seeds compiled the way the pir stage
                     does (clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone,
                     then opt -passes=mem2reg,...) when clang/opt are present
  OUT_DIR/report/    report.json / stages.jsonl / functions.json from a PRISM
                     run on testdata (PRISM_BIN or --prism; else the Python
                     engine), plus hand-written edge cases
  OUT_DIR/toml/      third_party/MANIFEST.toml and pyproject.toml
  OUT_DIR/libfuzzer/ the same seeds with the selector byte the C++ target
                     tests/fuzz/fuzz_cparse.cpp expects (0 C, 1 IR, 2 JSON)

Only compiles seeds (never runs them); the PRISM run uses --no-llm and no
--allow-exec, so nothing from the tree is executed.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAX_SEED = 64 * 1024

EDGE_JSON = [
    b"", b"{}", b"[]", b"null", b"0", b'""', b'{"stages": null}', b'{"stages": [null]}',
    b'{"stages": [{"findings": [null]}]}', b'{"functions": [1]}', b'{"stages": "x"}',
    b'{"started": "x", "visibility": [], "functions": [{"params": [1]}]}',
    b'{"stages": [{"name": 1, "status": {}, "elapsed": "NaN"}]}', b"[1,2,3]\n{}\n",
    b'{"stages": [{"findings": [{"line": "12", "extra": []}]}]}',
]


def seeds(pattern_roots: list[tuple[Path, tuple[str, ...]]], limit: int) -> list[Path]:
    out: list[Path] = []
    for root, exts in pattern_roots:
        if not root.is_dir():
            continue
        for p in sorted(root.rglob("*")):
            if p.is_file() and p.suffix in exts and p.stat().st_size <= MAX_SEED:
                out.append(p)
    return out[:limit]


def put(dir_: Path, data: bytes, ext: str) -> None:
    dir_.mkdir(parents=True, exist_ok=True)
    (dir_ / (hashlib.sha1(data).hexdigest()[:16] + ext)).write_bytes(data)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out")
    ap.add_argument("--prism", default=os.environ.get("PRISM_BIN"))
    ap.add_argument("--limit", type=int, default=200, help="max C seeds")
    a = ap.parse_args(argv)
    out = Path(a.out)
    csrc = seeds([(REPO / "testdata", (".c", ".cpp", ".h")),
                  (REPO / "tests" / "conformance" / "prism", (".c", ".cpp"))], a.limit)
    for p in csrc:
        put(out / "c", p.read_bytes(), p.suffix)

    clang, opt = shutil.which("clang"), shutil.which("opt") or shutil.which("opt-18")
    n_ll = 0
    if clang:
        with tempfile.TemporaryDirectory() as d:
            for p in csrc[:60]:
                if p.suffix not in (".c",):
                    continue
                ll = Path(d) / "x.ll"
                r = subprocess.run([clang, "-S", "-emit-llvm", "-O0", "-Xclang", "-disable-O0-optnone", "-g",
                                    "-w", str(p), "-o", str(ll)], capture_output=True, timeout=60)
                if r.returncode != 0:
                    continue
                put(out / "ll", ll.read_bytes(), ".ll")
                n_ll += 1
                if opt:
                    lo = Path(d) / "y.ll"
                    r = subprocess.run([opt, "-S", "-passes=mem2reg,lowerswitch,loop-simplify,lcssa,instnamer",
                                        str(ll), "-o", str(lo)], capture_output=True, timeout=60)
                    if r.returncode == 0:
                        put(out / "ll", lo.read_bytes(), ".ll")
                        n_ll += 1

    with tempfile.TemporaryDirectory() as d:
        cmd = [a.prism] if a.prism else [sys.executable, "-m", "prism"]
        env = dict(os.environ, PYTHONPATH=str(REPO))
        subprocess.run([*cmd, str(REPO / "testdata"), "--no-llm", "--stage", "inventory,classify,lints,bmc",
                        "--out", d], capture_output=True, timeout=1800, env=env, cwd=REPO)
        for name in ("report.json", "stages.jsonl", "functions.json"):
            f = Path(d) / name
            if f.is_file():
                data = f.read_bytes()
                put(out / "report", data[: 256 * 1024], Path(name).suffix)
    for e in EDGE_JSON:
        put(out / "report", e, ".json")

    for t in (REPO / "third_party" / "MANIFEST.toml", REPO / "pyproject.toml"):
        if t.is_file():
            put(out / "toml", t.read_bytes(), ".toml")

    sel = {"c": b"\x00", "ll": b"\x01", "report": b"\x02"}
    for kind, prefix in sel.items():
        for f in sorted((out / kind).glob("*")) if (out / kind).is_dir() else []:
            put(out / "libfuzzer", prefix + f.read_bytes(), "")
    counts = {k: len(list((out / k).glob("*"))) if (out / k).is_dir() else 0
              for k in ("c", "ll", "report", "toml", "libfuzzer")}
    print(" ".join(f"{k}={v}" for k, v in counts.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
