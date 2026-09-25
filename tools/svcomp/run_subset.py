#!/usr/bin/env python3
"""Run PRISM on the pinned SV-COMP subset and score it with SV-COMP points
(roadmap 6.3).

    PRISM_BIN=build/prism python tools/svcomp/run_subset.py [--property no-overflow|unreach-call]
                                                            [--jobs 2] [--out svcomp-out]

Each task in tests/conformance/sv-comp with a verdict for the property
(default ``no-overflow.prp``, the category the subset was pinned for) is
run the way BenchExec would run it: the command line comes from the
tool-info module (``tools/svcomp/prism.py``, ``Tool.cmdline``), the wrapper
``prism_svcomp.py`` is executed as a subprocess, and its output goes through
``Tool.determine_result``. When BenchExec is not installed the runner builds
the same command line itself and reads the wrapper's result line directly
(it says so in the output).

Scoring (SV-COMP, per task): correct true +2, correct false +1, incorrect
true -32, incorrect false -16, unknown / error / timeout 0. The competition
additionally requires every answer's witness to be confirmed by a validator;
this runner does not run validators, so the score is an unvalidated upper
bound under the official rules (docs/SVCOMP.md).

Replay executes the benchmark programs (Law 9): the runner passes
``--allow-exec`` because the pinned suite in this repository is trusted.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import importlib.util
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SUITE = REPO / "tests" / "conformance" / "sv-comp"
WRAPPER = HERE / "prism_svcomp.py"
PROPERTY = "no-overflow.prp"
POINTS = {("true", True): 2, ("false", False): 1, ("true", False): -32, ("false", True): -16}

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:  # pragma: no cover
    yaml = None


def _load_proctree() -> Any:
    # by path (tools/proctree.py), not through sys.path
    name = "prism_tools_proctree"
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, HERE.parent / "proctree.py")
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


PROCTREE = _load_proctree()


def load_tool() -> Any | None:
    """The tool-info module's Tool, or None when BenchExec is not installed."""
    try:
        import benchexec.tools.template  # noqa: F401
    except ImportError:
        return None
    spec = importlib.util.spec_from_file_location("prism_toolinfo", HERE / "prism.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.Tool()


def tasks(prop_name: str = PROPERTY) -> list[dict[str, Any]]:
    out = []
    for yml in sorted(SUITE.rglob("*.yml")):
        data = yaml.safe_load(yml.read_text(encoding="utf-8")) if yaml else json.loads(yml.read_text())
        if not isinstance(data, dict):
            continue
        for p in data.get("properties") or []:
            pf = Path(str(p.get("property_file", "")))
            if pf.name == prop_name and "expected_verdict" in p:
                inp = data["input_files"]
                inp = inp[0] if isinstance(inp, list) else inp
                out.append({
                    "id": str(yml.relative_to(SUITE)),
                    "yml": yml,
                    "input": (yml.parent / str(inp)).resolve(),
                    "property_file": (yml.parent / pf).resolve(),
                    "expected": bool(p["expected_verdict"]),
                    "options": dict(data.get("options") or {}),
                })
    return out


def command(tool: Any | None, prism: str, t: dict[str, Any], out: Path) -> list[str]:
    options = ["--allow-exec", "--prism", prism, "--out", str(out), "--witness", str(out / "witness.yml")]
    if tool is not None:
        from benchexec.tools.template import BaseTool2

        task = BaseTool2.Task.with_files([str(t["input"])], property_file=str(t["property_file"]),
                                         options=t["options"])
        return list(tool.cmdline(str(WRAPPER), options, task, BaseTool2.ResourceLimits()))
    cmd = [str(WRAPPER), *options, "--prop", str(t["property_file"])]
    if t["options"].get("data_model"):
        cmd += ["--data-model", str(t["options"]["data_model"])]
    return [*cmd, str(t["input"])]


def verdict_of(tool: Any | None, cmd: list[str], rc: int | None, lines: list[str], timed_out: bool) -> str:
    if tool is not None:
        from benchexec.tools.template import BaseTool2
        from benchexec.util import ProcessExitCode

        run = BaseTool2.Run(cmd, ProcessExitCode.create(value=rc if rc is not None else 0),
                            BaseTool2.RunOutput([ln + "\n" for ln in lines]), "cputime" if timed_out else None)
        return str(tool.determine_result(run))
    for ln in lines:
        if ln.startswith("PRISM-SVCOMP-RESULT: "):
            return ln.split(": ", 1)[1].strip()
    return "TIMEOUT" if timed_out else "ERROR"


def score(expected: bool, answer: str) -> tuple[str, int]:
    a = answer.lower()
    if a == "true":
        kind = "true"
    elif a.startswith("false"):
        kind = "false"
    else:
        return "unknown", 0
    pts = POINTS[(kind, expected)]
    return ("correct" if pts > 0 else "wrong"), pts


def run_one(tool: Any | None, prism: str, t: dict[str, Any], work: Path, timeout: float) -> dict[str, Any]:
    out = work / t["id"].replace("/", "__").removesuffix(".yml")
    cmd = command(tool, prism, t, out)
    t0 = time.monotonic()
    timed_out = False
    try:
        # a session of its own: a timeout kills the wrapper, PRISM and PRISM's solvers
        r = PROCTREE.run([sys.executable, *cmd], capture_output=True, text=True, timeout=timeout, cwd=REPO)
        lines, rc = r.stdout.splitlines(), r.returncode
    except subprocess.TimeoutExpired as e:
        raw = e.stdout or ""
        text = raw.decode(errors="replace") if isinstance(raw, bytes) else raw
        lines, rc, timed_out = text.splitlines(), None, True
    answer = verdict_of(tool, cmd, rc, lines, timed_out)
    outcome, pts = score(t["expected"], answer)
    reason = next((ln.split(": ", 1)[1] for ln in lines if ln.startswith("PRISM-SVCOMP-REASON: ")), "")
    wit = out / "witness.yml"
    return {"task": t["id"], "expected": t["expected"], "answer": answer, "outcome": outcome, "points": pts,
            "reason": reason, "witness": str(wit) if wit.exists() else None,
            "seconds": round(time.monotonic() - t0, 1), "data_model": t["options"].get("data_model", "")}


def markdown(rows: list[dict[str, Any]], meta: dict[str, Any]) -> str:
    total = sum(r["points"] for r in rows)
    n_true = sum(1 for r in rows if r["expected"])
    n_false = len(rows) - n_true
    best = 2 * n_true + n_false
    ct = sum(1 for r in rows if r["expected"] and r["outcome"] == "correct")
    cf_ = sum(1 for r in rows if not r["expected"] and r["outcome"] == "correct")
    wt = sum(1 for r in rows if not r["expected"] and r["answer"].lower() == "true")
    wf = sum(1 for r in rows if r["expected"] and r["answer"].lower().startswith("false"))
    unk = sum(1 for r in rows if r["outcome"] == "unknown")
    out = [f"- property: {meta['property']}",
           f"- tasks: {len(rows)} ({n_true} expected true, {n_false} expected false)",
           f"- score: **{total}** of a possible {best}",
           f"- correct true: {ct}, correct false: {cf_}, incorrect true: {wt}, incorrect false: {wf}, "
           f"unknown/error: {unk}",
           f"- result mapping via: {meta['via']}", f"- engine: {meta['prism']}", "",
           "| task | expected | answer | points | reason |", "|---|---|---|---|---|"]
    for r in rows:
        out.append(f"| {r['task']} | {str(r['expected']).lower()} | {r['answer']} | {r['points']} | "
                   f"{r['reason'][:160].replace('|', '/')} |")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--prism", default=os.environ.get("PRISM_BIN"), help="PRISM C++ binary (default PRISM_BIN)")
    ap.add_argument("--out", default="svcomp-out")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--timeout", type=float, default=900.0, help="wall-clock limit per task (s)")
    ap.add_argument("--only", help="substring filter on the task id")
    ap.add_argument("--property", default="no-overflow", choices=["no-overflow", "unreach-call"])
    a = ap.parse_args(argv)
    if not a.prism:
        ap.error("set PRISM_BIN or pass --prism (the C++ engine: the pir stage is C++ only)")
    prism = str(Path(a.prism).resolve())
    tool = load_tool()
    work = Path(a.out).resolve()
    work.mkdir(parents=True, exist_ok=True)
    ts = [t for t in tasks(a.property + ".prp") if not a.only or a.only in t["id"]]
    with cf.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        rows = list(ex.map(lambda t: run_one(tool, prism, t, work, a.timeout), ts))
    meta = {"via": "BenchExec tool-info determine_result" if tool else "wrapper result line (BenchExec not installed)",
            "prism": prism, "property": a.property}
    (work / "results.json").write_text(json.dumps({"meta": meta, "rows": rows}, indent=2), encoding="utf-8")
    md = markdown(rows, meta)
    (work / "score.md").write_text(md, encoding="utf-8")
    print(md)
    wrong = [r for r in rows if r["outcome"] == "wrong"]
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
