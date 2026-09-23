"""SARIF 2.1.0 export and CI exit policy. Same output as src/prism/sarif.cpp.

SARIF is what GitHub code scanning, VS Code and most CI dashboards read.
Laws still hold in this format:
  - only defects become results (FAILED / CRASH / SANFAIL); LLM output is a
    `note`, never an error, and is marked as a hypothesis;
  - a stage that could not run is a tool execution notification, so a
    missing tool is visible, not silently absent;
  - nothing here claims a proof.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from prism import __version__, laws
from prism.models import Finding, RunReport

SARIF_SCHEMA = "https://json.schemastore.org/sarif-2.1.0.json"
INFO_URI = "https://github.com/odin-loki/PRISM"

DEFECT_STATUSES = (laws.FAILED, laws.CRASH, laws.SANFAIL)
GAP_STATUSES = (laws.NOTRUN, laws.ERROR, laws.TIMEOUT)
FAIL_ON = ("never", "defect", "gap")
# --fail-on counts a defect only at error level: a finding whose
# extra.severity is one of these (a compiler/linter warning) is reported
# (SARIF level "warning") but does not fail the build.
NON_BLOCKING_SEVERITIES = ("warning", "note", "style")


def blocks(f: Finding) -> bool:
    """A defect --fail-on defect/gap counts (not a warning/note/style)."""
    if f.status not in DEFECT_STATUSES:
        return False
    return str((f.extra or {}).get("severity", "")).lower() not in NON_BLOCKING_SEVERITIES


def _level(f: Finding) -> str | None:
    if f.status in DEFECT_STATUSES:
        sev = str((f.extra or {}).get("severity", "")).lower()
        if sev in {"warning", "style"} or f.strength == laws.STRENGTH_SOME:
            return "warning"
        return "error"
    if f.status == laws.HYPOTHESIS:
        return "note"
    return None


def _uri(path: str) -> str:
    return path.replace("\\", "/").replace("%", "%25").replace(" ", "%20")


def _rule_id(f: Finding) -> str:
    return f.cls or f"{f.stage}/{f.status}"


def to_sarif(report: RunReport) -> dict[str, Any]:
    rules: dict[str, dict[str, Any]] = {}
    results: list[dict[str, Any]] = []
    for s in report.stages:
        for f in s.findings:
            level = _level(f)
            if level is None:
                continue
            rid = _rule_id(f)
            if rid not in rules:
                rules[rid] = {"id": rid, "shortDescription": {"text": rid}}
            text = f"[{f.stage} {f.status}] {f.message}"
            if f.counterexample:
                text += f" (counterexample: {f.counterexample})"
            res: dict[str, Any] = {
                "ruleId": rid,
                "level": level,
                "message": {"text": text},
                "properties": {
                    "stage": f.stage,
                    "status": f.status,
                    "strength": f.strength,
                },
            }
            if f.function:
                res["properties"]["function"] = f.function
            if f.status == laws.HYPOTHESIS:
                res["properties"]["hypothesis"] = True
            if f.file:
                phys: dict[str, Any] = {
                    "artifactLocation": {"uri": _uri(f.file), "uriBaseId": "SRCROOT"},
                }
                if f.line and int(f.line) > 0:
                    phys["region"] = {"startLine": int(f.line)}
                res["locations"] = [{"physicalLocation": phys}]
            results.append(res)
    notes = [
        {
            "level": "error" if s.status == "failed" else "warning",
            "message": {"text": f"{s.name} {s.status}: {s.detail or s.install}".rstrip(": ")},
        }
        for s in report.stages if s.status in {"NOTRUN", "failed"}
    ]
    root = Path(report.root) if report.root else None
    base = root if root is None or root.is_dir() else root.parent
    run: dict[str, Any] = {
        "tool": {"driver": {
            "name": "PRISM",
            "version": __version__,
            "informationUri": INFO_URI,
            "rules": sorted(rules.values(), key=lambda r: r["id"]),
        }},
        "invocations": [{
            "executionSuccessful": not any(s.status == "failed" for s in report.stages),
            "toolExecutionNotifications": notes,
        }],
        "results": results,
        "properties": {"confidence": report.confidence},
    }
    if base is not None:
        run["originalUriBaseIds"] = {"SRCROOT": {"uri": base.resolve().as_uri() + "/"}}
    return {"$schema": SARIF_SCHEMA, "version": "2.1.0", "runs": [run]}


def write_sarif(report: RunReport, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(to_sarif(report), indent=2), encoding="utf-8")


def exit_code(report: RunReport, fail_on: str) -> int:
    """2 = a stage crashed; 1 = --fail-on policy tripped; 0 otherwise.

    Defects with extra.severity warning/note/style never trip the policy.
    """
    if any(s.status == "failed" for s in report.stages):
        return 2
    if fail_on == "never":
        return 0
    findings = [f for s in report.stages for f in s.findings]
    if any(blocks(f) for f in findings):
        return 1
    if fail_on == "gap" and (
        any(s.status == "NOTRUN" for s in report.stages)
        or any(f.status in GAP_STATUSES for f in findings)
    ):
        return 1
    return 0
