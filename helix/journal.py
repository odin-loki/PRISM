"""Per-stage JSONL so the GUI can tail a live run.

PLAN: each stage writes JSONL; a killed run loses at most the stage it
was in. This is the on-disk form of that. Missing file is not CLEAN.
"""

from __future__ import annotations

from pathlib import Path
import json

from helix.models import StageResult, stage_from_dict


STAGES_JSONL = "stages.jsonl"
PROGRESS_JSON = "progress.json"


def reset(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for name in (STAGES_JSONL, PROGRESS_JSON):
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
