"""FuSeBMC closed loop: BMC cex -> fuzz seeds -> new coverage goals -> BMC.

A fuzzer that finds nothing is CLEAN, which is not a proof.
"""

from __future__ import annotations

from typing import Any

from dataclasses import replace
from pathlib import Path
import os
import re

from prism import laws
from prism.afl import afl_available, run_afl_fuzz
from prism.ai import LLM_INSTALL, LLM_SKIP_FUSE_MSG, create_prompt_from_source
from prism.bmc import HAS_Z3, bmc_function
from prism.config import adapter_install
from prism.cparse import body_needs_pointer_harness
from prism.fuzz import (
    bytes_from_cex,
    fuzz_function,
    param_nbytes,
    unencoded_syntax_reason,
)
from prism.models import Finding, FunctionInfo

_IF = re.compile(r"\bif\s*\(([^)]+)\)")
_WHILE = re.compile(r"\bwhile\s*\(([^)]+)\)")
_FOR = re.compile(r"\bfor\s*\(([^)]*)\)")
_SWITCH = re.compile(r"\bswitch\s*\(([^)]+)\)")
_CASE = re.compile(r"\bcase\s+([^:]+):")


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


def _norm_cond(s: str) -> str:
    return " ".join((s or "").split())


def _add_goal(seen: list[str], cond: str) -> None:
    cond = _norm_cond(cond)
    if cond and cond not in seen:
        seen.append(cond)


def branch_goals(fn: FunctionInfo) -> list[str]:
    """Coverage goals mined from FuSeBMC MyVisitor::check / checkStmt.

    Then-branch `if (cond)` (existing), implicit else `!(cond)` (`MUST_INSERT_ELSE`
    / `--add-else`), loop body + loop-exit (`PARENT_IS_LOOP` /
    `--add-label-after-loop`), and each `case` of a switch.
    """
    body = fn.body or ""
    seen: list[str] = []
    for m in _IF.finditer(body):
        cond = _norm_cond(m.group(1))
        if not cond:
            continue
        _add_goal(seen, cond)
        _add_goal(seen, f"!({cond})")
    for m in _WHILE.finditer(body):
        cond = _norm_cond(m.group(1))
        if not cond:
            continue
        _add_goal(seen, cond)
        _add_goal(seen, f"!({cond})")
    for m in _FOR.finditer(body):
        parts = m.group(1).split(";")
        if len(parts) < 2:
            continue
        cond = _norm_cond(parts[1])
        if not cond:
            continue
        _add_goal(seen, cond)
        _add_goal(seen, f"!({cond})")
    for sm in _SWITCH.finditer(body):
        expr = _norm_cond(sm.group(1))
        if not expr:
            continue
        rest = body[sm.end():]
        nxt = _SWITCH.search(rest)
        if nxt:
            rest = rest[: nxt.start()]
        for cm in _CASE.finditer(rest):
            lab = _norm_cond(cm.group(1))
            if lab:
                _add_goal(seen, f"({expr}) == ({lab})")
    return seen


def numbered_goals(fn: FunctionInfo) -> list[tuple[str, str]]:
    """FuSeBMC GoalCounter::GetNewGoalForFunc: GOAL_1, GOAL_2, ... per function.

    Counter starts at 0 and increments once per instrumented branch, matching
    ``GOAL_`` + to_string(++counter) in GoalCounter.cpp.
    """
    out: list[tuple[str, str]] = []
    counter = 0
    for cond in branch_goals(fn):
        counter += 1
        out.append((f"GOAL_{counter}", cond))
    return out


def _goals_hit_by_seeds(
    fn: FunctionInfo, labeled: list[tuple[str, str]], seeds: list[bytes],
) -> list[str]:
    """Which GOAL_* labels a fuzzer seed actually took (condition true)."""
    from prism.concrete import decode_args
    from prism.concolic import _eval_cond

    hit: list[str] = []
    seen: set[str] = set()
    for s in seeds or []:
        try:
            args = decode_args(fn, s)
        except Exception:
            continue
        for lab, cond in labeled:
            if lab in seen:
                continue
            try:
                v = _eval_cond(fn, args, cond)
            except Exception:
                continue
            if v is True:
                seen.add(lab)
                hit.append(lab)
    return hit


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
    if engine is not None and not _engine_up(engine) and functions:
        fn0 = functions[0]
        ingredients = create_prompt_from_source(
            name=fn0.name, body=fn0.body or "",
            source=_read_source(fn0, root),
        )
        out.append(Finding(
            stage="fuse", status=laws.NOTRUN, file=fn0.file, function=fn0.name,
            line=fn0.line, cls="", message=LLM_SKIP_FUSE_MSG,
            strength=laws.STRENGTH_READS,
            extra={
                "install": LLM_INSTALL, "autoprompt": "NOTRUN", "chatfuzz": "NOTRUN",
                "docstring": ingredients.get("docstring") or "",
            },
        ))
    elif engine is not None and _engine_up(engine) and functions:
        for fn in functions:
            ingredients = create_prompt_from_source(
                name=fn.name, body=fn.body or "", source=_read_source(fn, root),
            )
            distilled = ingredients.get("docstring") or ""
            try:
                from prism.agent import fuzz4all_autoprompt_text
                distilled = fuzz4all_autoprompt_text(engine, fn) or distilled
            except Exception:
                pass
            out.append(Finding(
                stage="fuse", status=laws.HYPOTHESIS, file=fn.file, function=fn.name,
                line=fn.line, cls="",
                message="Fuzz4All distilled prompt (hypothesis, not a proof)",
                strength=laws.STRENGTH_READS,
                extra={
                    "autoprompt": distilled[:800],
                    "docstring": (ingredients.get("docstring") or "")[:400],
                    "target_api": ingredients.get("target_api") or fn.name,
                },
            ))
    return out


def _read_source(fn: FunctionInfo, root: Path) -> str:
    candidates = [
        Path(fn.file) if fn.file else Path(),
        root / fn.file if fn.file else Path(),
        root / Path(fn.file).name if fn.file else Path(),
    ]
    if root.is_file():
        candidates.append(root)
    for p in candidates:
        if p.is_file():
            try:
                return p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                return ""
    return f"{fn.signature}\n{{{fn.body}\n}}"


def _engine_up(engine) -> bool:
    if engine is None:
        return False
    av = getattr(engine, "available", None)
    if av is None:
        return True
    return bool(av() if callable(av) else av)


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
    base: dict[str, Any] = dict(
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

    opted_afl = (
        os.environ.get("PRISM_AFL") == "1"
    )
    opted_libfuzzer = os.environ.get("PRISM_LIBFUZZER") == "1"
    afl_on_path = afl_available() is not None
    use_afl = opted_afl and afl_on_path

    src = root / fn.file if not Path(fn.file).is_absolute() else Path(fn.file)
    if not src.exists() and root.is_file():
        src = root
    if not src.exists() and (root / Path(fn.file).name).exists():
        src = root / Path(fn.file).name

    seeds = seeds_from_bmc(fn, bmc_findings)
    from_bmc = len(seeds)
    labeled = numbered_goals(fn)
    covered_goals: set[str] = set()
    last: Finding | None = None
    extra: dict = {
        "from_bmc": from_bmc,
        "rounds": 0,
        "goals": [lab for lab, _ in labeled],
        "goal_ids": [lab for lab, _ in labeled],
        "goal_map": {lab: cond for lab, cond in labeled},
        "new_bmc_seeds": 0,
        "new_goals": [],
        "noseed": from_bmc == 0,
    }
    chatfuzz_done = False
    mutate_done = False
    combine_done = False
    prev_interesting: bytes | None = None
    afl_tried = False
    libfuzzer_tried = False
    llm_up = _engine_up(engine)

    if opted_afl and not afl_on_path:
        extra["afl"] = "NOTRUN"
        extra["install"] = adapter_install("afl-fuzz")
    elif afl_on_path and not use_afl:
        extra["afl_available"] = True

    if engine is not None and not llm_up:
        extra["autoprompt"] = "NOTRUN"
        extra["chatfuzz"] = "NOTRUN"
        extra["install"] = LLM_INSTALL

    if llm_up:
        try:
            from prism.agent import fuzz4all_seeds

            n = param_nbytes(fn.params)
            added = 0
            for b in fuzz4all_seeds(engine, fn, n) or []:
                b = (b or b"")[:n].ljust(n, b"\x00")
                if b and b not in seeds:
                    seeds.append(b)
                    added += 1
            extra["fuzz4all"] = added
            extra["autoprompt"] = "ok"
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
                if afl_last.status == laws.NOTRUN:
                    # AFL half could not run (missing gcc/clang/afl-fuzz).
                    # Greybox CLEAN is not a proof and must not claim engine=afl.
                    extra["afl"] = "NOTRUN"
                    extra.pop("engine", None)
                    inst = (afl_last.extra or {}).get("install")
                    if inst:
                        extra["install"] = inst
                elif afl_last.status == laws.CRASH:
                    extra["engine"] = "afl"
                    afl_last.extra = {**(afl_last.extra or {}), **extra}
                    return afl_last
                elif afl_last.status == laws.ERROR:
                    extra["engine"] = "afl"
                    afl_last.extra = {**(afl_last.extra or {}), **extra}
                    return afl_last
                else:
                    extra["engine"] = "afl"

        if opted_libfuzzer and fn.kind == "SCALAR" and not libfuzzer_tried:
            libfuzzer_tried = True
            from prism.adapters_extra import _LIBFUZZER_INSTALL, _run_libfuzzer
            lf_last = _run_libfuzzer(fn, src, timeout=2.0)
            if lf_last.status == laws.NOTRUN:
                extra["libfuzzer"] = "NOTRUN"
                if extra.get("engine") == "libfuzzer":
                    extra.pop("engine", None)
                inst = (lf_last.extra or {}).get("install")
                extra["install"] = inst or _LIBFUZZER_INSTALL
            elif lf_last.status == laws.NEEDS_HARNESS:
                lf_last.extra = {**(lf_last.extra or {}), **extra}
                return lf_last
            elif lf_last.status == laws.CRASH:
                extra["engine"] = "libfuzzer"
                lf_last.extra = {**(lf_last.extra or {}), **extra}
                return lf_last
            elif lf_last.status == laws.ERROR:
                extra["engine"] = "libfuzzer"
                lf_last.extra = {**(lf_last.extra or {}), **extra}
                return lf_last
            else:
                extra["engine"] = "libfuzzer"
                extra["libfuzzer"] = "ok"

        new_cov = int((last.extra or {}).get("new_cov") or 0)
        fuzz_hit = _goals_hit_by_seeds(fn, labeled, seeds)
        newly = [g for g in fuzz_hit if g not in covered_goals]
        if newly:
            extra["new_goals"] = list(dict.fromkeys(list(extra.get("new_goals") or []) + newly))
            covered_goals.update(newly)
        if llm_up and not chatfuzz_done and new_cov == 0:
            # stall: no new coverage — ChatFuzz mutants for this one function
            chatfuzz_done = True
            try:
                from prism.agent import chatfuzz_mutants
                n = param_nbytes(fn.params)
                seed_hex = seeds[0].hex() if seeds else "00" * n
                extra["chatfuzz"] = True
                for m in chatfuzz_mutants(engine, fn, seed_hex) or []:
                    b = (m or b"")[:n].ljust(n, b"\x00")
                    if b and b not in seeds:
                        seeds.append(b)
            except Exception as ex:
                extra["chatfuzz_error"] = str(ex)[:200]
        if llm_up and new_cov > 0:
            # Fuzz4All Target.update: m_prompt mutate + c_prompt combine.
            try:
                from prism.agent import fuzz4all_combine, fuzz4all_mutate_interesting
                n = param_nbytes(fn.params)
                parent = seeds[-1] if seeds else b"\x00" * n
                if not mutate_done:
                    mutate_done = True
                    extra["fuzz4all_mutate"] = True
                    for m in fuzz4all_mutate_interesting(engine, fn, parent.hex()) or []:
                        b = (m or b"")[:n].ljust(n, b"\x00")
                        if b and b not in seeds:
                            seeds.append(b)
                prev = prev_interesting
                if prev is None and len(seeds) >= 2:
                    prev = seeds[-2]
                if prev and not combine_done:
                    combine_done = True
                    extra["fuzz4all_combine"] = True
                    for m in fuzz4all_combine(engine, fn, parent.hex(), prev.hex()) or []:
                        b = (m or b"")[:n].ljust(n, b"\x00")
                        if b and b not in seeds:
                            seeds.append(b)
                prev_interesting = parent
            except Exception as ex:
                extra["fuzz4all_mutate_error"] = str(ex)[:200]

        if not HAS_Z3:
            continue
        for lab, cond in labeled:
            if lab in covered_goals:
                continue
            # FuSeBMC esbmc-wrapper: --error-label GOAL_N as a BMC assumption.
            g = bmc_toward_goal(fn, cond, unwind=8)
            extra["rounds"] = r + 1
            if g.status == laws.FAILED and g.counterexample:
                covered_goals.add(lab)
                extra.setdefault("bmc_goals", []).append(lab)
                n = param_nbytes(fn.params)
                cex_b = bytes_from_cex(g.counterexample, n)
                if cex_b and cex_b not in seeds:
                    seeds.append(cex_b)
                    extra["new_bmc_seeds"] += 1
            elif g.status in {laws.PROVED, laws.PROVED_UNBOUNDED, laws.BOUNDED}:
                # BMC closed that branch for the encoded UB; not a fuse proof.
                covered_goals.add(lab)

    extra["covered_goals"] = sorted(covered_goals)
    extra["seeds"] = len(seeds)
    if extra.get("afl") == "NOTRUN" and extra.get("engine") == "afl":
        extra.pop("engine", None)
    if extra.get("libfuzzer") == "NOTRUN" and extra.get("engine") == "libfuzzer":
        extra.pop("engine", None)
    if use_afl and afl_tried and extra.get("afl") != "NOTRUN":
        extra["engine"] = "afl"
    msg = f"no crash in FuSeBMC loop ({extra['rounds']} rounds; not a proof)"
    return Finding(
        **base, status=laws.CLEAN, message=msg, extra=extra,
    )
