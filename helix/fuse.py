"""FuSeBMC closed loop: BMC cex -> fuzz seeds -> new coverage goals -> BMC.

A fuzzer that finds nothing is CLEAN, which is not a proof.
"""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path
import os
import re

from helix import laws
from helix.afl import afl_available, run_afl_fuzz
from helix.bmc import HAS_Z3, bmc_function, unencoded_syntax_reason
from helix.cparse import body_needs_pointer_harness
from helix.fuzz import bytes_from_cex, fuzz_function, param_nbytes
from helix.models import Finding, FunctionInfo

_IF = re.compile(r"\bif\s*\(([^)]+)\)")


def seeds_from_bmc(fn: FunctionInfo, bmc_findings: list[Finding]) -> list[bytes]:
    """Turn BMC counterexamples for `fn` into harness stdin bytes."""
    n = param_nbytes(fn.params)
    out: list[bytes] = []
    for f in bmc_findings:
        if f.function != fn.name:
            continue
        if f.status != laws.FAILED:
            continue
        b = bytes_from_cex(f.counterexample, n)
        if b:
            out.append(b)
    return out


def branch_goals(fn: FunctionInfo) -> list[str]:
    """Coverage goals: the boolean of each `if` (FuSeBMC-style labels)."""
    seen: list[str] = []
    for m in _IF.finditer(fn.body):
        cond = " ".join(m.group(1).split())
        if cond and cond not in seen:
            seen.append(cond)
    return seen


def bmc_toward_goal(fn: FunctionInfo, cond: str, unwind: int = 8) -> Finding:
    """Re-BMC with the branch forced true — an uncovered FuSeBMC goal."""
    cloned = replace(fn, body=f"if (!({cond})) return 0;\n{fn.body}")
    return bmc_function(cloned, unwind)


def run_fuse(
    functions: list[FunctionInfo],
    bmc_findings: list[Finding],
    root: Path,
    budget: float,
    iters: int,
    engine=None,
) -> list[Finding]:
    """BMC seeds a fuzzer; new coverage becomes new BMC goals; repeat.

    Statuses: CRASH (certainty), CLEAN (not a proof), ERROR, NEEDS-HARNESS.
    Never PROVED.

    `engine` is an optional LlamaEngine. On stall (no new coverage) FuSeBMC
    asks ChatFuzz for mutants of one function, then continues. Default None
    leaves the loop unchanged (besides concrete-oracle crashes from fuzz).
    """
    root = Path(root)
    out: list[Finding] = []
    rounds = 2
    slice_budget = max(0.05, float(budget) / rounds)
    slice_iters = max(1, int(iters) // rounds)
    for fn in functions:
        out.append(_fuse_one(
            fn, bmc_findings, root,
            budget=slice_budget, iters=slice_iters, rounds=rounds,
            engine=engine,
        ))
    return out


def _fuse_one(
    fn: FunctionInfo,
    bmc_findings: list[Finding],
    root: Path,
    *,
    budget: float,
    iters: int,
    rounds: int,
    engine=None,
) -> Finding:
    base = dict(
        stage="fuse", file=fn.file, function=fn.name, line=fn.line,
        cls="", strength=laws.STRENGTH_FINDS,
    )
    if fn.kind == "POINTER":
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="POINTER: FuSeBMC harness would invent a buffer or pass NULL",
        )
    if fn.kind == "OTHER":
        return Finding(**base, status=laws.NEEDS_HARNESS, message="OTHER signature, not harnessed")
    syn = unencoded_syntax_reason(fn, "FuSeBMC")
    if syn:
        return Finding(**base, status=laws.NEEDS_HARNESS, message=syn)
    if body_needs_pointer_harness(fn.body or ""):
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="local pointer or heap object: FuSeBMC harness would invent a buffer",
        )

    use_afl = os.environ.get("HELIX_AFL") == "1" and afl_available() is not None
    afl_on_path = afl_available() is not None

    src = root / fn.file if not Path(fn.file).is_absolute() else Path(fn.file)
    if not src.exists() and root.is_file():
        src = root
    if not src.exists() and (root / Path(fn.file).name).exists():
        src = root / Path(fn.file).name

    seeds = seeds_from_bmc(fn, bmc_findings)
    from_bmc = len(seeds)
    goals = branch_goals(fn)
    covered_goals: set[str] = set()
    last: Finding | None = None
    extra: dict = {
        "from_bmc": from_bmc,
        "rounds": 0,
        "goals": goals,
        "new_bmc_seeds": 0,
        "noseed": from_bmc == 0,
    }
    chatfuzz_done = False
    afl_tried = False

    if afl_on_path and not use_afl:
        extra["afl_available"] = True

    if engine is not None:
        try:
            from helix.agent import fuzz4all_seeds

            n = param_nbytes(fn.params)
            added = 0
            for b in fuzz4all_seeds(engine, fn, n) or []:
                b = (b or b"")[:n].ljust(n, b"\x00")
                if b and b not in seeds:
                    seeds.append(b)
                    added += 1
            extra["fuzz4all"] = added
        except Exception as ex:
            extra["fuzz4all_error"] = str(ex)[:200]

    for r in range(rounds):
        extra["rounds"] = r + 1
        try:
            last = fuzz_function(fn, src, budget=budget, iters=iters, seeds=seeds or None)
        except Exception as ex:
            return Finding(**base, status=laws.ERROR, message=str(ex), extra=extra)
        last.stage = "fuse"
        extra["fuzz_iters"] = (last.extra or {}).get("iters")
        extra["corpus"] = (last.extra or {}).get("corpus")
        extra["new_cov"] = (last.extra or {}).get("new_cov")
        if last.status == laws.CRASH:
            last.extra = {**(last.extra or {}), **extra}
            return last
        if last.status in {laws.ERROR, laws.NEEDS_HARNESS}:
            last.extra = {**(last.extra or {}), **extra}
            return last

        if use_afl and fn.kind == "SCALAR" and not afl_tried:
            afl_tried = True
            afl_last = run_afl_fuzz(fn, src, timeout=2.0)
            if afl_last is not None:
                extra["engine"] = "afl"
                if afl_last.status == laws.CRASH:
                    afl_last.extra = {**(afl_last.extra or {}), **extra}
                    return afl_last
                if afl_last.status == laws.ERROR:
                    afl_last.extra = {**(afl_last.extra or {}), **extra}
                    return afl_last

        new_cov = int((last.extra or {}).get("new_cov") or 0)
        if engine is not None and not chatfuzz_done and new_cov == 0:
            # stall: no new coverage — ChatFuzz mutants for this one function
            chatfuzz_done = True
            try:
                from helix.agent import chatfuzz_mutants
                n = param_nbytes(fn.params)
                seed_hex = seeds[0].hex() if seeds else "00" * n
                extra["chatfuzz"] = True
                for m in chatfuzz_mutants(engine, fn, seed_hex) or []:
                    b = (m or b"")[:n].ljust(n, b"\x00")
                    if b and b not in seeds:
                        seeds.append(b)
            except Exception as ex:
                extra["chatfuzz_error"] = str(ex)[:200]

        if not HAS_Z3:
            continue
        for cond in goals:
            if cond in covered_goals:
                continue
            g = bmc_toward_goal(fn, cond, unwind=8)
            extra["rounds"] = r + 1
            if g.status == laws.FAILED and g.counterexample:
                covered_goals.add(cond)
                n = param_nbytes(fn.params)
                b = bytes_from_cex(g.counterexample, n)
                if b and b not in seeds:
                    seeds.append(b)
                    extra["new_bmc_seeds"] += 1
            elif g.status in {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED}:
                # BMC closed that branch for the encoded UB; not a fuse proof.
                covered_goals.add(cond)

    extra["covered_goals"] = sorted(covered_goals)
    extra["seeds"] = len(seeds)
    if use_afl and afl_tried:
        extra["engine"] = "afl"
    msg = f"no crash in FuSeBMC loop ({extra['rounds']} rounds; not a proof)"
    return Finding(
        **base, status=laws.CLEAN, message=msg, extra=extra,
    )
