"""Dafny-style requires/ensures encoded as BMC assumptions/assertions.

A successful check is PROVED-ASSUMING, never PROVED: the requires clause
is an explicit harness, recorded on the finding.
"""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import re

from prism import laws
from prism.bmc import bmc_function
from prism.cparse import body_needs_pointer_harness
from prism.models import Finding, FunctionInfo

# Subset the instrumenter advertises. Other C predicates are still forwarded
# to the BMC frontend; a parse failure is ERROR, not a proof.
REQ_ATOM = re.compile(
    r"^([A-Za-z_]\w*)\s*(<|>=|!=)\s*(0|[1-9]\d*)$"
)
ENS_ATOM = re.compile(
    r"^result\s*(==|>=)\s*(.+)$"
)
# Simple identifier only. Compound measures (n - i, n - 1, *, tuples)
# are ERROR, never PROVED-ASSUMING: PRISM does not decide their
# well-foundedness the way Dafny's VC generator does.
_DEC_IDENT = re.compile(r"^[A-Za-z_]\w*$")
_INV_ATOM = re.compile(
    r"^([A-Za-z_]\w*)\s*(<=|>=|<|>|==|!=)\s*([A-Za-z_]\w*|0|[1-9]\d*)$"
)

_COMMENT_CLAUSE = re.compile(
    r"(?://|/\*|\*)\s*(requires|ensures|invariant|decreases|diff)\s*:\s*(.+?)(?:\*/)?\s*$",
    re.I,
)
_ACSL_KEYWORD = re.compile(
    r"^(requires|ensures|invariant|decreases|assigns)\s+(.+)$",
    re.I,
)


def _normalize_acsl_result(text: str) -> str:
    return text.replace("\\result", "result")


def _parse_acsl_body(body: str, spec: dict) -> None:
    """Split ACSL annotation body on ``;`` and accumulate clause lists."""
    body = _normalize_acsl_result(body)
    for part in body.split(";"):
        part = part.strip()
        if not part:
            continue
        m = _ACSL_KEYWORD.match(part)
        if not m:
            continue
        kind = m.group(1).lower()
        if kind == "assigns":
            continue
        val = m.group(2).strip()
        spec[kind].append(val)


def _acsl_preamble(lines: list[str], body_start: int) -> str:
    """Comment / blank lines immediately above the function, nothing else."""
    i = body_start - 1
    in_block = False
    while i >= 0:
        raw = lines[i]
        s = raw.strip()
        if in_block:
            if "/*@" in raw or s.startswith("/*"):
                in_block = False
            i -= 1
            continue
        if not s:
            i -= 1
            continue
        if s.startswith("//"):
            i -= 1
            continue
        if s.endswith("*/") or s.startswith("/*") or s.startswith("*"):
            if "/*" in raw and "*/" in raw:
                i -= 1
                continue
            in_block = True
            i -= 1
            continue
        break
    return "\n".join(lines[i + 1:body_start])


def _extract_acsl_blocks(text: str) -> list[str]:
    """Return bodies of ``/*@ ... */`` block annotations (not plain comments)."""
    bodies: list[str] = []
    i = 0
    while i < len(text):
        start = text.find("/*@", i)
        if start < 0:
            break
        end = text.find("*/", start + 3)
        if end < 0:
            break
        bodies.append(text[start + 3:end])
        i = end + 2
    return bodies


def parse_comments(fn: FunctionInfo) -> dict:
    """Parse `// requires:` / `// ensures:` (and kin) from the original source.

    FunctionInfo.body is comment-stripped by cparse, so this re-reads the file.
    """
    spec: dict = {
        "requires": [],
        "ensures": [],
        "invariant": [],
        "decreases": [],
        "diff": None,
    }
    text = _read_source(fn)
    if not text:
        return spec
    lines = text.splitlines()
    start = max(0, (fn.line or 1) - 2)
    end = fn.span[1] if fn.span and fn.span[1] else (fn.line or 1) + 40
    end = min(len(lines), end)
    region = lines[start:end]
    fn_line = fn.line or 1
    body_start = max(0, fn_line - 1)
    # Only comments immediately above this function. A previous function's
    # `/*@` must not leak into the next.
    for body in _extract_acsl_blocks(_acsl_preamble(lines, body_start)):
        _parse_acsl_body(body, spec)
    for body in _extract_acsl_blocks("\n".join(lines[body_start:end])):
        _parse_acsl_body(body, spec)
    for ln in region:
        stripped = ln.strip()
        if stripped.startswith("//@"):
            _parse_acsl_body(stripped[3:], spec)
            continue
        m = _COMMENT_CLAUSE.search(ln)
        if not m:
            continue
        kind = m.group(1).lower()
        val = m.group(2).strip()
        if val.endswith("*/"):
            val = val[:-2].strip()
        if kind == "diff":
            spec["diff"] = val.split()[0] if val else None
        else:
            spec[kind].append(val)
    return spec


def bmc_function_with_assume(
    fn: FunctionInfo,
    unwind: int,
    requires: str | None,
    ensures: str | None,
    decreases: str | None = None,
    invariant: str | None = None,
) -> Finding:
    """Instrument requires as an early return (assume) and ensures as assert."""
    base = dict(
        stage="contracts", file=fn.file, function=fn.name, line=fn.line,
        cls="FUNC-CONTRACT", strength=laws.STRENGTH_PROVES,
    )
    core = fn.body
    inv_extra: dict = {}
    dec_extra: dict = {}
    if invariant and _has_loop(core):
        if not _encode_invariant(invariant):
            return Finding(
                **base, status=laws.ERROR,
                message=f"cannot encode invariant ({invariant}) for BMC",
                extra={
                    "requires": requires, "ensures": ensures,
                    "invariant": invariant, "invariant_unencoded": True,
                },
            )
        core, ok = _instrument_invariant(core, invariant)
        inv_extra["invariant"] = invariant
        inv_extra["invariant_encoded"] = ok
        if not ok:
            return Finding(
                **base, status=laws.ERROR,
                message=f"invariant ({invariant}) not instrumented",
                extra={
                    **inv_extra, "requires": requires, "ensures": ensures,
                    "invariant_unencoded": True,
                },
            )
    if decreases:
        # Honesty: a non-identifier measure is ERROR even when there is no
        # loop to instrument. Never silently drop it into PROVED-ASSUMING.
        if not _encode_decreases(decreases):
            return Finding(
                **base, status=laws.ERROR,
                message=f"cannot encode decreases ({decreases}) for BMC",
                extra={
                    "requires": requires, "ensures": ensures,
                    "decreases": decreases, "decreases_unencoded": True,
                },
            )
        if _has_loop(core):
            core, ok = _instrument_decreases(core, decreases)
            dec_extra["decreases"] = decreases
            dec_extra["decreases_encoded"] = ok
            if not ok:
                return Finding(
                    **base, status=laws.ERROR,
                    message=f"decreases ({decreases}) not instrumented",
                    extra={
                        **dec_extra, "requires": requires, "ensures": ensures,
                        "decreases_unencoded": True,
                    },
                )

    cloned = replace(fn, body=_instrument(core, requires, ensures))
    r = bmc_function(cloned, unwind)
    extra = dict(r.extra or {})
    extra["requires"] = requires
    extra["ensures"] = ensures
    extra["assumed"] = bool(requires) or bool(invariant)
    extra["original_status"] = r.status
    extra.update(inv_extra)
    extra.update(dec_extra)
    r.extra = extra
    r.stage = "contracts"
    if not r.cls:
        r.cls = "FUNC-CONTRACT"
    if laws.is_proof(r.status) and (requires or invariant):
        r.status = laws.PROVED_ASSUMING
        parts: list[str] = []
        if requires:
            parts.append(f"({requires})")
        if invariant:
            parts.append(f"invariant ({invariant})")
        assumed = " assuming " + " and ".join(parts)
        r.message = f"ensures holds{assumed}; never an unconditional PROVED"
    return r


def prove_contracts(functions: list[FunctionInfo], unwind: int) -> list[Finding]:
    """Prove `// ensures:` under `// requires:` via BMC. Proofs are PROVED-ASSUMING."""
    out: list[Finding] = []
    for fn in functions:
        spec = parse_comments(fn)
        reqs: list[str] = spec.get("requires") or []
        ens: list[str] = spec.get("ensures") or []
        decs: list[str] = spec.get("decreases") or []
        invs: list[str] = spec.get("invariant") or []
        if not reqs and not ens and not decs and not invs:
            continue
        requires = " && ".join(reqs) if reqs else None
        ensures = " && ".join(ens) if ens else None
        decreases = decs[0] if decs else None
        invariant = " && ".join(invs) if invs else None
        ptr_body = body_needs_pointer_harness(fn.body or "")
        if fn.kind == "POINTER" or ptr_body or fn.kind != "SCALAR":
            why = "POINTER" if fn.kind == "POINTER" or ptr_body else fn.kind
            msg = (
                "POINTER: contract BMC would invent a buffer; not a proof"
                if why == "POINTER"
                else f"{fn.kind}: contract scalar subset only; not a proof"
            )
            out.append(Finding(
                stage="contracts",
                file=fn.file,
                function=fn.name,
                line=fn.line,
                status=laws.NEEDS_HARNESS,
                cls="FUNC-CONTRACT",
                message=msg,
                strength=laws.STRENGTH_PROVES,
                extra={
                    "requires": requires,
                    "ensures": ensures,
                    "decreases": decreases,
                    "invariant": invariant,
                },
            ))
            continue
        rec = bmc_function_with_assume(
            fn, unwind, requires, ensures, decreases=decreases, invariant=invariant,
        )
        if rec.status != laws.ERROR and invariant and _has_loop(fn.body):
            baseline = bmc_function_with_assume(
                fn, unwind, requires, ensures, decreases=decreases, invariant=None,
            )
            closed_without = baseline.status == laws.PROVED_UNBOUNDED
            if laws.is_proof(rec.extra.get("original_status", rec.status)):
                if rec.extra.get("original_status") == laws.PROVED_UNBOUNDED:
                    rec.extra["original_status"] = laws.PROVED_ASSUMING
                if laws.is_proof(rec.status) and rec.status == laws.PROVED_UNBOUNDED:
                    rec.status = laws.PROVED_ASSUMING
                    rec.message = (
                        f"ensures holds assuming invariant ({invariant}); "
                        "never PROVED-UNBOUNDED"
                    )
            elif closed_without and laws.is_proof(rec.status):
                rec.status = laws.PROVED_ASSUMING
                rec.message = (
                    f"ensures holds assuming invariant ({invariant}); "
                    "never PROVED-UNBOUNDED"
                )
        if rec.status != laws.ERROR and decreases and _has_loop(fn.body):
            baseline = bmc_function_with_assume(
                fn, unwind, requires, ensures, decreases=None,
            )
            closed_without = baseline.status == laws.PROVED_UNBOUNDED
            rec.extra["decreases_assumed"] = (
                laws.is_proof(rec.extra.get("original_status", rec.status))
                and not closed_without
            )
            if rec.extra["decreases_assumed"]:
                if rec.extra.get("original_status") == laws.PROVED_UNBOUNDED:
                    rec.extra["original_status"] = laws.PROVED_ASSUMING
                if laws.is_proof(rec.status) and rec.status == laws.PROVED_UNBOUNDED:
                    rec.status = laws.PROVED_ASSUMING
                    rec.message = (
                        "ensures holds under decreases variant; never PROVED-UNBOUNDED"
                    )
        rec.extra["subset"] = all(
            bool(REQ_ATOM.match(r.strip())) for r in reqs
        ) and all(bool(ENS_ATOM.match(e.strip())) for e in ens)
        out.append(rec)
    return out


def _has_loop(body: str) -> bool:
    return bool(re.search(r"\b(while|for)\b", body))


def _encode_decreases(expr: str) -> bool:
    compact = re.sub(r"\s+", "", expr.strip())
    return bool(compact and _DEC_IDENT.fullmatch(compact))


def _encode_invariant(expr: str) -> bool:
    parts = [p.strip() for p in expr.split("&&")]
    return bool(parts) and all(p and _INV_ATOM.match(p) for p in parts)


def _take_paren(text: str) -> tuple[str, str]:
    text = text.lstrip()
    if not text.startswith("("):
        raise ValueError("expected (")
    depth = 0
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return text[1:i], text[i + 1:]
    raise ValueError("unbalanced (")


def _take_brace(text: str) -> tuple[str, str]:
    text = text.lstrip()
    if not text.startswith("{"):
        raise ValueError("expected {")
    depth = 0
    for i, ch in enumerate(text):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return text[1:i], text[i + 1:]
    raise ValueError("unbalanced {")


def _take_block_or_stmt(text: str) -> tuple[str, str]:
    text = text.lstrip()
    if text.startswith("{"):
        inner, rest = _take_brace(text)
        return "{" + inner + "}", rest
    depth = 0
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == ";" and depth == 0:
            return text[: i + 1], text[i + 1:]
    raise ValueError("no semicolon")


def _wrap_loop_invariant_body(loop_body: str, inv: str) -> str:
    """Wrap a loop body with invariant asserts at entry and exit."""
    inner = loop_body.strip()
    if inner.startswith("{"):
        inner = inner[1:-1]
    else:
        inner = loop_body
    return (
        f"{{ {{ assert({inv}); }} "
        f"{inner} "
        f"{{ assert({inv}); }} }}"
    )


def _wrap_loop_body(loop_body: str, dec: str, vid: int) -> str:
    """Wrap a loop body with ranking checks (one assert per BMC block)."""
    vn = f"__prism_d{vid}"
    inner = loop_body.strip()
    if inner.startswith("{"):
        inner = inner[1:-1]
    else:
        inner = loop_body
    return (
        f"{{ {{ assert(({dec}) >= 0); }} "
        f"{{ int {vn} = ({dec}); {inner} assert(({dec}) < {vn}); }} }}"
    )


def _instrument_invariant(body: str, invariant: str) -> tuple[str, bool]:
    inv = invariant.strip()
    if not _encode_invariant(inv):
        return body, False

    def transform(text: str) -> str:
        out: list[str] = []
        i = 0
        while i < len(text):
            rest = text[i:]
            stripped = rest.lstrip()
            if not stripped:
                out.append(rest)
                break
            pad = rest[: len(rest) - len(stripped)]
            out.append(pad)
            i += len(pad)

            if stripped.startswith("{"):
                inner, after = _take_brace(stripped)
                out.append("{" + transform(inner) + "}")
                i += len(stripped) - len(after)
                continue

            matched = False
            for kw in ("while", "for"):
                if re.match(rf"\b{kw}\b", stripped):
                    header, after_paren = _take_paren(stripped[len(kw):])
                    loop_body, after_body = _take_block_or_stmt(after_paren)
                    out.append(
                        f"{{ assert({inv}); }} {kw} ({header}) "
                        f"{_wrap_loop_invariant_body(loop_body, inv)}"
                    )
                    i += len(stripped) - len(after_body)
                    matched = True
                    break
            if matched:
                continue

            if re.match(r"\bif\b", stripped):
                cond, after_paren = _take_paren(stripped[2:])
                then_src, after_then = _take_block_or_stmt(after_paren)
                after_then = after_then.lstrip()
                clause = f"if ({cond}) {transform(then_src)}"
                if after_then.startswith("else"):
                    else_src, after_else = _take_block_or_stmt(after_then[4:])
                    clause += f" else {transform(else_src)}"
                    after_then = after_else
                out.append(clause)
                i += len(stripped) - len(after_then)
                continue

            stmt, after = _take_block_or_stmt(stripped)
            out.append(stmt)
            i += len(stripped) - len(after)
        return "".join(out)

    try:
        return transform(body), True
    except ValueError:
        return body, False


def _instrument_decreases(body: str, decreases: str) -> tuple[str, bool]:
    dec = decreases.strip()
    if not _encode_decreases(dec):
        return body, False
    vid = 0

    def transform(text: str) -> str:
        nonlocal vid
        out: list[str] = []
        i = 0
        while i < len(text):
            rest = text[i:]
            stripped = rest.lstrip()
            if not stripped:
                out.append(rest)
                break
            pad = rest[: len(rest) - len(stripped)]
            out.append(pad)
            i += len(pad)

            if stripped.startswith("{"):
                inner, after = _take_brace(stripped)
                out.append("{" + transform(inner) + "}")
                i += len(stripped) - len(after)
                continue

            matched = False
            for kw in ("while", "for"):
                if re.match(rf"\b{kw}\b", stripped):
                    header, after_paren = _take_paren(stripped[len(kw):])
                    loop_body, after_body = _take_block_or_stmt(after_paren)
                    vid += 1
                    out.append(
                        f"{kw} ({header}) {_wrap_loop_body(loop_body, dec, vid)}"
                    )
                    i += len(stripped) - len(after_body)
                    matched = True
                    break
            if matched:
                continue

            if re.match(r"\bif\b", stripped):
                cond, after_paren = _take_paren(stripped[2:])
                then_src, after_then = _take_block_or_stmt(after_paren)
                after_then = after_then.lstrip()
                clause = f"if ({cond}) {transform(then_src)}"
                if after_then.startswith("else"):
                    else_src, after_else = _take_block_or_stmt(after_then[4:])
                    clause += f" else {transform(else_src)}"
                    after_then = after_else
                out.append(clause)
                i += len(stripped) - len(after_then)
                continue

            stmt, after = _take_block_or_stmt(stripped)
            out.append(stmt)
            i += len(stripped) - len(after)
        return "".join(out)

    try:
        return transform(body), True
    except ValueError:
        return body, False


def _instrument(body: str, requires: str | None, ensures: str | None) -> str:
    core = body
    if ensures:
        core = _rewrite_returns(body, ensures)
    parts: list[str] = []
    if requires:
        # Paths that miss the precondition leave without checking ensures.
        parts.append(f"if (!({requires})) return 0;")
    parts.append(core)
    if ensures and not re.search(r"\breturn\b", body):
        parts.append(f"assert({ensures});")
    return "\n".join(parts)


def _rewrite_returns(body: str, ensures: str) -> str:
    def repl(m: re.Match[str]) -> str:
        expr = m.group(1).strip()
        if not expr:
            return m.group(0)
        return f"{{ int result = {expr}; assert({ensures}); return result; }}"

    return re.sub(r"\breturn\s+([^;]+);", repl, body)


def _read_source(fn: FunctionInfo) -> str:
    p = _locate_source(fn)
    if p is None:
        return ""
    try:
        return p.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def _locate_source(fn: FunctionInfo) -> Path | None:
    if not fn.file:
        return None
    p = Path(fn.file)
    if p.is_file():
        return p
    cwd = Path.cwd() / p
    if cwd.is_file():
        return cwd
    td = Path(__file__).resolve().parents[1] / "testdata" / p.name
    if td.is_file():
        return td
    return None
