"""Documents shipped with every report (roadmap 3.2 / 8.3 / 6.4).

docs/TRUSTED_BASE.md says what a proof depends on; docs/VERDICTS.md defines
every verdict, and every finding in report.md links to its anchor there.
Both are written next to report.md, byte-identical to the C++ engine's copies
(src/prism/shipdocs.cpp embeds the same files at build time).
"""

from __future__ import annotations

import hashlib
from functools import lru_cache
from pathlib import Path

from prism import laws

DOCS = Path(__file__).resolve().parents[1] / "docs"
TRUSTED_BASE_FILE = "TRUSTED_BASE.md"
VERDICTS_FILE = "VERDICTS.md"
SHIPPED = (TRUSTED_BASE_FILE, VERDICTS_FILE)
_MISSING = "# {name}\n\nNOTRUN: {path} was not found next to the prism package; this copy is empty.\n"


@lru_cache(maxsize=None)
def doc_bytes(name: str) -> bytes:
    p = DOCS / name
    try:
        return p.read_bytes()
    except OSError:
        # Law 7: say so in the shipped file instead of dropping it quietly.
        return _MISSING.format(name=name, path=p).encode("utf-8")


def trusted_base_sha256() -> str:
    return hashlib.sha256(doc_bytes(TRUSTED_BASE_FILE)).hexdigest()


def verdict_anchor(status: str) -> str:
    """``verdict-proved-certified`` for a lattice status, "" otherwise."""
    if status not in laws.VERDICTS:
        return ""
    return "verdict-" + status.lower()


def write_shipped_docs(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    for name in SHIPPED:
        (directory / name).write_bytes(doc_bytes(name))
