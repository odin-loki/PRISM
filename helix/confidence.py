"""visibility × answer × resolution. A scope with no data scores 0, not n/a."""

from __future__ import annotations

from helix import laws
from helix.models import Finding, RunReport

_EMPTY_NOTE = "confidence 0: no functions parsed (no data, not clean)"


def _fn_key(file: str, function: str | None) -> str:
    return f"{file}::{function or ''}"


def score(report: RunReport) -> tuple[float, float, float, float]:
    n_fun = len(report.functions)
    if n_fun == 0:
        # Empty scope: product is 0, never None / n/a / skipped.
        return 0.0, 0.0, 0.0, 0.0

    bmc = next((s for s in report.stages if s.name == "bmc"), None)
    classified = n_fun
    visibility = classified / n_fun  # we parsed them; compile-reach is adapters

    answered = 0
    resolved = 0
    attempted = 0
    if bmc:
        by_fn: dict[str, list[Finding]] = {}
        for f in bmc.findings:
            by_fn.setdefault(_fn_key(f.file, f.function), []).append(f)
        scalar = [fn for fn in report.functions if fn.kind in {"SCALAR", "VOID"}]
        attempted = 0
        for fn in scalar or report.functions:
            recs = by_fn.get(_fn_key(fn.file, fn.name), [])
            if recs and recs[0].status == laws.NEEDS_HARNESS:
                # Same as POINTER: absence of a precondition, not a missing answer.
                continue
            attempted += 1
            if not recs:
                continue
            st = recs[0].status
            if st in laws.ANSWERED:
                answered += 1
                if st in {laws.PROVED, laws.PROVED_UNBOUNDED, laws.PROVED_ASSUMING}:
                    resolved += 1
                elif st == laws.FAILED:
                    # A counterexample is the instrument's answer. A
                    # failure with no cex still needs a person.
                    extra = recs[0].extra or {}
                    if recs[0].counterexample or extra.get("oracle") or extra.get("read"):
                        resolved += 1
                elif st == laws.BOUNDED:
                    resolved += 1
    vis = visibility
    ans = (answered / attempted) if attempted else 0.0
    res = (resolved / answered) if answered else 0.0
    conf = vis * ans * res
    return vis, ans, res, conf


def apply(report: RunReport) -> RunReport:
    v, a, r, c = score(report)
    report.visibility = round(v, 4)
    report.answer = round(a, 4)
    report.resolution = round(r, 4)
    report.confidence = round(c, 4)
    if not report.functions:
        if _EMPTY_NOTE not in report.notes:
            report.notes.append(_EMPTY_NOTE)
    return report
