"""One skip-directory list for every stage; skipped sources are written down.

Law 7: a vendor/build directory holding source files is not skipped
quietly: the inventory stage lists it as UNKNOWN. prism/scope.py and
src/prism/scope.cpp carry the same table (tests/test_polyglot.py).

python -m unittest tests.test_scope
"""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from prism import laws, scope
from prism.config import Config
from prism.pipeline import run_pipeline
from prism.polyglot import is_known_source, iter_polyglot_sources

ROOT = Path(__file__).resolve().parents[1]


def _tree(files: dict[str, str]) -> Path:
    d = Path(tempfile.mkdtemp(prefix="prism_scope_test_"))
    for rel, text in files.items():
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    return d


TREE = {
    "a.c": "int f(void) { return 0; }\n",
    "node_modules/m/x.js": "x\n",
    "node_modules/m/y.js": "y\n",
    "node_modules/m/README": "no source extension\n",
    "sub/build-rel/gen.c": "int g(void) { return 1; }\n",
    "third_party/lib/z.h": "int z;\n",
    ".git/HEAD": "ref: refs/heads/main\n",   # no source files: not listed
    "__pycache__/m.pyc": "\0\0",            # no source files: not listed
}


class TestScope(unittest.TestCase):
    def test_skip_dir(self):
        for name in ("node_modules", "third_party", ".git", "build", "build-release",
                     "prism-out-gui", ".venv", "target"):
            self.assertTrue(scope.skip_dir(name), name)
        for name in ("src", "lib", "rebuild", "tests"):
            self.assertFalse(scope.skip_dir(name), name)

    def test_skipped_path_counts_only_below_root(self):
        root = Path("/work/build/project")
        self.assertFalse(scope.skipped_path(root / "src" / "a.c", root))
        self.assertTrue(scope.skipped_path(root / "node_modules" / "x" / "a.js", root))
        self.assertFalse(scope.skipped_path(root / "build.c", root))  # a file, not a dir

    def test_skipped_dirs_lists_each_topmost_dir_with_sources(self):
        d = _tree(TREE)
        try:
            got = scope.skipped_dirs(d, is_known_source)
            walked = [p.relative_to(d).as_posix() for p in iter_polyglot_sources(d)]
        finally:
            shutil.rmtree(d, ignore_errors=True)
        self.assertEqual(got, [("node_modules", 2), ("sub/build-rel", 1), ("third_party", 1)])
        self.assertEqual(walked, ["a.c"])

    def test_inventory_writes_down_skipped_dirs(self):
        d = _tree(TREE)
        out = Path(tempfile.mkdtemp(prefix="prism_scope_out_"))
        try:
            rep = run_pipeline(Config(root=d, out=out, llm=False, stages=["inventory"]))
            md = (out / "report.md").read_text(encoding="utf-8")
        finally:
            shutil.rmtree(d, ignore_errors=True)
            shutil.rmtree(out, ignore_errors=True)
        inv = next(s for s in rep.stages if s.name == "inventory")
        rows = [f for f in inv.findings if f.status == laws.UNKNOWN]
        self.assertEqual([f.message for f in rows], [
            "skipped node_modules/ (2 source files): vendor/build directory",
            "skipped sub/build-rel/ (1 source files): vendor/build directory",
            "skipped third_party/ (1 source files): vendor/build directory",
        ])
        self.assertEqual(rows[0].extra, {"skipped": "node_modules", "files": "2"})
        self.assertIn("skipped node_modules/ (2 source files)", md)

    def test_both_engines_wire_it(self):
        pipe_py = (ROOT / "prism" / "pipeline.py").read_text(encoding="utf-8")
        self.assertIn("scope.skipped_dirs(root, is_known_source)", pipe_py)
        pipe_cpp = (ROOT / "src" / "prism" / "pipeline.cpp").read_text(encoding="utf-8")
        self.assertIn("scope::skipped_dirs(cfg.root, is_known_source)", pipe_cpp)
        scope_cpp = (ROOT / "src" / "prism" / "scope.cpp").read_text(encoding="utf-8")
        self.assertIn('"skipped " + dir + "/ (" + std::to_string(files) + " source files): '
                      'vendor/build directory"', scope_cpp)
        self.assertIn("src/prism/scope.cpp", (ROOT / "CMakeLists.txt").read_text("utf-8"))


if __name__ == "__main__":
    unittest.main()
