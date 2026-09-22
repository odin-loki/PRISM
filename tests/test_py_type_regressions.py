"""Regressions for real bugs that mypy / ruff surfaced in the Python engine.

- ``Finding(function=())`` used to become the string ``"()"``; an empty
  sequence has no function name, so it is ``None``.
- pbsd's sibling_guard bridge passed an empty ``unguarded`` tuple straight
  through as the function name.
- The Infer adapter created a scratch directory and then ignored it, so
  ``infer run -- cc -c`` wrote ``infer-out/`` and ``*.o`` into whatever
  directory PRISM was launched from. Both now go to the scratch directory.

python -m unittest tests.test_py_type_regressions
"""

from __future__ import annotations

import os
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

from prism import laws
from prism.config import Config
from prism.models import Finding


def _finding(function):
    return Finding(
        stage="pbsd", status=laws.FAILED, file="a.c", function=function,
        line=1, cls="", message="m", strength=laws.STRENGTH_FINDS,
    )


class TestFindingFunctionNormalised(unittest.TestCase):
    def test_empty_sequence_is_none_not_parens(self):
        self.assertIsNone(_finding(()).function)
        self.assertIsNone(_finding([]).function)

    def test_sequence_takes_first_name(self):
        self.assertEqual(_finding(("f", "g")).function, "f")
        self.assertEqual(_finding(["h"]).function, "h")

    def test_str_and_none_unchanged(self):
        self.assertEqual(_finding("main").function, "main")
        self.assertIsNone(_finding(None).function)


class TestSiblingGuardFunctionName(unittest.TestCase):
    def test_empty_unguarded_tuple_is_none(self):
        from prism.pbsd import _run_sibling_guard

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "tools").mkdir()
            (root / "tools" / "sibling_guard.py").write_text(textwrap.dedent("""
                class Hit:
                    def __init__(self, unguarded, line):
                        self.unguarded = unguarded
                        self.line = line
                        self.detail = "asymmetric guard"
                        self.kind = "sibling"

                def scan_file(path, rel, args):
                    return [Hit((), 3), Hit(("g", "h"), 4), Hit(7, 5)]
            """), encoding="utf-8")
            src = root / "a.c"
            src.write_text("int f(void){return 0;}\n", encoding="utf-8")
            invoked: list[str] = []
            ported: list[str] = []
            out = _run_sibling_guard(root, [src], Config(root=root), invoked, ported)
        self.assertEqual(invoked, ["sibling_guard"])
        self.assertEqual([f.function for f in out], [None, "g", "7"])
        self.assertTrue(all(f.status == laws.FAILED for f in out))


@unittest.skipIf(sys.platform == "win32", "uses a POSIX shell script as a fake infer")
class TestInferScratchDirectory(unittest.TestCase):
    def test_infer_output_stays_out_of_launch_directory(self):
        from prism.adapters_extra import _run_infer

        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            launch = base / "launch"
            launch.mkdir()
            fake = base / "infer"
            fake.write_text(
                # Like real Infer: results go to --results-dir (default
                # ./infer-out) and the object to -o (default ./<name>.o).
                "#!/bin/sh\nres=infer-out\nobj=planted.o\nprev=\n"
                "for a in \"$@\"; do\n"
                "  [ \"$prev\" = --results-dir ] && res=$a\n"
                "  [ \"$prev\" = -o ] && obj=$a\n"
                "  prev=$a\n"
                "done\n"
                "mkdir -p \"$res\" && : > \"$res/report.txt\" && : > \"$obj\"\n"
                "echo 'No issues found'\n",
                encoding="utf-8",
            )
            fake.chmod(0o755)
            src = base / "planted.c"
            src.write_text("int f(void){return 0;}\n", encoding="utf-8")
            old = os.getcwd()
            os.chdir(launch)
            try:
                with mock.patch("prism.adapters_extra.shutil.which", return_value="/bin/true"):
                    out = _run_infer(str(fake), [src], Config())
            finally:
                os.chdir(old)
            self.assertEqual(os.listdir(launch), [])
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0].status, laws.UNKNOWN)
        self.assertIn("not a proof", out[0].message)


if __name__ == "__main__":
    unittest.main()
