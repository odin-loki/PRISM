"""visibility × answer × resolution. A scope with no data scores 0, not n/a."""

from __future__ import annotations

from prism import laws
from prism.models import Finding, RunReport

_EMPTY_NOTE = "confidence 0: no functions parsed (no data, not clean)"


def _fn_key(file: str, function: str | None) -> str:
    return f"{file}::{function or ''}"


# The instruments that answer per function (Law 5 "answer"). pir counts like
# bmc: either one answering is an answer (src/prism/pipeline.cpp kFormalStages).
FORMAL_STAGES = ("bmc", "pir")


def _resolves(r: Finding) -> bool:
    if laws.is_proof(r.status) or r.status == laws.BOUNDED:
        return True
    if r.status == laws.FAILED:
        # A counterexample is the instrument's answer. A failure with no
        # cex still needs a person.
        extra = r.extra or {}
        return bool(r.counterexample or extra.get("oracle") or extra.get("read"))
    return False


def score(report: RunReport) -> tuple[float, float, float, float]:
    n_fun = len(report.functions)
    if n_fun == 0:
        # Empty scope: product is 0, never None / n/a / skipped.
        return 0.0, 0.0, 0.0, 0.0

    formal = [s for s in report.stages if s.name in FORMAL_STAGES]
    classified = n_fun  # we parsed them; compile-reach is adapters

    answered = 0
    resolved = 0
    attempted = 0
    if formal:
        # One record per function per formal instrument (bmc, pir): the
        # first one that stage wrote for it.
        by_fn: dict[str, list[Finding]] = {}
        for s in formal:
            seen: set[str] = set()
            for f in s.findings:
                k = _fn_key(f.file, f.function)
                if k not in seen:
                    seen.add(k)
                    by_fn.setdefault(k, []).append(f)
        scalar = [fn for fn in report.functions if fn.kind in {"SCALAR", "VOID"}]
        attempted = 0
        for fn in scalar or report.functions:
            recs = by_fn.get(_fn_key(fn.file, fn.name), [])
            if recs and all(r.status == laws.NEEDS_HARNESS for r in recs):
                # Same as POINTER: absence of a precondition, not a missing answer.
                continue
            attempted += 1
            answers = [r for r in recs if r.status in laws.ANSWERED]
            if not answers:
                continue
            answered += 1
            if any(_resolves(r) for r in answers):
                resolved += 1
    # Law 5 through the verdict lattice (prism/laws.py score_counts, the
    # proved `score` of proofs/Prism/Verdict.lean).
    return laws.score_counts(n_fun, classified, attempted, answered, resolved)


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
