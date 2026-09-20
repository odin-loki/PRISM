"""Shipped Coccinelle rules exist even when spatch is NOTRUN."""

from __future__ import annotations

import unittest
from pathlib import Path

from helix.adapters_extra import _cocci_rules

ROOT = Path(__file__).resolve().parents[1]
COCCI = ROOT / "helix" / "cocci"
TD = ROOT / "testdata"


class TestCocciRules(unittest.TestCase):
    def test_shipped_rules(self):
        names = {p.name for p in COCCI.glob("*.cocci")}
        for need in (
            "memcpy_self.cocci",
            "realloc_self.cocci",
            "shift_bit31.cocci",
            "getenv_null.cocci",
            "strcpy_self.cocci",
            "sprintf_unbounded.cocci",
            "strcat_self.cocci",
            "strncpy_self.cocci",
        ):
            self.assertIn(need, names)

    def test_getenv_null_mentions_getenv(self):
        text = (COCCI / "getenv_null.cocci").read_text(encoding="utf-8")
        self.assertIn("getenv", text)
        self.assertIn("@", text)

    def test_cocci_rules_discovers_getenv_null(self):
        names = {p.name for p in _cocci_rules([])}
        self.assertIn("getenv_null.cocci", names)
        names_td = {p.name for p in _cocci_rules([TD / "getenv_null.c"])}
        self.assertIn("getenv_null.cocci", names_td)

    def test_sprintf_unbounded_mentions_sprintf(self):
        text = (COCCI / "sprintf_unbounded.cocci").read_text(encoding="utf-8")
        self.assertIn("sprintf", text)
        self.assertNotIn("snprintf", text)
