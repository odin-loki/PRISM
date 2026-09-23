"""Strix-style LTL safety monitors over switch(state) machines."""

from __future__ import annotations

from collections import deque
from typing import Any
from pathlib import Path
import re
import shutil
import subprocess

from prism import laws
from prism.models import Finding, FunctionInfo

# Bounded eventually window for G (req -> F ack) and for the known
# liveness → safety strengthenings (GF p ⇒ G F_k p, p U q ⇒ p U_k q).
F_BOUND = 8

# Safety fragment we decide ourselves: G p, G (p -> X q), G (req -> F_k ack),
# and G (F_k p). Unbounded F / U / GF stay NOTRUN unless they match a
# known safety-approximation pattern below. A missing or successful strix
# binary is never PROVED of a formula outside that fragment.
SAFETY = re.compile(r"^G\s*\((.+)\)$")
G_BARE = re.compile(r"^G\s+(.+)$")
UNTIL = re.compile(r"(?<![A-Za-z_])U(?![A-Za-z_])")
GF_LIVE = re.compile(r"\bG\s*F\b|\bF\s*G\b|\bGF\b|\bFG\b")
_LTL_KW = frozenset({
    "G", "F", "X", "U", "W", "R", "M", "GF", "FG",
    "true", "false", "TRUE", "FALSE",
})
_STRIX_NOTE = (
    "strix realizability is not PROVED unless the formula is in PRISM's "
    "safety fragment (G p, G (p -> X q), G (req -> F_k ack), G (F_k p))"
)


_SWITCH_STATE = re.compile(
    r"\bswitch\s*\(\s*(?:[A-Za-z_]\w*\s*(?:->|\.)\s*)*state\s*\)"
)
# `state = DEST` / `state = (DEST)`; not `state ==` / `state !=` / `state +=`.
_STATE_ASG = re.compile(
    r"\bstate\s*(?<![<>=!])=(?!=)\s*\(?\s*([A-Za-z_]\w*|\d+)\s*\)?"
)
_ARM_LAB = re.compile(r"\b(?:case\s+([A-Za-z_]\w*|\d+)|default)\s*:")
_ARM_STOP = re.compile(r"\b(?:break|return|goto|continue)\b")


def extract_fsm(body: str) -> dict | None:
    """Finite machine from `switch (state)` / `switch(state)` only.

    Other switches (e.g. `switch (ev)`) are ignored even if the function
    mentions `state`. Fall-through `case A: case B: state = C;` shares dests.
    """
    switches = _switch_state_bodies(body)
    if not switches:
        return None
    cases: list[str] = []
    assigns: list[str] = []
    transitions: list[tuple[str, str]] = []
    default_dests: list[str] | None = None
    for sw in switches:
        arms = _parse_switch_arms(sw)
        assigns.extend(_STATE_ASG.findall(sw))
        for i, (labels, content) in enumerate(arms):
            dests, stay = _resolved_dests(arms, i)
            for lab in labels:
                if lab is None:
                    default_dests = list(dests)
                    continue
                cases.append(lab)
                if dests:
                    for d in dests:
                        transitions.append((lab, d))
                    if stay:
                        transitions.append((lab, lab))
                else:
                    transitions.append((lab, lab))
    if len(cases) < 1:
        return None
    states = sorted(set(cases + assigns + [s for e in transitions for s in e]))
    if len(states) < 2:
        return None
    has_out = {s for s, _ in transitions}
    for s in states:
        if s in has_out:
            continue
        if default_dests:
            for d in default_dests:
                transitions.append((s, d))
        else:
            # Unmatched enumerator (or default: break) keeps the state.
            transitions.append((s, s))
    return {
        "states": states,
        "cases": cases,
        "assigns": assigns,
        "transitions": transitions,
    }


def _switch_state_bodies(body: str) -> list[str]:
    """Brace-matched bodies of `switch (state)` / `p->state` / `obj.state`."""
    out: list[str] = []
    for m in _SWITCH_STATE.finditer(body):
        rest = body[m.end():]
        brace = rest.find("{")
        if brace < 0 or ";" in rest[:brace]:
            continue
        depth = 0
        end = None
        for k, ch in enumerate(rest[brace:]):
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end = brace + k
                    break
        if end is None:
            continue
        out.append(rest[brace + 1:end])
    return out


def _parse_switch_arms(sw: str) -> list[tuple[list[str | None], str]]:
    marks = list(_ARM_LAB.finditer(sw))
    if not marks:
        return []
    arms: list[tuple[list[str | None], str]] = []
    for i, m in enumerate(marks):
        lab: str | None = m.group(1)
        start = m.end()
        stop = marks[i + 1].start() if i + 1 < len(marks) else len(sw)
        content = sw[start:stop]
        if arms and not arms[-1][1].strip() and not _ARM_STOP.search(arms[-1][1]):
            # `case A: case B:` — attach the extra label to the open arm.
            arms[-1][0].append(lab)
            arms[-1] = (arms[-1][0], arms[-1][1] + content)
        else:
            arms.append(([lab], content))
    return arms


def _arm_dests(content: str) -> tuple[list[str], bool, bool]:
    dests = _STATE_ASG.findall(content)
    stops = bool(_ARM_STOP.search(content))
    stay = bool(re.search(r"\bif\b", content) and not re.search(r"\belse\b", content))
    return dests, stops, stay


def _resolved_dests(
    arms: list[tuple[list[str | None], str]], i: int
) -> tuple[list[str], bool]:
    dests, stops, stay = _arm_dests(arms[i][1])
    if stops:
        return dests, stay
    j = i
    while not stops and j + 1 < len(arms):
        j += 1
        d2, s2, stay2 = _arm_dests(arms[j][1])
        for d in d2:
            if d not in dests:
                dests.append(d)
        stay = stay or stay2
        stops = s2
        if stops:
            break
    return dests, stay


def _transitions(body: str, cases: list[str]) -> list[tuple[str, str]]:
    """Kept for callers; prefer extract_fsm which scopes to switch(state)."""
    fsm = extract_fsm(body)
    if fsm:
        return list(fsm["transitions"])
    out: list[tuple[str, str]] = []
    for lab in cases:
        out.append((lab, lab))
    return out


def parse_ltl_file(path: Path) -> list[str]:
    if not path.exists():
        return []
    out = []
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        ln = ln.strip()
        if not ln or ln.startswith("#") or ln.startswith("//"):
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
    # LTL implication is weaker than || / &&; G (p -> q) is G of a boolean.
    imps = _split_top(pred, "->")
    if len(imps) > 1:
        return (not _eval_pred(imps[0], state)) or _eval_pred(
            "->".join(imps[1:]), state
        )
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


def _split_until(s: str) -> tuple[str, str] | None:
    """Top-level ``p U q`` (paren-aware, word-boundary U). Nested U is not a known pattern."""
    depth = 0
    for i, ch in enumerate(s):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif depth == 0 and ch == "U":
            prev_ok = i == 0 or not (s[i - 1].isalnum() or s[i - 1] == "_")
            nxt = i + 1
            next_ok = nxt >= len(s) or not (s[nxt].isalnum() or s[nxt] == "_")
            if prev_ok and next_ok:
                left, right = s[:i].strip(), s[i + 1:].strip()
                if left and right and not UNTIL.search(left) and not UNTIL.search(right):
                    return left, right
                return None
    return None


def _liveness_approx(raw: str) -> tuple[str, Any] | None:
    """Known unbounded F / U / GF patterns → a safety strengthening.

    Only the atomic shapes GF p, FG p, G (F p), and top-level p U q.
    ``G (p U q)`` and nested liveness stay None (NOTRUN).
    """
    s = _strip_parens(raw.strip())
    m = re.match(r"^(?:G\s*F|GF)\s*(.+)$", s)
    if m:
        inner = _strip_parens(m.group(1).strip())
        if inner and not UNTIL.search(inner) and not GF_LIVE.search(inner):
            return "gf_approx", (inner, F_BOUND)
    m = re.match(r"^(?:F\s*G|FG)\s*(.+)$", s)
    if m:
        inner = _strip_parens(m.group(1).strip())
        if inner and not UNTIL.search(inner) and not GF_LIVE.search(inner):
            return "fg_approx", (inner, F_BOUND)
    if re.match(r"^G\b", s):
        gm = SAFETY.match(s) or G_BARE.match(s)
        if gm:
            return _g_f_inner(_strip_parens(gm.group(1)))
        return None
    parts = _split_until(s)
    if parts:
        p, q = parts
        if not GF_LIVE.search(p) and not GF_LIVE.search(q):
            return "until_approx", (p, q, F_BOUND)
    return None


def _g_f_inner(inner: str) -> tuple[str, Any] | None:
    """``G (F p)`` / ``G (F_k p)`` — explicit bound is the safety fragment."""
    inner = _strip_parens(inner)
    if _split_imp(inner):
        return None
    fm = re.match(r"^F(?:_(\d+))?\s*(.+)$", inner, re.S)
    if not fm:
        return None
    rest = _strip_parens(fm.group(2).strip())
    if not rest or UNTIL.search(rest) or GF_LIVE.search(rest):
        return None
    if fm.group(1):
        return "bounded_f", ("true", rest, int(fm.group(1)))
    return "gf_approx", (rest, F_BOUND)


def _classify(formula: str) -> tuple[str, Any]:
    """Return (kind, payload) where kind is invariant|next|bounded_f|
    f_approx|gf_approx|fg_approx|until_approx|nonsafety."""
    raw = formula.strip()
    if UNTIL.search(raw) or GF_LIVE.search(raw):
        return _liveness_approx(raw) or ("nonsafety", raw)
    m = SAFETY.match(raw) or (None if not G_BARE.match(raw) else G_BARE.match(raw))
    if not m:
        # top-level F / X / bare — not our safety fragment
        if re.match(r"^F\b", raw) or re.match(r"^X\b", raw):
            return _liveness_approx(raw) or ("nonsafety", raw)
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
            ack = _strip_parens(fm.group(2))
            if fm.group(1):
                return "bounded_f", (lhs.strip(), ack, int(fm.group(1)))
            # Unbounded F is not the safety fragment; F_k is the approximation.
            return "f_approx", (lhs.strip(), ack, F_BOUND)
        # G (p -> q) with no X/F is G of a boolean — still G p.
        return "invariant", inner
    # G (F p) / G (F_k p) — known recurrence pattern; explicit k is safety.
    gf = _g_f_inner(inner)
    if gf:
        return gf
    if re.search(r"(?<![A-Za-z_])F(?:_\d+)?(?![A-Za-z_])", inner):
        return "nonsafety", raw
    return "invariant", inner


def check_safety(formula: str, fsm: dict) -> Finding | None:
    """G p, G (p -> X q), G (req -> F_k ack) on a finite FSM.

    This is a monitor, not Strix. We only discharge formulas we can
    actually decide; the rest stay NOTRUN. Known GF / FG / U patterns
    and unbounded `G (req -> F ack)` get a *safety approximation*
    (BOUNDED on success, never PROVED of the unbounded original).
    """
    kind, payload = _classify(formula)
    try:
        if kind == "invariant":
            return _check_g(str(payload), fsm, formula)
        if kind == "next":
            p, q = payload
            return _check_next(p, q, fsm, formula)
        if kind == "bounded_f":
            req, ack, k = payload
            return _check_bounded_f(req, ack, int(k), fsm, formula)
        if kind == "f_approx":
            req, ack, k = payload
            f = _check_bounded_f(req, ack, int(k), fsm, formula)
            return _approx_finding(
                f, formula, f"G (({req}) -> F_{k} ({ack}))", "F"
            )
        if kind == "gf_approx":
            pred, k = payload
            f = _check_bounded_f("true", pred, int(k), fsm, formula)
            return _approx_finding(f, formula, f"G (F_{k} ({pred}))", "GF")
        if kind == "fg_approx":
            pred, k = payload
            return _check_fg_approx(pred, int(k), fsm, formula)
        if kind == "until_approx":
            p, q, k = payload
            return _check_until_approx(p, q, int(k), fsm, formula)
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
    p, q = payload
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


def _succ_map(fsm: dict) -> dict[str, list[str]]:
    succ = _successors(fsm.get("transitions") or [])
    for s in fsm["states"]:
        succ.setdefault(s, [s])
    return succ


def _invariant_from(pred: str, start: str, succ: dict[str, list[str]]) -> bool:
    """True iff every state reachable from start satisfies pred (G p at start)."""
    seen: set[str] = set()
    stack = [start]
    while stack:
        s = stack.pop()
        if s in seen:
            continue
        seen.add(s)
        if not _eval_pred(pred, s):
            return False
        stack.extend(succ.get(s) or [s])
    return True


def _avoids_states(start: str, targets: set[str], succ: dict[str, list[str]], k: int) -> bool:
    """True if some path of length <= k from start never visits targets."""
    if start in targets:
        return False
    q: deque[tuple[str, int]] = deque([(start, 0)])
    seen: set[tuple[str, int]] = {(start, 0)}
    while q:
        s, d = q.popleft()
        if d >= k:
            return True
        nxt = succ.get(s) or [s]
        progressed = False
        for sp in nxt:
            progressed = True
            if sp in targets:
                continue
            key = (sp, d + 1)
            if key not in seen:
                seen.add(key)
                q.append(key)
        if not progressed and d < k:
            return True
    return False


def _approx_finding(f: Finding, original: str, approx: str, kind: str) -> Finding:
    """Rewrite a safety-fragment verdict so unbounded GF/U is never PROVED."""
    extra = dict(f.extra or {})
    extra["formula"] = original
    extra["safety_approx"] = approx
    extra["approx_kind"] = kind
    extra["strix_not_proved"] = True
    extra["strix_note"] = _STRIX_NOTE
    extra["approx_note"] = (
        f"safety approximation of unbounded {kind}; "
        "not PROVED of the original formula"
    )
    if laws.is_proof(f.status):
        f.status = laws.BOUNDED
        f.message = (
            f"safety approximation {approx} of {original} holds on extracted FSM; "
            f"not a proof of unbounded {kind}"
        )
    elif f.status == laws.FAILED:
        f.message = (
            f"safety approximation {approx} of {original} violated"
        )
    f.extra = extra
    return f


def _check_fg_approx(pred: str, k: int, fsm: dict, formula: str) -> Finding:
    """F G p ≈ F_k (G p): every path hits a G-p region within k steps."""
    succ = _succ_map(fsm)
    good = {s for s in fsm["states"] if _invariant_from(pred, s, succ)}
    viol = [s for s in fsm["states"] if _avoids_states(s, good, succ, k)]
    extra = {
        "safety_approx": f"F_{k} (G ({pred}))",
        "approx_kind": "FG",
        "k": k,
        "strix_not_proved": True,
        "strix_note": _STRIX_NOTE,
        "approx_note": "safety approximation of unbounded FG; not PROVED of the original formula",
    }
    if viol:
        return _finding(
            laws.FAILED, formula,
            f"safety approximation F_{k} (G ({pred})) of {formula} violated from {viol}",
            extra=extra,
        )
    return _finding(
        laws.BOUNDED, formula,
        f"safety approximation F_{k} (G ({pred})) of {formula} holds on extracted FSM; "
        "not a proof of unbounded FG",
        extra=extra,
    )


def _until_from(start: str, p: str, q: str, succ: dict[str, list[str]], k: int) -> str:
    """'ok', 'real' (¬p ∧ ¬q before q), or 'bound' (p for k steps, no q)."""
    if _eval_pred(q, start):
        return "ok"
    if not _eval_pred(p, start):
        return "real"
    dq: deque[tuple[str, int]] = deque([(start, 0)])
    seen: set[tuple[str, int]] = {(start, 0)}
    saw_bound = False
    while dq:
        s, d = dq.popleft()
        if d >= k:
            saw_bound = True
            continue
        for sp in succ.get(s) or [s]:
            if _eval_pred(q, sp):
                continue
            if not _eval_pred(p, sp):
                return "real"
            key = (sp, d + 1)
            if key not in seen:
                seen.add(key)
                dq.append(key)
    return "bound" if saw_bound else "ok"


def _check_until_approx(p: str, q: str, k: int, fsm: dict, formula: str) -> Finding:
    succ = _succ_map(fsm)
    real: list[str] = []
    bound: list[str] = []
    for s in fsm["states"]:
        hit = _until_from(s, p, q, succ, k)
        if hit == "real":
            real.append(s)
        elif hit == "bound":
            bound.append(s)
    extra = {
        "safety_approx": f"({p}) U_{k} ({q})",
        "approx_kind": "UNTIL",
        "k": k,
        "strix_not_proved": True,
        "strix_note": _STRIX_NOTE,
        "approx_note": "safety approximation of unbounded U; not PROVED of the original formula",
    }
    if real:
        return _finding(
            laws.FAILED, formula,
            f"formula {formula} violated: {p} U {q} fails from {real} "
            "(¬p ∧ ¬q before q)",
            extra=extra,
        )
    if bound:
        return _finding(
            laws.FAILED, formula,
            f"safety approximation ({p}) U_{k} ({q}) of {formula} violated from {bound}",
            extra=extra,
        )
    return _finding(
        laws.BOUNDED, formula,
        f"safety approximation ({p}) U_{k} ({q}) of {formula} holds on extracted FSM; "
        "not a proof of unbounded U",
        extra=extra,
    )


# False = not looked yet; then the path found, or None.
_STRIX_CACHE: str | None | bool = False


def strix_available() -> str | None:
    """PATH, then a few known slots under third_party/strix. Never compile or walk."""
    global _STRIX_CACHE
    if not isinstance(_STRIX_CACHE, bool):
        return _STRIX_CACHE
    hit = shutil.which("strix") or shutil.which("strix.exe")
    if hit:
        _STRIX_CACHE = hit
        return hit
    root = Path(__file__).resolve().parents[1] / "third_party" / "strix"
    for sub in ("", "bin", "target/release", "target/debug", "build", "build/bin"):
        base = root.joinpath(*sub.split("/")) if sub else root
        for n in ("strix", "strix.exe"):
            cand = base / n
            try:
                if cand.is_file():
                    _STRIX_CACHE = str(cand)
                    return _STRIX_CACHE
            except OSError:
                continue
    _STRIX_CACHE = None
    return None


def _formula_atoms(formula: str) -> list[str]:
    toks = re.findall(r"[A-Za-z_]\w*", formula)
    return sorted({t for t in toks if t not in _LTL_KW})


def _probe_strix(formula: str, exe: str) -> dict:
    """Optional realizability probe. Success is recorded, never PROVED.

    run_ltl must not call this for a verdict: a REALIZABLE header is not
    a proof of a formula outside the safety fragment we decide ourselves.
    """
    extra: dict = {
        "strix": exe,
        "strix_not_proved": True,
        "strix_note": _STRIX_NOTE,
        "strix_ran": False,
    }
    atoms = [a for a in _formula_atoms(formula) if a.lower() != "state"]
    if not atoms:
        return extra
    cmd = [exe, "--realizability", "-f", formula, "--ins=", f"--outs={','.join(atoms)}"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=2)
    except (OSError, subprocess.TimeoutExpired):
        extra["strix_error"] = "timeout-or-os"
        return extra
    text = (r.stdout or "") + "\n" + (r.stderr or "")
    extra["strix_ran"] = True
    extra["strix_exit"] = r.returncode
    extra["strix_head"] = text[:240]
    if re.search(r"\bUNREALIZABLE\b", text):
        extra["strix_realizable"] = False
    elif re.search(r"\bREALIZABLE\b", text):
        extra["strix_realizable"] = True
    return extra


def _nonsafety_extra(formula: str, strix: str | None) -> dict:
    extra: dict = {
        "formula": formula,
        "install": "https://github.com/meyerphi/strix",
        "strix": strix or "",
        "strix_not_proved": True,
        "strix_note": _STRIX_NOTE,
    }
    # Do not invoke strix here: a REALIZABLE header is not PROVED of a
    # formula outside the safety fragment we decide ourselves. The path
    # (if any) is recorded so an adapter can run it; PRISM will not.
    return extra


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
    fsms: list[tuple[FunctionInfo, dict]] = []
    for fn in functions:
        fsm = extract_fsm(fn.body)
        if fsm:
            fsms.append((fn, fsm))
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
            extra = _nonsafety_extra(formula, strix)
            approx_kinds = {"gf_approx", "fg_approx", "until_approx", "f_approx"}
            if kind in approx_kinds:
                msg = (f"{formula}: known liveness pattern but predicates "
                       "were not evaluable on this FSM")
            elif kind != "nonsafety":
                if not fsms:
                    msg = (f"{formula}: in the safety fragment but no "
                           "switch(state) FSM extracted")
                else:
                    msg = (f"{formula}: in the safety fragment but predicates "
                           "were not evaluable on this FSM")
            elif strix:
                msg = (f"{formula}: not in the safety fragment PRISM decides "
                       f"(G p, G (p -> X q), G (req -> F_{F_BOUND} ack), G (F_{F_BOUND} p)); "
                       "strix is present but PRISM does not treat strix output as PROVED")
            else:
                msg = (f"{formula}: not in the safety fragment PRISM decides; "
                       "missing Strix binary")
            out.append(Finding(
                stage="ltl", status=laws.NOTRUN, file="", function=None, line=None,
                cls="LTL-SAFETY",
                message=msg,
                strength=laws.STRENGTH_PROVES,
                extra=extra,
            ))
    return out
