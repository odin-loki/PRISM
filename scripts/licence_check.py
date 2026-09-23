#!/usr/bin/env python3
"""Licence firewall (roadmap Part 1.2).

Fails (exit 1) when a `linked` component in third_party/MANIFEST.toml carries
a copyleft SPDX licence. GPL, LGPL, AGPL, SSPL, EUPL, OSL, CPAL, CC-BY-SA are
strong/network copyleft; MPL, EPL, CDDL are weak (file-level) copyleft and
also fail unless the component name is in ALLOW_WEAK_COPYLEFT after review.
External and system tools may carry any licence: PRISM runs them as separate,
unmodified processes and never links them.

Also fails when a linked licence is missing or NOASSERTION, and when a linked
component's recorded licence file no longer exists in tree.

  python scripts/licence_check.py [--manifest PATH]
"""

from __future__ import annotations

import argparse
import re
import sys
import tomllib
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "third_party" / "MANIFEST.toml"

STRONG_COPYLEFT = ("GPL", "LGPL", "AGPL", "SSPL", "EUPL", "OSL", "CPAL", "CC-BY-SA", "RPL", "QPL")
WEAK_COPYLEFT = ("MPL", "EPL", "CDDL", "CPL", "MS-RL", "APSL")
# Linked components allowed to carry a weak-copyleft licence after legal review.
# Empty on purpose: add a name only with a reviewed justification.
ALLOW_WEAK_COPYLEFT: frozenset[str] = frozenset()

# SPDX exceptions that make a copyleft licence safe to link (runtime-library style).
LINK_EXCEPTIONS = ("GCC-exception-3.1", "LLVM-exception", "Classpath-exception-2.0")


def _ids(expr: str) -> list[str]:
    """Licence ids of an SPDX expression, with any WITH-exception kept attached."""
    expr = expr.replace("(", " ").replace(")", " ")
    parts = re.split(r"\s+(?:AND|OR)\s+", expr.strip())
    return [p.strip() for p in parts if p.strip()]


def classify(spdx_id: str) -> str:
    """'permissive' | 'weak' | 'strong' for one id (optionally 'X WITH exc')."""
    base, _, exc = spdx_id.partition(" WITH ")
    b = base.strip().upper()
    if exc.strip() in LINK_EXCEPTIONS:
        return "permissive"
    for tag in STRONG_COPYLEFT:
        if b == tag or b.startswith(tag + "-") or b.startswith(tag + "V"):
            return "strong"
    for tag in WEAK_COPYLEFT:
        if b == tag or b.startswith(tag + "-"):
            return "weak"
    return "permissive"


def check(data: dict[str, Any], root: Path = ROOT) -> list[str]:
    problems: list[str] = []
    for c in data.get("component") or []:
        if c.get("kind") != "linked":
            continue
        name = c.get("name", "?")
        spdx = str(c.get("spdx") or "").strip()
        if not spdx or spdx.upper() == "NOASSERTION":
            problems.append(f"{name}: linked component without an SPDX licence")
            continue
        ids = _ids(spdx)
        # "A OR B": one permissive choice is enough. Otherwise every id counts.
        if " OR " in spdx and " AND " not in spdx:
            classes = {classify(i) for i in ids}
            worst = "permissive" if "permissive" in classes else ("weak" if "weak" in classes else "strong")
        else:
            order = {"permissive": 0, "weak": 1, "strong": 2}
            worst = max((classify(i) for i in ids), key=order.__getitem__)
        if worst == "strong":
            problems.append(f"{name}: linked component is copyleft ({spdx}); run it as an external process instead")
        elif worst == "weak" and name not in ALLOW_WEAK_COPYLEFT:
            problems.append(f"{name}: linked component is weak copyleft ({spdx}); needs review + ALLOW_WEAK_COPYLEFT")
        lf = str(c.get("licence_file") or "").split(" ")[0]
        path = c.get("path")
        if lf and path and not (root / path / lf).exists():
            problems.append(f"{name}: licence file {path}/{lf} is missing")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Fail when a linked dependency is copyleft.")
    ap.add_argument("--manifest", type=Path, default=MANIFEST)
    args = ap.parse_args(argv)
    with open(args.manifest, "rb") as fh:
        data = tomllib.load(fh)
    problems = check(data, args.manifest.resolve().parents[1])
    linked = [c for c in data.get("component") or [] if c.get("kind") == "linked"]
    for c in linked:
        print(f"linked   {c['name']:14} {c['spdx']}")
    if problems:
        for p in problems:
            print("FAIL " + p, file=sys.stderr)
        return 1
    print(f"licence check OK: {len(linked)} linked components, none copyleft")
    return 0


if __name__ == "__main__":
    sys.exit(main())
