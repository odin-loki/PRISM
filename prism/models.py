from __future__ import annotations

from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any
import json
import time


@dataclass
class FunctionInfo:
    file: str
    name: str
    kind: str  # SCALAR | POINTER | VOID | OTHER
    line: int
    signature: str
    params: list[tuple[str, str]] = field(default_factory=list)  # (type, name)
    return_type: str = "int"
    static: bool = False
    body: str = ""
    span: tuple[int, int] = (0, 0)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass
class Finding:
    stage: str
    status: str
    file: str
    function: str | None
    line: int | None
    cls: str  # taxonomy id, or ""
    message: str
    strength: str  # PROVES | FINDS | SOME | READS
    evidence: str = ""
    counterexample: str = ""
    extra: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if self.function is not None and not isinstance(self.function, str):
            v = self.function
            if isinstance(v, (tuple, list)):
                v = v[0] if v else None
            self.function = str(v) if v is not None else None
        if self.cls is not None and not isinstance(self.cls, str):
            self.cls = str(self.cls)
        if self.message is not None and not isinstance(self.message, str):
            self.message = str(self.message)

    def to_dict(self) -> dict[str, Any]:
        d = asdict(self)
        return d


@dataclass
class StageResult:
    name: str
    status: str  # ok | failed | NOTRUN | skipped | missing
    detail: str = ""
    started: float = 0.0
    elapsed: float = 0.0
    findings: list[Finding] = field(default_factory=list)
    records: int = 0
    install: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "status": self.status,
            "detail": self.detail,
            "started": self.started,
            "elapsed": self.elapsed,
            "records": self.records,
            "install": self.install,
            "findings": [f.to_dict() for f in self.findings],
        }


@dataclass
class RunReport:
    root: str
    started: float = field(default_factory=time.time)
    stages: list[StageResult] = field(default_factory=list)
    functions: list[FunctionInfo] = field(default_factory=list)
    visibility: float = 0.0
    answer: float = 0.0
    resolution: float = 0.0
    confidence: float = 0.0
    notes: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "root": self.root,
            "started": self.started,
            "visibility": self.visibility,
            "answer": self.answer,
            "resolution": self.resolution,
            "confidence": self.confidence,
            "notes": self.notes,
            "functions": [f.to_dict() for f in self.functions],
            "stages": [s.to_dict() for s in self.stages],
        }

    def dumps(self) -> str:
        return json.dumps(self.to_dict(), indent=2)

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(self.dumps(), encoding="utf-8")

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "RunReport":
        rec = cls(
            root=data.get("root", ""),
            started=float(data.get("started") or 0.0),
            visibility=float(data.get("visibility") or 0.0),
            answer=float(data.get("answer") or 0.0),
            resolution=float(data.get("resolution") or 0.0),
            confidence=float(data.get("confidence") or 0.0),
            notes=list(data.get("notes") or []),
        )
        rec.functions = [
            FunctionInfo(
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
            )
            for f in (data.get("functions") or [])
        ]
        rec.stages = [stage_from_dict(s) for s in (data.get("stages") or [])]
        return rec

    @classmethod
    def load(cls, path: Path) -> "RunReport | None":
        if not path.is_file():
            return None
        try:
            return cls.from_dict(json.loads(path.read_text(encoding="utf-8")))
        except (OSError, json.JSONDecodeError, TypeError, ValueError):
            return None


def finding_from_dict(d: dict[str, Any]) -> Finding:
    return Finding(
        stage=d.get("stage", ""),
        status=d.get("status", ""),
        file=d.get("file", ""),
        function=d.get("function"),
        line=d.get("line"),
        cls=d.get("cls", ""),
        message=d.get("message", ""),
        strength=d.get("strength", ""),
        evidence=d.get("evidence", ""),
        counterexample=d.get("counterexample", ""),
        extra=d.get("extra") or {},
    )


def stage_from_dict(d: dict[str, Any]) -> StageResult:
    findings = [finding_from_dict(f) for f in (d.get("findings") or [])]
    return StageResult(
        name=d.get("name", ""),
        status=d.get("status", ""),
        detail=d.get("detail", ""),
        started=float(d.get("started") or 0.0),
        elapsed=float(d.get("elapsed") or 0.0),
        findings=findings,
        records=int(d.get("records") or len(findings)),
        install=d.get("install", ""),
    )
