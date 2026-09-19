"""Drive ParanoidBSD portable checkers against the user's sources.

This is not a "tree found" note. When the tree is present the verify
scripts that do not need clang/cbmc/goto-cc are imported and run on
the files Helix was asked to check. onesided_index and capacity_first
also have Helix copies so they work on ordinary C, without FreeBSD
headers or KNF column-0 braces.

Missing clang/goto-cc/cbmc is recorded as NOTRUN, never as a clean sweep.
A missing tree is NOTRUN unless a Helix portable copy already fired.
"""

from __future__ import annotations

from pathlib import Path
import importlib
import importlib.util
import os
import shutil
import sys
from types import ModuleType, SimpleNamespace

from helix import laws
from helix.checkers import run_lints
from helix.config import Config
from helix.models import Finding

C_EXTS = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh"}

# tools/verify modules with a file-level scan that does not need clang.
VERIFY_SCANNERS = (
    "realloc_self",
    "onesided_index",
    "capacity_first",
    "nowait_check",
    "noreturn_check",
    "null_branch",
    "lock_balance",
    "masked_switch_check",
)

# Formal/analyser drivers. Invoking them is someone else's job; silence
# about them is not a proof they passed.
HEAVY = (
    ("clang", "analyze", "pkg install llvm / apt install clang"),
    ("goto-cc", "classify", "pkg install cbmc / apt install cbmc"),
    ("cbmc", "cbmc", "pkg install cbmc / apt install cbmc"),
)

PORTABLE_CLS = {"MEM-ONESIDED-INDEX", "MEM-CAPACITY-FIRST"}


def _looked_root(cfg: Config) -> Path:
    env = os.environ.get("HELIX_PBSD")
    if env:
        return Path(env)
    return Path(cfg.pbsd_root)


def discover_pbsd_root(cfg: Config) -> Path | None:
    """HELIX_PBSD, else cfg.pbsd_root (Desktop ParanoidBSD by default)."""
    cand = _looked_root(cfg)
    if (cand / "tools" / "verify").is_dir():
        return cand
    return None


def _not_run(message: str, install: str, extra: dict | None = None) -> Finding:
    return Finding(
        stage="pbsd", status=laws.NOTRUN, file="", function=None, line=None,
        cls="", message=message, strength=laws.STRENGTH_FINDS,
        extra={"install": install, **(extra or {})},
    )


def _rel(path: Path, cfg: Config) -> str:
    root = cfg.root
    try:
        if root.is_dir():
            return str(path.relative_to(root))
    except ValueError:
        pass
    return path.name


def _c_paths(paths: list[Path]) -> list[Path]:
    return [p for p in paths if p.suffix.lower() in C_EXTS and p.is_file()]


def _dedupe(findings: list[Finding]) -> list[Finding]:
    seen: set[tuple] = set()
    out: list[Finding] = []
    for f in findings:
        key = (f.file, f.line, f.cls, f.message)
        if key in seen:
            continue
        seen.add(key)
        out.append(f)
    return out


def _load_verify(verify: Path, name: str) -> ModuleType | None:
    loc = str(verify)
    if loc not in sys.path:
        sys.path.insert(0, loc)
    try:
        existing = sys.modules.get(name)
        if existing is not None:
            src = getattr(existing, "__file__", "") or ""
            try:
                if Path(src).resolve().parent == verify.resolve():
                    return existing
            except OSError:
                pass
        return importlib.import_module(name)
    except Exception:
        return None


def _helix_portable(paths: list[Path], cfg: Config) -> list[Finding]:
    """Helix copies of onesided_index / capacity_first (no FreeBSD headers)."""
    root = cfg.root if cfg.root.is_dir() else Path(".")
    out: list[Finding] = []
    for f in run_lints(paths, root):
        if f.cls not in PORTABLE_CLS:
            continue
        out.append(Finding(
            stage="pbsd", status=f.status, file=f.file, function=f.function,
            line=f.line, cls=f.cls, message=f.message, strength=f.strength,
            evidence=f.evidence, extra={"via": "helix.portable"},
        ))
    return out


def _run_realloc_self(mod, path: Path, rel: str) -> list[Finding]:
    out = []
    for line, target, src in mod.scan(path):
        out.append(Finding(
            stage="pbsd", status=laws.FAILED, file=rel, function=None,
            line=line, cls="MEM-REALLOC-SELF",
            message=f"{target} = realloc({target}, …) leaks the old block on failure",
            strength=laws.STRENGTH_FINDS, evidence=src,
            extra={"via": "realloc_self.scan"},
        ))
    return out


def _run_onesided_index(mod, path: Path, rel: str) -> list[Finding]:
    out = []
    for kind, ln, fn, v, ty, src in mod.scan(path):
        cls = "MEM-ONESIDED-INDEX" if kind == "ONESIDED" else "MEM-OOB-READ"
        out.append(Finding(
            stage="pbsd", status=laws.FAILED, file=rel, function=fn,
            line=ln, cls=cls,
            message=f"{kind}: {fn}() indexes with signed {v} ({ty})",
            strength=laws.STRENGTH_FINDS, evidence=src,
            extra={"via": "onesided_index.scan"},
        ))
    return out


def _run_capacity_first(mod, path: Path, rel: str) -> list[Finding]:
    out = []
    for line, var, src in mod.scan(path):
        out.append(Finding(
            stage="pbsd", status=laws.FAILED, file=rel, function=None,
            line=line, cls="MEM-CAPACITY-FIRST",
            message=f"{var} grown before the allocation that is supposed to earn it",
            strength=laws.STRENGTH_FINDS, evidence=src,
            extra={"via": "capacity_first.scan"},
        ))
    return out


def _run_nowait(mod, path: Path, rel: str) -> list[Finding]:
    text = path.read_text(encoding="utf-8", errors="replace")
    out = []
    for ln, var, alloc, _use in mod.sites(text.splitlines()):
        out.append(Finding(
            stage="pbsd", status=laws.FAILED, file=rel, function=None,
            line=ln, cls="MEM-NOWAIT",
            message=f"{var} used without a NULL check after M_NOWAIT allocation",
            strength=laws.STRENGTH_FINDS, evidence=alloc,
            extra={"via": "nowait_check.sites"},
        ))
    return out


def _run_noreturn(mod, path: Path, rel: str) -> list[Finding]:
    out = []
    for ln, name, callee in mod.scan(str(path)):
        out.append(Finding(
            stage="pbsd", status=laws.FAILED, file=rel, function=name,
            line=ln, cls="FUNC-NORETURN",
            message=f"{name}() ends in {callee}(), not declared noreturn",
            strength=laws.STRENGTH_FINDS,
            extra={"via": "noreturn_check.scan"},
        ))
    return out


def _run_null_branch(mod, path: Path, rel: str) -> list[Finding]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    raw_lines = raw.split("\n")
    clean = "\n".join(mod.strip_if0(mod.strip_noise(raw).split("\n")))
    out = []
    for ln, name, btext in mod.scan(path, clean, raw_lines):
        out.append(Finding(
            stage="pbsd", status=laws.FAILED, file=rel, function=None,
            line=ln, cls="PTR-NULL-DEREF",
            message=f"{name} dereferenced in the branch that proved it NULL",
            strength=laws.STRENGTH_FINDS, evidence=(btext or "").strip(),
            extra={"via": "null_branch.scan"},
        ))
    return out


def _run_lock_balance(mod, path: Path, rel: str) -> list[Finding]:
    out = []
    for ln, name, arg, head in mod.check(path):
        out.append(Finding(
            stage="pbsd", status=laws.FAILED, file=rel, function=None,
            line=ln, cls="LOCK-IMBALANCE",
            message=f"return holds {name}({arg}) taken in the function at :{head}",
            strength=laws.STRENGTH_FINDS, evidence=f"{name}({arg})",
            extra={"via": "lock_balance.check"},
        ))
    return out


def _run_masked_switch(mod, path: Path, rel: str) -> list[Finding]:
    out = []
    for ln, label, states, cases, escaping, src in mod.sites(path):
        who = escaping[0] if escaping else "?"
        out.append(Finding(
            stage="pbsd", status=laws.FAILED, file=rel, function=None,
            line=ln, cls="UNINIT-SWITCH",
            message=f"masked switch has {cases} arms for {states} states; {who} may escape uninitialised",
            strength=laws.STRENGTH_FINDS, evidence=(src or "")[:120],
            extra={"via": "masked_switch_check.sites", "mask": label},
        ))
    return out


_RUNNERS = {
    "realloc_self": _run_realloc_self,
    "onesided_index": _run_onesided_index,
    "capacity_first": _run_capacity_first,
    "nowait_check": _run_nowait,
    "noreturn_check": _run_noreturn,
    "null_branch": _run_null_branch,
    "lock_balance": _run_lock_balance,
    "masked_switch_check": _run_masked_switch,
}


def _run_imported(root: Path, paths: list[Path], cfg: Config) -> tuple[list[Finding], list[str], list[str]]:
    """Import tools/verify scanners; names that fail to import are ported."""
    verify = root / "tools" / "verify"
    findings: list[Finding] = []
    invoked: list[str] = []
    ported: list[str] = []
    for name in VERIFY_SCANNERS:
        mod = _load_verify(verify, name)
        runner = _RUNNERS.get(name)
        if mod is None or runner is None:
            ported.append(name)
            continue
        invoked.append(name)
        for path in paths:
            rel = _rel(path, cfg)
            try:
                findings.extend(runner(mod, path, rel))
            except Exception as ex:
                findings.append(Finding(
                    stage="pbsd", status=laws.ERROR, file=rel, function=None,
                    line=None, cls="", message=f"{name} failed: {ex}",
                    strength=laws.STRENGTH_FINDS,
                    extra={"via": name},
                ))
    findings.extend(_run_sibling_guard(root, paths, cfg, invoked, ported))
    return findings, invoked, ported


def _run_sibling_guard(
    root: Path, paths: list[Path], cfg: Config,
    invoked: list[str], ported: list[str],
) -> list[Finding]:
    script = root / "tools" / "sibling_guard.py"
    if not script.is_file():
        ported.append("sibling_guard")
        return []
    try:
        spec = importlib.util.spec_from_file_location("helix_pbsd_sibling_guard", script)
        if spec is None or spec.loader is None:
            ported.append("sibling_guard")
            return []
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
    except Exception:
        ported.append("sibling_guard")
        return []
    invoked.append("sibling_guard")
    args = SimpleNamespace(
        only_adjacent=False, only_sibling=False, window=12,
        include_pointers=False, compound_only=False, min="high",
    )
    out: list[Finding] = []
    for path in paths:
        rel = _rel(path, cfg)
        try:
            hits = mod.scan_file(str(path), rel, args)
        except Exception as ex:
            out.append(Finding(
                stage="pbsd", status=laws.ERROR, file=rel, function=None,
                line=None, cls="", message=f"sibling_guard failed: {ex}",
                strength=laws.STRENGTH_FINDS,
                extra={"via": "sibling_guard.scan_file"},
            ))
            continue
        for h in hits:
            unguarded = getattr(h, "unguarded", None)
            if isinstance(unguarded, (tuple, list)) and unguarded:
                unguarded = unguarded[0]
            out.append(Finding(
                stage="pbsd", status=laws.FAILED, file=rel,
                function=unguarded,
                line=getattr(h, "line", None), cls="CTRL-SIBLING-ASYMMETRY",
                message=str(getattr(h, "detail", h)),
                strength=laws.STRENGTH_FINDS,
                extra={"via": "sibling_guard.scan_file",
                       "kind": getattr(h, "kind", "")},
            ))
    return out


def _heavy_notrun() -> list[Finding]:
    out = []
    for binary, tool, how in HEAVY:
        if shutil.which(binary):
            continue
        out.append(_not_run(
            f"{binary} not on PATH (needed for {tool}; not a clean sweep)",
            how, extra={"tool": tool, "via": "missing-bin"},
        ))
    return out


def run_pbsd_lints(paths: list[Path], cfg: Config) -> list[Finding]:
    """Run portable ParanoidBSD checkers on `paths`. Never empty-ok if the tree is missing."""
    files = _c_paths(list(paths))
    looked = _looked_root(cfg)
    root = discover_pbsd_root(cfg)
    portable = _helix_portable(files, cfg)

    if root is None:
        if portable:
            return _dedupe(portable)
        install = f"set HELIX_PBSD to the ParanoidBSD tree (looked at {looked})"
        return [_not_run(
            f"ParanoidBSD tree not found at {looked}",
            install,
            extra={"looked": str(looked)},
        )]

    imported, invoked, ported = _run_imported(root, files, cfg)
    out = _dedupe(imported + portable + _heavy_notrun())
    if not out:
        # Checkers ran over the user's files and found nothing. That is
        # not a missing-clang sweep: heavy bins were on PATH or already
        # recorded above.
        return []
    for f in out:
        f.extra = {**(f.extra or {}), "invoked": invoked, "ported": ported}
    return out
