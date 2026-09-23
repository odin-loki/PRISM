#!/usr/bin/env python3
"""Random-program soundness testing for PRISM (roadmap 5.5 / 6.2).

Generate small C programs, run PRISM's verdict stages on them, and for every
function PRISM *proves* (PROVED / PROVED-UNBOUNDED / PROVED-ASSUMING /
PROVED-CERTIFIED) execute it concretely under UBSan+ASan on an edge-value
grid plus random inputs. Any sanitizer report on a proved function is a
soundness bug: the repro (program, function, inputs, sanitizer message) is
written to OUT/bugs/ and printed.

Refutations are checked too: a FAILED counterexample is replayed, and a
FAILED function on which neither the counterexample nor the input grid
triggers the sanitizer is listed as a suspected false alarm.

Generators:
  inhouse-ptr  pointer programs for the pir memory model: stack/heap int
           arrays, masked or reduced indices, pointer walks, memcpy, free
           (a minority reach one past the end or use after free).
  inhouse  (default) a targeted generator for the C subset PRISM claims to
           model: int/unsigned/short/char/long long parameters, locals,
           + - * / % << >> & | ^ ~ ! unary minus, casts, ternaries, if/else,
           early-return guards, bounded for loops and compound assignment.
           Guards and masks are biased so that many functions are UB-free
           (so PRISM proves them) while a minority hide UB behind edge cases.
  csmith   Csmith 2.3 (apt install csmith) in a scalar-only configuration
           (no pointers/structs/arrays/globals, --no-safe-math so UB can
           occur). Only functions with scalar parameters are driven.

Only programs this tool generates are compiled and run (inside bwrap when
available), never code from elsewhere.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import importlib.util
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

_HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("prism_conformance", _HERE / "conformance.py")
assert _spec and _spec.loader
conf = importlib.util.module_from_spec(_spec)
sys.modules["prism_conformance"] = conf
_spec.loader.exec_module(conf)

PROOF = conf.PROOF

# --------------------------------------------------------------------------- in-house generator

TYPES = ["int", "int", "int", "int", "unsigned", "short", "unsigned short", "signed char",
         "unsigned char", "long long"]
EDGE_CONSTS = [0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 100, 255, 1000, 46340, 46341, 65535, 65536,
               1000000, 2147483647]


@dataclass
class Var:
    name: str
    typ: str


class Gen:
    def __init__(self, rng: random.Random):
        self.r = rng
        self.lines: list[str] = []

    def const(self) -> str:
        r = self.r.random()
        if r < 0.6:
            return str(self.r.randint(0, 20))
        if r < 0.9:
            return str(self.r.choice(EDGE_CONSTS))
        return f"({-self.r.randint(1, 100)})"

    def leaf(self, env: list[Var]) -> str:
        if env and self.r.random() < 0.7:
            return self.r.choice(env).name
        return self.const()

    def expr(self, env: list[Var], depth: int) -> str:
        if depth <= 0 or self.r.random() < 0.25:
            return self.leaf(env)
        k = self.r.random()
        a = self.expr(env, depth - 1)
        if k < 0.08:
            return f"(-{a})"
        if k < 0.12:
            return f"(~{a})"
        if k < 0.15:
            return f"(!{a})"
        if k < 0.23:
            t = self.r.choice(["int", "short", "unsigned", "unsigned char", "signed char", "long long",
                               "unsigned short"])
            return f"(({t}){a})"
        if k < 0.30:
            c = self.cond(env, depth - 1)
            return f"({c} ? {a} : {self.expr(env, depth - 1)})"
        b = self.expr(env, depth - 1)
        op = self.r.choice(["+", "+", "-", "-", "*", "*", "/", "%", "<<", ">>", "&", "|", "^"])
        if op in ("/", "%"):
            s = self.r.random()
            if s < 0.45:
                b = f"({b} | 1)"
            elif s < 0.75:
                b = f"(({b} & 15) + 1)"
        elif op in ("<<", ">>"):
            s = self.r.random()
            if s < 0.5:
                b = f"({b} & {self.r.choice([7, 15, 31])})"
            elif s < 0.8:
                b = str(self.r.randint(0, 31))
            if op == "<<" and self.r.random() < 0.6:
                a = f"({a} & {self.r.choice([1, 3, 15, 255, 65535])})"
        elif op == "*" and self.r.random() < 0.6:
            a = f"({a} & {self.r.choice([255, 1023, 65535])})"
        return f"({a} {op} {b})"

    def cond(self, env: list[Var], depth: int) -> str:
        a = self.expr(env, max(0, depth - 1))
        op = self.r.choice(["<", ">", "<=", ">=", "==", "!="])
        return f"({a} {op} {self.const()})"

    def guard(self, v: Var, ind: str) -> str:
        lo = self.r.choice([0, -10, -100, -1000, -46340])
        hi = self.r.choice([10, 100, 1000, 46340, 65535])
        return f"{ind}if ({v.name} < {lo} || {v.name} > {hi}) return 0;"

    def stmt(self, env: list[Var], locals_: list[Var], ind: str, depth: int, loop_ok: bool) -> list[str]:
        k = self.r.random()
        if k < 0.35 and locals_:
            v = self.r.choice(locals_)
            op = self.r.choice(["=", "+=", "-=", "*=", "^=", "|=", "&=", "/=", "%=", "<<=", ">>="])
            rhs = self.expr(env, 2)
            if op in ("/=", "%="):
                rhs = f"(({rhs} & 7) + 1)"
            if op in ("<<=", ">>="):
                rhs = f"({rhs} & 15)"
            return [f"{ind}{v.name} {op} {rhs};"]
        if k < 0.45 and locals_:
            v = self.r.choice(locals_)
            return [f"{ind}{v.name}{self.r.choice(['++', '--'])};"]
        if k < 0.70 and depth > 0:
            out = [f"{ind}if {self.cond(env, 2)} {{"]
            for _ in range(self.r.randint(1, 2)):
                out += self.stmt(env, locals_, ind + "    ", depth - 1, loop_ok)
            if self.r.random() < 0.5:
                out.append(f"{ind}}} else {{")
                out += self.stmt(env, locals_, ind + "    ", depth - 1, loop_ok)
            out.append(f"{ind}}}")
            return out
        if k < 0.85 and depth > 0 and loop_ok and locals_:
            i = f"i{self.r.randint(0, 999)}"
            bound = self.r.randint(1, 6)
            v = self.r.choice(locals_)
            inner = env + [Var(i, "int")]
            return [
                f"{ind}for (int {i} = 0; {i} < {bound}; {i}++) {{",
                f"{ind}    {v.name} += {self.expr(inner, 1)};",
                f"{ind}}}",
            ]
        return [self.guard(self.r.choice(env), ind)] if env else []

    def function(self, name: str) -> str:
        params = [Var(f"p{i}", self.r.choice(TYPES)) for i in range(self.r.randint(1, 3))]
        env = list(params)
        body: list[str] = []
        for p in params:
            if self.r.random() < 0.6:
                body.append(self.guard(p, "    "))
        locals_: list[Var] = []
        for j in range(self.r.randint(1, 3)):
            t = self.r.choice(["int", "int", "unsigned", "long long"])
            v = Var(f"v{j}", t)
            body.append(f"    {t} {v.name} = {self.expr(env, 2)};")
            env.append(v)
            locals_.append(v)
        for _ in range(self.r.randint(1, 4)):
            body += self.stmt(env, locals_, "    ", 2, True)
        body.append(f"    return (int)({self.expr(env, 2)});")
        sig = ", ".join(f"{p.typ} {p.name}" for p in params)
        return f"int {name}({sig}) {{\n" + "\n".join(body) + "\n}\n"


class PtrGen(Gen):
    """Pointer programs for the pir memory model (docs/PIR.md "Memory model"):
    a stack or heap int array, indexed reads/writes, pointer walks, memcpy
    between arrays, free. Indices are masked or reduced so that most
    functions are memory-safe; a minority reach one past the end or use the
    heap array after free. Parameters stay scalar so functions can be driven."""

    def index(self, env: list[Var], n: int, bug: bool) -> str:
        e = self.expr(env, 1)
        if bug:
            return f"((unsigned)({e}) % {n + 1}u)"
        if n & (n - 1) == 0:
            return f"(({e}) & {n - 1})"
        return f"((unsigned)({e}) % {n}u)"

    def function(self, name: str) -> str:
        params = [Var(f"p{i}", self.r.choice(["int", "int", "unsigned", "short"])) for i in range(self.r.randint(1, 3))]
        env = list(params)
        body: list[str] = []
        for p in params:
            if self.r.random() < 0.5:
                body.append(self.guard(p, "    "))
        n = self.r.randint(2, 8)
        m = self.r.randint(2, 8)
        heap = self.r.random() < 0.35
        if heap:
            body += [f"    int *a = malloc({n} * sizeof(int));", "    if (!a) return 0;"]
        else:
            body.append(f"    int a[{n}];")
        body.append(f"    int b[{m}];")
        body.append(f"    for (int k = 0; k < {n}; k++) a[k] = k * {self.r.randint(1, 9)};")
        body.append(f"    for (int k = 0; k < {m}; k++) b[k] = {self.r.randint(0, 9)};")
        body.append("    int v = 0;")
        for _ in range(self.r.randint(2, 5)):
            bug = self.r.random() < 0.12
            k = self.r.random()
            if k < 0.35:
                body.append(f"    v += a[{self.index(env, n, bug)}];")
            elif k < 0.6:
                body.append(f"    a[{self.index(env, n, bug)}] = {self.r.randint(0, 50)};")
            elif k < 0.8:
                body.append(f"    {{ int *q = a + {self.index(env, n, bug)}; v ^= *q; }}")
            else:
                cap = min(n, m) + (1 if bug else 0)
                body.append(f"    memcpy(b, a, ((unsigned)({self.expr(env, 1)}) % {cap + 1}u) * sizeof(int));")
                body.append("    v += b[0];")
        if heap:
            body.append("    free(a);")
            if self.r.random() < 0.1:
                body.append("    v += a[0];")
        body.append("    return v;")
        sig = ", ".join(f"{p.typ} {p.name}" for p in params)
        return f"int {name}({sig}) {{\n" + "\n".join(body) + "\n}\n"


def gen_inhouse_ptr(seed: int, nfuncs: int) -> str:
    g = PtrGen(random.Random(seed))
    parts = [f"/* PRISM random soundness program, pointer generator, seed {seed} */\n"
             "#include <stdlib.h>\n#include <string.h>\n"]
    for k in range(nfuncs):
        parts.append(g.function(f"rf{seed}_{k}"))
    return "\n".join(parts)


def gen_inhouse(seed: int, nfuncs: int) -> str:
    g = Gen(random.Random(seed))
    parts = [f"/* PRISM random soundness program, in-house generator, seed {seed} */\n"]
    for k in range(nfuncs):
        parts.append(g.function(f"rf{seed}_{k}"))
    return "\n".join(parts)


# --------------------------------------------------------------------------- csmith

CSMITH_FLAGS = ["--no-pointers", "--no-structs", "--no-unions", "--no-arrays", "--no-safe-math",
                "--no-volatiles", "--no-bitfields", "--no-global-variables", "--no-checksum", "--nomain",
                "--max-funcs", "3", "--no-argc", "--no-jumps", "--no-float", "--no-packed-struct",
                "--max-block-depth", "3", "--max-expr-complexity", "5", "--no-inline-function", "--concise"]
CSMITH_H = """#ifndef PRISM_CSMITH_STUB_H
#define PRISM_CSMITH_STUB_H
/* Minimal stand-in for csmith.h: scalar-only programs need only stdint. */
#include <stdint.h>
#include <stdio.h>
#endif
"""


def gen_csmith(seed: int, work: Path) -> str | None:
    exe = shutil.which("csmith")
    if not exe:
        return None
    out = work / f"cs{seed}.c"
    # csmith drops platform.info into its cwd: keep it in the work dir.
    r = subprocess.run([exe, "--seed", str(seed), *CSMITH_FLAGS, "-o", str(out)], capture_output=True,
                       text=True, timeout=120, cwd=work)
    if r.returncode != 0 or not out.exists():
        return None
    text = out.read_text(encoding="utf-8", errors="replace")
    # csmith's helpers are `static`; make them visible to the driver's #include (same TU anyway).
    return text.replace('#include "csmith.h"', CSMITH_H)


# --------------------------------------------------------------------------- campaign


def analyse(program: Path, fns: list[str], prism_cmd: list[str], stages: list[str], work: Path,
            timeout: float) -> dict[str, Any]:
    task = conf.Task(ident=program.name, yml=program, source=program, origin="random", category="random",
                     lang="C", prop="", expected={f: True for f in fns})
    res = conf.run_prism(prism_cmd, task, stages, work, timeout, None)
    rec: dict[str, Any] = {"program": str(program), "functions": {}}
    if "error" in res:
        rec["error"] = res["error"]
        return rec
    for fn in fns:
        found = res["findings"].get("bmc", {}).get(fn, [])
        for extra in ("pir",):
            found += res["findings"].get(extra, {}).get(fn, [])
        rec["functions"][fn] = found
    return rec


def check_function(program: Path, fn: str, found: list[dict[str, Any]], work: Path, seed: int) -> dict[str, Any]:
    task = conf.Task(ident=program.name, yml=program, source=program, origin="prism", category="random",
                     lang="C", prop="", expected={fn: True})
    statuses = sorted({f["status"] for f in found})
    out: dict[str, Any] = {"function": fn, "statuses": statuses}
    params = conf.scalar_params(task, fn)
    if params is None:
        out["exec"] = "unsupported"
        return out
    wd = work / "exec" / program.stem / fn
    grid = conf.input_grid(params, n_random=3000, seed=seed)
    ex = conf.run_sanitized(task, fn, params, grid, wd / "grid", timeout=60)
    out["grid"] = {"outcome": ex.outcome, "detail": ex.detail}
    if "FAILED" in statuses:
        cex = next((f.get("counterexample") or "" for f in found if f["status"] == "FAILED"), "")
        rp = conf.replay(task, fn, cex, wd)
        out["replay"] = rp
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-n", "--programs", type=int, default=300)
    ap.add_argument("--funcs", type=int, default=3, help="functions per in-house program")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--generator", choices=["inhouse", "inhouse-ptr", "csmith"], default="inhouse")
    ap.add_argument("--prism", help="PRISM binary (default: $PRISM_BIN, else python -m prism)")
    ap.add_argument("--out", type=Path, default=Path("soundness-out"))
    ap.add_argument("--jobs", "-j", type=int, default=max(1, (os.cpu_count() or 2)))
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args(argv)

    if args.generator == "csmith" and not shutil.which("csmith"):
        print("csmith: NOTRUN (not on PATH; apt install csmith). Use --generator inhouse.", file=sys.stderr)
        return 3
    cmd, engine = conf.prism_command(args)
    listed = conf.list_stages(cmd)
    if not listed:
        print(f"cannot run PRISM: {' '.join(cmd)}", file=sys.stderr)
        return 3
    stages = [s for s in ["inventory", "classify", "bmc", "pir"] if s in listed]

    args.out.mkdir(parents=True, exist_ok=True)
    progs_dir = args.out / "programs"
    progs_dir.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="prism-rand-"))
    try:
        programs: list[tuple[Path, list[str]]] = []
        for k in range(args.programs):
            seed = args.seed + k
            if args.generator in ("inhouse", "inhouse-ptr"):
                gen = gen_inhouse if args.generator == "inhouse" else gen_inhouse_ptr
                text = gen(seed, args.funcs)
                fns = re.findall(r"(?m)^int (rf\d+_\d+)\(", text)
            else:
                text = gen_csmith(seed, work) or ""
                fns = [m for m in re.findall(r"(?m)^static \w+\s+(func_\d+)\(", text)]
                if not text:
                    continue
            p = progs_dir / f"{args.generator}_{seed}.c"
            p.write_text(text, encoding="utf-8")
            programs.append((p, sorted(set(fns))))

        with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            analysed = list(ex.map(lambda pf: analyse(pf[0], pf[1], cmd, stages, work, args.timeout), programs))

        jobs = []
        for (p, _), rec in zip(programs, analysed):
            for fn, found in rec.get("functions", {}).items():
                st = {f["status"] for f in found}
                if st & PROOF or "FAILED" in st:
                    jobs.append((p, fn, found))
        with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            checks = list(ex.map(lambda j: (j[0], check_function(j[0], j[1], j[2], work, args.seed)), jobs))

        tally: dict[str, int] = {}
        for rec in analysed:
            for fn, found in rec.get("functions", {}).items():
                key = "/".join(sorted({f["status"] for f in found})) or "MISSING"
                tally[key] = tally.get(key, 0) + 1
            if "error" in rec:
                tally["RUN-ERROR"] = tally.get("RUN-ERROR", 0) + 1
        bugs, alarms, proofs_checked, fails_replayed, fails = [], [], 0, 0, 0
        for p, c in checks:
            st = set(c["statuses"])
            if c.get("exec") == "unsupported":
                continue
            if st & PROOF:
                proofs_checked += 1
                if c["grid"]["outcome"] == "ub":
                    bugs.append((p, c))
            if "FAILED" in st:
                fails += 1
                if c.get("replay", {}).get("replay") == "replayed":
                    fails_replayed += 1
                elif c["grid"]["outcome"] != "ub":
                    alarms.append((p, c))
        summary = {
            "engine": engine, "command": cmd, "stages": stages, "generator": args.generator,
            "programs": len(programs), "functions": sum(len(f) for _, f in programs),
            "verdicts": tally, "proofs_executed": proofs_checked, "wrong_proofs": len(bugs),
            "failed": fails, "failed_cex_replayed": fails_replayed, "suspected_false_alarms": len(alarms),
            "sandbox": "bwrap" if conf.bwrap_ok() else "none",
        }
        bugdir = args.out / "bugs"
        bugdir.mkdir(exist_ok=True)
        lines = ["# PRISM random-program soundness campaign", "", "```", json.dumps(summary, indent=1), "```", ""]
        lines.append(f"## Wrong proofs ({len(bugs)})")
        for p, c in bugs:
            fn = c["function"]
            shutil.copy(p, bugdir / p.name)
            lines.append(f"- `{p.name}` `{fn}` {'/'.join(c['statuses'])}: {c['grid']['detail']}")
            src = p.read_text(encoding="utf-8")
            m = re.search(r"(?ms)^int " + re.escape(fn) + r"\(.*?^\}", src)
            if m:
                lines += ["", "```c", m.group(0), "```", ""]
        lines.append(f"\n## Suspected false alarms ({len(alarms)})")
        for p, c in alarms:
            rp = c.get("replay", {})
            lines.append(f"- `{p.name}` `{c['function']}` FAILED; cex replay {rp.get('replay')} "
                         f"inputs={rp.get('inputs')}; grid {c['grid']['outcome']}")
        (args.out / "summary.json").write_text(
            json.dumps({"summary": summary, "checks": [{"program": str(p), **c} for p, c in checks]}, indent=1),
            encoding="utf-8")
        (args.out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        print("\n".join(lines))
        return 1 if bugs else 0
    finally:
        if os.environ.get("PRISM_CONF_KEEP"):
            print(f"work dir kept: {work}", file=sys.stderr)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
