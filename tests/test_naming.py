"""The project is PRISM. The retired name must not creep back in.

python -m unittest tests.test_naming
"""

from __future__ import annotations

import os
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RETIRED = re.compile("hel" + "ix", re.I)  # split so this file does not match itself
SKIP_DIRS = {".git", "third_party", "__pycache__", "prism-out", "node_modules"}
# These two files state the retirement on purpose.
ALLOWED = {ROOT / "CLAUDE.md", ROOT / "AGENTS.md"}


def _skip_dir(name: str) -> bool:
    return name in SKIP_DIRS or name.startswith(("build", "prism-out"))


def _tracked_files():
    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if not _skip_dir(d)]
        for name in filenames:
            p = Path(dirpath) / name
            if p not in ALLOWED:
                yield p


class TestNaming(unittest.TestCase):
    def test_no_retired_name_in_paths(self):
        bad = [str(p.relative_to(ROOT)) for p in _tracked_files()
               if RETIRED.search(str(p.relative_to(ROOT)))]
        self.assertEqual(bad, [], "rename these to PRISM")

    def test_no_retired_name_in_text(self):
        bad = []
        for p in _tracked_files():
            try:
                text = p.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for i, line in enumerate(text.splitlines(), 1):
                if RETIRED.search(line):
                    bad.append(f"{p.relative_to(ROOT)}:{i}: {line.strip()[:100]}")
        self.assertEqual(bad, [], "the project is PRISM; rename these")

    def test_python_package_is_prism(self):
        self.assertTrue((ROOT / "prism" / "__init__.py").is_file())
        self.assertTrue((ROOT / "prism" / "__main__.py").is_file())


if __name__ == "__main__":
    unittest.main()
