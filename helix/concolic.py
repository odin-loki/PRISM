"""In-process KLEE-style concolic engine over the Helix SCALAR subset.

Concrete seeds always run (even without Z3). A CLEAN result is not a proof.
POINTER functions are skipped — never invent buffers.
"""

from __future__ import annotations

import re
from itertools import product
from typing import Any

from helix import laws
from helix.bmc import (
    HAS_Z3,
    INT_MAX,
    INT_MIN,
    WIDTH,
    Parser as BmcParser,
    _Enc,
    _as_bool,
    _has_self_call,
    _has_unencoded_cxx,
    _has_unencoded_float,
    _has_unencoded_throw,
    _has_unencoded_setjmp,
    harness_for_parsefail,
    unencoded_syntax_reason,
    extract_enums,
)
from helix.cparse import body_needs_pointer_harness
from helix.concrete import (
    decode_args,
    execute,
    i32,
    interesting_seeds,
)
from helix.models import Finding, FunctionInfo

_IF = re.compile(r"\bif\s*\(([^)]+)\)")

_EXTREMES = (0, 1, -1, INT_MAX, INT_MIN)


def run_concolic(functions: list[FunctionInfo], budget: int = 32) -> list[Finding]:
    """Concolic exploration: concrete seeds plus branch-negation neighbors."""
    return [concolic_function(fn, budget=budget) for fn in functions]


def concolic_function(fn: FunctionInfo, budget: int = 32) -> Finding:
    base = dict(
        stage="concolic",
        file=fn.file,
        function=fn.name,
        line=fn.line,
        cls="",
        strength=laws.STRENGTH_FINDS,
    )
    if fn.kind == "POINTER":
        return Finding(
            **base,
            status=laws.NEEDS_HARNESS,
            message="pointer parameter: concolic engine does not invent buffers",
        )
    if fn.kind == "OTHER":
        return Finding(
            **base,
            status=laws.NEEDS_HARNESS,
            message="non-scalar parameter: concolic engine does not invent objects",
        )
    if body_needs_pointer_harness(fn.body):
        return Finding(
            **base,
            status=laws.NEEDS_HARNESS,
            message="local pointer or heap object: concolic engine does not invent buffers",
        )
    if _has_self_call(fn):
        return Finding(
            **base,
            status=laws.NEEDS_HARNESS,
            message="recursive call unencoded: concrete bound is not a proof of the callee",
        )
    if _has_unencoded_float(fn):
        return Finding(
            **base,
            status=laws.NEEDS_HARNESS,
            message="float/double unencoded: concrete oracle is not an IEEE model",
        )
    if _has_unencoded_cxx(fn):
        return Finding(
            **base,
            status=laws.NEEDS_HARNESS,
            message="C++ view/span unencoded: concolic engine is not a lifetime model",
        )
    if _has_unencoded_throw(fn):
        return Finding(
            **base,
            status=laws.NEEDS_HARNESS,
            message="C++ throw unencoded: concolic engine is not an exception model",
        )
    if _has_unencoded_setjmp(fn):
        return Finding(
            **base,
            status=laws.NEEDS_HARNESS,
            message="setjmp/longjmp/va_list unencoded: concolic engine is not a nonlocal-control model",
        )
    syn = unencoded_syntax_reason(fn, "concolic engine")
    if syn:
        return Finding(**base, status=laws.NEEDS_HARNESS, message=syn)

    queue = _initial_seeds(fn)
    seen: set[tuple[tuple[str, int], ...]] = set()
    tried = 0
    generated = 0
    max_new = max(0, int(budget))

    while queue:
        args = queue.pop(0)
        key = _args_key(args)
        if key in seen:
            continue
        seen.add(key)
        tried += 1

        rec = execute(fn, args)
        if rec.error:
            err = rec.error or ""
            if err == "skip-pointer":
                return Finding(
                    **base,
                    status=laws.NEEDS_HARNESS,
                    message="pointer parameter: concolic engine does not invent buffers",
                    extra={"args": args, "tried": tried},
                )
            msg = harness_for_parsefail(err, "concolic engine")
            if msg:
                return Finding(
                    **base,
                    status=laws.NEEDS_HARNESS,
                    message=msg,
                    extra={"args": args, "tried": tried},
                )
            return Finding(
                **base,
                status=laws.ERROR,
                message=rec.error,
                extra={"args": args, "tried": tried},
            )
        if rec.ub:
            return _crash_finding(base, args, rec.ub, tried=tried, generated=generated)

        if generated >= max_new:
            continue
        for cond in _branch_conditions(fn):
            if generated >= max_new:
                break
            nxt = _neighbor_for_cond(fn, args, cond)
            if nxt is None:
                continue
            nkey = _args_key(nxt)
            if nkey in seen:
                continue
            generated += 1
            queue.append(nxt)

    return Finding(
        **base,
        status=laws.CLEAN,
        message=f"no UB in {tried} concolic inputs (not a proof)",
        extra={"tried": tried, "generated": generated, "oracle": "concrete"},
    )


def _args_key(args: dict[str, int]) -> tuple[tuple[str, int], ...]:
    return tuple(sorted((k, i32(v)) for k, v in args.items()))


def _initial_seeds(fn: FunctionInfo) -> list[dict[str, int]]:
    names = [name for _, name in fn.params if name]
    if not names:
        return [{}]

    out: list[dict[str, int]] = []
    seen: set[tuple[tuple[str, int], ...]] = set()

    def add(args: dict[str, int]) -> None:
        norm = {n: i32(args.get(n, 0)) for n in names}
        key = _args_key(norm)
        if key not in seen:
            seen.add(key)
            out.append(norm)

    add({n: 0 for n in names})
    for blob in interesting_seeds(fn):
        add(decode_args(fn, blob))

    base = {n: 0 for n in names}
    for name in names:
        for v in _EXTREMES:
            nxt = dict(base)
            nxt[name] = i32(v)
            add(nxt)
    if len(names) <= 3:
        for combo in product(_EXTREMES, repeat=len(names)):
            add(dict(zip(names, (i32(v) for v in combo))))

    return out


def _branch_conditions(fn: FunctionInfo) -> list[str]:
    seen: list[str] = []
    for m in _IF.finditer(fn.body or ""):
        cond = " ".join(m.group(1).split())
        if cond and cond not in seen:
            seen.append(cond)
    return seen


def _crash_finding(
    base: dict[str, Any],
    args: dict[str, int],
    cls: str,
    *,
    tried: int,
    generated: int,
) -> Finding:
    argstr = ", ".join(f"{k}={v}" for k, v in args.items())
    rec = dict(base)
    rec["cls"] = cls
    return Finding(
        **rec,
        status=laws.CRASH,
        message=f"{cls} on {argstr}",
        counterexample=argstr,
        extra={"args": args, "tried": tried, "generated": generated, "oracle": "concrete"},
    )


def _neighbor_for_cond(
    fn: FunctionInfo,
    args: dict[str, int],
    cond: str,
) -> dict[str, int] | None:
    cur = _eval_cond(fn, args, cond)
    if cur is None:
        return None
    want = not cur
    if HAS_Z3:
        solved = _z3_solve_flip(fn, args, cond, want=want)
        if solved is not None:
            return solved
    return _heuristic_flip(fn, args, cond, want=want)


def _eval_cond(fn: FunctionInfo, args: dict[str, int], cond: str) -> bool | None:
    try:
        from helix.concrete import _Parser, _St, _truth

        enums = _enums_for(fn)
        st = _St(fn.params, args, enums)
        p = _Parser("", st)
        return _truth(p._eval(cond))
    except Exception:
        return None


def _enums_for(fn: FunctionInfo) -> dict[str, int]:
    from pathlib import Path

    path = Path(fn.file) if fn.file else Path()
    if path.is_file():
        try:
            return extract_enums(path.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            return {}
    return {}


def _z3_solve_flip(
    fn: FunctionInfo,
    args: dict[str, int],
    cond: str,
    *,
    want: bool,
) -> dict[str, int] | None:
    if not HAS_Z3:
        return None
    try:
        import z3

        enc = _Enc(0)
        names = [name for _, name in fn.params if name]
        for name in names:
            enc.get(name)
        parser = BmcParser("", fn.params, 0, enums=_enums_for(fn))
        cond_z3 = _as_bool(parser._expr(enc, cond))
        s = z3.Solver()
        s.set("timeout", 2000)
        s.add(cond_z3 if want else z3.Not(cond_z3))
        # Prefer a input different from the seed when possible.
        diff: list[Any] = []
        for name in names:
            bv = enc.vars.get(name)
            if bv is None:
                continue
            cur = i32(args.get(name, 0))
            diff.append(bv != z3.BitVecVal(cur & 0xFFFFFFFF, WIDTH))
        if diff:
            s.push()
            s.add(z3.Or(*diff))
            if s.check() != z3.sat:
                s.pop()
        if s.check() != z3.sat:
            return None
        model = s.model()
        out: dict[str, int] = {}
        for name in names:
            bv = enc.vars.get(name)
            if bv is None:
                out[name] = i32(args.get(name, 0))
                continue
            val = model.eval(bv, model_completion=True)
            out[name] = i32(val.as_long())
        return out
    except Exception:
        return None


def _heuristic_flip(
    fn: FunctionInfo,
    args: dict[str, int],
    cond: str,
    *,
    want: bool,
) -> dict[str, int] | None:
    param_names = {name for _, name in fn.params if name}
    cond = " ".join(cond.split())

    m = re.match(r"(\w+)\s*(>=|<=|==|!=|>|<)\s*(-?\d+)", cond)
    if m:
        var, op, raw = m.group(1), m.group(2), int(m.group(3))
        if var not in param_names:
            return None
        val = i32(raw)
        cur = i32(args.get(var, 0))
        nxt = dict(args)
        if op == ">":
            nxt[var] = val + 1 if want else val
        elif op == ">=":
            nxt[var] = val if want else val - 1
        elif op == "<":
            nxt[var] = val - 1 if want else val
        elif op == "<=":
            nxt[var] = val if want else val + 1
        elif op == "==":
            nxt[var] = val if want else val + 1
        elif op == "!=":
            nxt[var] = val + 1 if want else val
        else:
            return None
        nxt[var] = i32(nxt[var])
        got = _eval_cond(fn, nxt, cond)
        if got is want and nxt != args:
            return nxt
        if got is want:
            return nxt
        return None

    m = re.match(r"(\w+)\s*(>=|<=|==|!=|>|<)\s*(\w+)", cond)
    if m:
        left, op, right = m.group(1), m.group(2), m.group(3)
        if left not in param_names:
            return None
        nxt = dict(args)
        cur = i32(args.get(left, 0))
        if right in param_names:
            rhs = i32(args.get(right, 0))
        else:
            try:
                rhs = i32(int(right, 0))
            except ValueError:
                return None
        if op == "==":
            nxt[left] = rhs if want else rhs + 1
        elif op == "!=":
            nxt[left] = rhs + 1 if want else rhs
        elif op == "<":
            nxt[left] = rhs - 1 if want else rhs
        elif op == ">":
            nxt[left] = rhs + 1 if want else rhs
        elif op == "<=":
            nxt[left] = rhs if want else rhs + 1
        elif op == ">=":
            nxt[left] = rhs if want else rhs - 1
        else:
            return None
        nxt[left] = i32(nxt[left])
        got = _eval_cond(fn, nxt, cond)
        if got is want:
            return nxt
        return None

    if re.fullmatch(r"(\w+)", cond):
        var = cond
        if var not in param_names:
            return None
        nxt = dict(args)
        nxt[var] = 1 if want else 0
        got = _eval_cond(fn, nxt, cond)
        if got is want:
            return nxt

    # Last resort: flip one parameter sign/value.
    for name in param_names:
        nxt = dict(args)
        v = i32(args.get(name, 0))
        for candidate in (0, 1, -1, INT_MAX, INT_MIN, -v, v + 1, v - 1):
            nxt[name] = i32(candidate)
            got = _eval_cond(fn, nxt, cond)
            if got is want and _args_key(nxt) != _args_key(args):
                return nxt
    return None
