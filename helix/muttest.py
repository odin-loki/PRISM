"""Mutation testing against rapid property tests.

Clone the body, flip one operator (`+` to `-`, `<` to `>`, `==` to `!=`),
re-run the same requires/ensures samples. A mutant that still passes is
FAILED ("mutant survived"). One that the tests catch is CLEAN ("mutant
killed"). Silence is not a proof: killing mutants does not prove the original.
"""

from __future__ import annotations

from dataclasses import replace

from helix import laws
from helix.models import Finding, FunctionInfo
from helix.rapid import _finding_from_plan, plan_trials, run_plan


def run_muttest(functions: list[FunctionInfo], trials: int = 32) -> list[Finding]:
    """Score operator mutants against rapid-like contract checks."""
    out: list[Finding] = []
    for fn in functions:
        out.extend(_muttest_function(fn, trials))
    return out


def _muttest_function(fn: FunctionInfo, trials: int) -> list[Finding]:
    plan = plan_trials(fn, trials)
    if plan is None:
        return []
    sites = list(iter_mutations(fn.body))
    if not sites:
        return []
    baseline = run_plan(fn, plan)
    if baseline.get("error") and not baseline.get("counterexample"):
        rec = _finding_from_plan(fn, plan, stage="muttest")
        rec.message = f"cannot score mutants: {baseline['error']}"
        rec.status = laws.ERROR
        return [rec]
    out: list[Finding] = []
    for start, end, src, dst in sites:
        mutated = fn.body[:start] + dst + fn.body[end:]
        cloned = replace(fn, body=mutated)
        info = run_plan(cloned, plan)
        extra = {
            "from": src,
            "to": dst,
            "mutation": f"{src} -> {dst}",
            "offset": start,
            "trials": trials,
            "engine": info.get("engine") or "",
            "requires": plan.get("requires") or [],
            "ensures": plan.get("ensures") or [],
        }
        killed = (not info.get("ok")) or bool(info.get("error"))
        if killed:
            out.append(Finding(
                stage="muttest",
                status=laws.CLEAN,
                file=fn.file,
                function=fn.name,
                line=fn.line,
                cls="",
                message=(
                    f"mutant killed: {src} -> {dst} "
                    f"({info.get('counterexample') or info.get('error') or 'tests failed on mutant'}); "
                    "silence is not a proof"
                ),
                strength=laws.STRENGTH_SOME,
                extra=extra,
            ))
        else:
            out.append(Finding(
                stage="muttest",
                status=laws.FAILED,
                file=fn.file,
                function=fn.name,
                line=fn.line,
                cls="FUNC-CONTRACT",
                message=(
                    f"mutant survived: {src} -> {dst} "
                    f"(all {len(plan['samples'])} trials still hold; not a proof)"
                ),
                strength=laws.STRENGTH_FINDS,
                evidence=mutated.strip()[:400],
                extra=extra,
            ))
    return out


def iter_mutations(body: str) -> list[tuple[int, int, str, str]]:
    """One-at-a-time operator sites: + to -, < to >, == to !=. Skip ++, +=, <<, <=."""
    sites: list[tuple[int, int, str, str]] = []
    i = 0
    n = len(body)
    quote: str | None = None
    while i < n:
        c = body[i]
        if quote:
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c in "\"'":
            quote = c
            i += 1
            continue
        if body.startswith("==", i):
            prev = body[i - 1] if i else ""
            if prev not in "=!":
                sites.append((i, i + 2, "==", "!="))
            i += 2
            continue
        if c == "+":
            nxt = body[i + 1] if i + 1 < n else ""
            prev = body[i - 1] if i else ""
            if nxt not in "+=" and prev != "+":
                sites.append((i, i + 1, "+", "-"))
            i += 1
            continue
        if c == "<":
            nxt = body[i + 1] if i + 1 < n else ""
            prev = body[i - 1] if i else ""
            if nxt not in "<=" and prev != "<":
                sites.append((i, i + 1, "<", ">"))
            i += 1
            continue
        i += 1
    return sites
