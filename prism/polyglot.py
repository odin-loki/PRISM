"""Polyglot stage: every language in the tree, not just C/C++.

Two halves:

1. Built-in scans over every text source (C/C++ included): VCS conflict
   markers and leaked credentials. These need no tool and always run.
2. Per-language checkers (syntax first, then linters / type checkers). Each
   checker is a group of interchangeable tools; the first one found runs.
   A language present in the tree with no tool for a group is NOTRUN with an
   install line — never CLEAN.

Status mapping (same as the C++ engine, src/prism/polyglot.cpp):
  diagnostic            -> FAILED   (strength FINDS; extra.severity/rule/tool)
  tool ran, silent      -> UNKNOWN  ("no diagnostics (not a proof)")
  tool crashed/unusable -> ERROR    (tail of its output)
  tool timed out        -> TIMEOUT
  tool missing          -> NOTRUN   (extra.install)
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

from prism import laws
from prism.config import Config, resolve_adapter
from prism.models import Finding

STAGE = "polyglot"

# Extension -> language. C/C++ stay with the C pipeline; they are only
# scanned by the built-in half here.
LANG_EXTS: dict[str, str] = {
    ".py": "python", ".pyi": "python",
    ".js": "javascript", ".mjs": "javascript", ".cjs": "javascript", ".jsx": "javascript",
    ".ts": "typescript", ".tsx": "typescript", ".mts": "typescript", ".cts": "typescript",
    ".sh": "shell", ".bash": "shell",
    ".go": "go",
    ".rs": "rust",
    ".rb": "ruby",
    ".php": "php",
    ".pl": "perl", ".pm": "perl",
    ".lua": "lua",
    ".json": "json",
    ".toml": "toml",
    ".yml": "yaml", ".yaml": "yaml",
}

C_FAMILY_EXTS = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".hxx", ".cu"}

# Extra text files the built-in scan reads (credentials land in these).
TEXT_ONLY_EXTS = {".env", ".ini", ".cfg", ".conf", ".properties", ".xml", ".java",
                  ".kt", ".cs", ".swift", ".scala", ".sql", ".tf", ".gradle"}

SKIP_DIRS = {
    ".git", "prism-out", "third_party", "build", "node_modules", "__pycache__",
    ".venv", "venv", "target", ".tox", ".mypy_cache", ".ruff_cache", ".pytest_cache",
}

MAX_FILE_BYTES = 2_000_000
MAX_FILES_PER_TOOL = 2000
BATCH = 200


# The syntax helper both engines run under a Python interpreter. The C++
# engine embeds this exact text (tests/test_polyglot.py checks they match).
SYNTAX_HELPER = r'''
import json, re, sys
try:
    import tomllib
except ImportError:
    tomllib = None
for path in sys.argv[1:]:
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as ex:
        print("PRISM-SYNTAX\t%s\t0\t0\tunreadable: %s" % (path, ex))
        continue
    low = path.lower()
    try:
        if low.endswith((".py", ".pyi")):
            compile(data, path, "exec", dont_inherit=True)
        elif low.endswith(".json"):
            json.loads(data.decode("utf-8-sig"))
        elif low.endswith(".toml"):
            if tomllib is None:
                print("PRISM-NOTOML\t%s" % path)
                continue
            tomllib.loads(data.decode("utf-8"))
    except SyntaxError as ex:
        msg = "%s: %s" % (type(ex).__name__, ex.msg)
        print("PRISM-SYNTAX\t%s\t%d\t%d\t%s" % (path, ex.lineno or 0, ex.offset or 0, msg))
    except json.JSONDecodeError as ex:
        print("PRISM-SYNTAX\t%s\t%d\t%d\tJSON: %s" % (path, ex.lineno, ex.colno, ex.msg))
    except ValueError as ex:
        m = re.search(r"line (\d+), column (\d+)", str(ex))
        ln, col = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        print("PRISM-SYNTAX\t%s\t%d\t%d\t%s: %s" % (path, ln, col, type(ex).__name__, ex))
    else:
        print("PRISM-OK\t%s" % path)
'''


@dataclass(frozen=True)
class Tool:
    name: str
    exes: tuple[str, ...]
    argv: tuple[str, ...]      # {exe} and {files}/{file} placeholders
    pattern: str               # regex with file/line/col/sev/rule/msg groups
    per_file: bool = False
    ok_rcs: tuple[int, ...] = (0, 1)
    timeout: float = 120.0
    cwd_marker: str = ""       # run once per dir holding this file (e.g. Cargo.toml)
    unconfigured: str = ""     # output regex meaning "project not set up for this tool"


@dataclass(frozen=True)
class Check:
    group: str                 # e.g. "python-syntax"
    languages: tuple[str, ...]
    kind: str                  # "syntax" | "lint" | "type"
    tools: tuple[Tool, ...]
    install: str


_GCC = r"^(?P<file>[^\s:][^:]*?):(?P<line>\d+):(?:(?P<col>\d+):)?\s*(?:(?P<sev>error|warning|note|info|style|fatal)\s*:\s*)?(?P<msg>.+)$"

CHECKS: tuple[Check, ...] = (
    Check("python-syntax", ("python", "json", "toml"), "syntax", (
        Tool("prism-syntax", ("python3", "python"), ("{exe}", "{helper}", "{files}"),
             r"^PRISM-SYNTAX\t(?P<file>[^\t]+)\t(?P<line>\d+)\t(?P<col>\d+)\t(?P<msg>.+)$",
             ok_rcs=(0,)),
    ), "install Python 3.11+ (python3 on PATH)"),
    Check("python-lint", ("python",), "lint", (
        Tool("ruff", ("ruff",), ("{exe}", "check", "--output-format=concise", "--no-cache",
                                 "--quiet", "{files}"),
             r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): (?P<rule>[A-Z]+\d+) (?P<msg>.+)$"),
        Tool("pyflakes", ("pyflakes",), ("{exe}", "{files}"),
             r"^(?P<file>.+?):(?P<line>\d+):(?:(?P<col>\d+):?)?\s+(?P<msg>.+)$"),
    ), "pip install ruff  (or pyflakes)"),
    Check("python-types", ("python",), "type", (
        Tool("mypy", ("mypy",), ("{exe}", "--ignore-missing-imports", "--no-error-summary",
                                 "--show-column-numbers", "--no-color-output",
                                 "--no-incremental", "--cache-dir={devnull}",
                                 "{files}"),
             r"^(?P<file>.+?):(?P<line>\d+):(?:(?P<col>\d+):)? (?P<sev>error): (?P<msg>.+?)(?:\s+\[(?P<rule>[\w-]+)\])?$",
             timeout=600.0),
    ), "pip install mypy"),
    Check("javascript-syntax", ("javascript",), "syntax", (
        Tool("node", ("node",), ("{exe}", "--check", "{file}"),
             r"(?ms)^(?P<file>[^\n]+?):(?P<line>\d+)\n.*?^(?P<msg>(?:SyntaxError|Error)[^\n]*)",
             per_file=True, ok_rcs=(0,)),
    ), "install Node.js (node on PATH)"),
    Check("typescript-types", ("typescript",), "type", (
        Tool("tsc", ("tsc",), ("{exe}", "--noEmit", "--pretty", "false", "--skipLibCheck",
                               "--jsx", "preserve", "--allowJs", "{files}"),
             r"^(?P<file>.+?)\((?P<line>\d+),(?P<col>\d+)\): (?P<sev>error|warning) (?P<rule>TS\d+): (?P<msg>.+)$",
             ok_rcs=(0, 1, 2), timeout=600.0),
    ), "npm install -g typescript"),
    Check("javascript-lint", ("javascript", "typescript"), "lint", (
        Tool("eslint", ("eslint",), ("{exe}", "--format", "unix", "{files}"),
             r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): (?P<msg>.+?)(?: \[(?P<sev>Error|Warning)/(?P<rule>[^\]]+)\])?$",
             unconfigured=r"couldn't find (?:a|an) (?:eslint\.config|configuration file)"),
    ), "npm install -g eslint  (needs an eslint.config.* in the project)"),
    Check("shell-syntax", ("shell",), "syntax", (
        Tool("bash", ("bash",), ("{exe}", "-n", "{file}"),
             r"^(?P<file>.+?): line (?P<line>\d+): (?P<msg>.+)$", per_file=True, ok_rcs=(0,)),
    ), "install bash"),
    Check("shell-lint", ("shell",), "lint", (
        Tool("shellcheck", ("shellcheck",), ("{exe}", "-f", "gcc", "{files}"), _GCC),
    ), "apt install shellcheck"),
    Check("go-syntax", ("go",), "syntax", (
        Tool("gofmt", ("gofmt",), ("{exe}", "-e", "-l", "{files}"),
             r"^(?P<file>.+?\.go):(?P<line>\d+):(?P<col>\d+): (?P<msg>.+)$", ok_rcs=(0,)),
    ), "install Go (gofmt on PATH)"),
    Check("rust-lint", ("rust",), "lint", (
        Tool("cargo-clippy", ("cargo",), ("{exe}", "clippy", "--quiet", "--message-format=short"),
             r"^(?P<file>[^\s:][^:]*?\.rs):(?P<line>\d+):(?P<col>\d+): (?P<sev>error|warning)(?:\[(?P<rule>[^\]]+)\])?: (?P<msg>.+)$",
             ok_rcs=(0, 101), timeout=900.0, cwd_marker="Cargo.toml"),
    ), "install Rust (rustup component add clippy); needs a Cargo.toml"),
    Check("ruby-syntax", ("ruby",), "syntax", (
        Tool("ruby", ("ruby",), ("{exe}", "-wc", "{file}"),
             r"^(?:\S*ruby\S*: )?(?P<file>[^:\n]+?):(?P<line>\d+): (?:(?P<sev>warning): )?(?P<msg>.+)$",
             per_file=True, ok_rcs=(0,)),
    ), "install Ruby"),
    Check("php-syntax", ("php",), "syntax", (
        Tool("php", ("php",), ("{exe}", "-l", "{file}"),
             r"^(?:PHP )?(?P<msg>(?:Parse|Fatal) error:.+?) in (?P<file>.+?) on line (?P<line>\d+)$",
             per_file=True, ok_rcs=(0,)),
    ), "install PHP CLI"),
    Check("perl-syntax", ("perl",), "syntax", (
        Tool("perl", ("perl",), ("{exe}", "-c", "{file}"),
             r"^(?P<msg>.+?) at (?P<file>.+?) line (?P<line>\d+)[.,]",
             per_file=True, ok_rcs=(0,)),
    ), "install Perl"),
    Check("lua-syntax", ("lua",), "syntax", (
        Tool("luac", ("luac", "luac5.4", "luac5.3"), ("{exe}", "-p", "{file}"),
             r"^\S*luac\S*: (?P<file>.+?):(?P<line>\d+): (?P<msg>.+)$",
             per_file=True, ok_rcs=(0,)),
    ), "install Lua (luac on PATH)"),
    Check("yaml-lint", ("yaml",), "lint", (
        Tool("yamllint", ("yamllint",), ("{exe}", "-f", "parsable", "{files}"),
             r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): \[(?P<sev>error|warning)\] (?P<msg>.+?)(?: \((?P<rule>[\w-]+)\))?$"),
    ), "pip install yamllint"),
)


# Built-in scans: (cls, regex, message). Applied line by line.
BUILTIN_SCANS: tuple[tuple[str, str, str], ...] = (
    ("VCS-CONFLICT-MARKER", r"^(?:<{7}|>{7})(?: |$)", "unresolved merge conflict marker"),
    ("SECRET-PRIVATE-KEY", r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |ENCRYPTED )?PRIVATE KEY(?: BLOCK)?-----",
     "private key committed in source"),
    ("SECRET-AWS-KEY", r"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b", "AWS access key id in source"),
    ("SECRET-GITHUB-TOKEN", r"\b(?:gh[pousr]_[A-Za-z0-9]{36,}|github_pat_[A-Za-z0-9_]{50,})\b",
     "GitHub token in source"),
    ("SECRET-SLACK-TOKEN", r"\bxox[abprs]-[A-Za-z0-9-]{10,}\b", "Slack token in source"),
    ("SECRET-GOOGLE-API-KEY", r"\bAIza[0-9A-Za-z_-]{35}\b", "Google API key in source"),
    ("SECRET-STRIPE-KEY", r"\b[sr]k_live_[0-9A-Za-z]{20,}\b", "Stripe live secret key in source"),
)

_BUILTIN_RX = [(cls, re.compile(rx), msg) for cls, rx, msg in BUILTIN_SCANS]

# A line carrying this marker is a deliberate fixture (e.g. a fake key in a
# test); the built-in scan skips it. Same marker in the C++ engine.
ALLOW_MARKER = "prism:allow"


def _skip_dir(name: str) -> bool:
    return name in SKIP_DIRS or name.startswith(("prism-out", "build"))


def iter_polyglot_sources(root: Path) -> list[Path]:
    """Every file the stage looks at: language files, C family, text configs."""
    wanted = set(LANG_EXTS) | C_FAMILY_EXTS | TEXT_ONLY_EXTS
    if root.is_file():
        return [root] if _classify(root, wanted) else []
    out: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if not _skip_dir(d))
        for name in sorted(filenames):
            p = Path(dirpath) / name
            if _classify(p, wanted):
                out.append(p)
    return out


def _classify(p: Path, wanted: set[str]) -> bool:
    return p.suffix.lower() in wanted or p.name == ".env"


def language_of(p: Path) -> str | None:
    return LANG_EXTS.get(p.suffix.lower())


def _rel(p: Path | str, root: Path) -> str:
    s = str(p)
    base = root if root.is_dir() else root.parent
    try:
        return Path(s).resolve().relative_to(base.resolve()).as_posix()
    except (ValueError, OSError):
        return Path(s).as_posix()


def _finding(status: str, file: str, line: int | None, cls: str, message: str,
             **extra: str) -> Finding:
    return Finding(stage=STAGE, status=status, file=file, function=None, line=line,
                   cls=cls, message=message, strength=laws.STRENGTH_FINDS,
                   extra={k: v for k, v in extra.items() if v})


def builtin_scan(files: list[Path], root: Path) -> list[Finding]:
    out: list[Finding] = []
    scanned = 0
    for p in files:
        try:
            if p.stat().st_size > MAX_FILE_BYTES:
                continue
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        scanned += 1
        rel = _rel(p, root)
        for i, line in enumerate(text.splitlines(), 1):
            if ALLOW_MARKER in line:
                continue
            for cls, rx, msg in _BUILTIN_RX:
                if rx.search(line):
                    out.append(_finding(laws.FAILED, rel, i, cls, msg, tool="prism-builtin"))
    if not out:
        out.append(_finding(
            laws.CLEAN, "", None, "",
            f"prism-builtin: {scanned} files, no conflict markers or leaked credentials "
            "(not a proof)", tool="prism-builtin",
        ))
    return out


def _run(cmd: list[str], timeout: float, cwd: Path | None = None) -> tuple[int, str, bool]:
    """(rc, stdout+stderr, timed_out). Same merged stream as the C++ run_argv."""
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, encoding="utf-8", errors="replace",
                           timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired as ex:
        out = ex.stdout if isinstance(ex.stdout, str) else ""
        return -1, out or "", True
    except OSError as ex:
        return 127, str(ex), False
    return r.returncode, r.stdout or "", False


def _resolve(tool: Tool, cfg: Config) -> str | None:
    if tool.name == "prism-syntax":
        explicit = (cfg.tools or {}).get("prism-syntax") or (cfg.tools or {}).get("python3")
        if explicit and Path(explicit).is_file():
            return explicit
        return sys.executable or resolve_adapter(cfg, tool.name, tool.exes)
    return resolve_adapter(cfg, tool.name, tool.exes)


def _expand(tool: Tool, exe: str, files: list[str], helper: str = "") -> list[str]:
    cmd: list[str] = []
    for a in tool.argv:
        if a == "{exe}":
            cmd.append(exe)
        elif a == "{helper}":
            cmd.append(helper)
        elif a in {"{files}", "{file}"}:
            cmd.extend(files)
        else:
            cmd.append(a.replace("{devnull}", os.devnull))
    return cmd


def parse_output(tool: Tool, text: str, root: Path, cwd: Path | None,
                 check: Check) -> list[Finding]:
    rx = re.compile(tool.pattern, re.M)
    out: list[Finding] = []
    for m in rx.finditer(text):
        gd = m.groupdict()
        sev = (gd.get("sev") or "").lower()
        if sev in {"note", "info"}:
            continue
        file = (gd.get("file") or "").strip()
        if cwd is not None and file and not Path(file).is_absolute():
            file = str(cwd / file)
        msg = re.sub(r"^\[\*\]\s*", "", (gd.get("msg") or "").strip())
        rule = gd.get("rule") or ""
        cls = {"syntax": "SYNTAX-ERROR", "type": "TYPE-ERROR"}.get(check.kind, "LANG-LINT")
        if sev == "warning" and check.kind == "syntax":
            cls = "LANG-LINT"
        out.append(_finding(
            laws.FAILED, _rel(file, root) if file else "",
            int(gd["line"]) if gd.get("line") else None, cls,
            f"{tool.name}: {msg}", tool=tool.name, rule=rule, severity=sev,
            language="/".join(check.languages), check=check.group,
            col=gd.get("col") or "",
        ))
    return out


def _invocations(tool: Tool, files: list[Path]) -> list[tuple[list[str], Path | None]]:
    """(file args, cwd) per process launch."""
    if tool.cwd_marker:
        dirs: list[Path] = []
        for f in files:
            for d in (f.parent, *f.parents):
                if (d / tool.cwd_marker).is_file():
                    if d not in dirs:
                        dirs.append(d)
                    break
        return [([], d) for d in dirs]
    names = [str(f) for f in files]
    if tool.per_file:
        return [([n], None) for n in names]
    return [(names[i:i + BATCH], None) for i in range(0, len(names), BATCH)]


def run_check(check: Check, files: list[Path], root: Path, cfg: Config) -> list[Finding]:
    tool = exe = None
    for t in check.tools:
        exe = _resolve(t, cfg)
        if exe:
            tool = t
            break
    langs = "/".join(check.languages)
    if tool is None or exe is None:
        names = " / ".join(t.name for t in check.tools)
        return [_finding(laws.NOTRUN, "", None, "",
                         f"{check.group}: {names} not found for {len(files)} {langs} file(s)",
                         install=check.install, check=check.group)]
    note: list[Finding] = []
    if len(files) > MAX_FILES_PER_TOOL:
        note.append(_finding(laws.UNKNOWN, "", None, "",
                             f"{tool.name}: checked first {MAX_FILES_PER_TOOL} of "
                             f"{len(files)} {langs} files (rest not checked)",
                             tool=tool.name, check=check.group))
        files = files[:MAX_FILES_PER_TOOL]
    calls = _invocations(tool, files)
    if not calls:
        return [_finding(laws.NOTRUN, "", None, "",
                         f"{check.group}: no {tool.cwd_marker} above {len(files)} {langs} "
                         "file(s); not checked", install=check.install, check=check.group)]

    tmp = tempfile.TemporaryDirectory(prefix="prism_polyglot_")
    helper = Path(tmp.name) / "prism_syntax.py"
    helper.write_text(SYNTAX_HELPER, encoding="utf-8")

    def one(call: tuple[list[str], Path | None]) -> list[Finding]:
        args, cwd = call
        rc, text, timed_out = _run(_expand(tool, exe, args, str(helper)), tool.timeout, cwd)
        if timed_out:
            return [_finding(laws.TIMEOUT, "", None, "",
                             f"{tool.name} timed out after {tool.timeout:.0f}s",
                             tool=tool.name, check=check.group)]
        if tool.unconfigured and re.search(tool.unconfigured, text, re.I):
            return [_finding(laws.NOTRUN, "", None, "",
                             f"{tool.name} present but the project has no config for it",
                             install=check.install, tool=tool.name, check=check.group)]
        hits = parse_output(tool, text, root, cwd, check)
        for m in re.finditer(r"^PRISM-NOTOML\t(.+)$", text, re.M):
            hits.append(_finding(laws.NOTRUN, _rel(m.group(1), root), None, "",
                                 "toml not checked: interpreter has no tomllib (Python < 3.11)",
                                 install=check.install, tool=tool.name, check=check.group))
        if hits:
            return hits
        if rc == 127 or (rc not in tool.ok_rcs and not tool.per_file):
            return [_finding(laws.ERROR, "", None, "",
                             f"{tool.name} exit {rc}: {text.strip()[-400:]}",
                             tool=tool.name, check=check.group)]
        if rc not in tool.ok_rcs:
            where = _rel(args[0], root) if args else ""
            return [_finding(laws.FAILED, where, None,
                             "SYNTAX-ERROR" if check.kind == "syntax" else "LANG-LINT",
                             f"{tool.name} exit {rc}: {text.strip()[-300:]}",
                             tool=tool.name, check=check.group)]
        return []

    jobs = max(1, int(getattr(cfg, "jobs", 1) or 1))
    results: list[Finding] = []
    try:
        if jobs > 1 and len(calls) > 1:
            with ThreadPoolExecutor(max_workers=jobs) as pool:
                for part in pool.map(one, calls):
                    results.extend(part)
        else:
            for c in calls:
                results.extend(one(c))
    finally:
        tmp.cleanup()
    if not results:
        results.append(_finding(
            laws.UNKNOWN, "", None, "",
            f"{tool.name}: {len(files)} {langs} file(s), no diagnostics (not a proof)",
            tool=tool.name, check=check.group,
        ))
    return note + results


def run_polyglot(root: Path, cfg: Config) -> list[Finding]:
    files = iter_polyglot_sources(root)
    if not files:
        return [_finding(laws.UNKNOWN, "", None, "", "no source files in scope")]
    out = builtin_scan(files, root)
    by_lang: dict[str, list[Path]] = {}
    for p in files:
        lang = language_of(p)
        if lang:
            by_lang.setdefault(lang, []).append(p)
    # A file that does not parse makes whole-program type checkers (mypy, tsc)
    # abort and hide every other file's errors. It already has its
    # SYNTAX-ERROR; keep it away from the type checkers.
    broken: set[str] = set()
    for check in CHECKS:
        mine = [p for lang in check.languages for p in by_lang.get(lang, [])]
        if check.kind == "type":
            mine = [p for p in mine if _rel(p, root) not in broken]
        if not mine:
            continue
        found = run_check(check, mine, root, cfg)
        if check.kind == "syntax":
            broken.update(f.file for f in found
                          if f.status == laws.FAILED and f.cls == "SYNTAX-ERROR" and f.file)
        out.extend(found)
    return out
