"""Cross-cutting optional-adapter honesty.

Search order: config → third_party/<vendor> built exe → PATH → NOTRUN.
Vendored *source* is not a proof. clang-tidy is not vendored.
python -m unittest tests.test_optional_honesty
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from helix.config import (
    VENDOR_DIR,
    adapter_install,
    find_vendored_exe,
    resolve_adapter,
    Config,
)

_LISTED = (
    "cppcheck", "clang-tidy", "semgrep", "infer", "frama-c",
    "klee", "esbmc", "dafny", "cbmc", "codeql", "spatch", "strix",
)

_NAMES = {
    "cppcheck": ("cppcheck", "cppcheck.exe"),
    "clang-tidy": ("clang-tidy", "clang-tidy.exe"),
    "semgrep": ("semgrep", "semgrep.exe"),
    "infer": ("infer",),
    "frama-c": ("frama-c", "frama-c.exe"),
    "klee": ("klee",),
    "esbmc": ("esbmc", "esbmc.exe"),
    "dafny": ("dafny", "dafny.exe"),
    "cbmc": ("cbmc", "cbmc.exe"),
    "codeql": ("codeql", "codeql.exe"),
    "spatch": ("spatch", "spatch.exe"),
    "strix": ("strix", "strix.exe"),
}


class TestOptionalSearchAndVendor(unittest.TestCase):
    def test_install_hints_point_at_vendored_trees_except_clang_tidy(self):
        for stage in _LISTED:
            hint = adapter_install(stage)
            self.assertIn("SOURCES.md", hint, msg=stage)
            self.assertIn("third_party", hint, msg=stage)
            if stage == "clang-tidy":
                self.assertIn("not vendored", hint)
                self.assertNotIn(stage, VENDOR_DIR)
                continue
            self.assertIn(stage, VENDOR_DIR)
            self.assertIn(f"third_party/{VENDOR_DIR[stage]}", hint)
            self.assertIn("build from vendored", hint)

    def test_vendored_source_files_are_not_exe(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "third_party").mkdir()
            (root / "third_party" / "SOURCES.md").write_text("vendored\n", encoding="utf-8")
            for stage in _LISTED:
                vendor = VENDOR_DIR.get(stage)
                if not vendor:
                    continue
                d = root / "third_party" / vendor
                d.mkdir(parents=True, exist_ok=True)
                (d / f"{_NAMES[stage][0]}.py").write_text("print('source')\n", encoding="utf-8")
                (d / f"{_NAMES[stage][0]}.c").write_text("int main(void) { return 0; }\n", encoding="utf-8")
                (d / f"{_NAMES[stage][0]}.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
                (d / "README.md").write_text("source tree, not a binary\n", encoding="utf-8")
                src_dir = d / "src"
                src_dir.mkdir(exist_ok=True)
                naked = src_dir / _NAMES[stage][0]
                naked.write_text("// vendored source\n", encoding="utf-8")
                if sys.platform != "win32":
                    naked.chmod(0o755)
            with mock.patch("helix.config.repo_root", return_value=root), \
                 mock.patch("helix.config.shutil.which", return_value=None):
                for stage in _LISTED:
                    names = _NAMES[stage]
                    self.assertIsNone(
                        find_vendored_exe(stage, names),
                        msg=f"{stage} must not treat vendored source as an exe",
                    )
                    self.assertIsNone(
                        resolve_adapter(Config(), stage, names),
                        msg=f"{stage} must fall through source tree to missing",
                    )

    def test_clang_tidy_is_not_vendored_even_if_tree_exists(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "third_party").mkdir()
            (root / "third_party" / "SOURCES.md").write_text("vendored\n", encoding="utf-8")
            fake = root / "third_party" / "clang-tidy" / "bin"
            fake.mkdir(parents=True)
            exe = fake / ("clang-tidy.exe" if sys.platform == "win32" else "clang-tidy")
            exe.write_bytes(b"MZ" if sys.platform == "win32" else b"\x7fELF")
            if sys.platform != "win32":
                exe.chmod(0o755)
            with mock.patch("helix.config.repo_root", return_value=root), \
                 mock.patch("helix.config.shutil.which", return_value=None):
                self.assertIsNone(find_vendored_exe("clang-tidy", _NAMES["clang-tidy"]))
                self.assertIsNone(resolve_adapter(Config(), "clang-tidy", _NAMES["clang-tidy"]))

    def test_config_beats_vendor_beats_path(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            explicit = Path(td) / ("klee.exe" if sys.platform == "win32" else "klee")
            explicit.write_bytes(b"x")
            pathbin = Path(td) / ("path-klee.exe" if sys.platform == "win32" else "path-klee")
            pathbin.write_bytes(b"x")
            vendor = Path(td) / "vendor-klee"
            vendor.write_bytes(b"x")
            if sys.platform != "win32":
                for p in (explicit, pathbin, vendor):
                    p.chmod(0o755)
            cfg = Config(tools={"klee": str(explicit)})
            with mock.patch("helix.config.find_vendored_exe", return_value=str(vendor)), \
                 mock.patch("helix.config.shutil.which", return_value=str(pathbin)):
                hit = resolve_adapter(cfg, "klee", ("klee",))
            self.assertEqual(Path(hit).resolve(), explicit.resolve())

            cfg2 = Config()
            with mock.patch("helix.config.find_vendored_exe", return_value=str(vendor)), \
                 mock.patch("helix.config.shutil.which", return_value=str(pathbin)):
                hit = resolve_adapter(cfg2, "klee", ("klee",))
            self.assertEqual(Path(hit).resolve(), vendor.resolve())

            with mock.patch("helix.config.find_vendored_exe", return_value=None), \
                 mock.patch("helix.config.shutil.which", return_value=str(pathbin)):
                hit = resolve_adapter(Config(), "klee", ("klee",))
            self.assertEqual(Path(hit).resolve(), pathbin.resolve())

            with mock.patch("helix.config.find_vendored_exe", return_value=None), \
                 mock.patch("helix.config.shutil.which", return_value=None):
                self.assertIsNone(resolve_adapter(Config(), "klee", ("klee",)))


if __name__ == "__main__":
    unittest.main()
