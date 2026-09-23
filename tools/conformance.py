#!/usr/bin/env python3
"""PRISM conformance suite runner (roadmap 2.7) and release gate (roadmap 6.1).

Runs PRISM on tests/conformance/ (in-house tasks, a pinned SV-COMP subset and,
optionally, the NIST Juliet C/C++ subsets fetched with --fetch-juliet) and
computes, per verdict stage:

  SOUNDNESS     wrong proofs: a PROVED / PROVED-UNBOUNDED / PROVED-ASSUMING /
                PROVED-CERTIFIED verdict on a function whose task says a
                violation exists. Must be 0; any wrong proof exits 1.
  COMPLETENESS  share of `true` functions proved.
  DETECTION     share of `false` functions refuted with FAILED and a
                counterexample that replays: the task is compiled with
                -fsanitize=undefined,address and run on the counterexample
                inputs (inside bwrap when available); the sanitizer must fire.
  FALSE ALARMS  FAILED on a `true` function.

With --certified the pir stage is run a second time with `prism --certified`
and scored as the verdict stage `pir-certified` (roadmap 3.2). The report then
also says how many loop-free `true` functions became PROVED-CERTIFIED (the
roadmap 3 exit criterion: all of them) and why the others did not.

Task format (tests/conformance/prism/**.yml, one sidecar per source file):

  format_version: 1
  input_files: add_false.c
  language: C            # or C++
  property: no-overflow  # no-overflow | no-div0 | no-shift-ub | no-oob |
                         # no-null-deref | memsafety
                         # concurrency/: norace | noassert | nodeadlock
  expected:              # function -> true (no UB for any input) | false
    add_false: false
  witness:               # false functions: inputs that trigger the UB
    add_false: [2147483647, 1]
  expect_status:         # optional: the exact status the laws demand
    f: NEEDS-HARNESS     # (pointer parameters, Law 6)

SV-COMP task definitions (format 2.0 .yml with `properties:`) are read as-is;
the `no-overflow` / `valid-memsafety` verdict applies to `main`.

Concurrency tasks (tests/conformance/concurrency, roadmap 2.6) are whole
programs that create threads; `expected: {main: ...}` is scored by the conc
stage only. conc never proves (a context-switch bound is not a proof), so
its soundness line is zero wrong proofs by construction; what it measures is
detection (FAILED in the task's property class) and false alarms. Its
counterexamples are schedules, not inputs, so they are "refuted, not
replayed".

`--self-check` validates the suite's own labels without PRISM: every
witness must trip a sanitizer, and every `true` function must survive an
edge-value grid plus random inputs under the sanitizers.

This executes task code: only the trusted suite in this repository (or the
pinned Juliet archive, whose hash is checked) is ever compiled and run.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:  # pragma: no cover - CI installs PyYAML
    yaml = None

REPO = Path(__file__).resolve().parents[1]
SUITE = REPO / "tests" / "conformance"

PROOF = {"PROVED", "PROVED-UNBOUNDED", "PROVED-ASSUMING", "PROVED-CERTIFIED"}
BASE_STAGES = ["inventory", "classify", "bmc", "harness"]
VERDICT_STAGES = ["bmc", "harness", "pir", "conc"]
# Stages that only speak about part of the functions (harness: POINTER
# functions under `// requires:`). A function they do not mention is out of
# scope, not silently skipped; bmc and pir must report every function.
SCOPED_STAGES = {"harness"}
# The concurrency stage (roadmap 2.6) is scored only on the thread programs
# of tests/conformance/concurrency, and those only by it: the other stages
# check single functions, conc checks whole programs that create threads.
STAGE_TASK_ORIGINS = {"conc": {"concurrency"}}
ORIGIN_STAGES = {"concurrency": {"conc"}}
# Suite directories run by default (tests/conformance/<dir>).
DEFAULT_ROOTS = ("prism", "sv-comp", "concurrency", "esbmc-cpp", "libc-models")
# Task origins whose labels speak for one property only (another class of
# FAILED is reported separately, not as a false alarm).
PROPERTY_SCOPED = {"sv-comp", "concurrency"}
# --certified: the pir stage again with `prism --certified`, scored under this name.
CERT_STAGE = "pir-certified"
# Finding extras kept in results.json (pir: loop count and certificate fields).
KEEP_EXTRA = ("loops", "properties", "certificate", "certificate_vcs", "certify_note", "certified_mode", "solver",
             "certificate_bitblast")

# SV-COMP property file -> suite property
SV_PROPERTIES = {"no-overflow.prp": "no-overflow", "valid-memsafety.prp": "memsafety"}
# Which PRISM classes witness which property (for property-scoped tasks).
PROPERTY_CLASSES = {
    "no-overflow": {"INT-SIGNED-OVF"},
    "no-div0": {"INT-DIV-ZERO"},
    "no-shift-ub": {"INT-SHIFT-UB"},
    "no-oob": {"MEM-OOB-READ", "MEM-OOB-WRITE", "MEM-PTR-ARITH"},
    "no-null-deref": {"MEM-NULL-DEREF", "NULL-DEREF", "PTR-NULL-DEREF"},
    # pir memory model classes (docs/PIR.md "Memory model")
    "memsafety": {"MEM-OOB-READ", "MEM-OOB-WRITE", "MEM-NULL-DEREF", "NULL-DEREF", "MEM-USE-AFTER-FREE",
                  "PTR-NULL-DEREF", "PTR-INVALID-DEREF", "MEM-UAF", "MEM-DOUBLE-FREE", "MEM-INVALID-FREE",
                  "MEM-MISMATCHED-FREE", "MEM-PTR-ARITH", "MEM-OVERLAP", "MEM-VLA-SIZE", "MEM-STACK-ESCAPE",
                  "MEM-MISALIGNED", "MEM-WRITE-CONST", "PTR-COMPARE", "UNINIT-READ"},
    # concurrency tasks (whole programs; the verdict applies to main)
    "norace": {"CONC-DATA-RACE"},
    "noassert": {"FUNC-CONTRACT"},
    "nodeadlock": {"CONC-DEADLOCK"},
}

# NIST SARD Juliet C/C++ 1.3 (kept out of git; fetched and hash-checked).
JULIET_URL = (
    "https://samate.nist.gov/SARD/downloads/test-suites/"
    "2017-10-01-juliet-test-suite-for-c-cplusplus-v1-3.zip"
)
JULIET_SHA256 = "ada9d7e1c323d283446df3f55bdee0d00bda1fed786785fe98764d58688f38eb"
JULIET_SIZE = 152957342
JULIET_CWES = ("CWE190", "CWE191", "CWE369", "CWE476", "CWE680")
# Only data types whose overflow is undefined behaviour: unsigned wrap and
# char/short arithmetic (done in int) are not UB, so Juliet's CWE190/191
# variants on those types are not `false` tasks for a UB checker.
JULIET_TYPE = {
    "CWE190": re.compile(r"__(int|int64_t)_"),
    "CWE191": re.compile(r"__(int|int64_t)_"),
    "CWE369": re.compile(r"__int_"),
    "CWE476": re.compile(r"__(int|long|int64_t|char|struct)_\d\d"),
    "CWE680": re.compile(r"__"),
}
JULIET_PROPERTY = {
    "CWE190": "no-overflow",
    "CWE191": "no-overflow",
    "CWE369": "no-div0",
    "CWE476": "no-null-deref",
    "CWE680": "memsafety",
}

# ESBMC C++ regression tests (kept out of git; fetched at a pinned commit with
# --fetch-esbmc; a curated subset is committed under tests/conformance/esbmc-cpp).
ESBMC_REPO = "https://github.com/esbmc/esbmc.git"
ESBMC_COMMIT = "653926f91580d8d67858d42db814f09d9a9fa257"
ESBMC_DIRS = tuple(f"regression/esbmc-{d}" for d in ("cpp", "cpp11", "cpp14", "cpp17", "cpp20", "cpp23"))
ESBMC_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hh", ".hpp", ".hxx", ".inc", ".tcc"}
# Options that change what "VERIFICATION SUCCESSFUL/FAILED" speaks about.
ESBMC_SKIP_OPTIONS = (
    "--no-assertions", "--no-bounds-check", "--no-pointer-check", "--no-div-by-zero-check",
    "--no-align-check", "--no-pointer-relation-check", "--memory-leak-check", "--overflow-check",
    "--unsigned-overflow-check", "--ub-shift-check", "--nan-check", "--data-races-check",
    "--deadlock-check", "--function", "--cheri", "--struct-fields-check", "--is-instr-modelling",
)
# FAILED verdicts that come from a bound of ESBMC's C++ operational model
# (its fixed-capacity string/stream models), not from the program.
ESBMC_MODEL_FAILURE = re.compile(r"capacity exceed|forgotten memory|memory leak", re.I)
# Labels that contradict the C++ standard (and a native sanitizer run of the
# deterministic program): skipped at conversion, each with the reason.
ESBMC_DISPUTED = {
    "esbmc-cpp/cpp/github_6199_fail": "std::string(nullptr, 0): [nullptr, nullptr + 0) is a valid empty range "
                                       "([string.cons]); ESBMC's string model checks null first",
    "esbmc-cpp/cpp/github_6588_multidim_fail": "new int[2][3]() value-initialises to zero ([expr.new], "
                                               "[dcl.init]); the assert holds",
}
# Programs whose single run is not their only behaviour.
ESBMC_NONDET = re.compile(r"\bnondet_|__VERIFIER_nondet|\brand\s*\(|\bcin\b|\bscanf\b|\bgetchar\b|"
                          r"\bfgets\b|\bargv\b|\btime\s*\(|\bstd::random_device|\bthread\b|pthread_")
# ESBMC's default property set: assertions, bounds, pointer safety, division
# by zero (signed overflow, shifts and uninitialised reads are not checked
# without extra options, so a PRISM FAILED of those classes on a SUCCESSFUL
# task is "other property"). An uncaught exception / violated noexcept
# aborts the program and ESBMC reports it.
ESBMC_CLASSES = {"FUNC-CONTRACT", "INT-DIV-ZERO", "CXX-UNREACHABLE", "CXX-THROW-NOEXCEPT", "CXX-OPTIONAL-NULL",
                 "CXX-VECTOR-INDEX", "CXX-ARRAY-INDEX", "CXX-DEQUE-INDEX", "CXX-BITSET-INDEX",
                 "MEM-OOB-READ", "MEM-OOB-WRITE", "MEM-NULL-DEREF", "NULL-DEREF", "MEM-USE-AFTER-FREE",
                 "PTR-NULL-DEREF", "PTR-INVALID-DEREF", "MEM-UAF", "MEM-DOUBLE-FREE", "MEM-INVALID-FREE",
                 "MEM-MISMATCHED-FREE", "MEM-PTR-ARITH", "MEM-STACK-ESCAPE"}
PROPERTY_CLASSES["esbmc-cpp"] = ESBMC_CLASSES
PROPERTY_SCOPED.add("esbmc-cpp")

SAN_FLAGS = ["-g", "-O0", "-w", "-fsanitize=undefined,address", "-fno-sanitize-recover=all"]
SAN_ENV = {
    "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=0:halt_on_error=1",
    "UBSAN_OPTIONS": "print_stacktrace=0:halt_on_error=1",
}


# --------------------------------------------------------------------------- tasks


@dataclass
class Task:
    ident: str  # path of the sidecar relative to the suite root
    yml: Path
    source: Path
    origin: str  # prism | sv-comp | juliet
    category: str
    lang: str
    prop: str
    expected: dict[str, bool]
    witness: dict[str, list[int]] = field(default_factory=dict)
    expect_status: dict[str, str] = field(default_factory=dict)
    sanitizer_blind: set[str] = field(default_factory=set)
    data_model: str = ""
    std: str = ""  # whole-program tasks (esbmc-cpp): language standard of the native self-check
    deterministic: bool = False  # whole program whose one run is its only behaviour
    unwind: int = 0  # per-task --unwind (0: the run's default)
    expect_class: dict[str, set[str]] = field(default_factory=dict)  # false fn -> classes that refute it


def _load_yaml(path: Path) -> Any:
    text = path.read_text(encoding="utf-8")
    if yaml is not None:
        return yaml.safe_load(text)
    return json.loads(text)  # JSON is YAML; lets the loader run without PyYAML


def _as_bool(v: Any) -> bool:
    if isinstance(v, bool):
        return v
    return str(v).strip().lower() == "true"


def load_task(yml: Path, root: Path) -> Task | None:
    data = _load_yaml(yml)
    if not isinstance(data, dict) or "input_files" not in data:
        return None
    inp = data["input_files"]
    if isinstance(inp, list):
        inp = inp[0]
    source = yml.parent / str(inp).strip("'\"")
    rel = yml.relative_to(root)
    origin = rel.parts[0] if len(rel.parts) > 1 else "prism"
    category = rel.parts[1] if len(rel.parts) > 2 else origin
    if "expected" in data:
        exp = {str(k): _as_bool(v) for k, v in (data.get("expected") or {}).items()}
        return Task(
            ident=str(rel),
            yml=yml,
            source=source,
            origin=origin,
            category=category,
            lang=str(data.get("language", "C")),
            prop=str(data.get("property", "")),
            expected=exp,
            witness={str(k): [int(x) for x in v] for k, v in (data.get("witness") or {}).items()},
            expect_status={str(k): str(v) for k, v in (data.get("expect_status") or {}).items()},
            sanitizer_blind={str(x) for x in (data.get("sanitizer_blind") or [])},
            std=str(data.get("std", "")),
            deterministic=_as_bool(data.get("deterministic", False)),
            unwind=int(data.get("unwind", 0) or 0),
            expect_class={str(k): set(str(v).split("|")) for k, v in (data.get("expect_class") or {}).items()},
        )
    # SV-COMP task-definition format 2.0
    props = data.get("properties") or []
    for p in props:
        pf = Path(str(p.get("property_file", ""))).name
        if pf in SV_PROPERTIES and "expected_verdict" in p:
            opts = data.get("options") or {}
            return Task(
                ident=str(rel),
                yml=yml,
                source=source,
                origin=origin,
                category=category,
                lang=str(opts.get("language", "C")),
                prop=SV_PROPERTIES[pf],
                expected={"main": _as_bool(p["expected_verdict"])},
                data_model=str(opts.get("data_model", "")),
            )
    return None


def discover(roots: list[Path]) -> list[Task]:
    tasks: list[Task] = []
    for root in roots:
        base = SUITE if root.is_relative_to(SUITE) and root != SUITE else root.parent
        if root == SUITE:
            base = SUITE
        for yml in sorted(root.rglob("*.yml")):
            t = load_task(yml, base)
            if t is not None and t.source.exists():
                tasks.append(t)
    return tasks


# --------------------------------------------------------------------------- signatures / drivers

INT_TYPES: dict[str, tuple[int, int]] = {}
for _names, _bits, _signed in [
    (("int", "signed", "signed int", "int32_t"), 32, True),
    (("unsigned", "unsigned int", "uint32_t"), 32, False),
    (("long", "long int", "signed long", "long long", "long long int", "int64_t", "ssize_t"), 64, True),
    (("unsigned long", "unsigned long int", "unsigned long long", "uint64_t", "size_t", "std::size_t"), 64, False),
    (("short", "short int", "signed short", "int16_t"), 16, True),
    (("unsigned short", "uint16_t"), 16, False),
    (("char", "signed char", "int8_t"), 8, True),
    (("unsigned char", "uint8_t"), 8, False),
    (("bool", "_Bool"), 1, False),
]:
    for _n in _names:
        INT_TYPES[_n] = (-(1 << (_bits - 1)) if _signed else 0, (1 << (_bits - 1)) - 1 if _signed else (1 << _bits) - 1)


def _norm_type(t: str) -> str:
    """Value type of a parameter: qualifiers dropped, `T&` read as `T`."""
    t = re.sub(r"\b(const|volatile|register|static|inline|constexpr|extern)\b", " ", t)
    t = t.replace("&", " ")
    return " ".join(t.split())


def type_range(t: str) -> tuple[int, int] | None:
    t = _norm_type(t)
    if t in INT_TYPES:
        return INT_TYPES[t]
    m = re.fullmatch(r"(unsigned\s+)?_BitInt\((\d+)\)", t)
    if m:
        bits = int(m.group(2))
        return (0, (1 << bits) - 1) if m.group(1) else (-(1 << (bits - 1)), (1 << (bits - 1)) - 1)
    return None


def find_signature(text: str, fn: str) -> tuple[str, list[tuple[str, str]]] | None:
    """(return type, [(param type, param name)]) of fn's definition, or None."""
    m = re.search(
        r"(?m)^[ \t]*(?:\[\[[^\]]*\]\]\s*)*(?P<ret>[\w:<>\s\*&]*?)\b" + re.escape(fn)
        + r"\s*\((?P<params>[^()]*)\)\s*(?:->\s*[\w:]+\s*)?(?:noexcept\s*)?\{",
        text,
    )
    if not m:
        return None
    params: list[tuple[str, str]] = []
    raw = m.group("params").strip()
    if raw and raw != "void":
        for p in raw.split(","):
            p = p.strip()
            pm = re.fullmatch(r"(?P<t>.*?[\s\*&])(?P<n>\w+)", p)
            if not pm:
                return None
            params.append((pm.group("t").strip(), pm.group("n")))
    return m.group("ret").strip(), params


def c_literal(v: int, typ: str) -> str:
    if v == -(1 << 63):
        lit = "(-9223372036854775807LL - 1)"
    elif v < 0:
        lit = f"({v}LL)"
    elif v > (1 << 63) - 1:
        lit = f"{v}ULL"
    else:
        lit = f"{v}LL"
    return f"({_norm_type(typ)}){lit}"


def scalar_params(task: Task, fn: str) -> list[tuple[str, str]] | None:
    sig = find_signature(task.source.read_text(encoding="utf-8", errors="replace"), fn)
    if sig is None:
        return None
    params = sig[1]
    for t, _ in params:
        if type_range(t) is None:
            return None
    return params


def driver_source(task: Task, fn: str, params: list[tuple[str, str]], rows: list[list[int]]) -> str:
    inc = json.dumps(str(task.source.resolve()))
    lines = [f"#include {inc}", "#include <stddef.h>"]
    if params:
        fields = " ".join(f"{_norm_type(t)} p{i};" for i, (t, _) in enumerate(params))
        lines.append(f"static struct {{ {fields} }} prism_inputs[] = {{")
        for row in rows:
            vals = ", ".join(c_literal(v, t) for v, (t, _) in zip(row, params))
            lines.append(f"    {{ {vals} }},")
        lines.append("};")
        call_args = ", ".join(f"prism_inputs[k].p{i}" for i in range(len(params)))
        lines += [
            "int main(void) {",
            "    for (size_t k = 0; k < sizeof prism_inputs / sizeof prism_inputs[0]; ++k)",
            f"        (void){fn}({call_args});",
            "    return 0;",
            "}",
        ]
    else:
        lines += ["int main(void) {", f"    (void){fn}();", "    return 0;", "}"]
    return "\n".join(lines) + "\n"


_BWRAP: bool | None = None


def bwrap_ok() -> bool:
    global _BWRAP
    if _BWRAP is None:
        exe = shutil.which("bwrap")
        _BWRAP = False
        if exe:
            try:
                r = subprocess.run(
                    [exe, "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc",
                     "--unshare-all", "--die-with-parent", "true"],
                    capture_output=True, timeout=20,
                )
                _BWRAP = r.returncode == 0
            except (OSError, subprocess.TimeoutExpired):
                _BWRAP = False
    return _BWRAP


_SAN_CC: tuple[str, str] | None = None


def sanitizer_compilers() -> tuple[str, str]:
    """(C compiler, C++ compiler) that can link UBSan+ASan: clang, else gcc."""
    global _SAN_CC
    if _SAN_CC is None:
        env = (os.environ.get("CC_SAN"), os.environ.get("CXX_SAN"))
        if env[0] and env[1]:
            _SAN_CC = (env[0], env[1])
            return _SAN_CC
        _SAN_CC = ("clang", "clang++")
        for cc, cxx in (("clang", "clang++"), ("gcc", "g++")):
            if not shutil.which(cc):
                continue
            with tempfile.TemporaryDirectory() as d:
                src = Path(d) / "probe.c"
                src.write_text("int main(void) { return 0; }\n")
                r = subprocess.run([cc, str(src), *SAN_FLAGS, "-o", str(Path(d) / "probe")],
                                   capture_output=True, timeout=120)
                if r.returncode == 0:
                    _SAN_CC = (cc, cxx)
                    break
    return _SAN_CC


def compiler_for(lang: str) -> list[str]:
    cc, cxx = sanitizer_compilers()
    if lang.upper() in {"C++", "CXX", "CPP"}:
        return [cxx, "-std=c++23", "-x", "c++"]
    return [cc, "-std=c2x", "-x", "c"]


@dataclass
class Exec:
    outcome: str  # ub | clean | compile-error | timeout | crash | unsupported
    detail: str = ""


def run_sanitized(task: Task, fn: str, params: list[tuple[str, str]], rows: list[list[int]],
                  work: Path, timeout: float = 60.0) -> Exec:
    work.mkdir(parents=True, exist_ok=True)
    drv = work / ("driver.cpp" if task.lang.upper().startswith("C+") else "driver.c")
    drv.write_text(driver_source(task, fn, params, rows), encoding="utf-8")
    exe = work / "driver.bin"
    cc = compiler_for(task.lang) + [str(drv), "-x", "none", *SAN_FLAGS, "-o", str(exe)]
    if not task.lang.upper().startswith("C+"):
        cc.append("-lm")
    try:
        r = subprocess.run(cc, capture_output=True, text=True, timeout=180)
    except (OSError, subprocess.TimeoutExpired) as e:
        return Exec("compile-error", str(e))
    if r.returncode != 0:
        return Exec("compile-error", (r.stderr or r.stdout)[-800:])
    cmd = [str(exe)]
    if bwrap_ok():
        cmd = ["bwrap", "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc",
               "--tmpfs", "/tmp", "--ro-bind", str(work), str(work),
               "--unshare-all", "--die-with-parent", "--new-session", *cmd]
    env = dict(os.environ, **SAN_ENV)
    try:
        r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace", timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return Exec("timeout", f">{timeout}s")
    err = r.stderr or ""
    hit = re.search(r"runtime error: .*|ERROR: AddressSanitizer: \S+.*", err)
    if hit:
        return Exec("ub", hit.group(0)[:300])
    if r.returncode != 0:
        return Exec("crash", f"exit {r.returncode}: {err[-300:]}")
    return Exec("clean")


def edge_values(lo: int, hi: int) -> list[int]:
    cand = {lo, lo + 1, -1000000, -46341, -1000, -64, -33, -32, -31, -2, -1, 0, 1, 2, 3, 7, 8, 16,
            31, 32, 33, 63, 64, 255, 256, 1000, 46340, 46341, 65535, 65536, 1 << 30, hi - 1, hi}
    return sorted(v for v in cand if lo <= v <= hi)


def input_grid(params: list[tuple[str, str]], n_random: int = 2000, seed: int = 7) -> list[list[int]]:
    ranges = [type_range(t) or (0, 0) for t, _ in params]
    edges = [edge_values(lo, hi) for lo, hi in ranges]
    rows: list[list[int]] = [[]]
    for ev in edges:
        if len(rows) * len(ev) > 20000:
            ev = ev[:: max(1, len(ev) // 8)]
        rows = [r + [v] for r in rows for v in ev]
    rng = random.Random(seed)
    for _ in range(n_random if params else 0):
        row = []
        for lo, hi in ranges:
            pick = rng.random()
            if pick < 0.5:
                row.append(rng.randint(lo, hi))
            else:
                row.append(max(lo, min(hi, rng.randint(-2000, 2000))))
        rows.append(row)
    return rows


# --------------------------------------------------------------------------- self-check


def self_check(tasks: list[Task], work: Path, jobs: int) -> tuple[list[dict[str, Any]], int]:
    jobs_list = []
    for t in tasks:
        if t.origin not in ("prism", "esbmc-cpp"):
            continue
        for fn, exp in t.expected.items():
            jobs_list.append((t, fn, exp))

    def one(item: tuple[Task, str, bool]) -> dict[str, Any]:
        t, fn, exp = item
        rec: dict[str, Any] = {"task": t.ident, "function": fn, "expected": exp}
        if t.origin == "esbmc-cpp":
            # whole program: a deterministic one has one behaviour, so one
            # native run under the sanitizers (assertions on) decides it
            if not t.deterministic:
                rec.update(check="skipped", why="nondeterministic program (label is ESBMC's)")
                return rec
            res = run_program(t, work / "selfcheck" / t.ident.replace("/", "__"))
            ok = res.outcome == ("clean" if exp else "ub")
            rec.update(check="ok" if ok else "FAIL", outcome=res.outcome, detail=res.detail)
            return rec
        if fn in t.expect_status:
            rec.update(check="skipped", why="non-scalar parameters (expect_status)")
            return rec
        params = scalar_params(t, fn)
        if params is None:
            rec.update(check="skipped", why="no scalar signature")
            return rec
        wd = work / "selfcheck" / t.ident.replace("/", "__") / fn
        if exp:
            res = run_sanitized(t, fn, params, input_grid(params), wd)
            ok = res.outcome == "clean"
        else:
            if fn not in t.witness:
                rec.update(check="FAIL", why="false task without witness")
                return rec
            res = run_sanitized(t, fn, params, [t.witness[fn]], wd)
            ok = res.outcome == "ub" or (fn in t.sanitizer_blind and res.outcome == "clean")
            if fn in t.sanitizer_blind:
                rec["sanitizer_blind"] = True
        rec.update(check="ok" if ok else "FAIL", outcome=res.outcome, detail=res.detail)
        return rec

    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        out = list(ex.map(one, jobs_list))
    bad = sum(1 for r in out if r["check"] == "FAIL")
    return out, bad


# --------------------------------------------------------------------------- running PRISM


def prism_command(args: argparse.Namespace) -> tuple[list[str], str]:
    binp = args.prism or os.environ.get("PRISM_BIN")
    if binp:
        return [str(Path(binp).resolve())], "cpp"
    return [sys.executable, "-m", "prism"], "python"


def list_stages(cmd: list[str]) -> list[str]:
    try:
        r = subprocess.run(cmd + ["--list-stages"], capture_output=True, text=True, timeout=60, cwd=REPO)
    except (OSError, subprocess.TimeoutExpired):
        return []
    return [s.strip() for s in r.stdout.split() if s.strip()]


def run_prism(cmd: list[str], task: Task, stages: list[str], work: Path, timeout: float,
              unwind: int | None, extra_args: list[str] | None = None,
              rename: dict[str, str] | None = None, tag: str = "prism") -> dict[str, Any]:
    out = work / tag / task.ident.replace("/", "__")
    if out.exists():
        shutil.rmtree(out)
    argv = cmd + [str(task.source), "--no-llm", "--stage", ",".join(stages), "--out", str(out)]
    if task.unwind or unwind:
        argv += ["--unwind", str(task.unwind or unwind)]
    argv += extra_args or []
    t0 = time.monotonic()
    env = dict(os.environ)
    env.setdefault("PYTHONPATH", str(REPO))
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, cwd=REPO, env=env)
        rc = r.returncode
        tail = (r.stderr or "")[-400:]
    except subprocess.TimeoutExpired:
        return {"error": "TIMEOUT", "seconds": timeout, "argv": argv}
    secs = round(time.monotonic() - t0, 2)
    rep = out / "report.json"
    if not rep.exists():
        return {"error": f"no report.json (exit {rc}): {tail}", "seconds": secs, "argv": argv}
    data = json.loads(rep.read_text(encoding="utf-8"))
    per: dict[str, dict[str, list[dict[str, Any]]]] = {}
    stage_status: dict[str, str] = {}
    for st in data.get("stages", []):
        name = st.get("name")
        stage_status[name] = st.get("status", "")
        if name not in VERDICT_STAGES:
            continue
        name = (rename or {}).get(name, name)
        for f in st.get("findings", []):
            fn = f.get("function")
            if fn in task.expected:
                rec = {k: f.get(k) for k in ("status", "cls", "message", "counterexample", "line")}
                ex = {k: v for k, v in (f.get("extra") or {}).items() if k in KEEP_EXTRA}
                if ex:
                    rec["extra"] = ex
                per.setdefault(name, {}).setdefault(fn, []).append(rec)
        # A unit-level ERROR (e.g. the Clang front end rejects the file) speaks
        # for every function of the unit: recorded as ERROR, not as missing.
        unit_err = next((f for f in st.get("findings", []) if not f.get("function") and f.get("status") == "ERROR"),
                        None)
        if unit_err is not None:
            for fn in task.expected:
                lst = per.setdefault(name, {}).setdefault(fn, [])
                if not lst:
                    lst.append({"status": "ERROR", "cls": "", "message": str(unit_err.get("message", ""))[:300],
                                "counterexample": "", "line": None})
        if st.get("status") == "failed":
            # A crashed stage loses every function of the file: record why
            # instead of reporting the functions as silently missing.
            for fn in task.expected:
                lst = per.setdefault(name, {}).setdefault(fn, [])
                if not lst:
                    lst.append({"status": "STAGE-FAILED", "cls": "", "message": str(st.get("detail", ""))[:300],
                                "counterexample": "", "line": None})
    return {"findings": per, "stage_status": stage_status, "seconds": secs, "argv": argv, "exit": rc}


CEX_RE = re.compile(r"(\w+)\s*=\s*(#x[0-9a-fA-F]+|#b[01]+|-?0[xX][0-9a-fA-F]+|-?\d+|true|false)")


def parse_cex(cex: str) -> dict[str, int]:
    """`a=1, b=-2` (Python engine) or `a=#x0000000f` (C++ engine, Z3 bit-vectors).

    Bit-vector literals are raw bit patterns; replay() reinterprets them in
    the parameter's C type.
    """
    vals: dict[str, int] = {}
    for name, v in CEX_RE.findall(cex or ""):
        if v in ("true", "false"):
            vals[name] = 1 if v == "true" else 0
        elif v.startswith("#x"):
            vals[name] = int(v[2:], 16)
        elif v.startswith("#b"):
            vals[name] = int(v[2:], 2)
        else:
            vals[name] = int(v, 0)
    return vals


def replay(task: Task, fn: str, cex: str, work: Path) -> dict[str, Any]:
    if task.origin != "prism":
        return {"replay": "unsupported", "why": f"{task.origin} task: whole program with nondet inputs"}
    params = scalar_params(task, fn)
    if params is None:
        return {"replay": "unsupported", "why": "non-scalar parameters"}
    vals = parse_cex(cex)
    if params and not vals:
        return {"replay": "no-cex", "why": "FAILED without parseable counterexample"}
    missing = [n for _, n in params if n not in vals]
    row = []
    for t, n in params:
        lo, hi = type_range(t) or (0, 0)
        v = vals.get(n, 0)
        # The solver reports bit patterns; reinterpret into the C type's range.
        span = hi - lo + 1
        if v > hi or v < lo:
            v = (v - lo) % span + lo
        row.append(v)
    res = run_sanitized(task, fn, params, [row], work / "replay" / task.ident.replace("/", "__") / fn)
    rec: dict[str, Any] = {"replay": "replayed" if res.outcome == "ub" else "not-replayed", "outcome": res.outcome,
           "detail": res.detail, "inputs": dict(zip([n for _, n in params], row))}
    if missing:
        rec["missing_params"] = missing
    if fn in task.sanitizer_blind and res.outcome == "clean":
        rec["replay"] = "sanitizer-blind"
    return rec


def classify(task: Task, fn: str, found: list[dict[str, Any]]) -> str:
    statuses = {f["status"] for f in found}
    expected = task.expected[fn]
    prop_scoped = task.origin in PROPERTY_SCOPED  # SV-COMP / concurrency labels speak for one property only
    classes = PROPERTY_CLASSES.get(task.prop, set())
    failed = [f for f in found if f["status"] == "FAILED"]
    failed_in_prop = [f for f in failed if not prop_scoped or f.get("cls") in classes]
    if not expected and fn in task.expect_class:
        # the refutation must be for the violation the task plants
        failed_in_prop = [f for f in failed_in_prop if f.get("cls") in task.expect_class[fn]]
    if not statuses:
        return "missing"
    if not expected and statuses & PROOF:
        return "wrong-proof"
    if fn in task.expect_status:
        want = task.expect_status[fn]
        if failed and expected:
            return "false-alarm"
        if statuses & PROOF or failed:
            return "law6-violation" if want == "NEEDS-HARNESS" else "unexpected-status"
        return "law-ok" if want in statuses else "unexpected-status"
    if expected and failed_in_prop:
        return "false-alarm"
    if expected and failed:
        return "failed-other-property"
    if expected and statuses & PROOF:
        return "proved"
    if not expected and failed_in_prop:
        return "refuted"
    if not expected and failed:
        return "failed-other-property"
    if "BOUNDED" in statuses:
        return "bounded"
    return "no-answer"


# --------------------------------------------------------------------------- metrics / reports


def pct(a: int, b: int) -> str:
    return f"{100.0 * a / b:.1f}%" if b else "n/a"


def compute_metrics(rows: list[dict[str, Any]], stages: list[str]) -> dict[str, Any]:
    metrics: dict[str, Any] = {}
    origins = sorted({r["origin"] for r in rows})
    for stage in stages:
        metrics[stage] = {}
        for origin in origins + ["all"]:
            sel = [r for r in rows if r["stage"] == stage and (origin == "all" or r["origin"] == origin)]
            scored = [r for r in sel if not r["law_task"]]
            true_n = sum(1 for r in scored if r["expected"])
            false_n = sum(1 for r in scored if not r["expected"])
            c = {k: sum(1 for r in sel if r["outcome"] == k) for k in (
                "wrong-proof", "false-alarm", "proved", "refuted", "bounded", "no-answer", "missing",
                "failed-other-property", "law-ok", "law6-violation", "unexpected-status")}
            replayed = sum(1 for r in sel if r["outcome"] == "refuted" and r.get("replay", {}).get("replay") == "replayed")
            law_n = sum(1 for r in sel if r["law_task"])
            metrics[stage][origin] = {
                "functions": len(sel),
                "true": true_n,
                "false": false_n,
                "wrong_proofs": c["wrong-proof"],
                "false_alarms": c["false-alarm"],
                "proved": c["proved"],
                "refuted": c["refuted"],
                "refuted_replayed": replayed,
                "bounded": c["bounded"],
                "no_answer": c["no-answer"] + c["missing"],
                "failed_other_property": c["failed-other-property"],
                "law_tasks": law_n,
                "law_ok": c["law-ok"],
                "law6_violations": c["law6-violation"] + c["unexpected-status"],
                "soundness_ok": c["wrong-proof"] == 0,
                "completeness": round(c["proved"] / true_n, 4) if true_n else None,
                "detection": round(replayed / false_n, 4) if false_n else None,
                "detection_unreplayed": round(c["refuted"] / false_n, 4) if false_n else None,
            }
    return metrics


def certified_summary(rows: list[dict[str, Any]]) -> dict[str, Any]:
    """Roadmap 3 exit criterion: loop-free `true` functions -> PROVED-CERTIFIED.

    Loop-free means the pir stage encoded the function and found no loop
    (extra.loops == "0"). A function pir did not encode (NEEDS-HARNESS,
    UNKNOWN before encoding) has no loop count and is listed separately.
    """
    sel = [r for r in rows if r["stage"] == CERT_STAGE and r["expected"] and not r["law_task"]]
    loop_free, looped, unencoded = [], [], []
    for r in sel:
        loops = next((f.get("extra", {}).get("loops") for f in r["findings"] if f.get("extra", {}).get("loops")),
                     None)
        (loop_free if loops == "0" else looped if loops else unencoded).append(r)

    def st(r: dict[str, Any]) -> set[str]:
        return {f["status"] for f in r["findings"]}

    cert = [r for r in loop_free if "PROVED-CERTIFIED" in st(r)]
    proved = [r for r in loop_free if st(r) & PROOF]

    def ex(r: dict[str, Any], key: str) -> str:
        return next((str(f.get("extra", {}).get(key, "")) for f in r["findings"] if f.get("extra", {}).get(key)), "")

    # A function with no VC stays PROVED (a certificate that checks nothing is
    # not a certificate): counted apart, not as "proved, not certified".
    no_vcs = [r for r in proved if r not in cert and ex(r, "certificate_vcs") == "0"]
    not_cert = []
    for r in proved:
        if r in cert or r in no_vcs:
            continue
        not_cert.append({"task": r["task"], "function": r["function"], "note": ex(r, "certify_note")})

    def lean_share(r: dict[str, Any]) -> tuple[int, int]:
        m = re.match(r"(\d+)/(\d+)", ex(r, "certificate_bitblast"))
        return (int(m.group(1)), int(m.group(2))) if m else (0, 0)

    wrong_cert = [r for r in rows if r["stage"] == CERT_STAGE and not r["expected"]
                  and "PROVED-CERTIFIED" in st(r)]
    return {
        "true_functions": len(sel),
        "loop_free_true": len(loop_free),
        "loop_free_true_proved": len(proved),
        # PROVED with no VC at all: nothing to certify (docs/TRUSTED_BASE.md)
        "loop_free_true_no_vcs": len(no_vcs),
        "loop_free_true_certified": len(cert),
        # which bit-blaster made the CNFs of the certified functions
        "loop_free_true_certified_lean": sum(1 for r in cert if lean_share(r)[1] and
                                             lean_share(r)[0] == lean_share(r)[1]),
        "loop_free_true_certified_z3": sum(1 for r in cert if lean_share(r)[0] == 0),
        "loop_free_true_certified_mixed": sum(1 for r in cert if 0 < lean_share(r)[0] < lean_share(r)[1]),
        "looped_true": len(looped),
        "looped_true_certified": sum(1 for r in looped if "PROVED-CERTIFIED" in st(r)),
        "not_encoded_true": len(unencoded),
        "wrong_certified": len(wrong_cert),
        "proved_not_certified": not_cert,
    }


def markdown(metrics: dict[str, Any], rows: list[dict[str, Any]], meta: dict[str, Any]) -> str:
    out = ["# PRISM conformance results", ""]
    out.append(f"- engine: `{meta['engine']}` (`{' '.join(meta['command'])}`)")
    out.append(f"- stages run: `{','.join(meta['stages'])}`")
    out.append(f"- tasks: {meta['tasks']} files, {meta['functions']} scored functions")
    out.append(f"- sandbox for counterexample replay: {meta['sandbox']}")
    out.append(f"- **release gate: {'PASS' if meta['wrong_proofs'] == 0 else 'FAIL'}** "
               f"({meta['wrong_proofs']} wrong proof(s))")
    out.append("")
    out.append("| stage | origin | true | false | wrong proofs | completeness | detection (replayed cex) "
               "| refuted, not replayed | false alarms | BOUNDED | no answer | Law 6 ok |")
    out.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for stage, by in metrics.items():
        for origin, m in by.items():
            if not m["functions"]:
                continue
            out.append(
                f"| {stage} | {origin} | {m['true']} | {m['false']} | **{m['wrong_proofs']}** "
                f"| {m['proved']}/{m['true']} ({pct(m['proved'], m['true'])}) "
                f"| {m['refuted_replayed']}/{m['false']} ({pct(m['refuted_replayed'], m['false'])}) "
                f"| {m['refuted'] - m['refuted_replayed']} | {m['false_alarms']} | {m['bounded']} "
                f"| {m['no_answer']} | {m['law_ok']}/{m['law_tasks']} |"
            )
    out.append("")
    cs = meta.get("certified")
    if cs:
        out += ["## Certified mode (roadmap 3.2, `prism --certified`)", "",
                f"- loop-free `true` functions encoded by pir: {cs['loop_free_true']}",
                f"- of those PROVED (any proof) under --certified: {cs['loop_free_true_proved']}",
                f"- of those with no VC at all (PROVED, nothing to certify): {cs['loop_free_true_no_vcs']}",
                f"- of those **PROVED-CERTIFIED**: **{cs['loop_free_true_certified']}/{cs['loop_free_true']}** "
                f"(CNF by the Lean-proved bit-blaster: {cs['loop_free_true_certified_lean']}, "
                f"by Z3's tactics: {cs['loop_free_true_certified_z3']}, "
                f"mixed: {cs['loop_free_true_certified_mixed']})",
                f"- `true` functions with loops: {cs['looped_true']} "
                f"({cs['looped_true_certified']} PROVED-CERTIFIED: loops closed within the unwind)",
                f"- `true` functions pir did not encode (NEEDS-HARNESS etc.): {cs['not_encoded_true']}",
                f"- PROVED-CERTIFIED on a `false` function (must be 0): **{cs['wrong_certified']}**", ""]
        for x in cs["proved_not_certified"]:
            out.append(f"  - proved, not certified: `{x['task']}` `{x['function']}`: {x['note']}")
        out.append("")
    # per-category matrix (first stage that ran, in-house tasks)
    cats = sorted({(r["origin"], r["category"]) for r in rows})
    stages = list(metrics)
    if cats:
        out += ["## Per category", "",
                "| origin/category | " + " | ".join(f"{s}: proved true | {s}: refuted false" for s in stages) + " |",
                "|---|" + "---|---|" * len(stages)]
        for origin, cat in cats:
            cells = []
            for s in stages:
                sel = [r for r in rows if r["stage"] == s and r["origin"] == origin and r["category"] == cat
                       and not r["law_task"]]
                tn = sum(1 for r in sel if r["expected"])
                fn = sum(1 for r in sel if not r["expected"])
                pr = sum(1 for r in sel if r["outcome"] == "proved")
                rf = sum(1 for r in sel if r["outcome"] == "refuted")
                cells += [f"{pr}/{tn}", f"{rf}/{fn}"]
            out.append(f"| {origin}/{cat} | " + " | ".join(cells) + " |")
        out.append("")

    def listing(title: str, pred: Any) -> None:
        sel = [r for r in rows if pred(r)]
        out.append(f"## {title} ({len(sel)})")
        out.append("")
        if not sel:
            out.append("None.")
        for r in sel:
            f = r["findings"]
            desc = "; ".join(f"{x['status']} {x.get('cls') or ''} {x.get('message') or ''}".strip() for x in f)
            cex = "; ".join(x["counterexample"] for x in f if x.get("counterexample"))
            out.append(f"- `{r['task']}` `{r['function']}` [{r['stage']}]: {desc}"
                       + (f" (cex: `{cex}`)" if cex else ""))
            rp = r.get("replay")
            if rp and rp.get("replay") not in (None, "replayed"):
                out.append(f"  - replay: {rp.get('replay')} {rp.get('outcome', '')} {rp.get('why', '')}"
                           f" {rp.get('detail', '')}".rstrip())
        out.append("")

    listing("Wrong proofs (soundness bugs)", lambda r: r["outcome"] == "wrong-proof")
    listing("False alarms", lambda r: r["outcome"] == "false-alarm")
    listing("Law 6 / expected-status violations",
            lambda r: r["outcome"] in ("law6-violation", "unexpected-status"))
    listing("Stage crashed on the task (every function of the file lost)",
            lambda r: any(f["status"] == "STAGE-FAILED" for f in r["findings"]))
    listing("Function never reported by the stage (silently skipped, Law 7)",
            lambda r: r["outcome"] == "missing")
    listing("Refuted but counterexample did not replay",
            lambda r: r["outcome"] == "refuted" and r.get("replay", {}).get("replay") != "replayed"
            and r["origin"] == "prism")
    return "\n".join(out) + "\n"


# --------------------------------------------------------------------------- Juliet


def fetch_juliet(dest: Path, flows: list[str]) -> Path:
    dest.mkdir(parents=True, exist_ok=True)
    zpath = dest / "juliet-c-cplusplus-v1.3.zip"
    if not zpath.exists() or zpath.stat().st_size != JULIET_SIZE:
        print(f"downloading {JULIET_URL}", file=sys.stderr)
        tmp = zpath.with_suffix(".part")
        # NIST answers 403 to the default Python-urllib User-Agent.
        req = urllib.request.Request(JULIET_URL, headers={"User-Agent": "prism-conformance/1 (curl-compatible)"})
        with urllib.request.urlopen(req, timeout=600) as r, open(tmp, "wb") as fh:
            shutil.copyfileobj(r, fh)
        tmp.replace(zpath)
    h = hashlib.sha256()
    with open(zpath, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    if h.hexdigest() != JULIET_SHA256:
        raise SystemExit(f"Juliet archive hash mismatch: {h.hexdigest()} != {JULIET_SHA256}")
    tasks_root = dest / "juliet"
    name_re = re.compile(r"^(CWE\d+)_\w+?_(\d\d)\.c$")
    written = 0
    with zipfile.ZipFile(zpath) as z:
        for info in z.infolist():
            parts = info.filename.split("/")
            if len(parts) < 3 or parts[0] != "C":
                continue
            if parts[1] == "testcasesupport" and not info.is_dir():
                z.extract(info, dest)
                continue
            if parts[1] != "testcases" or info.is_dir():
                continue
            base = parts[-1]
            m = name_re.match(base)
            if not m or m.group(1) not in JULIET_CWES or m.group(2) not in flows:
                continue
            if not JULIET_TYPE[m.group(1)].search(base):
                continue
            text = z.read(info).decode("latin-1")
            stem = base[:-2]
            expected: dict[str, bool] = {}
            for fm in re.finditer(r"(?m)^(?:static\s+)?void\s+(\w+)\s*\(\s*(?:void)?\s*\)\s*$\s*\{", text):
                fname = fm.group(1)
                if fname.endswith("_bad"):
                    expected[fname] = False
                elif fname.startswith("good") or fname.endswith("_good"):
                    expected[fname] = True
            if not expected:
                continue
            out_dir = tasks_root / m.group(1)
            out_dir.mkdir(parents=True, exist_ok=True)
            (out_dir / base).write_text(text, encoding="utf-8")
            lines = ["format_version: 1", f"input_files: {base}", "language: C",
                     f"property: {JULIET_PROPERTY[m.group(1)]}", "origin: juliet-1.3", "expected:"]
            lines += [f"  {k}: {'true' if v else 'false'}" for k, v in expected.items()]
            (out_dir / f"{stem}.yml").write_text("\n".join(lines) + "\n", encoding="utf-8")
            written += 1
    print(f"juliet: {written} task files under {tasks_root}", file=sys.stderr)
    return tasks_root


# --------------------------------------------------------------------------- ESBMC C++ regression tests


def _esbmc_option_skip(opts: str) -> str:
    for flag in ESBMC_SKIP_OPTIONS:
        if re.search(r"(?:^|\s)" + re.escape(flag) + r"(?:[\s=]|$)", opts):
            return f"option {flag} changes the checked property set"
    return ""


def esbmc_convert(test_dir: Path, regression: Path, thorough: bool = False) -> tuple[dict[str, Any] | None, str]:
    """One ESBMC regression test -> (task description, "") or (None, skip reason).

    `test.desc`: line 1 the test level (CORE / THOROUGH / KNOWNBUG), line 2
    the input file, line 3 ESBMC's options, then regexes over its output, one
    of which is `^VERIFICATION SUCCESSFUL$` or `^VERIFICATION FAILED$`.
    ESBMC checks, by default, assertions, array bounds, pointer safety and
    division by zero from `main`; signed overflow only with
    `--overflow-check` (such tests are skipped: another property set).
    """
    desc = (test_dir / "test.desc").read_text(encoding="utf-8", errors="replace").splitlines()
    if len(desc) < 3 or desc[0].lstrip().startswith("<"):
        return None, "test.desc not in the CORE/THOROUGH line format"
    level = desc[0].strip()
    up = test_dir.relative_to(regression).as_posix()
    if up in ESBMC_DISPUTED:
        return None, "label contradicts the C++ standard (ESBMC_DISPUTED)"
    if level == "KNOWNBUG":
        return None, "KNOWNBUG (ESBMC's own label is not trusted upstream)"
    if level not in ("CORE", "THOROUGH"):
        return None, f"test level {level!r}"
    if level == "THOROUGH" and not thorough:
        return None, "THOROUGH (slow tier; --esbmc-thorough)"
    main_file = desc[1].strip()
    sources = [p for p in test_dir.iterdir() if p.is_file() and p.suffix.lower() in ESBMC_SOURCE_SUFFIXES]
    if not main_file or Path(main_file).suffix.lower() not in (".cpp", ".cc", ".cxx", ".c++"):
        return None, "input is not a single C++ file"
    if len(sources) != 1 or sources[0].name != main_file:
        return None, "several source/header files (PRISM tasks are single files)"
    opts = desc[2].strip()
    why = _esbmc_option_skip(opts)
    if why:
        return None, why
    verdicts = {m.group(1) for line in desc[3:]
                for m in [re.fullmatch(r"\s*\^?\s*VERIFICATION (SUCCESSFUL|FAILED)\s*\$?\s*", line)] if m}
    if len(verdicts) != 1:
        return None, "no single VERIFICATION SUCCESSFUL/FAILED line"
    expected = verdicts.pop() == "SUCCESSFUL"
    patterns = "\n".join(desc[3:])
    if not expected and ESBMC_MODEL_FAILURE.search(patterns):
        return None, "failure is a limit of ESBMC's operational model, not of the program"
    text = sources[0].read_text(encoding="utf-8", errors="replace")
    if "__ESBMC" in text:
        return None, "uses ESBMC intrinsics"
    std = ""
    m = re.search(r"--std[= ]\s*(\S+)", opts)
    if m:
        std = m.group(1).replace("gnu++", "c++")
    unwind = re.search(r"--unwind\s+(\d+)", opts)
    rel = test_dir.relative_to(regression)
    return {
        "source": sources[0],
        "text": text,
        "upstream": str(rel),
        "level": level,
        "options": opts,
        "std": std,
        "expected": expected,
        # a SUCCESSFUL under --no-unwinding-assertions only speaks up to the bound
        "label_bound": int(unwind.group(1)) if expected and unwind and "--no-unwinding-assertions" in opts else 0,
        "deterministic": not ESBMC_NONDET.search(text),
        "reason": [ln.strip() for ln in desc[3:] if ln.strip() and "VERIFICATION" not in ln][:3],
    }, ""


def esbmc_task_name(upstream: str) -> tuple[str, str]:
    """regression/<dir>/<sub>/.../<test> -> (category, file stem)."""
    parts = Path(upstream).parts
    if parts[0] == "esbmc-cpp" and len(parts) > 2:
        return "cpp03_" + re.sub(r"\W", "_", parts[1]), "__".join(parts[2:])
    return parts[0].replace("esbmc-", ""), "__".join(parts[1:])


def esbmc_task_yaml(t: dict[str, Any], src_name: str) -> str:
    lines = ["format_version: 1", f"input_files: {src_name}", "language: C++", "property: esbmc-cpp",
             f"upstream: esbmc@{ESBMC_COMMIT[:12]} regression/{t['upstream']}",
             f"esbmc_options: {json.dumps(t['options'])}"]
    if t["std"]:
        lines.append(f"std: {t['std']}")
    if t["label_bound"]:
        lines.append(f"label_bound: {t['label_bound']}")
    lines.append(f"deterministic: {'true' if t['deterministic'] else 'false'}")
    lines += ["expected:", f"  main: {'true' if t['expected'] else 'false'}"]
    return "\n".join(lines) + "\n"


def fetch_esbmc(dest: Path, thorough: bool = False) -> Path:
    """Clone ESBMC's C++ regression directories at ESBMC_COMMIT and convert them.

    GitHub archive downloads are not used (some proxies refuse them): a
    shallow, blob-filtered, sparse fetch of the one pinned commit is, and
    HEAD is checked against the pin.
    """
    dest.mkdir(parents=True, exist_ok=True)
    repo = dest / "esbmc-src"

    def git(*a: str) -> str:
        r = subprocess.run(["git", "-C", str(repo), *a], capture_output=True, text=True, timeout=1800)
        if r.returncode != 0:
            raise SystemExit(f"git {' '.join(a)}: {r.stderr.strip()[-400:]}")
        return r.stdout

    if not (repo / ".git").exists():
        repo.mkdir(parents=True, exist_ok=True)
        git("init", "-q")
        git("remote", "add", "origin", ESBMC_REPO)
    head = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if head != ESBMC_COMMIT:
        print(f"fetching {ESBMC_REPO} @ {ESBMC_COMMIT}", file=sys.stderr)
        git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", ESBMC_COMMIT)
        git("sparse-checkout", "set", *ESBMC_DIRS)
        git("checkout", "-q", "--detach", "FETCH_HEAD")
    else:
        git("sparse-checkout", "set", *ESBMC_DIRS)
    head = git("rev-parse", "HEAD").strip()
    if head != ESBMC_COMMIT:
        raise SystemExit(f"ESBMC checkout is at {head}, pinned {ESBMC_COMMIT}")
    regression = repo / "regression"
    tasks_root = dest / "esbmc-cpp"
    if tasks_root.exists():
        shutil.rmtree(tasks_root)
    skipped: dict[str, int] = {}
    written = 0
    for d in ESBMC_DIRS:
        for desc in sorted((repo / d).rglob("test.desc")):
            t, why = esbmc_convert(desc.parent, regression, thorough)
            if t is None:
                skipped[why] = skipped.get(why, 0) + 1
                continue
            cat, stem = esbmc_task_name(t["upstream"])
            out = tasks_root / cat
            out.mkdir(parents=True, exist_ok=True)
            src_name = stem + ".cpp"
            (out / src_name).write_text(t["text"], encoding="utf-8")
            (out / f"{stem}.yml").write_text(esbmc_task_yaml(t, src_name), encoding="utf-8")
            written += 1
    print(f"esbmc-cpp: {written} task files under {tasks_root}", file=sys.stderr)
    for why, n in sorted(skipped.items(), key=lambda kv: -kv[1]):
        print(f"  skipped {n:5d}: {why}", file=sys.stderr)
    return tasks_root


# Curated subset (tests/conformance/esbmc-cpp): categories and files whose
# licence is not ESBMC's own Apache-2.0 are left out (CBMC's BSD-4-clause
# tests, GCC testsuite files, Qt, textbook listings).
ESBMC_CURATE_SKIP_CATEGORIES = {"cpp03_cbmc", "cpp03_gcc_template_tests", "cpp03_qt", "cpp03_esbmc_systemc"}
ESBMC_FOREIGN_TEXT = re.compile(r"copyright|\(c\)|deitel|pearson|fig\.\s*\d|llbmc|gnu general|licen[cs]e|"
                                r"\bgcc\b|\bdg-", re.I)


def curate_esbmc(fetched: Path, dest: Path, per_verdict: int, per_category: int, work: Path) -> int:
    """Copy a label-checked, licence-clean sample of the fetched tasks into dest.

    Candidates: deterministic programs (one run is their only behaviour),
    no foreign copyright/licence text, not in ESBMC_CURATE_SKIP_CATEGORIES.
    Order: SHA-256 of the upstream path (stable, not chosen by PRISM's
    results). A candidate is kept only if one native run under the
    sanitizers agrees with ESBMC's label (run_program); at most
    `per_category` tasks per category and verdict, `per_verdict` per verdict.
    """
    cands = []
    for yml in sorted(fetched.rglob("*.yml")):
        t = load_task(yml, fetched.parent)
        if t is None or not t.deterministic or t.category in ESBMC_CURATE_SKIP_CATEGORIES:
            continue
        text = t.source.read_text(encoding="utf-8", errors="replace")
        if ESBMC_FOREIGN_TEXT.search(text):
            continue
        up = next((ln.split(" ", 2)[-1] for ln in yml.read_text().splitlines() if ln.startswith("upstream:")), "")
        cands.append((hashlib.sha256(up.encode()).hexdigest(), t))
    cands.sort(key=lambda c: c[0])
    kept: dict[bool, int] = {True: 0, False: 0}
    per_cat: dict[tuple[str, bool], int] = {}
    for sub in (dest.iterdir() if dest.exists() else []):
        if sub.is_dir():  # task directories; LICENSE/NOTICE files stay
            shutil.rmtree(sub)
    for _, t in cands:
        exp = t.expected["main"]
        if kept[exp] >= per_verdict or per_cat.get((t.category, exp), 0) >= per_category:
            continue
        res = run_program(t, work / "curate" / t.ident.replace("/", "__"))
        if res.outcome != ("clean" if exp else "ub"):
            continue
        out = dest / t.category
        out.mkdir(parents=True, exist_ok=True)
        shutil.copy2(t.source, out / t.source.name)
        shutil.copy2(t.yml, out / t.yml.name)
        kept[exp] += 1
        per_cat[(t.category, exp)] = per_cat.get((t.category, exp), 0) + 1
        if all(v >= per_verdict for v in kept.values()):
            break
    print(f"esbmc-cpp subset: {kept[True]} true, {kept[False]} false under {dest}", file=sys.stderr)
    return kept[True] + kept[False]


def run_program(task: Task, work: Path, timeout: float = 60.0) -> Exec:
    """Compile a whole-program task natively (sanitizers, assertions on) and run main.

    For a deterministic program this one execution is the program's only
    behaviour, so it decides the label: `ub` covers a sanitizer report, a
    failed assert() and an uncaught exception (both abort).
    """
    work.mkdir(parents=True, exist_ok=True)
    exe = work / "prog.bin"
    std = task.std or ("c++17" if task.lang.upper().startswith("C+") else "c17")
    cc, cxx = sanitizer_compilers()
    comp = cxx if task.lang.upper().startswith("C+") else cc
    cmd = [comp, f"-std={std}", str(task.source), *SAN_FLAGS, "-UNDEBUG", "-o", str(exe)]
    try:
        r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace", timeout=300)
    except (OSError, subprocess.TimeoutExpired) as e:
        return Exec("compile-error", str(e))
    if r.returncode != 0:
        return Exec("compile-error", (r.stderr or r.stdout)[-400:])
    run = [str(exe)]
    if bwrap_ok():
        run = ["bwrap", "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
               "--ro-bind", str(work), str(work), "--unshare-all", "--die-with-parent", "--new-session", *run]
    try:
        r = subprocess.run(run, capture_output=True, encoding="utf-8", errors="replace", timeout=timeout, stdin=subprocess.DEVNULL,
                           env=dict(os.environ, **SAN_ENV))
    except subprocess.TimeoutExpired:
        return Exec("timeout", f">{timeout}s")
    err = r.stderr or ""
    hit = re.search(r"runtime error: .*|ERROR: AddressSanitizer: \S+.*|Assertion `.*' failed|"
                    r"terminate called .*", err)
    if hit:
        return Exec("ub", hit.group(0)[:300])
    if r.returncode != 0:
        return Exec("crash", f"exit {r.returncode}: {err[-300:]}")
    return Exec("clean")


# --------------------------------------------------------------------------- main


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--prism", help="PRISM binary (default: $PRISM_BIN, else python -m prism)")
    ap.add_argument("--suite", action="append", type=Path,
                    help="task roots (default: tests/conformance/prism and tests/conformance/sv-comp)")
    ap.add_argument("--juliet", type=Path, help="include Juliet tasks from DIR/juliet (see --fetch-juliet)")
    ap.add_argument("--fetch-juliet", type=Path, metavar="DIR",
                    help="download NIST Juliet 1.3, check sha256, extract CWE190/191/369/476/680 into DIR")
    ap.add_argument("--juliet-flows", default="01", help="Juliet flow variants to keep (comma list, default 01)")
    ap.add_argument("--esbmc", type=Path, help="include ESBMC C++ regression tasks from DIR/esbmc-cpp")
    ap.add_argument("--fetch-esbmc", type=Path, metavar="DIR",
                    help=f"clone ESBMC's C++ regression tests at {ESBMC_COMMIT[:12]} and convert them into DIR")
    ap.add_argument("--esbmc-thorough", action="store_true", help="also convert ESBMC's THOROUGH tests")
    ap.add_argument("--curate-esbmc", type=Path, metavar="DIR",
                    help="write the committed subset (tests/conformance/esbmc-cpp) from fetched tasks in DIR")
    ap.add_argument("--stages", help="override verdict stages (default: bmc,harness + pir,conc when listed)")
    ap.add_argument("--out", type=Path, default=Path("conformance-out"))
    ap.add_argument("--jobs", "-j", type=int, default=max(1, (os.cpu_count() or 2)))
    ap.add_argument("--timeout", type=float, default=180.0, help="seconds per PRISM run")
    ap.add_argument("--unwind", type=int)
    ap.add_argument("--filter", help="regex on task ident")
    ap.add_argument("--no-replay", action="store_true",
                    help="do not compile/run counterexamples (detection is then reported as 0 replayed)")
    ap.add_argument("--self-check", action="store_true",
                    help="validate the suite labels with sanitizers instead of running PRISM")
    ap.add_argument("--certified", action="store_true",
                    help="also run the pir stage with --certified and score it as 'pir-certified'")
    ap.add_argument("--solver-cache", type=Path,
                    help="solver query cache for the pir runs (default: a fresh one in the work dir, "
                         "so no answer comes from an earlier run)")
    args = ap.parse_args(argv)

    if args.fetch_juliet:
        fetch_juliet(args.fetch_juliet, args.juliet_flows.split(","))
        return 0
    if args.fetch_esbmc:
        fetch_esbmc(args.fetch_esbmc, args.esbmc_thorough)
        return 0
    if args.curate_esbmc:
        src = args.curate_esbmc / "esbmc-cpp" if (args.curate_esbmc / "esbmc-cpp").exists() else args.curate_esbmc
        with tempfile.TemporaryDirectory(prefix="prism-curate-") as d:
            n = curate_esbmc(src, SUITE / "esbmc-cpp", 32, 3, Path(d))
        return 0 if n else 1

    roots = args.suite or [SUITE / d for d in DEFAULT_ROOTS]
    tasks = discover([r.resolve() for r in roots if r.exists()])
    if args.juliet:
        jr = args.juliet / "juliet" if (args.juliet / "juliet").exists() else args.juliet
        for yml in sorted(jr.rglob("*.yml")):
            t = load_task(yml, jr.parent)
            if t is not None:
                t.origin = "juliet"
                tasks.append(t)
    if args.esbmc:
        er = args.esbmc / "esbmc-cpp" if (args.esbmc / "esbmc-cpp").exists() else args.esbmc
        for yml in sorted(er.rglob("*.yml")):
            t = load_task(yml, er.parent)
            if t is not None:
                t.origin = "esbmc-cpp"
                t.ident = "esbmc-fetched/" + str(yml.relative_to(er))
                tasks.append(t)
    if args.filter:
        rx = re.compile(args.filter)
        tasks = [t for t in tasks if rx.search(t.ident)]
    args.out.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="prism-conf-"))

    try:
        if args.self_check:
            recs, bad = self_check(tasks, work, args.jobs)
            (args.out / "selfcheck.json").write_text(json.dumps(recs, indent=1), encoding="utf-8")
            counts: dict[str, int] = {}
            for r in recs:
                counts[r["check"]] = counts.get(r["check"], 0) + 1
            print(f"self-check: {counts}")
            for r in recs:
                if r["check"] == "FAIL":
                    print(f"  FAIL {r['task']} {r['function']} expected={r['expected']} "
                          f"{r.get('outcome', '')} {r.get('why', '')} {r.get('detail', '')[:200]}")
            return 1 if bad else 0

        cmd, engine = prism_command(args)
        listed = list_stages(cmd)
        if not listed:
            print(f"cannot run PRISM: {' '.join(cmd)} --list-stages gave nothing", file=sys.stderr)
            return 3
        if args.stages:
            vstages = [s for s in args.stages.split(",") if s]
        else:
            vstages = [s for s in VERDICT_STAGES if s in listed]
        run_stages = [s for s in BASE_STAGES if s in listed]
        run_stages += [s for s in vstages if s not in run_stages]
        # keep the pipeline's own order
        run_stages.sort(key=lambda s: listed.index(s) if s in listed else 999)

        cache = args.solver_cache or (work / "solver-cache")
        extra = ["--solver-cache", str(cache)] if engine == "cpp" else []
        cert_on = bool(args.certified) and "pir" in listed
        if cert_on:
            vstages.append(CERT_STAGE)
        cert_stages = [s for s in ("inventory", "classify", "pir") if s in listed]

        def one(t: Task) -> tuple[Task, dict[str, Any]]:
            res = run_prism(cmd, t, run_stages, work, args.timeout, args.unwind, extra)
            if cert_on and "error" not in res:
                # Certificates cost time (bit-blast, LRAT, checking): twice the budget.
                cres = run_prism(cmd, t, cert_stages, work, 2 * args.timeout, args.unwind,
                                 extra + ["--certified"], rename={"pir": CERT_STAGE}, tag="prism-cert")
                if "error" in cres:
                    res.setdefault("findings", {})[CERT_STAGE] = {
                        fn: [{"status": "STAGE-FAILED", "cls": "", "message": cres["error"][:300],
                              "counterexample": "", "line": None}] for fn in t.expected}
                else:
                    res.setdefault("findings", {}).update(cres.get("findings", {}))
                res["seconds_certified"] = cres.get("seconds")
            return t, res

        rows: list[dict[str, Any]] = []
        with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            results = list(ex.map(one, tasks))
        for t, res in results:
            for stage in vstages:
                if stage in STAGE_TASK_ORIGINS and t.origin not in STAGE_TASK_ORIGINS[stage]:
                    continue
                if t.origin in ORIGIN_STAGES and stage not in ORIGIN_STAGES[t.origin]:
                    continue
                for fn, exp in t.expected.items():
                    found = res.get("findings", {}).get(stage, {}).get(fn, [])
                    if "error" in res:
                        found = []
                    if not found and stage in SCOPED_STAGES and "error" not in res:
                        continue  # outside the stage's scope (e.g. harness: pointer functions)
                    row: dict[str, Any] = {
                        "task": t.ident, "origin": t.origin, "category": t.category, "function": fn,
                        "expected": exp, "property": t.prop, "stage": stage,
                        "law_task": fn in t.expect_status, "findings": found,
                        "seconds": res.get("seconds"),
                    }
                    if "error" in res:
                        row["error"] = res["error"]
                    row["outcome"] = classify(t, fn, found)
                    rows.append(row)

        # counterexample replay for refutations of in-house tasks
        def rep(row: dict[str, Any]) -> None:
            t = next(x for x in tasks if x.ident == row["task"])
            cex = next((f.get("counterexample") or "" for f in row["findings"] if f["status"] == "FAILED"), "")
            row["replay"] = replay(t, row["function"], cex, work / row["stage"])

        to_replay = [r for r in rows if r["outcome"] == "refuted"]
        if args.no_replay:
            for r in to_replay:
                r["replay"] = {"replay": "skipped", "why": "--no-replay"}
        else:
            with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
                list(ex.map(rep, to_replay))

        metrics = compute_metrics(rows, vstages)
        wrong = sum(1 for r in rows if r["outcome"] == "wrong-proof")
        meta = {
            "engine": engine, "command": cmd, "stages": run_stages, "verdict_stages": vstages,
            "tasks": len(tasks), "functions": len({(r['task'], r['function']) for r in rows}),
            "sandbox": "bwrap" if bwrap_ok() else "none (bwrap unavailable)",
            "wrong_proofs": wrong, "release_gate": "PASS" if wrong == 0 else "FAIL",
            "errors": sorted({r["task"] + ": " + r["error"] for r in rows if "error" in r}),
        }
        if cert_on:
            # Wrong proofs above already include the pir-certified rows.
            meta["certified"] = certified_summary(rows)
        (args.out / "results.json").write_text(json.dumps(rows, indent=1), encoding="utf-8")
        (args.out / "metrics.json").write_text(json.dumps({"meta": meta, "metrics": metrics}, indent=1),
                                               encoding="utf-8")
        md = markdown(metrics, rows, meta)
        (args.out / "metrics.md").write_text(md, encoding="utf-8")
        print(md)
        if wrong:
            print(f"RELEASE GATE FAILED: {wrong} wrong proof(s)", file=sys.stderr)
            return 1
        return 0
    finally:
        if os.environ.get("PRISM_CONF_KEEP"):
            print(f"work dir kept: {work}", file=sys.stderr)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
