"""Generative property tests from `// requires:` / `// ensures:`.

RapidCheck-style sampling. CLEAN means every trial held - that is not a
proof, and the finding says so. FAILED is a concrete counterexample.

SCALAR bodies go through helix.concrete.execute first (UB is a failure).
gcc/clang is the fallback when the oracle cannot parse the body.
"""

from __future__ import annotations

from pathlib import Path
import ast
import hashlib
import os
import random
import re
import shutil
import subprocess
import tempfile

from helix import laws
from helix.contracts import ENS_ATOM, REQ_ATOM
from helix.models import Finding, FunctionInfo

_COMMENT_CLAUSE = re.compile(
    r"(?://|/\*|\*)\s*(requires|ensures|invariant|decreases|diff)\s*:\s*(.+?)(?:\*/)?\s*$",
    re.I,
)

try:
    from helix.concrete import execute as concrete_execute  # type: ignore
except Exception:  # pragma: no cover
    concrete_execute = None

DEFAULT_LO = -256
DEFAULT_HI = 256
_EXTRA_ATOM = re.compile(r"^([A-Za-z_]\w*)\s*(<=|>|==)\s*(0|[1-9]\d*|-?[1-9]\d*)$")
_IDENT = re.compile(r"[A-Za-z_]\w*")


def _spec_comments(fn: FunctionInfo) -> dict:
    """requires/ensures on this function only (header..body), not neighbors.

    contracts.parse_comments looks ~12 lines above the header, which pulls in
    the previous function when several contracted fns share a file.
    """
    spec: dict = {"requires": [], "ensures": [], "invariant": [], "decreases": [], "diff": None}
    text = _read_fn_source(fn)
    if not text:
        return spec
    lines = text.splitlines()
    # One line above the header through the closing brace.
    start = max(0, (fn.line or 1) - 2)
    end = fn.span[1] if fn.span and fn.span[1] else (fn.line or 1) + 20
    end = min(len(lines), end)
    for ln in lines[start:end]:
        m = _COMMENT_CLAUSE.search(ln)
        if not m:
            continue
        kind = m.group(1).lower()
        val = m.group(2).strip().rstrip("*/").strip()
        if kind == "diff":
            spec["diff"] = val.split()[0] if val else None
        elif kind in spec:
            spec[kind].append(val)
    return spec


def _read_fn_source(fn: FunctionInfo) -> str:
    if not fn.file:
        return ""
    candidates = [
        Path(fn.file),
        Path.cwd() / fn.file,
        Path(__file__).resolve().parents[1] / "testdata" / Path(fn.file).name,
    ]
    for p in candidates:
        if p.is_file():
            try:
                return p.read_text(encoding="utf-8", errors="replace")
            except OSError:
                return ""
    return ""


def run_rapid(functions: list[FunctionInfo], trials: int = 64) -> list[Finding]:
    """Sample SCALAR args under requires; check ensures. No ensures: skip."""
    out: list[Finding] = []
    for fn in functions:
        rec = check_function(fn, trials)
        if rec is not None:
            out.append(rec)
    return out


def check_function(fn: FunctionInfo, trials: int = 64) -> Finding | None:
    """One finding, or None when the function is out of scope."""
    plan = plan_trials(fn, trials)
    if plan is None:
        return None
    return _finding_from_plan(fn, plan, stage="rapid")


def plan_trials(fn: FunctionInfo, trials: int, rng: random.Random | None = None) -> dict | None:
    """Build samples. None = skip (not SCALAR, or no ensures)."""
    if fn.kind != "SCALAR":
        return None
    spec = _spec_comments(fn)
    ensures = list(spec.get("ensures") or [])
    if not ensures:
        return None
    requires = list(spec.get("requires") or [])
    if rng is None:
        seed = int(hashlib.md5(f"{fn.file}:{fn.name}:{trials}".encode()).hexdigest()[:8], 16)
        rng = random.Random(seed)
    try:
        samples = _sample_args(fn, requires, trials, rng)
    except ValueError as ex:
        return {
            "spec": spec,
            "requires": requires,
            "ensures": ensures,
            "samples": [],
            "error": str(ex),
            "trials": trials,
        }
    return {
        "spec": spec,
        "requires": requires,
        "ensures": ensures,
        "samples": samples,
        "error": None,
        "trials": trials,
    }


def run_plan(fn: FunctionInfo, plan: dict) -> dict:
    """Execute planned samples. Returns ok/fail details (not a Finding)."""
    if plan.get("error"):
        return {"ok": False, "error": plan["error"], "counterexample": "", "engine": ""}
    samples: list[dict[str, int]] = plan["samples"]
    ensures: list[str] = plan["ensures"]
    results, engine, err = _execute_many(fn, samples)
    if err:
        return {"ok": False, "error": err, "counterexample": "", "engine": engine}
    for env, result in zip(samples, results):
        if result is None:
            cex = ", ".join(f"{k}={v}" for k, v in env.items())
            return {
                "ok": False,
                "error": None,
                "counterexample": f"{cex} -> undefined-behavior",
                "engine": engine,
                "clause": "undefined-behavior",
                "result": None,
                "env": env,
            }
        failed = _failing_ensures(ensures, env, result)
        if failed:
            cex = ", ".join(f"{k}={v}" for k, v in env.items())
            return {
                "ok": False,
                "error": None,
                "counterexample": f"{cex} -> result={result}",
                "engine": engine,
                "clause": failed,
                "result": result,
                "env": env,
            }
    return {
        "ok": True,
        "error": None,
        "counterexample": "",
        "engine": engine,
        "n": len(samples),
    }


def _finding_from_plan(fn: FunctionInfo, plan: dict, *, stage: str) -> Finding:
    base = dict(
        stage=stage,
        file=fn.file,
        function=fn.name,
        line=fn.line,
    )
    extra = {
        "requires": plan.get("requires") or [],
        "ensures": plan.get("ensures") or [],
        "trials": plan.get("trials"),
        "sampled": True,
    }
    if plan.get("error") and not plan.get("samples"):
        return Finding(
            **base,
            status=laws.ERROR,
            cls="",
            message=plan["error"],
            strength=laws.STRENGTH_SOME,
            extra=extra,
        )
    info = run_plan(fn, plan)
    extra["engine"] = info.get("engine") or ""
    if info.get("error") and not info.get("counterexample"):
        return Finding(
            **base,
            status=laws.ERROR,
            cls="",
            message=info["error"],
            strength=laws.STRENGTH_SOME,
            extra=extra,
        )
    if not info.get("ok"):
        clause = info.get("clause") or " && ".join(plan["ensures"])
        return Finding(
            **base,
            status=laws.FAILED,
            cls="FUNC-CONTRACT",
            message=f"ensures ({clause}) failed on {info['counterexample']}",
            strength=laws.STRENGTH_FINDS,
            evidence=fn.body.strip()[:400],
            counterexample=str(info.get("counterexample") or ""),
            extra=extra,
        )
    ens = " && ".join(plan["ensures"])
    n = info.get("n") or len(plan["samples"])
    return Finding(
        **base,
        status=laws.CLEAN,
        cls="",
        message=(
            f"all {n} trials hold for ensures ({ens}); "
            "not a proof - sampling is not forall"
        ),
        strength=laws.STRENGTH_SOME,
        extra=extra,
    )


def _sample_args(
    fn: FunctionInfo,
    requires: list[str],
    trials: int,
    rng: random.Random,
) -> list[dict[str, int]]:
    bounds = _bounds(fn, requires)
    for name, b in bounds.items():
        if b["lo"] > b["hi"]:
            raise ValueError(f"empty domain for {name} after requires")
        viable = b["hi"] - b["lo"] + 1 - sum(1 for x in b["neq"] if b["lo"] <= x <= b["hi"])
        if viable <= 0:
            raise ValueError(f"empty domain for {name} after requires")
    names = [n for _, n in fn.params if n]
    interesting = [0, 1, -1, 2, -2, 42, 99, 100, 127, -128, 255, -256]
    samples: list[dict[str, int]] = []
    # Prefer in-range interesting values, then uniform.
    for _ in range(max(0, trials)):
        env: dict[str, int] = {}
        for name in names:
            b = bounds[name]
            env[name] = _pick(rng, b, interesting if len(samples) < 12 else None)
        samples.append(env)
    return samples


def _bounds(fn: FunctionInfo, requires: list[str]) -> dict[str, dict]:
    out: dict[str, dict] = {}
    for typ, name in fn.params:
        if not name:
            continue
        lo, hi = DEFAULT_LO, DEFAULT_HI
        if _unsigned(typ):
            lo = 0
        out[name] = {"lo": lo, "hi": hi, "neq": set()}
    for atom in requires:
        _apply_atom(out, atom)
    return out


def _unsigned(typ: str) -> bool:
    t = " ".join(typ.lower().replace("*", " ").split())
    return "unsigned" in t or t.startswith("uint") or t in {"size_t", "_bool", "bool"}


def _apply_atom(bounds: dict[str, dict], atom: str) -> None:
    s = atom.strip()
    m = REQ_ATOM.match(s) or _EXTRA_ATOM.match(s)
    if not m:
        return
    name, op, raw = m.group(1), m.group(2), int(m.group(3))
    if name not in bounds:
        bounds[name] = {"lo": DEFAULT_LO, "hi": DEFAULT_HI, "neq": set()}
    b = bounds[name]
    if op == "<":
        b["hi"] = min(b["hi"], raw - 1)
    elif op == "<=":
        b["hi"] = min(b["hi"], raw)
    elif op == ">":
        b["lo"] = max(b["lo"], raw + 1)
    elif op == ">=":
        b["lo"] = max(b["lo"], raw)
    elif op == "!=":
        b["neq"].add(raw)
    elif op == "==":
        b["lo"] = max(b["lo"], raw)
        b["hi"] = min(b["hi"], raw)


def _pick(rng: random.Random, b: dict, interesting: list[int] | None) -> int:
    lo, hi, neq = b["lo"], b["hi"], b["neq"]
    if interesting:
        cand = [v for v in interesting if lo <= v <= hi and v not in neq]
        if cand:
            return rng.choice(cand)
    for _ in range(64):
        v = rng.randint(lo, hi)
        if v not in neq:
            return v
    for v in range(lo, hi + 1):
        if v not in neq:
            return v
    raise ValueError("empty domain")


def _failing_ensures(ensures: list[str], env: dict[str, int], result: int) -> str | None:
    full = dict(env)
    full["result"] = result
    for clause in ensures:
        s = clause.strip()
        m = ENS_ATOM.match(s)
        if m:
            op, rhs = m.group(1), m.group(2)
            try:
                rv = _eval_c_expr(rhs, full)
            except Exception:
                if not _eval_c_expr(s, full):
                    return clause
                continue
            if op == "==" and result != rv:
                return clause
            if op == ">=" and not (result >= rv):
                return clause
            continue
        try:
            if not _eval_c_expr(s, full):
                return clause
        except Exception:
            return clause
    return None


def _execute_many(
    fn: FunctionInfo,
    samples: list[dict[str, int]],
) -> tuple[list[int | None], str, str]:
    if not samples:
        return [], "", ""
    if concrete_execute is not None:
        try:
            out: list[int | None] = []
            ub_err = ""
            for env in samples:
                rec = concrete_execute(fn, env)
                if rec.error:
                    ub_err = rec.error
                    break
                if rec.ub:
                    out.append(None)
                    continue
                out.append(0 if rec.value is None else int(rec.value))
            else:
                return out, "concrete", ""
            if not ub_err:
                return out, "concrete", ""
        except Exception:
            pass
    ok, results, err = _execute_gcc(fn, samples)
    if ok:
        return results, "gcc", ""
    try:
        out = [_interpret(fn.body, env) for env in samples]
        return out, "interp", ""
    except Exception as ex:
        msg = err or str(ex)
        return [], "interp", f"cannot evaluate {fn.name}: {msg[:400]}"


def _execute_gcc(
    fn: FunctionInfo,
    samples: list[dict[str, int]],
) -> tuple[bool, list[int], str]:
    cc = shutil.which("gcc") or shutil.which("clang")
    if not cc:
        return False, [], "no gcc/clang on PATH"
    names = [n for _, n in fn.params if n]
    src = _harness_source(fn)
    try:
        with tempfile.TemporaryDirectory(prefix="helix_rapid_") as td:
            td_path = Path(td)
            cfile = td_path / f"{fn.name}.c"
            exe = td_path / f"{fn.name}.exe"
            cfile.write_text(src, encoding="utf-8")
            cmd = [cc, "-O0", "-std=c11", str(cfile), "-o", str(exe)]
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
            if p.returncode != 0:
                return False, [], (p.stderr or p.stdout)[-800:]
            lines = [" ".join(str(env[n]) for n in names) for env in samples]
            payload = "\n".join(lines) + "\n"
            env = os.environ.copy()
            env["MSYSTEM"] = env.get("MSYSTEM", "")
            r = subprocess.run(
                [str(exe)],
                input=payload,
                capture_output=True,
                text=True,
                timeout=10,
                env=env,
            )
            if r.returncode != 0:
                return False, [], (r.stderr or r.stdout)[-800:] or f"exit {r.returncode}"
            out: list[int] = []
            for ln in (r.stdout or "").splitlines():
                ln = ln.strip()
                if not ln:
                    continue
                out.append(int(ln, 10))
            if len(out) != len(samples):
                return False, [], f"expected {len(samples)} results, got {len(out)}"
            return True, out, ""
    except (subprocess.TimeoutExpired, OSError, ValueError) as ex:
        return False, [], str(ex)


def _harness_source(fn: FunctionInfo) -> str:
    args = ", ".join(
        f"{(t.strip() or 'int')} {n}" for t, n in fn.params if n
    ) or "void"
    call = ", ".join(n for _, n in fn.params if n)
    scans = " ".join("%d" for _, n in fn.params if n) or "%d"
    addrs = ", ".join(f"&{n}" for _, n in fn.params if n) or "&_unused"
    decls = "\n".join(
        f"        {(t.strip() or 'int')} {n};" for t, n in fn.params if n
    ) or "        int _unused;"
    ret = (fn.return_type or "int").replace("*", "").strip() or "int"
    nexpect = max(1, sum(1 for _, n in fn.params if n))
    return f'''#include <stdio.h>
#include <stdint.h>
{ret} {fn.name}({args}) {{
{fn.body}
}}
int main(void) {{
    for (;;) {{
{decls}
        if (scanf("{scans}", {addrs}) != {nexpect}) break;
        printf("%d\\n", (int){fn.name}({call}));
        fflush(stdout);
    }}
    return 0;
}}
'''


# --- tiny SCALAR interpreter (return / if / return) ---

def _eval_c_expr(expr: str, env: dict[str, int]) -> int:
    src = expr.strip()
    if not src:
        raise ValueError("empty expression")
    src = src.replace("&&", " and ").replace("||", " or ")
    src = re.sub(r"(?<![\w=])!(?!=)", " not ", src)
    tree = ast.parse(src, mode="eval")
    return int(_eval_ast(tree.body, env))


def _eval_ast(node: ast.AST, env: dict[str, int]) -> int:
    if isinstance(node, ast.Constant):
        if isinstance(node.value, bool):
            return int(node.value)
        if isinstance(node.value, (int, float)):
            return int(node.value)
        raise ValueError("unsupported constant")
    if isinstance(node, ast.Name):
        if node.id not in env:
            raise ValueError(f"unknown name {node.id}")
        return int(env[node.id])
    if isinstance(node, ast.UnaryOp):
        v = _eval_ast(node.operand, env)
        if isinstance(node.op, ast.UAdd):
            return +v
        if isinstance(node.op, ast.USub):
            return -v
        if isinstance(node.op, ast.Not):
            return int(not v)
        raise ValueError("unsupported unary")
    if isinstance(node, ast.BinOp):
        a = _eval_ast(node.left, env)
        b = _eval_ast(node.right, env)
        if isinstance(node.op, ast.Add):
            return a + b
        if isinstance(node.op, ast.Sub):
            return a - b
        if isinstance(node.op, ast.Mult):
            return a * b
        if isinstance(node.op, (ast.Div, ast.FloorDiv)):
            if b == 0:
                raise ZeroDivisionError("division by zero")
            return int(a / b)
        if isinstance(node.op, ast.Mod):
            if b == 0:
                raise ZeroDivisionError("modulo by zero")
            return a % b
        raise ValueError("unsupported binop")
    if isinstance(node, ast.BoolOp):
        if isinstance(node.op, ast.And):
            for v in node.values:
                if not _eval_ast(v, env):
                    return 0
            return 1
        if isinstance(node.op, ast.Or):
            for v in node.values:
                if _eval_ast(v, env):
                    return 1
            return 0
    if isinstance(node, ast.Compare):
        left = _eval_ast(node.left, env)
        for op, rhs_n in zip(node.ops, node.comparators):
            right = _eval_ast(rhs_n, env)
            if isinstance(op, ast.Eq):
                ok = left == right
            elif isinstance(op, ast.NotEq):
                ok = left != right
            elif isinstance(op, ast.Lt):
                ok = left < right
            elif isinstance(op, ast.LtE):
                ok = left <= right
            elif isinstance(op, ast.Gt):
                ok = left > right
            elif isinstance(op, ast.GtE):
                ok = left >= right
            else:
                raise ValueError("unsupported compare")
            if not ok:
                return 0
            left = right
        return 1
    if isinstance(node, ast.IfExp):
        return _eval_ast(node.body if _eval_ast(node.test, env) else node.orelse, env)
    raise ValueError(f"unsupported expression {type(node).__name__}")


def _interpret(body: str, env: dict[str, int]) -> int:
    """Subset: `return x+1`, `return x`, if/return. Locals are ints."""
    val = _exec_block(body, dict(env))
    if val is None:
        raise ValueError("function did not return")
    return val


def _is_kw(text: str, i: int, kw: str) -> bool:
    n = len(kw)
    if not text.startswith(kw, i):
        return False
    if i > 0 and _IDENT.match(text[i - 1]):
        return False
    if i + n < len(text) and _IDENT.match(text[i + n]):
        return False
    return True


def _skip_ws(text: str, i: int) -> int:
    n = len(text)
    while i < n and text[i].isspace():
        i += 1
    return i


def _match_pair(text: str, open_idx: int, open_ch: str, close_ch: str) -> int:
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        c = text[i]
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _parse_stmt(text: str, start: int) -> tuple[str, int]:
    i = _skip_ws(text, start)
    if i >= len(text):
        return "", i
    if text[i] == "{":
        j = _match_pair(text, i, "{", "}")
        if j < 0:
            raise ValueError("unbalanced {")
        return text[i + 1 : j], j + 1
    if _is_kw(text, i, "if"):
        p = text.find("(", i)
        if p < 0:
            raise ValueError("if without (")
        q = _match_pair(text, p, "(", ")")
        then_body, then_end = _parse_stmt(text, q + 1)
        rest = _skip_ws(text, then_end)
        if _is_kw(text, rest, "else"):
            else_body, else_end = _parse_stmt(text, rest + 4)
            return text[i:else_end], else_end
        return text[i:then_end], then_end
    k = text.find(";", i)
    if k < 0:
        return text[i:], len(text)
    return text[i : k + 1], k + 1


def _exec_block(text: str, env: dict[str, int]) -> int | None:
    i = 0
    n = len(text)
    while i < n:
        i = _skip_ws(text, i)
        if i >= n:
            break
        if text[i] == "{":
            j = _match_pair(text, i, "{", "}")
            if j < 0:
                raise ValueError("unbalanced {")
            got = _exec_block(text[i + 1 : j], env)
            if got is not None:
                return got
            i = j + 1
            continue
        if _is_kw(text, i, "return"):
            k = text.find(";", i)
            if k < 0:
                raise ValueError("return without ;")
            return _eval_c_expr(text[i + 6 : k], env)
        if _is_kw(text, i, "if"):
            p = text.find("(", i)
            if p < 0:
                raise ValueError("if without (")
            q = _match_pair(text, p, "(", ")")
            cond = _eval_c_expr(text[p + 1 : q], env)
            then_body, then_end = _parse_stmt(text, q + 1)
            else_body = None
            rest = _skip_ws(text, then_end)
            if _is_kw(text, rest, "else"):
                else_body, rest = _parse_stmt(text, rest + 4)
            chosen = then_body if cond else (else_body or "")
            if chosen:
                got = _exec_block(chosen, env)
                if got is not None:
                    return got
            i = rest
            continue
        decl = re.match(
            r"(?:(?:unsigned|signed|const|static)\s+)*(?:int|long|short|char)\s+"
            r"([A-Za-z_]\w*)\s*(?:=\s*([^;]+))?;",
            text[i:],
        )
        if decl:
            name = decl.group(1)
            if decl.group(2) is not None:
                env[name] = _eval_c_expr(decl.group(2), env)
            else:
                env[name] = 0
            i += decl.end()
            continue
        assign = re.match(r"([A-Za-z_]\w*)\s*=\s*([^;]+);", text[i:])
        if assign:
            env[assign.group(1)] = _eval_c_expr(assign.group(2), env)
            i += assign.end()
            continue
        k = text.find(";", i)
        if k < 0:
            break
        i = k + 1
    return None
