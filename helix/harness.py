"""POINTER → SCALAR/VOID harnesses from `// requires:` comments.

An honest harness never invents a buffer size. It only materializes when
requires constrain pointers (`p != 0`) and/or a length (`n >= 0`, `n < K`).
A successful BMC check is PROVED-ASSUMING, never an unconditional PROVED.
"""

from __future__ import annotations

from dataclasses import replace
import re

from helix import laws
from helix.bmc import bmc_function
from helix.contracts import parse_comments
from helix.models import Finding, FunctionInfo

_ATOM = re.compile(
    r"^([A-Za-z_]\w*)\s*(==|!=|<=|>=|<|>)\s*(0|[1-9]\d*)$"
)


def materialize(fn: FunctionInfo) -> FunctionInfo | None:
    """Rewrite a POINTER function into a SCALAR/VOID check, or None.

    None means the caller must keep NEEDS-HARNESS: no honest requires.
    """
    if fn.kind != "POINTER":
        return None
    spec = parse_comments(fn)
    reqs: list[str] = [r.strip() for r in (spec.get("requires") or []) if r.strip()]
    if not reqs:
        return None

    ptrs = _ptr_params(fn)
    if not ptrs:
        return None
    scalars = _scalar_params(fn)
    ptr_names = {n for _, n in ptrs}
    scalar_names = {n for _, n in scalars}

    atoms = []
    for req in reqs:
        m = _ATOM.match(req)
        if m:
            atoms.append((m.group(1), m.group(2), int(m.group(3)), req))

    k = _honest_size(atoms, ptr_names, scalar_names)
    if k is None:
        return None

    decls: list[str] = []
    for _, name in ptrs:
        decls.append(f"int _h_{name}[{k}];")
        decls.append(f"int *{name} = _h_{name};")

    guard = " && ".join(f"({r})" for r in reqs)
    parts = list(decls)
    if guard:
        parts.append(f"if (!({guard})) return 0;")
    parts.append(fn.body)
    body = "\n".join(parts)

    kind = "SCALAR" if scalars else "VOID"
    param_s = ", ".join(f"{t} {n}".strip() for t, n in scalars) or "void"
    ret = fn.return_type or "int"
    return replace(
        fn,
        kind=kind,
        params=list(scalars),
        signature=f"{ret} {fn.name}({param_s})",
        body=body,
    )


def run_harness_bmc(functions: list[FunctionInfo], unwind: int) -> list[Finding]:
    """BMC POINTER functions that materialize. Proofs become PROVED-ASSUMING."""
    out: list[Finding] = []
    for fn in functions:
        if fn.kind != "POINTER":
            continue
        harnessed = materialize(fn)
        if harnessed is None:
            continue
        spec = parse_comments(fn)
        reqs = [r.strip() for r in (spec.get("requires") or []) if r.strip()]
        r = bmc_function(harnessed, unwind, allow_local_pointers=True)
        extra = dict(r.extra or {})
        extra["assumed"] = True
        extra["requires"] = reqs
        extra["original_status"] = r.status
        extra["harness"] = True
        r.extra = extra
        r.stage = "harness"
        if r.status in {laws.PROVED, laws.PROVED_UNBOUNDED}:
            r.status = laws.PROVED_ASSUMING
            assumed = " && ".join(reqs)
            r.message = (
                f"encoded UB properties hold assuming ({assumed}); "
                "never an unconditional PROVED"
            )
        out.append(r)
    return out


def _ptr_params(fn: FunctionInfo) -> list[tuple[str, str]]:
    return [(t, n) for t, n in fn.params if n and ("*" in t or "[" in t)]


def _scalar_params(fn: FunctionInfo) -> list[tuple[str, str]]:
    ptrs = {n for _, n in _ptr_params(fn)}
    return [(t, n) for t, n in fn.params if n and n not in ptrs]


def _honest_size(
    atoms: list[tuple[str, str, int, str]],
    ptr_names: set[str],
    scalar_names: set[str],
) -> int | None:
    """Buffer length implied by requires, or None if we would have to invent it."""
    sizes: list[int] = []
    for name, op, val, _req in atoms:
        if name in ptr_names:
            continue
        if name not in scalar_names and scalar_names:
            # bound on a name that is not a parameter: not a length we can use
            continue
        if op == "<" and val > 0:
            sizes.append(val)
        elif op == "<=" and val >= 0:
            sizes.append(val + 1)
    if sizes:
        k = min(sizes)
        return k if k > 0 else None

    # No length bound: a single-object buffer is honest iff every pointer
    # is required non-null. Size 1 is that object, not an invented array.
    null_ok = {
        name for name, op, val, _ in atoms
        if name in ptr_names and op == "!=" and val == 0
    }
    if ptr_names and ptr_names <= null_ok:
        return 1
    return None
