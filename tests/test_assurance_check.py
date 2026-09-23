"""The assurance package (docs/assurance, roadmap 6.5) cites only artefacts
that exist, and tools/assurance_check.py really catches a missing one."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def _load():
    spec = importlib.util.spec_from_file_location("prism_assurance_check", REPO / "tools" / "assurance_check.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules["prism_assurance_check"] = mod
    spec.loader.exec_module(mod)
    return mod


AC = _load()


class AssuranceCheck(unittest.TestCase):
    def test_package_is_complete(self) -> None:
        errors, counts = AC.scan(REPO / "docs" / "assurance")
        self.assertEqual(errors, [])
        self.assertGreaterEqual(counts["files"], 7)
        self.assertGreater(counts["theorems"], 20)

    def test_catches_missing_artefacts(self) -> None:
        with tempfile.TemporaryDirectory(dir=REPO / "docs") as d:
            doc = Path(d) / "bogus.md"
            doc.write_text(
                "`thm:no_such_theorem_anywhere` `thm:proved_bounded_never_merge` "
                "`thm:Houdini.houdini_sound` `thm:Houdini.bmc_sound`\n"
                "`docs/NO_SUCH.md` `docs/VERDICTS.md#no-such-anchor` `docs/VERDICTS.md#verdict-proved`\n"
                "`tests/test_verdict.py::NoSuchClass` `tests/test_verdict.py::TestAudit`\n"
                "`thm:<Name>` is a placeholder\n",
                encoding="utf-8")
            errors, counts = AC.scan(Path(d))
        joined = "\n".join(errors)
        self.assertEqual(len(errors), 5, joined)
        self.assertIn("no_such_theorem_anywhere", joined)
        self.assertIn("not in namespace Houdini", joined)
        self.assertIn("docs/NO_SUCH.md", joined)
        self.assertIn("#no-such-anchor", joined)
        self.assertIn("NoSuchClass", joined)
        self.assertEqual(counts["theorems"], 4)


if __name__ == "__main__":
    unittest.main()
