#!/usr/bin/env python3
"""Check that every artefact the assurance package cites exists (roadmap 6.5).

    python tools/assurance_check.py [--docs docs/assurance]

Scans docs/assurance/*.md for references written in backticks:

  `path/to/file`             a repository path (must exist). Recognised when
                             it starts with a top-level directory of this
                             repository (docs/, proofs/, tests/, tools/, src/,
                             include/, prism/, scripts/, testdata/, grammars/,
                             .github/) or is a known top-level file
  `path/to/file::name`       the file exists and contains `name` (a test
                             class, function or doctest TEST_CASE)
  `docs/X.md#anchor`         the Markdown file exists and has that heading
                             anchor (or an explicit <a id>)
  `thm:Name`                 a Lean theorem or lemma: `theorem Name` / `lemma
  `thm:Ns.Name`              Name` must be declared in proofs/**/*.lean (the
                             last dotted component is matched; a namespace
                             prefix must also occur in the declaring file)

Exit 1 with one line per missing artefact. The assurance documents may cite
nothing that this script cannot find, so a renamed theorem, a deleted test or
a moved workflow breaks CI (.github/workflows/docs.yml) instead of leaving a
claim without evidence.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TOP_DIRS = ("docs/", "proofs/", "tests/", "tools/", "src/", "include/", "prism/", "scripts/", "testdata/",
            "grammars/", ".github/", "third_party/")
TOP_FILES = {"CMakeLists.txt", "pyproject.toml", "README.md", "CLAUDE.md", "LICENSE", "NOTICE", "Dockerfile",
             "requirements.txt"}
TICK = re.compile(r"`([^`\n]+)`")
DECL = re.compile(r"^\s*(?:@\[[^\]]*\]\s*)?(?:private\s+|protected\s+)?(?:theorem|lemma)\s+([\w.'!?]+)", re.M)
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$", re.M)
EXPLICIT = re.compile(r"<a\s+(?:id|name)=\"([^\"]+)\"", re.I)


def slug(heading: str) -> str:
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading)
    text = text.replace("`", "").strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors_of(md: Path) -> set[str]:
    text = md.read_text(encoding="utf-8")
    out = set(EXPLICIT.findall(text))
    seen: dict[str, int] = {}
    for _, h in HEADING.findall(re.sub(r"```.*?```", "", text, flags=re.S)):
        s = slug(h)
        n = seen.get(s, 0)
        out.add(s if n == 0 else f"{s}-{n}")
        seen[s] = n + 1
    return out


def lean_index() -> dict[str, list[tuple[Path, str]]]:
    """last name component -> [(file, declared name)] over proofs/**/*.lean."""
    idx: dict[str, list[tuple[Path, str]]] = {}
    for f in sorted((REPO / "proofs").rglob("*.lean")):
        if ".lake" in f.parts:
            continue
        for name in DECL.findall(f.read_text(encoding="utf-8")):
            idx.setdefault(name.split(".")[-1], []).append((f, name))
    return idx


def is_path_ref(tok: str) -> bool:
    base = tok.split("::", 1)[0].split("#", 1)[0]
    if " " in base or "*" in base or "<" in base:
        return False
    return base.startswith(TOP_DIRS) or base in TOP_FILES


def check_theorem(ref: str, idx: dict[str, list[tuple[Path, str]]]) -> str | None:
    parts = ref.split(".")
    hits = idx.get(parts[-1], [])
    if not hits:
        return f"thm:{ref}: no `theorem {parts[-1]}` in proofs/**/*.lean"
    if len(parts) > 1:
        ns = parts[:-1]
        for f, declared in hits:
            text = f.read_text(encoding="utf-8")
            if declared.endswith(ref) or all(re.search(r"\b" + re.escape(n) + r"\b", text) for n in ns):
                return None
        return f"thm:{ref}: `{parts[-1]}` exists but not in namespace {'.'.join(ns)}"
    return None


def check_path(tok: str) -> str | None:
    base, _, member = tok.partition("::")
    base, _, anchor = base.partition("#")
    p = REPO / base.rstrip("/")
    if not p.exists():
        return f"{tok}: path does not exist"
    if member:
        if not p.is_file():
            return f"{tok}: {base} is not a file"
        if member not in p.read_text(encoding="utf-8", errors="replace"):
            return f"{tok}: `{member}` not found in {base}"
    if anchor:
        if p.suffix != ".md":
            return f"{tok}: anchors are only checked in Markdown files"
        if anchor not in anchors_of(p):
            return f"{tok}: no anchor #{anchor} in {base}"
    return None


def scan(docs: Path) -> tuple[list[str], dict[str, int]]:
    idx = lean_index()
    errors: list[str] = []
    counts = {"paths": 0, "theorems": 0, "files": 0}
    for md in sorted(docs.glob("*.md")):
        counts["files"] += 1
        text = md.read_text(encoding="utf-8")
        for n, line in enumerate(text.splitlines(), 1):
            for tok in TICK.findall(line):
                tok = tok.strip()
                if "<" in tok:  # a placeholder such as `thm:<Name>`, not a reference
                    continue
                err = None
                if tok.startswith("thm:"):
                    counts["theorems"] += 1
                    err = check_theorem(tok[4:], idx)
                elif is_path_ref(tok):
                    counts["paths"] += 1
                    err = check_path(tok)
                if err:
                    errors.append(f"{md.relative_to(REPO)}:{n}: {err}")
    return errors, counts


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--docs", default=str(REPO / "docs" / "assurance"))
    a = ap.parse_args(argv)
    docs = Path(a.docs)
    if not docs.is_dir():
        print(f"assurance_check: {docs} does not exist", file=sys.stderr)
        return 2
    errors, counts = scan(docs)
    for e in errors:
        print(e)
    print(f"assurance_check: {counts['files']} documents, {counts['paths']} artefact paths, "
          f"{counts['theorems']} theorem references, {len(errors)} missing")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
