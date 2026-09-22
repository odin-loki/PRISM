"""Frama-C WP-shaped weakest-precondition discharge.

Mined from third_party/Frama-C/src/plugins/wp:

- calculus.ml `get_weakest_precondition`: the postcondition is a goal
  pushed backward through the CFG; `requires` is a hypothesis, not a
  proof of the body on the whole domain.
- cfgDump.ml: call-pre is "Prove PreCond"; post is folded as hyp.
- wp_error.ml `unsupported`: a construct the model does not encode
  aborts the calculus — that is ERROR, never a closed proof.
- VC.ml: ACSL requires/ensures become verification conditions (VCs).
- QED (wp_behav.c `qed_ok`) discharges trivial VCs without a prover.

This is PRISM's in-tree engine. Do not wrap the `frama-c` binary
(adapters_extra `_run_frama_c` is EVA, and a missing binary is NOTRUN).

A closed check is PROVED-ASSUMING, never PROVED / PROVED-UNBOUNDED:
requires is an explicit harness. POINTER stays NEEDS-HARNESS.
Unencodable ACSL (\\valid, \\forall, \\old, ...) is ERROR, not a proof.
"""

from __future__ import annotations

import re

from prism import laws
from prism.cparse import body_needs_pointer_harness
from prism.contracts import bmc_function_with_assume, parse_comments
from prism.models import Finding, FunctionInfo

_RETURN = re.compile(r"\breturn\s+([^;]+);")

# ACSL logic tokens Frama-C WP encodes in a memory/logic model we do not have.
# `\result` is the one exception: it is the returned scalar.
_ACSL_UNENC = re.compile(
    r"\\(?:"
    r"valid(?:_read|_write|_index)?|forall|exists|old|at|"
    r"separated|initialized|dangling|null|base_addr|block_length|"
    r"offset|allocable|freeable|fresh|from|nothing|let|lambda|"
    r"true|false|union|inter|subset|empty|is_finite|is_NaN|"
    r"min|max|sum|product|numof|matches|unspecified|allocation"
    r")\b",
    re.I,
)

_TOK = re.compile(
    r"\s+|"
    r"==>|==|!=|<=|>=|&&|\|\||<<|>>|->|"
    r"[+\-*/%<>=!&|^~?:()]|"
    r"0[xX][0-9A-Fa-f]+|"
    r"\d+|"
    r"[A-Za-z_]\w*|"
    r"."
)

_IDENT = re.compile(r"^[A-Za-z_]\w*$")
_NUM = re.compile(r"^(?:0[xX][0-9A-Fa-f]+|\d+)$")
_BINOP = {
    "+", "-", "*", "/", "%", "<", ">", "<=", ">=", "==", "!=",
    "&&", "||", "<<", ">>", "&", "|", "^", "?", ":",
}
_UNARY_OK = {"+", "-", "!", "~"}
_BEFORE_UNARY = {None, "(", "?", ":"} | _BINOP


def encode_predicate(text: str) -> str | None:
    """Map an ACSL / `// requires:` atom to a C scalar expression, or None.

    None means unencodable: the caller must ERROR, never PROVED-ASSUMING.
    """
    raw = (text or "").strip()
    if not raw:
        return None
    raw = raw.replace("\\result", "result")
    if _ACSL_UNENC.search(raw) or "\\ " in raw or raw.startswith("\\"):
        return None
    if "<==>" in raw or "^^" in raw:
        return None
    rewritten = _rewrite_implies(raw)
    if rewritten is None:
        return None
    return _encode_scalar(rewritten)


def run_wp(functions: list[FunctionInfo], unwind: int = 8) -> list[Finding]:
    out: list[Finding] = []
    for fn in functions:
        spec = parse_comments(fn)
        reqs: list[str] = spec.get("requires") or []
        ens: list[str] = spec.get("ensures") or []
        if not ens:
            continue
        base = dict(
            stage="wp", file=fn.file, function=fn.name, line=fn.line,
            cls="FUNC-CONTRACT", strength=laws.STRENGTH_PROVES,
        )
        if fn.kind == "POINTER" or body_needs_pointer_harness(fn.body or ""):
            out.append(Finding(
                **base, status=laws.NEEDS_HARNESS,
                message=(
                    "POINTER: WP would invent a buffer; Frama-C WP binary "
                    "is not a proof either"
                ),
                extra={"engine": "prism-wp", "wp": "return-substitution"},
            ))
            continue
        if fn.kind != "SCALAR":
            out.append(Finding(
                **base, status=laws.NEEDS_HARNESS,
                message=f"{fn.kind}: WP scalar subset only; not a proof",
                extra={"engine": "prism-wp", "wp": "return-substitution"},
            ))
            continue

        encoded_req: list[str] = []
        bad: str | None = None
        for r in reqs:
            e = encode_predicate(r)
            if e is None:
                bad = r
                break
            encoded_req.append(e)
        encoded_ens: list[str] = []
        if bad is None:
            for e0 in ens:
                e = encode_predicate(e0)
                if e is None:
                    bad = e0
                    break
                encoded_ens.append(e)
        if bad is not None:
            out.append(Finding(
                **base, status=laws.ERROR,
                message=f"cannot encode WP predicate ({bad})",
                extra={
                    "engine": "prism-wp",
                    "wp": "return-substitution",
                    "wp_unencoded": True,
                    "requires": " && ".join(reqs) if reqs else None,
                    "ensures": " && ".join(ens),
                },
            ))
            continue

        requires = " && ".join(encoded_req) if encoded_req else None
        ensures = " && ".join(encoded_ens)
        returns = [m.strip() for m in _RETURN.findall(fn.body or "")][:8]
        vcs = [_subst_result(ensures, expr) for expr in returns] or [ensures]
        extra = {
            "engine": "prism-wp",
            "wp": "return-substitution",
            "wp_returns": returns,
            "wp_vc": (
                (f"({requires}) ==> " if requires else "")
                + " && ".join(f"({v})" for v in vcs)
            ),
            "requires": requires,
            "ensures": ensures,
        }
        if vcs and all(_is_tautology(v) for v in vcs):
            extra["wp_qed"] = True
            out.append(Finding(
                **base, status=laws.PROVED_ASSUMING,
                message=(
                    f"WP of ensures ({ensures}) holds"
                    + (f" assuming ({requires})" if requires else "")
                    + "; never an unconditional PROVED"
                ),
                extra=extra,
            ))
            continue

        rec = bmc_function_with_assume(fn, unwind, requires, ensures)
        rec.stage = "wp"
        merged = dict(rec.extra or {})
        merged.update(extra)
        rec.extra = merged
        if laws.is_proof(rec.status):
            was = rec.status
            rec.status = laws.PROVED_ASSUMING
            if was == laws.PROVED_UNBOUNDED:
                rec.message = (
                    f"WP of ensures ({ensures}) holds for all unrollings"
                    + (f" assuming ({requires})" if requires else "")
                    + "; never PROVED-UNBOUNDED from WP"
                )
            else:
                rec.message = (
                    f"WP of ensures ({ensures}) holds"
                    + (f" assuming ({requires})" if requires else "")
                    + "; never an unconditional PROVED"
                )
        out.append(rec)
    return out


def _rewrite_implies(text: str) -> str | None:
    """ACSL `==>` → C `!(lhs) || (rhs)`. Nested rewrite; None on junk."""
    text = text.strip()
    if _outer_parens(text):
        inner = _rewrite_implies(text[1:-1])
        return None if inner is None else f"({inner})"
    split = _split_top(text, "==>")
    if split is None:
        return text
    lhs, rhs = split
    left = _rewrite_implies(lhs)
    right = _rewrite_implies(rhs)
    if left is None or right is None:
        return None
    return f"!({left}) || ({right})"


def _split_top(text: str, sep: str) -> tuple[str, str] | None:
    depth = 0
    i = 0
    n = len(sep)
    while i <= len(text) - n:
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth < 0:
                return None
        elif depth == 0 and text[i:i + n] == sep:
            return text[:i], text[i + n:]
        i += 1
    return None


def _encode_scalar(text: str) -> str | None:
    toks: list[str] = []
    for m in _TOK.finditer(text):
        t = m.group(0)
        if t.isspace() or not t:
            continue
        toks.append(t)
    if not toks:
        return None
    prev: str | None = None
    i = 0
    while i < len(toks):
        t = toks[i]
        if t in {"[", "]", "{", "}", ".", ",", ";", "@", "\\", "#"}:
            return None
        if t == "->":
            return None
        if _IDENT.match(t):
            nxt = toks[i + 1] if i + 1 < len(toks) else None
            if nxt == "(":
                return None
            prev = t
            i += 1
            continue
        if _NUM.match(t):
            prev = t
            i += 1
            continue
        if t in _UNARY_OK or t in {"*", "&"}:
            unary = prev in _BEFORE_UNARY or prev in _UNARY_OK or prev in {"*", "&"}
            if t in {"*", "&"} and unary:
                return None
            if t in _UNARY_OK and unary:
                prev = t
                i += 1
                continue
        if t in _BINOP or t in {"(", ")"}:
            prev = t
            i += 1
            continue
        return None
    return text.strip()


def _subst_result(ensures: str, expr: str) -> str:
    return re.sub(r"\bresult\b", f"({expr})", ensures)


def _is_tautology(pred: str) -> bool:
    """QED-shaped: both sides of == / >= / <= match after stripping parens."""
    p = pred.strip()
    if p in {"1", "true", "True"}:
        return True
    parts = _split_and(p)
    if len(parts) > 1:
        return all(_is_tautology(x) for x in parts)
    for op in ("==", ">=", "<="):
        split = _split_top(p, op)
        if split is None:
            continue
        a, b = split
        if _norm_expr(a) == _norm_expr(b) and _norm_expr(a):
            return True
    return False


def _split_and(text: str) -> list[str]:
    out: list[str] = []
    depth = 0
    last = 0
    i = 0
    while i < len(text) - 1:
        ch = text[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif depth == 0 and text[i:i + 2] == "&&":
            out.append(text[last:i].strip())
            last = i + 2
            i += 2
            continue
        i += 1
    out.append(text[last:].strip())
    return [p for p in out if p]


def _norm_expr(text: str) -> str:
    s = text.strip()
    while _outer_parens(s):
        s = s[1:-1].strip()
    return re.sub(r"\s+", "", s)


def _outer_parens(s: str) -> bool:
    if not (s.startswith("(") and s.endswith(")")):
        return False
    depth = 0
    for i, ch in enumerate(s):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i == len(s) - 1
    return False
