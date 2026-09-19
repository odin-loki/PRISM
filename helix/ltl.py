"""Strix-style LTL safety monitors over switch(state) machines."""

from __future__ import annotations

from collections import deque
from pathlib import Path
import re
import shutil

from helix import laws
from helix.models import Finding, FunctionInfo

# Bounded eventually window for G (req -> F ack).
F_BOUND = 8

# Safety fragment we decide ourselves. Anything with unbounded F / U is
# handed to strix if present; otherwise NOTRUN for that formula.
SAFETY = re.compile(r"^G\s*\((.+)\)$")
G_BARE = re.compile(r"^G\s+(.+)$")
UNTIL = re.compile(r"(?<![A-Za-z_])U(?![A-Za-z_])")
GF_LIVE = re.compile(r"\bG\s*F\b|\bF\s*G\b|\bGF\b|\bFG\b")


def extract_fsm(body: str) -> dict | None:
    """enum-like switch(state) with assignments state = ..."""
    if "switch" not in body or "state" not in body:
        return None
    cases = re.findall(r"case\s+([A-Za-z_]\w*|\d+)\s*:", body)
    assigns = re.findall(r"\bstate\s*=\s*([A-Za-z_]\w*|\d+)", body)
    if len(cases) < 2:
        return None
    transitions = _transitions(body, cases)
    states = sorted(set(cases + assigns + [s for e in transitions for s in e]))
    # States that only appear as assign targets still exist; default is a self-loop.
    has_out = {s for s, _ in transitions}
    for s in states:
        if s not in has_out:
            transitions.append((s, s))
    return {
        "states": states,
        "cases": cases,
        "assigns": assigns,
        "transitions": transitions,
    }


def _transitions(body: str, cases: list[str]) -> list[tuple[str, str]]:
    parts = re.split(r"\bcase\s+([A-Za-z_]\w*|\d+)\s*:", body)
    out: list[tuple[str, str]] = []
    it = iter(parts[1:])
    for lab in it:
        content = next(it, "")
        # Strip subsequent default: ... from this case chunk if split missed it
        content = re.split(r"\bdefault\s*:", content)[0]
        dests = re.findall(r"\bstate\s*=\s*([A-Za-z_]\w*|\d+)", content)
        for d in dests:
            out.append((lab, d))
        if not dests:
            out.append((lab, lab))
        elif re.search(r"\bif\b", content) and not re.search(r"\belse\b", content):
            out.append((lab, lab))
    return out


def parse_ltl_file(path: Path) -> list[str]:
    if not path.exists():
        return []
    out = []
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        out.append(ln)
    return out


def _strip_parens(s: str) -> str:
    s = s.strip()
    while s.startswith("(") and s.endswith(")"):
        depth = 0
        ok = True
        for i, ch in enumerate(s):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0 and i != len(s) - 1:
                    ok = False
                    break
        if not ok or depth != 0:
            break
        s = s[1:-1].strip()
    return s


def _split_top(s: str, sep: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    cur: list[str] = []
    i = 0
    while i < len(s):
        ch = s[i]
        if ch == "(":
            depth += 1
            cur.append(ch)
        elif ch == ")":
            depth -= 1
            cur.append(ch)
        elif depth == 0 and s.startswith(sep, i):
            parts.append("".join(cur))
            cur = []
            i += len(sep)
            continue
        else:
            cur.append(ch)
        i += 1
    parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def _split_imp(inner: str) -> tuple[str, str] | None:
    parts = _split_top(inner, "->")
    if len(parts) != 2:
        return None
    return parts[0], parts[1]


class PredFail(Exception):
    pass


def _eval_pred(pred: str, state: str) -> bool:
    pred = _strip_parens(pred.strip())
    if not pred:
        raise PredFail("empty predicate")
    low = pred.lower()
    if low in {"true", "1"}:
        return True
    if low in {"false", "0"}:
        return False
    ors = _split_top(pred, "||")
    if len(ors) > 1:
        return any(_eval_pred(p, state) for p in ors)
    ands = _split_top(pred, "&&")
    if len(ands) > 1:
        return all(_eval_pred(p, state) for p in ands)
    if pred.startswith("!"):
        return not _eval_pred(pred[1:], state)
    m = re.match(r"state\s*==\s*([A-Za-z_]\w*|\d+)$", pred)
    if m:
        return state == m.group(1)
    m = re.match(r"state\s*!=\s*([A-Za-z_]\w*|\d+)$", pred)
    if m:
        tok = m.group(1)
        if tok.upper() in {"BAD", "ERROR"}:
            return not (("BAD" in state.upper()) or ("ERROR" in state.upper()))
        return state != tok
    if re.fullmatch(r"[A-Za-z_]\w*", pred):
        if pred.upper() in {"BAD", "ERROR"}:
            return ("BAD" in state.upper()) or ("ERROR" in state.upper())
        return state == pred
    raise PredFail(pred)


def _classify(formula: str) -> tuple[str, object]:
    """Return (kind, payload) where kind is invariant|next|bounded_f|nonsafety."""
    raw = formula.strip()
    if UNTIL.search(raw) or GF_LIVE.search(raw):
        return "nonsafety", raw
    m = SAFETY.match(raw) or (None if not G_BARE.match(raw) else G_BARE.match(raw))
    if not m:
        # top-level F / X / bare — not our safety fragment
        if re.match(r"^F\b", raw) or re.match(r"^X\b", raw):
            return "nonsafety", raw
        return "nonsafety", raw
    inner = _strip_parens(m.group(1))
    imp = _split_imp(inner)
    if imp:
        lhs, rhs = imp
        rhs = rhs.strip()
        xm = re.match(r"^X\s*\(?(.+?)\)?$", rhs, re.S)
        if xm:
            return "next", (lhs.strip(), _strip_parens(xm.group(1)))
        fm = re.match(r"^F(?:_(\d+))?\s*\(?(.+?)\)?$", rhs, re.S)
        if fm:
            k = int(fm.group(1)) if fm.group(1) else F_BOUND
            return "bounded_f", (lhs.strip(), _strip_parens(fm.group(2)), k)
        return "nonsafety", raw
    # G p  /  G (p) — but a lone F inside p is liveness we do not decide
    if re.search(r"(?<![A-Za-z_])F(?:_\d+)?(?![A-Za-z_])", inner):
        return "nonsafety", raw
    return "invariant", inner


def check_safety(formula: str, fsm: dict) -> Finding | None:
    """G p, G (p -> X q), G (req -> F_k ack) on a finite FSM.

    This is a monitor, not Strix. We only discharge formulas we can
    actually decide; the rest stay NOTRUN.
    """
    kind, payload = _classify(formula)
    try:
        if kind == "invariant":
            return _check_g(str(payload), fsm, formula)
        if kind == "next":
            p, q = payload  # type: ignore[misc]
            return _check_next(p, q, fsm, formula)
        if kind == "bounded_f":
            req, ack, k = payload  # type: ignore[misc]
            return _check_bounded_f(req, ack, int(k), fsm, formula)
    except PredFail:
        return None
    return None


def _finding(
    status: str,
    formula: str,
    message: str,
    extra: dict | None = None,
    strength: str = laws.STRENGTH_PROVES,
) -> Finding:
    return Finding(
        stage="ltl", status=status, file="", function=None, line=None,
        cls="LTL-SAFETY", message=message,
        strength=strength,
        extra={"formula": formula, **(extra or {})},
    )


def _check_g(pred: str, fsm: dict, formula: str) -> Finding:
    bad = []
    for s in fsm["states"]:
        if not _eval_pred(pred, s):
            bad.append(s)
    # G (state != BAD): a violating state that is never assigned is unreachable.
    if bad:
        assigned = set(fsm.get("assigns") or [])
        cases = set(fsm.get("cases") or [])
        live = assigned | cases
        live_bad = [s for s in bad if s in live] if live else bad
        # Keep the historical polarity: assigning an error state is a violation.
        if live_bad:
            return _finding(
                laws.FAILED, formula,
                f"formula {formula} violated: FSM assigns an error state"
                if any("BAD" in x.upper() or "ERROR" in x.upper() for x in live_bad)
                else f"formula {formula} violated on states {live_bad}",
                extra={"fsm": {k: fsm[k] for k in ("states", "cases", "assigns") if k in fsm}},
            )
    return _finding(laws.PROVED, formula, f"safety {formula} holds on extracted FSM")


def _successors(trans: list[tuple[str, str]]) -> dict[str, list[str]]:
    succ: dict[str, list[str]] = {}
    for s, sp in trans:
        succ.setdefault(s, []).append(sp)
    return succ


def synthesize_missing(fsm: dict, formula: str) -> Finding | None:
    """G (p -> X q) incomplete FSM: suggest edges, never a proof."""
    kind, payload = _classify(formula)
    if kind != "next":
        return None
    p, q = payload  # type: ignore[misc]
    try:
        trans = fsm.get("transitions") or []
        for s, sp in trans:
            if _eval_pred(p, s) and not _eval_pred(q, sp):
                return None
        succ = _successors(trans)
        incomplete: list[str] = []
        synthesis: list[tuple[str, str]] = []
        q_states = [t for t in fsm["states"] if _eval_pred(q, t)]
        for s in fsm["states"]:
            if not _eval_pred(p, s):
                continue
            outs = succ.get(s, [])
            if not outs or not any(_eval_pred(q, sp) for sp in outs):
                incomplete.append(s)
                for t in q_states:
                    synthesis.append((s, t))
        if not incomplete:
            return None
        return _finding(
            laws.HYPOTHESIS, formula,
            f"missing transition from {incomplete[:6]} to a state satisfying q; synthesis is HYPOTHESIS",
            extra={"synthesis": synthesis[:24], "incomplete": incomplete[:12]},
            strength=laws.STRENGTH_READS,
        )
    except PredFail:
        return None


def _check_next(p: str, q: str, fsm: dict, formula: str) -> Finding:
    trans = fsm.get("transitions") or []
    viol = []
    for s, sp in trans:
        if _eval_pred(p, s) and not _eval_pred(q, sp):
            viol.append((s, sp))
    if viol:
        return _finding(
            laws.FAILED, formula,
            f"formula {formula} violated on transitions {viol[:6]}",
            extra={"violations": viol[:12]},
        )
    syn = synthesize_missing(fsm, formula)
    if syn:
        return syn
    return _finding(laws.PROVED, formula, f"safety {formula} holds on extracted FSM")


def _check_bounded_f(req: str, ack: str, k: int, fsm: dict, formula: str) -> Finding:
    trans = fsm.get("transitions") or []
    succ: dict[str, list[str]] = {}
    for s, sp in trans:
        succ.setdefault(s, []).append(sp)
    for s in fsm["states"]:
        succ.setdefault(s, [s])
    viol_from = []
    for s in fsm["states"]:
        if not _eval_pred(req, s):
            continue
        if _avoids_ack(s, ack, succ, k):
            viol_from.append(s)
    if viol_from:
        return _finding(
            laws.FAILED, formula,
            f"formula {formula} violated: from {viol_from} ack is avoidable within F_{k}",
        )
    return _finding(
        laws.PROVED, formula,
        f"safety {formula} holds on extracted FSM (F bound {k})",
        extra={"k": k},
    )


def _avoids_ack(start: str, ack: str, succ: dict[str, list[str]], k: int) -> bool:
    """True if some path of length <= k from start never visits an ack state."""
    if _eval_pred(ack, start):
        return False
    # BFS on (state, steps_taken) among states that have not seen ack.
    q: deque[tuple[str, int]] = deque([(start, 0)])
    seen: set[tuple[str, int]] = {(start, 0)}
    while q:
        s, d = q.popleft()
        if d >= k:
            return True
        nxt = succ.get(s) or [s]
        progressed = False
        for sp in nxt:
            if _eval_pred(ack, sp):
                progressed = True
                continue
            progressed = True
            key = (sp, d + 1)
            if key not in seen:
                seen.add(key)
                q.append(key)
        if not progressed and d < k:
            return True
    # Every path hit ack before bound.
    return False


def strix_available() -> str | None:
    return shutil.which("strix")


def run_ltl(functions: list[FunctionInfo], spec_paths: list[Path]) -> list[Finding]:
    formulas: list[str] = []
    for p in spec_paths:
        formulas.extend(parse_ltl_file(p))
    if not formulas:
        # default: no spec file is NOT a proof and not a skip-with-ok
        return [Finding(
            stage="ltl", status=laws.NOTRUN, file="", function=None, line=None,
            cls="LTL-SAFETY", message="no .ltl spec next to the sources",
            strength=laws.STRENGTH_PROVES, extra={"install": "add a file with G (...)"},
        )]
    out: list[Finding] = []
    fsms = [(fn, extract_fsm(fn.body)) for fn in functions]
    fsms = [(fn, fsm) for fn, fsm in fsms if fsm]
    strix = strix_available()
    for formula in formulas:
        decided = False
        kind, _ = _classify(formula)
        for fn, fsm in fsms:
            f = check_safety(formula, fsm)
            if not f:
                f = synthesize_missing(fsm, formula)
            if f:
                f.file = fn.file
                f.function = fn.name
                f.line = fn.line
                out.append(f)
                decided = True
        if not decided:
            extra = {"formula": formula, "install": "https://github.com/meyerphi/strix"}
            if kind != "nonsafety":
                msg = f"{formula}: not in the safety fragment Helix decides; needs Strix"
            elif strix:
                msg = (f"{formula}: not in the safety fragment Helix decides "
                       f"(G p, G (p -> X q), G (req -> F_{F_BOUND} ack)); "
                       "strix is on PATH but Helix does not drive full synthesis")
            else:
                msg = (f"{formula}: not in the safety fragment Helix decides; "
                       "missing Strix binary")
            extra["strix"] = strix or ""
            out.append(Finding(
                stage="ltl", status=laws.NOTRUN, file="", function=None, line=None,
                cls="LTL-SAFETY",
                message=msg,
                strength=laws.STRENGTH_PROVES,
                extra=extra,
            ))
    return out
