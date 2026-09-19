"""Dafny-shaped contracts, RLEF repair, OpenCodeInterpreter execute loop."""

from __future__ import annotations

from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

from helix import laws
from helix.ai import (
    SYSTEM_AUDITOR,
    SYSTEM_CHATFUZZ,
    SYSTEM_DAFNY,
    SYSTEM_FUZZ4ALL,
    SYSTEM_HARNESS,
    SYSTEM_REPAIR,
    LlamaEngine,
    extract_json,
)
from helix.models import Finding, FunctionInfo


def hypothesize(engine: LlamaEngine, functions: list[FunctionInfo], budget: int = 4) -> list[Finding]:
    if not engine.available():
        return [Finding(
            stage="llm", status=laws.NOTRUN, file="", function=None, line=None,
            cls="INTENT", message="llama.cpp/Ollama not reachable",
            strength=laws.STRENGTH_READS,
            extra={"install": "ollama serve  (qwen3.5:9b) or build native/llama.cpp"},
        )]
    out: list[Finding] = []
    for fn in functions[:budget]:
        src = f"{fn.signature}\n{{{fn.body}\n}}"
        r = engine.complete([
            {"role": "system", "content": SYSTEM_AUDITOR},
            {"role": "user", "content": src[:6000]},
        ])
        if r.error:
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
        data = extract_json(r.text or "") if not r.error else None
        out.append(Finding(
            stage="contracts", status=laws.HYPOTHESIS if data else (laws.ERROR if r.error else laws.HYPOTHESIS),
            file=fn.file, function=fn.name, line=fn.line, cls="FUNC-CONTRACT",
            message="Dafny-style spec (hypothesis, not proved)",
            strength=laws.STRENGTH_READS,
            extra={"spec": data, "raw": (r.text or r.error or "")[:800], "backend": r.backend},
        ))
    return out


def fuzz4all_seeds(engine: LlamaEngine, fn: FunctionInfo, nbytes: int) -> list[bytes]:
    if not engine.available():
        return []
    r = engine.complete([
        {"role": "system", "content": SYSTEM_FUZZ4ALL},
        {"role": "user", "content": f"nbytes={nbytes}\n{fn.signature}\n{{{fn.body}\n}}"},
    ], timeout=90)
    data = extract_json(r.text or "") if not r.error else None
    seeds: list[bytes] = []
    if isinstance(data, dict):
        for h in data.get("seeds") or []:
            try:
                raw = bytes.fromhex(str(h).strip().replace("0x", ""))
                seeds.append(raw)
            except ValueError:
                continue
    return seeds


def chatfuzz_mutants(engine: LlamaEngine, fn: FunctionInfo, seed_hex: str) -> list[bytes]:
    if not engine.available():
        return []
    r = engine.complete([
        {"role": "system", "content": SYSTEM_CHATFUZZ},
        {"role": "user", "content": f"seed={seed_hex}\n{fn.signature}\n{{{fn.body}\n}}"},
    ], timeout=90)
    data = extract_json(r.text or "") if not r.error else None
    out = []
    if isinstance(data, dict):
        for h in data.get("mutants") or []:
            try:
                out.append(bytes.fromhex(str(h).replace("0x", "")))
            except ValueError:
                continue
    return out


def find_cc() -> str | None:
    return shutil.which("gcc") or shutil.which("clang")


def sandbox_run(source: str, timeout: float = 8.0) -> dict:
    """OpenCodeInterpreter execute step: real gcc/clang compile+run in a tempdir."""
    cc = find_cc()
    if not cc:
        return {"ok": False, "error": "no compiler", "stdout": "", "stderr": "", "code": None}
    suffix = ".exe" if sys.platform == "win32" else ""
    with tempfile.TemporaryDirectory(prefix="helix_oci_") as td:
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
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired as ex:
            out = (ex.stdout or "") if isinstance(ex.stdout, str) else ""
            err = (ex.stderr or "") if isinstance(ex.stderr, str) else ""
            return {
                "ok": False, "error": "timeout",
                "stdout": out[-2000:], "stderr": err[-2000:], "code": None,
            }
        return {
            "ok": r.returncode == 0,
            "error": None if r.returncode == 0 else "exit",
            "stdout": (r.stdout or "")[-2000:],
            "stderr": (r.stderr or "")[-2000:],
            "code": r.returncode,
        }


def interpreter_loop(engine: LlamaEngine, prompt: str, rounds: int = 3) -> list[Finding]:
    if not engine.available():
        return [Finding(
            stage="execute", status=laws.NOTRUN, file="", function=None, line=None,
            cls="", message="llama.cpp/Ollama not reachable",
            strength=laws.STRENGTH_READS,
        )]
    if not find_cc():
        return [Finding(
            stage="execute", status=laws.NOTRUN, file="", function=None, line=None,
            cls="", message="gcc/clang not on PATH",
            strength=laws.STRENGTH_FINDS,
            extra={"install": "install gcc or clang"},
        )]
    messages = [
        {"role": "system", "content": SYSTEM_HARNESS},
        {"role": "user", "content": prompt},
    ]
    last_src = ""
    for i in range(rounds):
        r = engine.complete(messages, timeout=120)
        if r.error:
            return [Finding(
                stage="execute", status=laws.ERROR, file="", function=None, line=None,
                cls="", message=r.error, strength=laws.STRENGTH_READS,
            )]
        src = r.text.strip()
        if src.startswith("```"):
            src = src.strip("`")
            src = src.split("\n", 1)[-1]
            if "```" in src:
                src = src[: src.rfind("```")]
        last_src = src
        result = sandbox_run(src)
        if result["ok"]:
            return [Finding(
                stage="execute", status=laws.CLEAN, file="", function=None, line=None,
                cls="", message=f"interpreter harness passed on round {i+1}",
                strength=laws.STRENGTH_FINDS,
                extra={"stdout": result["stdout"][-400:], "rounds": i + 1},
            )]
        messages.append({"role": "assistant", "content": r.text})
        messages.append({
            "role": "user",
            "content": f"execution failed: {json.dumps(result)[:1500]}. Fix the C file.",
        })
    return [Finding(
        stage="execute", status=laws.FAILED, file="", function=None, line=None,
        cls="", message=f"interpreter loop exhausted ({rounds} rounds)",
        strength=laws.STRENGTH_FINDS, extra={"last_src": last_src[:500]},
    )]


def rlef_repair(engine: LlamaEngine, source: str, feedback: str, rounds: int) -> list[Finding]:
    """Execution feedback as the reward. Keep the best candidate."""
    if not engine.available():
        return [Finding(
            stage="repair", status=laws.NOTRUN, file="", function=None, line=None,
            cls="", message="llama.cpp/Ollama not reachable",
            strength=laws.STRENGTH_READS,
        )]
    best_score = -1
    best_src = source
    messages = [
        {"role": "system", "content": SYSTEM_REPAIR},
        {"role": "user", "content": f"SOURCE:\n{source}\n\nFEEDBACK:\n{feedback}"},
    ]
    history = []
    for i in range(rounds):
        r = engine.complete(messages, timeout=120)
        if r.error:
            history.append(r.error)
            break
        src = r.text
        if "```" in src:
            src = src.split("```")[1]
            src = src.split("\n", 1)[-1]
        result = sandbox_run(src)
        score = 0
        if result.get("error") != "compile":
            score += 1
        if result["ok"]:
            score += 2
        history.append({"round": i + 1, "score": score, "error": result.get("error")})
        if score > best_score:
            best_score = score
            best_src = src
        if result["ok"]:
            break
        messages.append({"role": "assistant", "content": r.text})
        messages.append({"role": "user", "content": json.dumps(result)[:1500]})
    st = laws.CLEAN if best_score >= 3 else laws.HYPOTHESIS
    return [Finding(
        stage="repair", status=st, file="", function=None, line=None, cls="",
        message=f"RLEF best score {best_score} over {len(history)} rounds",
        strength=laws.STRENGTH_READS,
        extra={"history": history, "best": best_src[:1000]},
    )]
