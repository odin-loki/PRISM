"""CodeQL is not invoked by PRISM (roadmap 1.2, licence firewall).

The CodeQL engine/CLI is under the GitHub CodeQL Terms, which restrict
commercial use, so the adapter was removed from both engines. This file used
to test that adapter's honesty; its premise changed, so it now locks the
removal: no optional-tool row, no runner, no install hint, no coverage claim,
and a `codeql` on PATH is never executed.
python -m unittest tests.test_codeql
"""

from __future__ import annotations

import re
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from prism import adapters_extra, taxonomy
from prism.adapters_extra import OPTIONAL_TOOLS, run_optional_tools
from prism.config import VENDOR_DIR, Config

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "src" / "prism"


class TestCodeqlRemoved(unittest.TestCase):
    def test_python_engine_has_no_codeql_adapter(self):
        self.assertNotIn("codeql", [stage for stage, _names in OPTIONAL_TOOLS])
        self.assertNotIn("codeql", VENDOR_DIR)
        self.assertFalse(hasattr(adapters_extra, "_run_codeql"))
        self.assertFalse(hasattr(adapters_extra, "_find_codeql_db"))

    def test_cpp_engine_has_no_codeql_adapter(self):
        for name in ("adapters.cpp", "config.cpp"):
            src = (CPP / name).read_text(encoding="utf-8")
            code = re.sub(r"//[^\n]*", "", src)  # a comment may say why it is gone
            self.assertIsNone(re.search(r"codeql", code, re.I), msg=name)

    def test_codeql_on_path_is_never_run(self):
        """Even with a `codeql` binary present, no stage invokes it."""
        with tempfile.TemporaryDirectory() as td:
            calls: list[str] = []

            def fake_resolve(_cfg, stage, names):
                calls.append(stage)
                return None

            with mock.patch("prism.adapters_extra.resolve_adapter", side_effect=fake_resolve), \
                 mock.patch("prism.adapters_extra.shutil.which", return_value=None):
                findings = run_optional_tools([Path(td)], Config())
        self.assertNotIn("codeql", calls)
        self.assertFalse(any(f.stage == "codeql" for f in findings))

    def test_taxonomy_claims_no_codeql_coverage(self):
        py = Path(taxonomy.__file__).read_text(encoding="utf-8")
        cpp = (CPP / "taxonomy.cpp").read_text(encoding="utf-8")
        self.assertIsNone(re.search(r"codeql", py, re.I))
        self.assertIsNone(re.search(r"codeql", cpp, re.I))

    def test_manifest_records_why(self):
        text = (ROOT / "third_party" / "MANIFEST.toml").read_text(encoding="utf-8")
        self.assertIn('name = "codeql"', text)
        self.assertIn("restricted commercial use", text)
        self.assertIn("adapter was removed", text)


if __name__ == "__main__":
    unittest.main()
