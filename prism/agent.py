"""Dafny-shaped contracts, RLEF repair, OpenCodeInterpreter execute loop."""

from __future__ import annotations

from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile

from prism import laws, sandbox
from prism.ai import (
    AP_INSTRUCTION,
    AP_SYSTEM_MESSAGE,
    LLM_INSTALL,
    LLM_UNAVAILABLE_MSG,
    SYSTEM_AUDITOR,
    SYSTEM_CHATFUZZ,
    SYSTEM_DAFNY,
    SYSTEM_FUZZ4ALL,
    SYSTEM_FUZZ4ALL_COMBINE,
    SYSTEM_FUZZ4ALL_MUTATE,
    llm_complete_unavailable,
    SYSTEM_HARNESS,
    SYSTEM_REPAIR,
    LlamaEngine,
    create_prompt_from_source,
    extract_json,
    fuzz4all_update_strategy,
    parse_hex_seeds,
    pick_best_prompt,
)
from prism.bmc import _has_unencoded_float
from prism.models import Finding, FunctionInfo


def hypothesize(engine: LlamaEngine, functions: list[FunctionInfo], budget: int = 4) -> list[Finding]:
    if not engine.available():
        return [Finding(
            stage="llm", status=laws.NOTRUN, file="", function=None, line=None,
            cls="INTENT", message=LLM_UNAVAILABLE_MSG,
            strength=laws.STRENGTH_READS,
            extra={"install": LLM_INSTALL},
        )]
    out: list[Finding] = []
    for fn in functions[:budget]:
        src = f"{fn.signature}\n{{{fn.body}\n}}"
        r = engine.complete([
            {"role": "system", "content": SYSTEM_AUDITOR},
            {"role": "user", "content": src[:6000]},
        ])
        if r.error:
            if llm_complete_unavailable(r.error):
                out.append(Finding(
                    stage="llm", status=laws.NOTRUN, file=fn.file, function=fn.name,
                    line=fn.line, cls="INTENT", message=r.error,
                    strength=laws.STRENGTH_READS,
                    extra={"install": LLM_INSTALL, "backend": r.backend},
                ))
            else:
                out.append(Finding(
                    stage="llm", status=laws.ERROR, file=fn.file, function=fn.name,
                    line=fn.line, cls="INTENT", message=r.error,
                    strength=laws.STRENGTH_READS,
                ))
            continue
        data = extract_json(r.text) or {}
        hyps = data.get("hypotheses") if isinstance(data, dict) else None
        if not hyps:
            out.append(Finding(
                stage="llm", status=laws.HYPOTHESIS, file=fn.file, function=fn.name,
                line=fn.line, cls="INTENT", message=(r.text or "")[:400],
                strength=laws.STRENGTH_READS, extra={"backend": r.backend},
            ))
            continue
        for h in hyps:
            out.append(Finding(
                stage="llm", status=laws.HYPOTHESIS, file=fn.file,
                function=h.get("function", fn.name), line=h.get("line", fn.line),
                cls=h.get("cls", "INTENT"), message=h.get("why", ""),
                strength=laws.STRENGTH_READS, extra={"backend": r.backend},
            ))
    return out


def dafny_specs(engine: LlamaEngine, functions: list[FunctionInfo], budget: int = 3) -> list[Finding]:
    if not engine.available():
        return [Finding(
            stage="contracts", status=laws.NOTRUN, file="", function=None, line=None,
            cls="FUNC-CONTRACT", message="llama.cpp/Ollama not reachable",
            strength=laws.STRENGTH_READS,
        )]
    out = []
    for fn in functions[:budget]:
        r = engine.complete([
            {"role": "system", "content": SYSTEM_DAFNY},
            {"role": "user", "content": f"{fn.signature}\n{{{fn.body}\n}}"},
        ])
        if r.error:
            if llm_complete_unavailable(r.error):
                out.append(Finding(
                    stage="contracts", status=laws.NOTRUN, file=fn.file, function=fn.name,
                    line=fn.line, cls="FUNC-CONTRACT", message=r.error,
                    strength=laws.STRENGTH_READS,
                    extra={"install": LLM_INSTALL, "backend": r.backend},
                ))
            else:
                out.append(Finding(
                    stage="contracts", status=laws.ERROR, file=fn.file, function=fn.name,
                    line=fn.line, cls="FUNC-CONTRACT", message=r.error,
                    strength=laws.STRENGTH_READS,
                    extra={"backend": r.backend},
                ))
            continue
        data = extract_json(r.text or "")
        out.append(Finding(
            stage="contracts", status=laws.HYPOTHESIS,
            file=fn.file, function=fn.name, line=fn.line, cls="FUNC-CONTRACT",
            message="Dafny-style spec (hypothesis, not proved)",
            strength=laws.STRENGTH_READS,
            extra={"spec": data, "raw": (r.text or "")[:800], "backend": r.backend},
        ))
    return out


def fuzz4all_seeds(engine: LlamaEngine, fn: FunctionInfo, nbytes: int) -> list[bytes]:
    """LLM seeds, scored like Fuzz4All Target.validate_prompt (unique valid encodings).

    Unavailable llama is a skip: the caller writes NOTRUN. Never CLEAN.
    """
    if not engine.available():
        return []
    user = f"nbytes={nbytes}\n{fn.signature}\n{{{fn.body}\n}}"
    r = engine.complete([
        {"role": "system", "content": SYSTEM_FUZZ4ALL},
        {"role": "user", "content": user},
    ], timeout=90)
    data = extract_json(r.text or "") if not r.error else None
    seeds_a = parse_hex_seeds(data, "seeds")
    prompt_a = ""
    if isinstance(data, dict):
        prompt_a = str(data.get("prompt") or "")
    candidates: list[tuple[str, list[bytes]]] = [(prompt_a or SYSTEM_FUZZ4ALL, seeds_a)]
    distilled = fuzz4all_autoprompt_text(engine, fn)
    if distilled:
        r2 = engine.complete([
            {"role": "system", "content": SYSTEM_FUZZ4ALL + "\n" + distilled},
            {"role": "user", "content": user},
        ], timeout=90)
        data2 = extract_json(r2.text or "") if not r2.error else None
        candidates.append((distilled, parse_hex_seeds(data2, "seeds")))
    _, best, _ = pick_best_prompt(candidates, nbytes)
    return best


def _source_for(fn: FunctionInfo) -> str:
    if not fn.file:
        return f"{fn.signature}\n{{{fn.body}\n}}"
    p = Path(fn.file)
    if p.is_file():
        try:
            return p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            pass
    return f"{fn.signature}\n{{{fn.body}\n}}"


def fuzz4all_prompt_ingredients(fn: FunctionInfo) -> dict[str, str]:
    """Fuzz4All _create_prompt_from_config analog: docs + example + handwritten."""
    return create_prompt_from_source(
        name=fn.name, body=fn.body or "", source=_source_for(fn),
    )


def fuzz4all_autoprompt_text(engine: LlamaEngine, fn: FunctionInfo) -> str:
    """Fuzz4All Target.auto_prompt: distill a concise usage prompt. Empty if LLM is down."""
    if not engine.available():
        return ""
    ingredients = fuzz4all_prompt_ingredients(fn)
    message = ingredients.get("docstring") or f"{fn.signature}\n{{{fn.body}\n}}"
    r = engine.complete([
        {"role": "system", "content": AP_SYSTEM_MESSAGE},
        {"role": "user", "content": message[:6000] + "\n" + AP_INSTRUCTION},
    ], timeout=90)
    if r.error:
        return ""
    return (r.text or "").strip()[:2000]


def fuzz4all_mutate_interesting(
    engine: LlamaEngine, fn: FunctionInfo, seed_hex: str, prev_hex: str | None = None,
    strategy: int = 1,
) -> list[bytes]:
    """Fuzz4All Target.update: mutate (m_prompt) or combine (c_prompt) an interesting generation."""
    if not engine.available():
        return []
    user = fuzz4all_update_strategy(seed_hex, prev_hex, strategy=strategy)
    user += f"\n{fn.signature}\n{{{fn.body}\n}}"
    sysmsg = SYSTEM_FUZZ4ALL_COMBINE if strategy == 3 and prev_hex else SYSTEM_FUZZ4ALL_MUTATE
    r = engine.complete([
        {"role": "system", "content": sysmsg},
        {"role": "user", "content": user},
    ], timeout=90)
    data = extract_json(r.text or "") if not r.error else None
    return parse_hex_seeds(data, "mutants")


def fuzz4all_combine(
    engine: LlamaEngine, fn: FunctionInfo, seed_hex: str, prev_hex: str,
) -> list[bytes]:
    """Fuzz4All Target.c_prompt: combine two previous interesting encodings."""
    return fuzz4all_mutate_interesting(
        engine, fn, seed_hex, prev_hex, strategy=3,
    )


def chatfuzz_mutants(engine: LlamaEngine, fn: FunctionInfo, seed_hex: str) -> list[bytes]:
    if not engine.available():
        return []
    r = engine.complete([
        {"role": "system", "content": SYSTEM_CHATFUZZ},
        {"role": "user", "content": f"seed={seed_hex}\n{fn.signature}\n{{{fn.body}\n}}"},
    ], timeout=90)
    data = extract_json(r.text or "") if not r.error else None
    return parse_hex_seeds(data, "mutants")


CC_INSTALL = "install gcc or clang"
CC_MISSING_MSG = "gcc/clang not on PATH"


def find_cc() -> str | None:
    return shutil.which("gcc") or shutil.which("clang")


def _is_crash_code(code: int | None) -> bool:
    if code is None:
        return False
    if code < 0:
        return True
    # Windows NTSTATUS (e.g. STATUS_ACCESS_VIOLATION 0xC0000005).
    if code >= 0xC0000000:
        return True
    return False


def sandbox_verdict(result: dict) -> str:
    """Map sandbox_run to a status. Missing compiler is NOTRUN. Never PROVED."""
    err = result.get("error")
    if err in {"no compiler", "exec-disabled"}:
        return laws.NOTRUN
    if result.get("ok"):
        return laws.CLEAN
    if err == "crash":
        return laws.CRASH
    return laws.FAILED


def rlef_reward(result: dict, bmc_status: str | None = None) -> tuple[int, str | None]:
    """RLEF reward: compile + tests + sanitizer + BMC.

    BMC FAILED is negative. BMC PROVED* is terminal success (returned as
    the second value). Sandbox/fuzzer CLEAN and BMC BOUNDED are never a
    proof and are never merged with PROVED.
    """
    err = result.get("error")
    score = 0
    if err not in {"compile", "compile-timeout", "no compiler"}:
        score += 1
    if result.get("ok"):
        score += 2
    if err == "crash":
        score -= 1
    if bmc_status and laws.is_proof(bmc_status):
        return score, bmc_status
    if bmc_status == laws.FAILED:
        score -= 2
    return score, None


def _bmc_status_of_source(source: str, unwind: int = 2) -> str | None:
    """First SCALAR function, small unwind. None if BMC cannot answer.

    Lazy-imports prism.bmc so unit tests that patch this (or never compile
    a candidate) do not pay the BMC import. NOTRUN/ERROR are not a reward.
    """
    src = (source or "").strip()
    if not src:
        return None
    try:
        from prism.bmc import HAS_Z3, bmc_function
        from prism.cparse import extract_functions
    except Exception:
        return None
    if not HAS_Z3:
        return None
    with tempfile.TemporaryDirectory(prefix="prism_rlef_bmc_", ignore_cleanup_errors=True) as td:
        p = Path(td) / "cand.c"
        p.write_text(src, encoding="utf-8")
        try:
            fns = extract_functions(p, "cand.c")
        except Exception:
            return None
        fn = next((f for f in fns if f.kind == "SCALAR"), None)
        if fn is None:
            return None
        try:
            rec = bmc_function(fn, unwind=unwind)
        except Exception:
            return None
    st = rec.status
    if st in {laws.NOTRUN, laws.ERROR, laws.TIMEOUT, laws.NEEDS_HARNESS, laws.UNKNOWN}:
        return None
    if st == laws.CLEAN:
        return None
    return st


def _c_from_llm(text: str | None) -> str:
    src = (text or "").strip()
    if not src:
        return ""
    if src.startswith("```"):
        src = src.strip("`")
        src = src.split("\n", 1)[-1] if "\n" in src else ""
        if "```" in src:
            src = src[: src.rfind("```")]
        return src.strip()
    if "```" in src:
        src = src.split("```", 1)[1]
        src = src.split("\n", 1)[-1] if "\n" in src else src
        if "```" in src:
            src = src[: src.rfind("```")]
        return src.strip()
    return src


def _silence_win_abort() -> None:
    """Stop Windows from hanging a sandbox child on abort()/AV with a crash box."""
    if sys.platform != "win32":
        return
    try:
        import ctypes
        # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
    except Exception:
        pass


def _exec_ok(allow_exec: bool | None) -> bool:
    return sandbox.allowed() if allow_exec is None else bool(allow_exec)


def sandbox_run(source: str, timeout: float = 8.0, *, allow_exec: bool | None = None) -> dict:
    """OpenCodeInterpreter execute step: real gcc/clang compile+run in a tempdir.

    Law 9: the program is LLM-written, so it runs only with --allow-exec
    (``allow_exec``, else the run's policy) and then inside
    prism.sandbox (bubblewrap when available, rlimits always).
    """
    if not _exec_ok(allow_exec):
        return {"ok": False, "error": "exec-disabled", "stdout": "", "stderr": "", "code": None}
    cc = find_cc()
    if not cc:
        return {"ok": False, "error": "no compiler", "stdout": "", "stderr": "", "code": None}
    suffix = ".exe" if sys.platform == "win32" else ""
    _silence_win_abort()
    with tempfile.TemporaryDirectory(prefix="prism_oci_", ignore_cleanup_errors=True) as td:
        src = Path(td) / "prog.c"
        src.write_text(source, encoding="utf-8")
        exe = Path(td) / f"prog{suffix}"
        try:
            p = subprocess.run(
                [cc, "-std=c11", "-O0", "-Wall", str(src), "-o", str(exe)],
                capture_output=True, text=True, timeout=30,
            )
        except subprocess.TimeoutExpired:
            return {"ok": False, "error": "compile-timeout", "stdout": "", "stderr": "", "code": None}
        if p.returncode != 0:
            return {
                "ok": False, "error": "compile",
                "stdout": (p.stdout or "")[-2000:],
                "stderr": (p.stderr or "")[-2000:],
                "code": p.returncode,
            }
        try:
            r = sandbox.run_binary([str(exe)], scratch=Path(td), timeout=timeout, text=True)
        except subprocess.TimeoutExpired as ex:
            out = (ex.stdout or "") if isinstance(ex.stdout, str) else ""
            err = (ex.stderr or "") if isinstance(ex.stderr, str) else ""
            return {
                "ok": False, "error": "timeout",
                "stdout": out[-2000:], "stderr": err[-2000:], "code": None,
            }
        err_text = r.stderr or ""
        crashed = _is_crash_code(r.returncode) or "Aborted" in err_text
        if r.returncode == 0:
            error = None
        elif crashed:
            error = "crash"
        else:
            error = "exit"
        return {
            "ok": r.returncode == 0,
            "error": error,
            "stdout": (r.stdout or "")[-2000:],
            "stderr": err_text[-2000:],
            "code": r.returncode,
            "sandbox": sandbox.sandbox_kind(),
        }


def interpreter_loop(
    engine: LlamaEngine, prompt: str, rounds: int = 3, *, allow_exec: bool | None = None,
) -> list[Finding]:
    if not engine.available():
        return [Finding(
            stage="execute", status=laws.NOTRUN, file="", function=None, line=None,
            cls="", message=LLM_UNAVAILABLE_MSG,
            strength=laws.STRENGTH_READS,
            extra={"install": LLM_INSTALL},
        )]
    if not find_cc():
        return [Finding(
            stage="execute", status=laws.NOTRUN, file="", function=None, line=None,
            cls="", message=CC_MISSING_MSG,
            strength=laws.STRENGTH_FINDS,
            extra={"install": CC_INSTALL},
        )]
    if not _exec_ok(allow_exec):
        return [sandbox.exec_notrun("execute", "execute (LLM-written C)", half="llm")]
    messages = [
        {"role": "system", "content": SYSTEM_HARNESS},
        {"role": "user", "content": prompt},
    ]
    last_src = ""
    ran = False
    for i in range(rounds):
        r = engine.complete(messages, timeout=120)
        if r.error:
            st = laws.NOTRUN if llm_complete_unavailable(r.error) else laws.ERROR
            extra = {"install": LLM_INSTALL} if st == laws.NOTRUN else {}
            return [Finding(
                stage="execute", status=st, file="", function=None, line=None,
                cls="", message=r.error, strength=laws.STRENGTH_READS, extra=extra,
            )]
        src = _c_from_llm(r.text)
        if not src:
            messages.append({"role": "assistant", "content": r.text or ""})
            messages.append({"role": "user", "content": "Reply with a complete C file only."})
            continue
        last_src = src
        result = sandbox_run(src, allow_exec=True)
        ran = True
        verdict = sandbox_verdict(result)
        if verdict == laws.NOTRUN:
            return [Finding(
                stage="execute", status=laws.NOTRUN, file="", function=None, line=None,
                cls="", message=CC_MISSING_MSG,
                strength=laws.STRENGTH_FINDS,
                extra={"install": CC_INSTALL},
            )]
        if verdict == laws.CLEAN:
            return [Finding(
                stage="execute", status=laws.CLEAN, file="", function=None, line=None,
                cls="", message=f"interpreter harness passed on round {i+1} (not a proof)",
                strength=laws.STRENGTH_FINDS,
                extra={"stdout": result["stdout"][-400:], "rounds": i + 1,
                       "sandbox": result.get("sandbox", "")},
            )]
        if verdict == laws.CRASH:
            return [Finding(
                stage="execute", status=laws.CRASH, file="", function=None, line=None,
                cls="", message=f"sandbox crash on round {i+1} (not a proof)",
                strength=laws.STRENGTH_FINDS,
                extra={"stderr": (result.get("stderr") or "")[-400:], "rounds": i + 1,
                       "code": result.get("code"), "sandbox": result.get("sandbox", "")},
            )]
        messages.append({"role": "assistant", "content": r.text})
        messages.append({
            "role": "user",
            "content": f"execution failed: {json.dumps(result)[:1500]}. Fix the C file.",
        })
    if not ran:
        return [Finding(
            stage="execute", status=laws.HYPOTHESIS, file="", function=None, line=None,
            cls="", message="LLM produced no C to run",
            strength=laws.STRENGTH_READS,
        )]
    return [Finding(
        stage="execute", status=laws.FAILED, file="", function=None, line=None,
        cls="", message=f"interpreter loop exhausted ({rounds} rounds)",
        strength=laws.STRENGTH_FINDS, extra={"last_src": last_src[:500]},
    )]


def rlef_repair(
    engine: LlamaEngine, source: str, feedback: str, rounds: int,
    *, bmc_oracle=None, allow_exec: bool | None = None,
) -> list[Finding]:
    """Execution feedback as the reward. Keep the best candidate.

    Sandbox CLEAN is not a proof. BMC FAILED is negative. BMC PROVED is
    terminal success. BOUNDED is not merged with PROVED.
    """
    if not engine.available():
        return [Finding(
            stage="repair", status=laws.NOTRUN, file="", function=None, line=None,
            cls="", message=LLM_UNAVAILABLE_MSG,
            strength=laws.STRENGTH_READS,
            extra={"install": LLM_INSTALL},
        )]
    if not find_cc():
        return [Finding(
            stage="repair", status=laws.NOTRUN, file="", function=None, line=None,
            cls="", message=CC_MISSING_MSG,
            strength=laws.STRENGTH_FINDS,
            extra={"install": CC_INSTALL},
        )]
    if not _exec_ok(allow_exec):
        return [sandbox.exec_notrun("repair", "repair (LLM-written candidates)",
                                    strength=laws.STRENGTH_READS)]
    oracle = bmc_oracle if bmc_oracle is not None else _bmc_status_of_source
    best_score = -1
    best_src = source
    messages = [
        {"role": "system", "content": SYSTEM_REPAIR},
        {"role": "user", "content": f"SOURCE:\n{source}\n\nFEEDBACK:\n{feedback}"},
    ]
    history: list = []
    for i in range(rounds):
        r = engine.complete(messages, timeout=120)
        if r.error:
            if llm_complete_unavailable(r.error) and best_score < 0:
                return [Finding(
                    stage="repair", status=laws.NOTRUN, file="", function=None, line=None,
                    cls="", message=r.error,
                    strength=laws.STRENGTH_READS,
                    extra={"install": LLM_INSTALL, "history": history},
                )]
            history.append(r.error)
            break
        src = _c_from_llm(r.text)
        if not src:
            history.append({"round": i + 1, "score": None, "error": "silent"})
            messages.append({"role": "assistant", "content": r.text or ""})
            messages.append({"role": "user", "content": "Reply with a complete corrected C file only."})
            continue
        result = sandbox_run(src, allow_exec=True)
        if result.get("error") == "no compiler":
            return [Finding(
                stage="repair", status=laws.NOTRUN, file="", function=None, line=None,
                cls="", message=CC_MISSING_MSG,
                strength=laws.STRENGTH_FINDS,
                extra={"install": CC_INSTALL, "history": history},
            )]
        bmc_st = None
        err = result.get("error")
        if err not in {"compile", "compile-timeout"}:
            try:
                bmc_st = oracle(src)
            except Exception:
                bmc_st = None
        score, proved = rlef_reward(result, bmc_st)
        history.append({"round": i + 1, "score": score, "error": err, "bmc": bmc_st})
        if score > best_score:
            best_score = score
            best_src = src
        if proved:
            # The proof is about the LLM-written patch, not the scanned code:
            # HYPOTHESIS / READS with the patch's verdict alongside (Law 4),
            # so the verdict audit has nothing to demote.
            return [Finding(
                stage="repair", status=laws.HYPOTHESIS, file="", function=None, line=None, cls="",
                message=(f"verified fix: BMC {proved} on the LLM-written patch, round {i+1} "
                         "(terminal success; a claim about the patch, not the scanned code)"),
                strength=laws.STRENGTH_READS,
                extra={"history": history, "best": best_src[:1000], "bmc": proved,
                       "patch_verdict": proved, "fix_label": "verified fix"},
            )]
        if result.get("ok") and bmc_st != laws.FAILED:
            break
        messages.append({"role": "assistant", "content": r.text})
        messages.append({"role": "user", "content": json.dumps(result)[:1500]})
    # A high score means the patch ran. That is CLEAN, never PROVED.
    st = laws.CLEAN if best_score >= 3 else laws.HYPOTHESIS
    return [Finding(
        stage="repair", status=st, file="", function=None, line=None, cls="",
        message=f"RLEF best score {best_score} over {len(history)} rounds (not a proof)",
        strength=laws.STRENGTH_READS,
        extra={"history": history, "best": best_src[:1000]},
    )]


def execute_cex(
    fails: list[Finding],
    functions: list[FunctionInfo],
    *,
    llm: bool = False,
    engine: LlamaEngine | None = None,
    allow_exec: bool | None = None,
) -> list[Finding]:
    """Concrete cex replay plus optional OpenCodeInterpreter sandbox.

    The replay interprets (prism.concrete), so it always runs. The LLM half
    compiles and runs LLM-written C: Law 9, only with --allow-exec.

    Replay CLEAN is not a proof. LLM down is NOTRUN for that half, never CLEAN.
    Missing compiler is NOTRUN inside interpreter_loop.
    """
    ordered = sorted(fails, key=lambda f: (0 if f.status == laws.CRASH else 1, f.stage))
    out: list[Finding] = []
    n = 0
    for f0 in ordered:
        if n >= 16:
            break
        fn = next(
            (x for x in functions
             if f0.function and x.name == f0.function
             and (x.file == f0.file or Path(x.file).name == Path(f0.file).name)),
            None,
        )
        if fn is None and f0.function:
            fn = next((x for x in functions if x.name == f0.function), None)
        if not fn:
            continue
        if fn.kind in {"POINTER", "OTHER"}:
            out.append(Finding(
                stage="execute", status=laws.NEEDS_HARNESS, file=fn.file,
                function=fn.name, line=fn.line, cls="",
                message=f"{fn.kind}: cex replay would invent a buffer or object",
                strength=laws.STRENGTH_FINDS,
                extra={"oracle": "concrete-replay"},
            ))
            continue
        if _has_unencoded_float(fn):
            out.append(Finding(
                stage="execute", status=laws.NEEDS_HARNESS, file=fn.file,
                function=fn.name, line=fn.line, cls="",
                message="float/double unencoded: cex replay oracle is not an IEEE model",
                strength=laws.STRENGTH_FINDS,
                extra={"oracle": "concrete-replay"},
            ))
            continue
        args: dict[str, int] = {}
        blob = f0.counterexample or ""
        for part in blob.replace(";", ",").split(","):
            if "=" not in part:
                continue
            k, v = part.split("=", 1)
            k = k.strip().split()[-1]
            try:
                args[k] = int(str(v).strip().split()[0], 0)
            except ValueError:
                continue
        if not args:
            continue
        n += 1
        from prism.concrete import execute as cexec
        rec = cexec(fn, args)
        if rec.ub:
            out.append(Finding(
                stage="execute", status=laws.CRASH, file=fn.file,
                function=fn.name, line=fn.line, cls=rec.ub,
                message=f"cex replay trapped {rec.ub}",
                strength=laws.STRENGTH_FINDS,
                counterexample=blob[:200],
                extra={"oracle": "concrete-replay"},
            ))
        else:
            out.append(Finding(
                stage="execute", status=laws.CLEAN, file=fn.file,
                function=fn.name, line=fn.line, cls="",
                message="cex did not trap in concrete replay (not a proof)",
                strength=laws.STRENGTH_FINDS,
                extra={"oracle": "concrete-replay"},
            ))
    if llm and fails:
        if engine is None or not engine.available():
            out.append(Finding(
                stage="execute", status=laws.NOTRUN, file="", function=None, line=None,
                cls="", message=LLM_UNAVAILABLE_MSG,
                strength=laws.STRENGTH_READS,
                extra={"install": LLM_INSTALL, "half": "llm"},
            ))
        else:
            f0 = fails[0]
            prompt = (
                f"Write a C main() that demonstrates this finding is real or not.\n"
                f"{f0.file}:{f0.line} {f0.function} {f0.cls}: {f0.message}\n"
                f"counterexample: {f0.counterexample}"
            )
            out.extend(interpreter_loop(engine, prompt, rounds=2, allow_exec=allow_exec))
    if not out:
        return [Finding(
            stage="execute", status=laws.NOTRUN, file="", function=None,
            line=None, cls="", message="no FAILED/CRASH cex to replay",
            strength=laws.STRENGTH_READS,
        )]
    return out
