"""FuSeBMC loop + Fuzz4All seeds + ChatFuzz stall mutants.

COMPILE + RUN. A crash is certainty. Finding nothing is CLEAN, not a proof.
POINTER functions are not harnessed.
"""

from __future__ import annotations

from pathlib import Path
import hashlib
import os
import shutil
import struct
import subprocess
import tempfile
import time

from prism import laws
from prism.ai import LLM_INSTALL, LLM_UNAVAILABLE_MSG
from prism.bmc import unencoded_syntax_reason
from prism.concrete import decode_args, execute, interesting_seeds
from prism.cparse import body_needs_pointer_harness
from prism.models import Finding, FunctionInfo
from prism.simdmut import havoc, coverage_hash

C_TYPE_SIZE = {
    "char": 1, "signed char": 1, "unsigned char": 1,
    "short": 2, "unsigned short": 2,
    "int": 4, "unsigned": 4, "unsigned int": 4,
    "long": 8, "unsigned long": 8,
    "long long": 8, "unsigned long long": 8,
    "int8_t": 1, "uint8_t": 1,
    "int16_t": 2, "uint16_t": 2,
    "int32_t": 4, "uint32_t": 4,
    "int64_t": 8, "uint64_t": 8,
    "size_t": 8, "ssize_t": 8, "bool": 1, "_Bool": 1,
}


def param_nbytes(params: list[tuple[str, str]]) -> int:
    n = 0
    for typ, _ in params:
        key = " ".join(typ.replace("*", " ").split())
        n += C_TYPE_SIZE.get(key, 4)
    return n or 1


def harness_source(fn: FunctionInfo, src_rel: str) -> str:
    nbytes = param_nbytes(fn.params)
    reads = []
    args = []
    off = 0
    for typ, name in fn.params:
        key = " ".join(typ.replace("*", " ").split())
        sz = C_TYPE_SIZE.get(key, 4)
        t = key if key in C_TYPE_SIZE else "int"
        reads.append(f"    memcpy(&{name}, buf + {off}, {sz});")
        args.append(name)
        off += sz
    decls = "\n".join(f"    {t if False else 'int'} {name};"  # filled below
                      for typ, name in fn.params)
    # proper types
    dlines = []
    for typ, name in fn.params:
        key = " ".join(typ.split()) or "int"
        dlines.append(f"    {key} {name};")
    return f'''#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "{src_rel.replace(chr(92), "/")}"

int main(void) {{
    unsigned char buf[{nbytes}];
    if (fread(buf, 1, {nbytes}, stdin) != {nbytes}) return 0;
{chr(10).join(dlines)}
{chr(10).join(reads)}
    (void){fn.name}({", ".join(args)});
    return 0;
}}
'''


def _compile(harness: Path, out_exe: Path) -> tuple[bool, str]:
    cc = shutil.which("gcc") or shutil.which("clang") or shutil.which("cl")
    if not cc:
        return False, "no C compiler on PATH"
    cmd = [cc, "-O0", "-g", "-std=c11", str(harness), "-o", str(out_exe)]
    # Prefer ASan+UBSan together; fall back to one sanitizer, then bare.
    # TSan cannot combine with ASan. Missing sanitizer runtime is not a fake CLEAN.
    san_tries: list[list[str]] = []
    if "gcc" in Path(cc).name.lower() or "clang" in Path(cc).name.lower():
        san_tries = [
            ["-fsanitize=address,undefined", "-fno-sanitize-recover=address,undefined"],
            ["-fsanitize=undefined", "-fno-sanitize-recover=undefined"],
            ["-fsanitize=address", "-fno-sanitize-recover=address"],
        ]
    try:
        p = None
        for san in san_tries:
            p = subprocess.run([*cmd[:1], *san, *cmd[1:]], capture_output=True, text=True, timeout=30)
            if p.returncode == 0:
                break
        if p is None or p.returncode != 0:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return False, "compile timeout"
    if p.returncode != 0:
        return False, (p.stderr or p.stdout)[-1500:]
    return True, ""


def _run(exe: Path, data: bytes, timeout: float = 1.0) -> tuple[str, str]:
    try:
        p = subprocess.run(
            [str(exe)], input=data, capture_output=True, timeout=timeout
        )
    except subprocess.TimeoutExpired:
        return "timeout", ""
    if p.returncode < 0:
        return "crash", f"signal {-p.returncode}"
    if p.returncode == 0:
        return "ok", (p.stderr or b"").decode("utf-8", "replace")[-200:]
    # gcc ubsan often exits 1; ASan reports ERROR: AddressSanitizer
    err = (p.stderr or b"").decode("utf-8", "replace")
    low = err.lower()
    if (
        "runtime error" in low
        or "undefinedbehaviorsanitizer" in low
        or "addresssanitizer" in low
        or "heap-buffer-overflow" in low
        or "heap-use-after-free" in low
    ):
        return "crash", err[-800:]
    if p.returncode != 0:
        # could be the program returning non-zero; not a crash
        return "ok", err[-200:]
    return "ok", ""


def _crash_finding(base: dict, child: bytes, args: dict[str, int], cls: str,
                   extra: dict, evidence: str = "") -> Finding:
    argstr = ", ".join(f"{k}={v}" for k, v in args.items())
    rec = dict(base)
    rec["cls"] = cls
    return Finding(
        **rec, status=laws.CRASH,
        message=f"{cls} on {child[:16].hex()} {argstr}".strip(),
        evidence=evidence[:800],
        counterexample=f"{child.hex()} {argstr}".strip(),
        extra={**extra, "args": args, "oracle": "concrete"},
    )


def _concrete_ub(fn: FunctionInfo, child: bytes) -> tuple[str | None, dict[str, int]]:
    """Decode little-endian params and run the UB oracle. POINTER is skipped."""
    if fn.kind == "POINTER":
        return None, {}
    args = decode_args(fn, child)
    rec = execute(fn, args)
    return rec.ub, args


def fuzz_function(
    fn: FunctionInfo,
    src: Path,
    *,
    budget: float,
    iters: int,
    seeds: list[bytes] | None = None,
    work: Path | None = None,
) -> Finding:
    base = dict(stage="fuzz", file=fn.file, function=fn.name, line=fn.line,
                cls="", strength=laws.STRENGTH_FINDS)
    if fn.kind == "POINTER":
        return Finding(**base, status=laws.NEEDS_HARNESS,
                       message="POINTER: no honest fuzzer harness (would invent a buffer or pass NULL)")
    if fn.kind == "OTHER":
        return Finding(**base, status=laws.NEEDS_HARNESS,
                       message="OTHER signature, not harnessed")
    syn = unencoded_syntax_reason(fn, "fuzzer")
    if syn:
        return Finding(**base, status=laws.NEEDS_HARNESS, message=syn)
    if body_needs_pointer_harness(fn.body or ""):
        return Finding(
            **base, status=laws.NEEDS_HARNESS,
            message="local pointer or heap object: fuzzer would invent a buffer",
        )

    nbytes = param_nbytes(fn.params)
    corpus: list[bytes] = []
    for s in interesting_seeds(fn):
        corpus.append(s[:nbytes].ljust(nbytes, b"\x00"))
    for s in seeds or []:
        corpus.append(s[:nbytes].ljust(nbytes, b"\x00"))
    if not corpus:
        corpus.append(os.urandom(nbytes))
    noseed = not seeds

    t0 = time.time()
    seen: set[int] = set()
    new_cov = 0
    stall = 0
    i = 0

    def note_cov(child: bytes, detail: str = "") -> None:
        nonlocal new_cov, stall
        h = coverage_hash(child + detail.encode("utf-8", "replace"))
        if h not in seen:
            seen.add(h)
            corpus.append(child)
            new_cov += 1
            stall = 0
        else:
            stall += 1

    # Concrete oracle FIRST — host gcc (Strawberry) often has no UBSan.
    # Dictionary seeds always run (small); they are what crash the planted bugs.
    queue = list(corpus)
    while queue and (time.time() - t0) < max(budget, 1.0):
        child = queue.pop(0)
        i += 1
        ub, args = _concrete_ub(fn, child)
        extra = {"iters": i, "corpus": len(corpus), "new_cov": new_cov,
                 "noseed": noseed, "stall": stall}
        if ub:
            return _crash_finding(base, child, args, ub, extra)
        note_cov(child)

    while i < iters and (time.time() - t0) < budget:
        parent = corpus[i % len(corpus)]
        child = havoc(parent)
        i += 1
        ub, args = _concrete_ub(fn, child)
        extra = {"iters": i, "corpus": len(corpus), "new_cov": new_cov,
                 "noseed": noseed, "stall": stall}
        if ub:
            return _crash_finding(base, child, args, ub, extra)
        note_cov(child)

    extra = {"iters": i, "corpus": len(corpus), "new_cov": new_cov,
             "noseed": noseed, "stall": stall, "oracle": "concrete"}

    # Still compile+run when gcc/clang actually works (sanitizers, SIGSEGV).
    bin_crash = _binary_fuzz(fn, src, corpus, nbytes, budget=max(0.05, budget - (time.time() - t0)),
                             iters=min(32, max(1, iters)), work=work)
    extra.update(bin_crash.get("extra") or {})
    if bin_crash.get("crash"):
        child, detail = bin_crash["crash"]
        args = decode_args(fn, child)
        rec = dict(base)
        rec["cls"] = "FUZZ-CRASH"
        return Finding(
            **rec, status=laws.CRASH,
            message=f"crash on {child[:16].hex()}…",
            evidence=detail[:800],
            counterexample=child.hex(),
            extra={**extra, "args": args, "oracle": "binary"},
        )

    return Finding(
        **base, status=laws.CLEAN,
        message=f"no crash in {i} iters / {budget:.1f}s (not a proof)",
        extra=extra,
    )


def _binary_fuzz(
    fn: FunctionInfo,
    src: Path,
    corpus: list[bytes],
    nbytes: int,
    *,
    budget: float,
    iters: int,
    work: Path | None,
) -> dict:
    """Optional compile+run. Failures are silent: concrete already decided."""
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        return {}
    work = work or Path(tempfile.mkdtemp(prefix="prism_fuzz_"))
    work.mkdir(parents=True, exist_ok=True)
    src_copy = work / src.name
    if not src_copy.exists():
        try:
            src_copy.write_text(src.read_text(encoding="utf-8", errors="replace"), encoding="utf-8")
        except OSError:
            return {}
    hpath = work / f"harness_{fn.name}.c"
    hpath.write_text(harness_source(fn, src.name), encoding="utf-8")
    exe = work / f"harness_{fn.name}.exe"
    ok, _err = _compile(hpath, exe)
    if not ok:
        return {"extra": {"binary": "compile-failed"}}
    t0 = time.time()
    n = 0
    for child in corpus[: max(1, iters)]:
        if (time.time() - t0) > budget:
            break
        n += 1
        st, detail = _run(exe, child)
        if st == "crash":
            return {"crash": (child, detail), "extra": {"binary_iters": n}}
    while n < iters and (time.time() - t0) < budget:
        child = havoc(corpus[n % len(corpus)])
        n += 1
        st, detail = _run(exe, child)
        if st == "crash":
            return {"crash": (child, detail), "extra": {"binary_iters": n}}
    return {"extra": {"binary_iters": n}}


def run_fuzz(
    functions: list[FunctionInfo],
    root: Path,
    *,
    budget: float,
    iters: int,
    seeds_by_fn: dict[str, list[bytes]] | None = None,
    engine=None,
) -> list[Finding]:
    seeds_by_fn = seeds_by_fn or {}
    out = []
    llm_up = False
    if engine is not None:
        av = getattr(engine, "available", None)
        if av is None:
            llm_up = True
        else:
            llm_up = bool(av() if callable(av) else av)
    for fn in functions:
        src = root / fn.file if not Path(fn.file).is_absolute() else Path(fn.file)
        if not src.exists() and root.is_file():
            src = root
        try:
            rec = fuzz_function(
                fn, src, budget=budget, iters=iters,
                seeds=seeds_by_fn.get(fn.name),
            )
        except Exception as ex:
            rec = Finding(
                stage="fuzz", status=laws.ERROR, file=fn.file, function=fn.name,
                line=fn.line, cls="", message=str(ex), strength=laws.STRENGTH_FINDS,
            )
        if llm_up and rec.status == laws.CLEAN:
            rec = _fuzz4all_mutate_on_interesting(engine, fn, rec)
        out.append(rec)
    if engine is not None and not llm_up and functions:
        fn0 = functions[0]
        out.append(Finding(
            stage="fuzz", status=laws.NOTRUN, file=fn0.file, function=fn0.name,
            line=fn0.line, cls="", message=LLM_UNAVAILABLE_MSG,
            strength=laws.STRENGTH_READS,
            extra={"autoprompt": "NOTRUN", "install": LLM_INSTALL},
        ))
    return out


def _fuzz4all_mutate_on_interesting(engine, fn: FunctionInfo, rec: Finding) -> Finding:
    """Fuzz4All m_prompt/c_prompt on an interesting seed. LLM stays HYPOTHESIS/READS."""
    extra = dict(rec.extra or {})
    try:
        from prism.agent import fuzz4all_combine, fuzz4all_mutate_interesting
        n = param_nbytes(fn.params)
        parent = interesting_seeds(fn)
        seed = (parent[0] if parent else b"\x00" * n)[:n].ljust(n, b"\x00")
        extra["fuzz4all_mutate"] = True
        mutants = fuzz4all_mutate_interesting(engine, fn, seed.hex()) or []
        if len(parent) >= 2:
            extra["fuzz4all_combine"] = True
            mutants.extend(
                fuzz4all_combine(
                    engine, fn, seed.hex(), parent[1][:n].ljust(n, b"\x00").hex(),
                ) or []
            )
        extra["fuzz4all_mutants"] = [m.hex() for m in mutants[:8]]
    except Exception as ex:
        extra["fuzz4all_mutate_error"] = str(ex)[:200]
    rec.extra = extra
    return rec


def bytes_from_cex(cex: str, nbytes: int) -> bytes | None:
    """Turn 'x=5, y=-1' into little-endian arg bytes."""
    if not cex:
        return None
    vals = []
    for part in cex.split(","):
        if "=" not in part:
            continue
        _, v = part.split("=", 1)
        try:
            vals.append(int(str(v).strip(), 0))
        except ValueError:
            return None
    raw = b""
    for v in vals:
        raw += struct.pack("<I", v & 0xFFFFFFFF)
    return raw[:nbytes].ljust(nbytes, b"\x00")
