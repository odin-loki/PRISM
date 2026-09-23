#!/usr/bin/env python3
"""Mutation fuzzer for the Python engine's input parsers (roadmap 6.2).

    python tools/fuzz_self/fuzz_py.py CORPUS_DIR [--seconds 120] [--out fuzz-self-out] [--target NAME]

CORPUS_DIR comes from tools/fuzz_self/make_corpus.py. Targets and the
contract each must keep for *arbitrary* bytes:

  cparse     prism.cparse.extract_functions_from_text / extract_functions:
             returns a list, never raises
  report     prism.models.RunReport.load: a RunReport or None, never raises
  journal    prism.journal.read_stages / read_functions / completed_ok:
             a list/dict, never raises
  manifest   prism.config.load_manifest: a dict ({} when unreadable),
             never raises

Any other exception, and any input that takes longer than --hang seconds,
is a finding. Findings are deduplicated by (target, exception type,
innermost prism/ frame), minimised by chunk deletion, and written to
OUT/<target>-<n>.bin with a summary in OUT/findings.json. The fuzzer does
not fix anything: repros go to docs/FUZZ_SELF.md.

Mutations: byte flips, inserts, deletes, chunk duplication, splicing two
seeds, token insertion from per-format dictionaries, and (for JSON inputs)
structure-aware mutation that swaps any node for a value of another type.
Deterministic for a given --seed.
"""

from __future__ import annotations

import argparse
import json
import random
import sys
import tempfile
import time
import traceback
from collections.abc import Callable
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from prism import cparse, journal  # noqa: E402
from prism import config as prism_config  # noqa: E402
from prism.models import RunReport  # noqa: E402

TOKENS = {
    "c": [b"{", b"}", b"(", b")", b";", b"/*", b"*/", b"//", b'"', b"'", b"\\", b"\n", b"#define X(", b"#if 0",
          b"#endif", b"int f(int a) {", b"template<class T>", b"[[nodiscard]]", b"R\"(", b")\"", b"<<", b">>",
          b"::", b"->", b"extern \"C\" {", b"__attribute__((", b"\x00", b"\xff", b"typedef", b"struct S {", b"??/"],
    "json": [b"{", b"}", b"[", b"]", b'"', b",", b":", b"null", b"true", b"1e999", b"-0", b"NaN", b"\\u0000",
             b'"stages"', b'"findings"', b'"functions"', b'"params"', b'"span"', b"\n", b"\xff"],
    "toml": [b"[[component]]", b"name = ", b"commit = ", b'"', b"'''", b'"""', b"[", b"]", b"=", b"\n",
             b"kind = \"external\"", b"{", b"}", b"1979-05-27", b"inf", b"nan"],
}


def rand_value(r: random.Random) -> Any:
    return r.choice([None, True, 0, -1, 2**63, 1.5, float("nan") if r.random() < 0.1 else 0.0, "", "x" * 3,
                     [], {}, [None], {"a": None}, "12", [1, 2]])


def mutate_json(data: bytes, r: random.Random) -> bytes | None:
    """Swap one node of a JSON (or JSON-lines) document for another type."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return None
    lines = text.splitlines() or [text]
    idx = r.randrange(len(lines))
    try:
        doc = json.loads(lines[idx])
    except (json.JSONDecodeError, RecursionError):
        return None
    nodes: list[tuple[Any, Any]] = []

    def walk(v: Any, depth: int = 0) -> None:
        if depth > 12:
            return
        if isinstance(v, dict):
            for k in list(v):
                nodes.append((v, k))
                walk(v[k], depth + 1)
        elif isinstance(v, list):
            for i in range(len(v)):
                nodes.append((v, i))
                walk(v[i], depth + 1)

    walk(doc)
    if not nodes:
        doc = rand_value(r)
    else:
        parent, key = r.choice(nodes)
        roll = r.random()
        if roll < 0.7:
            parent[key] = rand_value(r)
        elif roll < 0.85 and isinstance(parent, dict):
            del parent[key]
        elif isinstance(parent, list):
            parent.append(parent[key])
    try:
        lines[idx] = json.dumps(doc)
    except (TypeError, ValueError):
        return None
    return "\n".join(lines).encode()


def mutate(data: bytes, pool: list[bytes], kind: str, r: random.Random) -> bytes:
    if kind == "json" and r.random() < 0.4:
        m = mutate_json(data, r)
        if m is not None:
            return m
    b = bytearray(data)
    for _ in range(r.choice([1, 1, 2, 4, 8])):
        op = r.randrange(7)
        if op == 0 and b:
            i = r.randrange(len(b))
            b[i] ^= 1 << r.randrange(8)
        elif op == 1:
            i = r.randrange(len(b) + 1)
            b[i:i] = bytes([r.randrange(256)])
        elif op == 2 and b:
            i = r.randrange(len(b))
            del b[i:i + r.randrange(1, 64)]
        elif op == 3 and b:
            i = r.randrange(len(b))
            j = min(len(b), i + r.randrange(1, 256))
            k = r.randrange(len(b) + 1)
            b[k:k] = b[i:j]
        elif op == 4 and pool:
            other = r.choice(pool)
            if other:
                i = r.randrange(len(b) + 1)
                j = r.randrange(len(other))
                b = b[:i] + bytearray(other[j:j + r.randrange(1, 512)])
        elif op == 5:
            i = r.randrange(len(b) + 1)
            b[i:i] = r.choice(TOKENS[kind])
        elif op == 6 and b:
            i = r.randrange(len(b))
            b[i:i] = b[i:i + 1] * r.randrange(2, 200)  # long runs (nesting depth, long lines)
    return bytes(b[: 256 * 1024])


# --------------------------------------------------------------------------- targets


class Target:
    def __init__(self, name: str, kind: str, seeds_dirs: list[str], fn: Callable[[bytes, Path], None]):
        self.name, self.kind, self.seed_dirs, self.fn = name, kind, seeds_dirs, fn


def t_cparse(data: bytes, work: Path) -> None:
    text = data.decode("utf-8", errors="replace")
    fns = cparse.extract_functions_from_text(text, "input.c")
    assert isinstance(fns, list)
    p = work / "input.cpp"
    p.write_bytes(data)
    assert isinstance(cparse.extract_functions(p, "input.cpp"), list)


def t_report(data: bytes, work: Path) -> None:
    p = work / "report.json"
    p.write_bytes(data)
    r = RunReport.load(p)
    assert r is None or isinstance(r, RunReport)


def t_journal(data: bytes, work: Path) -> None:
    (work / journal.STAGES_JSONL).write_bytes(data)
    (work / journal.FUNCTIONS_JSON).write_bytes(data)
    assert isinstance(journal.read_stages(work), list)
    assert isinstance(journal.read_functions(work), list)
    assert isinstance(journal.completed_ok(work), dict)


def t_manifest(data: bytes, work: Path) -> None:
    (work / "third_party").mkdir(exist_ok=True)
    (work / "third_party" / "MANIFEST.toml").write_bytes(data)
    assert isinstance(prism_config.load_manifest(work), dict)


TARGETS = [
    Target("cparse", "c", ["c"], t_cparse),
    Target("report", "json", ["report"], t_report),
    Target("journal", "json", ["report"], t_journal),
    Target("manifest", "toml", ["toml"], t_manifest),
]


def signature(target: str, exc: BaseException) -> str:
    frames = [f for f in traceback.extract_tb(exc.__traceback__) if "/prism/" in f.filename.replace("\\", "/")]
    where = f"{Path(frames[-1].filename).name}:{frames[-1].lineno}" if frames else "?"
    return f"{target}:{type(exc).__name__}:{where}"


def run_once(t: Target, data: bytes, work: Path) -> BaseException | None:
    try:
        t.fn(data, work)
    except Exception as e:  # noqa: BLE001 - every exception is a finding
        return e
    return None


def minimise(t: Target, data: bytes, sig: str, work: Path, budget: float = 20.0) -> bytes:
    end = time.monotonic() + budget
    chunk = max(1, len(data) // 2)
    while chunk >= 1 and time.monotonic() < end:
        i, changed = 0, False
        while i < len(data) and time.monotonic() < end:
            cand = data[:i] + data[i + chunk:]
            e = run_once(t, cand, work)
            if e is not None and signature(t.name, e) == sig:
                data, changed = cand, True
            else:
                i += chunk
        if not changed:
            chunk //= 2
    return data


def fuzz(t: Target, corpus: Path, seconds: float, hang: float, r: random.Random, out: Path,
         findings: dict[str, dict[str, Any]]) -> dict[str, Any]:
    pool = [p.read_bytes() for d in t.seed_dirs for p in sorted((corpus / d).glob("*")) if p.is_file()]
    if not pool:
        return {"target": t.name, "execs": 0, "note": "no seeds"}
    execs, slow = 0, 0
    end = time.monotonic() + seconds
    with tempfile.TemporaryDirectory(prefix=f"prism-fuzz-{t.name}-") as d:
        work = Path(d)
        while time.monotonic() < end:
            data = mutate(r.choice(pool), pool, t.kind, r)
            t0 = time.monotonic()
            e = run_once(t, data, work)
            dt = time.monotonic() - t0
            execs += 1
            if e is None and dt > hang:
                e = TimeoutError(f"{dt:.1f}s")
                slow += 1
            if e is None:
                if r.random() < 0.02 and len(pool) < 2000:
                    pool.append(data)  # keep some diversity (no coverage feedback)
                continue
            sig = signature(t.name, e) if not isinstance(e, TimeoutError) else f"{t.name}:hang"
            if sig in findings:
                findings[sig]["count"] += 1
                continue
            small = minimise(t, data, sig, work) if not isinstance(e, TimeoutError) else data
            n = len(findings) + 1
            path = out / f"{t.name}-{n}.bin"
            path.write_bytes(small)
            findings[sig] = {"target": t.name, "signature": sig, "exception": f"{type(e).__name__}: {e}"[:300],
                             "repro": str(path), "repro_bytes": len(small), "repro_preview": small[:200].decode(
                                 "utf-8", errors="backslashreplace"), "count": 1}
    return {"target": t.name, "execs": execs, "seconds": seconds, "slow_inputs": slow}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("corpus")
    ap.add_argument("--seconds", type=float, default=120.0, help="budget per target")
    ap.add_argument("--hang", type=float, default=5.0, help="seconds per input that count as a hang")
    ap.add_argument("--out", default="fuzz-self-out")
    ap.add_argument("--target", action="append", help="only these targets (repeatable)")
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    r = random.Random(a.seed)
    findings: dict[str, dict[str, Any]] = {}
    stats = [fuzz(t, Path(a.corpus), a.seconds, a.hang, r, out, findings)
             for t in TARGETS if not a.target or t.name in a.target]
    summary = {"stats": stats, "findings": list(findings.values())}
    (out / "findings.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    for s in stats:
        print(f"{s['target']}: {s['execs']} execs")
    for f in findings.values():
        print(f"FINDING {f['signature']} x{f['count']}: {f['exception']} -> {f['repro']} ({f['repro_bytes']} bytes)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
