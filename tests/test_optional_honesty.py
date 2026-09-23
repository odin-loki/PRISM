"""Cross-cutting optional-adapter honesty.

Search order: config → pinned fetch_deps build
(~/.prism/tools/<component>/<manifest commit>/bin) → PATH → NOTRUN.
Source trees are never executables. clang-tidy is a system tool (not pinned).
python -m unittest tests.test_optional_honesty
"""

from __future__ import annotations

import os
import sys
import tempfile
import tomllib
import unittest
from pathlib import Path
from unittest import mock

from prism.config import (
    VENDOR_DIR,
    adapter_install,
    find_vendored_exe,
    pinned_commit,
    resolve_adapter,
    Config,
)

ROOT = Path(__file__).resolve().parents[1]

_LISTED = (
    "cppcheck", "clang-tidy", "semgrep", "infer", "frama-c",
    "klee", "esbmc", "dafny", "cbmc", "spatch", "strix",
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
    "spatch": ("spatch", "spatch.exe"),
    "strix": ("strix", "strix.exe"),
}


def _manifest() -> dict[str, dict]:
    with open(ROOT / "third_party" / "MANIFEST.toml", "rb") as fh:
        data = tomllib.load(fh)
    return {c["name"]: c for c in data["component"]}


class TestOptionalSearchAndVendor(unittest.TestCase):
    def test_install_hints_name_fetch_deps_except_clang_tidy(self):
        for stage in _LISTED:
            hint = adapter_install(stage)
            self.assertIn("third_party/MANIFEST.toml", hint, msg=stage)
            self.assertNotIn("SOURCES.md", hint, msg=stage)
            if stage == "clang-tidy":
                self.assertIn("system tool", hint)
                self.assertNotIn(stage, VENDOR_DIR)
                continue
            self.assertIn(stage, VENDOR_DIR)
            self.assertIn(f"python scripts/fetch_deps.py --tool {VENDOR_DIR[stage]}", hint)

    def test_every_stage_maps_to_a_pinned_external_component(self):
        rows = _manifest()
        for stage, comp in VENDOR_DIR.items():
            self.assertIn(comp, rows, msg=f"{stage} -> {comp} not in MANIFEST.toml")
            self.assertEqual(rows[comp]["kind"], "external", msg=comp)
            self.assertEqual(pinned_commit(comp), rows[comp]["commit"], msg=comp)
            self.assertIn(stage, rows[comp].get("stages", []), msg=comp)
        self.assertEqual(rows["clang-tidy"]["kind"], "system")
        self.assertNotIn("codeql", VENDOR_DIR)

    def test_source_files_in_the_tools_dir_are_not_exe(self):
        with tempfile.TemporaryDirectory() as td:
            tools = Path(td)
            for stage in _LISTED:
                comp = VENDOR_DIR.get(stage)
                if not comp:
                    continue
                d = tools / comp / str(pinned_commit(comp))
                (d / "src").mkdir(parents=True, exist_ok=True)
                (d / "bin").mkdir(exist_ok=True)
                # Not executable: a source file / README where the binary would be.
                (d / "bin" / _NAMES[stage][0]).write_text("int main(void){return 0;}\n", encoding="utf-8")
                naked = d / "src" / _NAMES[stage][0]
                naked.write_text("// source\n", encoding="utf-8")
                if sys.platform != "win32":
                    naked.chmod(0o755)  # executable, but under src/: never searched
            with mock.patch.dict(os.environ, {"PRISM_TOOLS_DIR": str(tools)}), \
                 mock.patch("prism.config.shutil.which", return_value=None):
                for stage in _LISTED:
                    names = _NAMES[stage]
                    self.assertIsNone(
                        find_vendored_exe(stage, names),
                        msg=f"{stage} must not treat a source file as an exe",
                    )
                    self.assertIsNone(
                        resolve_adapter(Config(), stage, names),
                        msg=f"{stage} must fall through to missing",
                    )

    def test_clang_tidy_is_not_pinned_even_if_dir_exists(self):
        with tempfile.TemporaryDirectory() as td:
            tools = Path(td)
            fake = tools / "clang-tidy" / ("a" * 40) / "bin"
            fake.mkdir(parents=True)
            exe = fake / ("clang-tidy.exe" if sys.platform == "win32" else "clang-tidy")
            exe.write_bytes(b"MZ" if sys.platform == "win32" else b"\x7fELF")
            if sys.platform != "win32":
                exe.chmod(0o755)
            with mock.patch.dict(os.environ, {"PRISM_TOOLS_DIR": str(tools)}), \
                 mock.patch("prism.config.shutil.which", return_value=None):
                self.assertIsNone(find_vendored_exe("clang-tidy", _NAMES["clang-tidy"]))
                self.assertIsNone(resolve_adapter(Config(), "clang-tidy", _NAMES["clang-tidy"]))

    def test_config_beats_vendor_beats_path(self):
        with tempfile.TemporaryDirectory() as td:
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
            with mock.patch("prism.config.find_vendored_exe", return_value=str(vendor)), \
                 mock.patch("prism.config.shutil.which", return_value=str(pathbin)):
                hit = resolve_adapter(cfg, "klee", ("klee",))
            self.assertEqual(Path(hit).resolve(), explicit.resolve())

            cfg2 = Config()
            with mock.patch("prism.config.find_vendored_exe", return_value=str(vendor)), \
                 mock.patch("prism.config.shutil.which", return_value=str(pathbin)):
                hit = resolve_adapter(cfg2, "klee", ("klee",))
            self.assertEqual(Path(hit).resolve(), vendor.resolve())

            with mock.patch("prism.config.find_vendored_exe", return_value=None), \
                 mock.patch("prism.config.shutil.which", return_value=str(pathbin)):
                hit = resolve_adapter(Config(), "klee", ("klee",))
            self.assertEqual(Path(hit).resolve(), pathbin.resolve())

            with mock.patch("prism.config.find_vendored_exe", return_value=None), \
                 mock.patch("prism.config.shutil.which", return_value=None):
                self.assertIsNone(resolve_adapter(Config(), "klee", ("klee",)))


if __name__ == "__main__":
    unittest.main()
