"""ParanoidBSD bridge: portable checkers, never empty-ok when the tree is gone."""

from __future__ import annotations

import os
import unittest
from pathlib import Path

from helix import laws
from helix.checkers import run_lints
from helix.config import Config
from helix.pbsd import discover_pbsd_root, run_pbsd_lints

ROOT = Path(__file__).resolve().parents[1]
TD = ROOT / "testdata"
MISSING = Path(os.path.abspath(os.sep)) / "helix-no-such-paranoidbsd"


def _cfg(*, pbsd: Path | None = None) -> Config:
    kw = {"root": TD}
    if pbsd is not None:
        kw["pbsd_root"] = pbsd
    return Config(**kw)


class TestOnesidedLint(unittest.TestCase):
    def test_onesided_lint_fires(self):
        hits = [f for f in run_lints([TD / "onesided.c"], TD)
                if f.cls == "MEM-ONESIDED-INDEX"]
        self.assertTrue(hits)
        names = {f.function for f in hits}
        self.assertIn("onesided_index", names)
        self.assertNotIn("two_sided_index", names)
        self.assertNotIn("unsigned_index", names)

    def test_capacity_lint_fires(self):
        hits = [f for f in run_lints([TD / "capacity.c"], TD)
                if f.cls == "MEM-CAPACITY-FIRST"]
        self.assertTrue(hits)


class TestPbsdBridge(unittest.TestCase):
    def test_missing_tree_not_empty_ok(self):
        old = os.environ.pop("HELIX_PBSD", None)
        try:
            hits = run_pbsd_lints([TD / "abs_ok.c"], _cfg(pbsd=MISSING))
            self.assertTrue(hits, "missing tree must not return an empty-ok list")
            self.assertTrue(any(f.status == laws.NOTRUN for f in hits))
            self.assertFalse(any(f.status == laws.CLEAN for f in hits))
            self.assertTrue(any(f.extra.get("install") for f in hits if f.status == laws.NOTRUN))
        finally:
            if old is not None:
                os.environ["HELIX_PBSD"] = old

    def test_missing_tree_portable_onesided_is_a_finding(self):
        old = os.environ.pop("HELIX_PBSD", None)
        try:
            hits = run_pbsd_lints([TD / "onesided.c"], _cfg(pbsd=MISSING))
            self.assertTrue(hits)
            self.assertTrue(any(f.cls == "MEM-ONESIDED-INDEX" for f in hits))
            self.assertFalse(any(f.status == laws.CLEAN for f in hits))
        finally:
            if old is not None:
                os.environ["HELIX_PBSD"] = old

    def test_present_tree_invokes_scanners_not_a_note(self):
        old = os.environ.pop("HELIX_PBSD", None)
        try:
            cfg = _cfg()
            if discover_pbsd_root(cfg) is None:
                self.skipTest("ParanoidBSD tree not on this machine")
            hits = run_pbsd_lints([TD / "realloc_self.c", TD / "capacity.c"], cfg)
            self.assertFalse(any("use --pbsd-sweep" in (f.message or "") for f in hits))
            self.assertFalse(any(f.status == laws.CLEAN for f in hits))
            vias = [str((f.extra or {}).get("via", "")) for f in hits]
            self.assertTrue(
                any(v.startswith("realloc_self") for v in vias),
                f"realloc_self.scan should have been invoked; vias={vias}",
            )
            self.assertTrue(
                any(f.cls == "MEM-CAPACITY-FIRST" for f in hits),
                "capacity_first should fire on testdata/capacity.c",
            )
        finally:
            if old is not None:
                os.environ["HELIX_PBSD"] = old


if __name__ == "__main__":
    unittest.main()
