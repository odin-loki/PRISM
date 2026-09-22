"""Per-stage JSONL so the GUI can tail a live run.

PLAN: each stage writes JSONL; `--resume` skips stages already `ok` /
`NOTRUN`. A killed run loses at most the stage it was in. Missing file
is not CLEAN. Functions are snapshotted after classify so resume does
not skip classify into an empty analysis. `report.json` is a fallback
only when stages.jsonl is absent — a failed jsonl must not revive a
previous complete report.
"""

from __future__ import annotations

from pathlib import Path
import json

from prism.models import FunctionInfo, StageResult, stage_from_dict


STAGES_JSONL = "stages.jsonl"
PROGRESS_JSON = "progress.json"
FUNCTIONS_JSON = "functions.json"

_RESUME_OK = frozenset({"ok", "NOTRUN"})


def reset(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for name in (STAGES_JSONL, PROGRESS_JSON, FUNCTIONS_JSON):
        p = out / name
        try:
            p.unlink()
        except OSError:
            pass


def append_stage(out: Path, rec: StageResult) -> None:
    out.mkdir(parents=True, exist_ok=True)
    line = json.dumps(rec.to_dict(), ensure_ascii=False)
    with (out / STAGES_JSONL).open("a", encoding="utf-8") as f:
        f.write(line + "\n")
    (out / PROGRESS_JSON).write_text(
        json.dumps({
            "last": rec.name,
            "status": rec.status,
            "records": rec.records,
            "elapsed": rec.elapsed,
        }),
        encoding="utf-8",
    )


def read_stages(out: Path) -> list[StageResult]:
    p = out / STAGES_JSONL
    if not p.is_file():
        return []
    recs: list[StageResult] = []
    try:
        text = p.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    for ln in text.splitlines():
        ln = ln.strip()
        if not ln:
            continue
        try:
            recs.append(stage_from_dict(json.loads(ln)))
        except (json.JSONDecodeError, TypeError, ValueError):
            continue
    return recs


def stages_present(out: Path) -> bool:
    """True when stages.jsonl exists. An empty/failed log is not a missing log."""
    return (out / STAGES_JSONL).is_file()


def completed_ok(out: Path) -> dict[str, StageResult]:
    """Last `ok`/`NOTRUN` per stage name. Failed/skipped are not resumable."""
    recs: dict[str, StageResult] = {}
    for s in read_stages(out):
        if s.name and s.status in _RESUME_OK:
            recs[s.name] = s
    return recs


def write_functions(out: Path, functions: list[FunctionInfo]) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / FUNCTIONS_JSON).write_text(
        json.dumps([f.to_dict() for f in functions], ensure_ascii=False),
        encoding="utf-8",
    )


def read_functions(out: Path) -> list[FunctionInfo]:
    p = out / FUNCTIONS_JSON
    if not p.is_file():
        return []
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, TypeError, ValueError):
        return []
    if not isinstance(data, list):
        return []
    out_fns: list[FunctionInfo] = []
    for f in data:
        if not isinstance(f, dict):
            continue
        try:
            out_fns.append(FunctionInfo(
                file=f.get("file", ""),
                name=f.get("name", ""),
                kind=f.get("kind", "OTHER"),
                line=int(f.get("line") or 0),
                signature=f.get("signature", ""),
                params=[tuple(p) for p in (f.get("params") or [])],
                return_type=f.get("return_type", "int"),
                static=bool(f.get("static")),
                body=f.get("body", ""),
                span=tuple(f.get("span") or (0, 0)),
            ))
        except (TypeError, ValueError):
            continue
    return out_fns
