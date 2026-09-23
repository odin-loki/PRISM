"""User documentation completeness (roadmap 6.4).

- Every command-line flag that ``--help`` prints (C++ engine via PRISM_BIN,
  its ``prove`` subcommand, and ``python -m prism``) is documented in
  docs/USER_GUIDE.md.
- Every Markdown link with an anchor (``VERDICTS.md#verdict-proved``, ``#section``)
  in docs/, README.md and the report writers of both engines points at a
  heading or an explicit ``<a id>`` that exists.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GUIDE = REPO / "docs" / "USER_GUIDE.md"

# Double-dash flags anywhere; single-dash flags only where help text lists an
# option (usage brackets, line start, argparse's "--jobs JOBS, -j JOBS"), so
# prose such as "perl -c" is not mistaken for a PRISM flag.
LONG_FLAG = re.compile(r"(?<![\w-])(--[A-Za-z][\w-]*)")
SHORT_FLAG = re.compile(r"(?:^\s*|\[|,\s)(-[A-Za-z])\b", re.M)


def flags_of(help_text: str) -> set[str]:
    # argparse wraps "--allow-exec" as "--allow-" / "exec": join it back.
    help_text = re.sub(r"(--[\w-]*-)\n\s+", r"\1", help_text)
    return set(LONG_FLAG.findall(help_text)) | set(SHORT_FLAG.findall(help_text))


def documented(flag: str, guide: str) -> bool:
    return re.search(r"`[^`\n]*(?<![\w-])" + re.escape(flag) + r"(?![\w-])[^`\n]*`", guide) is not None


def python_help() -> str:
    env = dict(os.environ, PYTHONPATH=str(REPO))
    r = subprocess.run([sys.executable, "-m", "prism", "--help"], capture_output=True, text=True, cwd=REPO,
                       env=env, timeout=120)
    assert r.returncode == 0, r.stderr
    return r.stdout


class CliFlagsDocumented(unittest.TestCase):
    def setUp(self) -> None:
        self.guide = GUIDE.read_text(encoding="utf-8")

    def check(self, help_text: str, who: str) -> None:
        flags = flags_of(help_text)
        self.assertTrue(flags, f"no flags parsed from {who} --help")
        missing = sorted(f for f in flags if not documented(f, self.guide))
        self.assertEqual(missing, [], f"{who} --help flags missing from docs/USER_GUIDE.md")

    def test_python_engine(self) -> None:
        self.check(python_help(), "python -m prism")

    @unittest.skipUnless(os.environ.get("PRISM_BIN"), "PRISM_BIN not set")
    def test_cpp_engine(self) -> None:
        r = subprocess.run([os.environ["PRISM_BIN"], "--help"], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0)
        self.check(r.stdout, "prism")

    @unittest.skipUnless(os.environ.get("PRISM_BIN"), "PRISM_BIN not set")
    def test_cpp_prove_subcommand(self) -> None:
        r = subprocess.run([os.environ["PRISM_BIN"], "prove", "--help"], capture_output=True, text=True, timeout=60)
        self.assertEqual(r.returncode, 0)
        self.check(r.stdout, "prism prove")

    def test_stage_order_listed(self) -> None:
        sys.path.insert(0, str(REPO))
        from prism.pipeline import STAGE_ORDER

        m = re.search(r"The stages run in a fixed order \(`--list-stages`\):(.*?)\. What", self.guide, re.S)
        assert m is not None, "USER_GUIDE.md lost its stage-order sentence"
        listed = [s.strip() for s in " ".join(m.group(1).split()).split(",")]
        self.assertEqual(listed, list(STAGE_ORDER))

    def test_parser_ignores_prose(self) -> None:
        self.assertEqual(flags_of("run perl -c on it\n  --jobs JOBS, -j JOBS  workers\n[--out DIR]"),
                         {"--jobs", "-j", "--out"})


# --------------------------------------------------------------------------- anchors

HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$", re.M)
EXPLICIT = re.compile(r"<a\s+(?:id|name)=\"([^\"]+)\"", re.I)
LINK = re.compile(r"\]\(([^)\s]*?)#([^)\s]+)\)")
# Anchor references written by the report writers (report.md / SARIF helpUri).
CODE_REF = re.compile(r"([A-Z_]+\.md)#([a-z0-9-]+)")


def slug(heading: str) -> str:
    """GitHub's heading anchor: lower case, drop punctuation except - and _,
    spaces to hyphens (inline code and links reduced to their text)."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading)
    text = text.replace("`", "").strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors_of(md: Path) -> set[str]:
    text = md.read_text(encoding="utf-8")
    text_nocode = re.sub(r"```.*?```", "", text, flags=re.S)
    out: set[str] = set(EXPLICIT.findall(text))
    seen: dict[str, int] = {}
    for _, h in HEADING.findall(text_nocode):
        s = slug(h)
        n = seen.get(s, 0)
        out.add(s if n == 0 else f"{s}-{n}")
        seen[s] = n + 1
    return out


def markdown_files() -> list[Path]:
    files = sorted((REPO / "docs").rglob("*.md")) + [REPO / "README.md"]
    return [f for f in files if f.exists()]


def anchor_refs() -> list[tuple[Path, Path, str]]:
    """(referring file, target file, anchor) for every anchored link."""
    refs: list[tuple[Path, Path, str]] = []
    for md in markdown_files():
        text = re.sub(r"```.*?```", "", md.read_text(encoding="utf-8"), flags=re.S)
        for target, anchor in LINK.findall(text):
            if target.startswith(("http://", "https://", "mailto:")):
                continue
            tgt = md if not target else (md.parent / target).resolve()
            refs.append((md, tgt, anchor))
    code = [*sorted((REPO / "src").rglob("*.cpp")), *sorted((REPO / "include").rglob("*.hpp")),
            *sorted((REPO / "prism").glob("*.py"))]
    for src in code:
        for name, anchor in CODE_REF.findall(src.read_text(encoding="utf-8", errors="replace")):
            refs.append((src, REPO / "docs" / name, anchor))
    return refs


class AnchorsExist(unittest.TestCase):
    def test_slug(self) -> None:
        self.assertEqual(slug("Pipeline order is the method"), "pipeline-order-is-the-method")
        self.assertEqual(slug("Running on untrusted code (Law 9)"), "running-on-untrusted-code-law-9")
        self.assertEqual(slug("5. Exit codes and `--fail-on`"), "5-exit-codes-and---fail-on")

    def test_every_anchor_exists(self) -> None:
        cache: dict[Path, set[str]] = {}
        bad = []
        for src, tgt, anchor in anchor_refs():
            if not tgt.exists():
                bad.append(f"{src.relative_to(REPO)}: {tgt} does not exist")
                continue
            if tgt.suffix != ".md":
                continue
            if tgt not in cache:
                cache[tgt] = anchors_of(tgt)
            if anchor not in cache[tgt]:
                bad.append(f"{src.relative_to(REPO)}: {tgt.relative_to(REPO)}#{anchor}")
        self.assertEqual(bad, [])

    def test_every_verdict_has_an_anchor(self) -> None:
        sys.path.insert(0, str(REPO))
        from prism import laws

        anchors = anchors_of(REPO / "docs" / "VERDICTS.md")
        missing = [v for v in laws.VERDICTS if v.lower() not in anchors]
        self.assertEqual(missing, [], "docs/VERDICTS.md needs an anchor per verdict (reports link to them)")


if __name__ == "__main__":
    unittest.main()
