"""Supply chain (roadmap Part 1): manifest, fetch_deps, licence firewall,
SBOM, tool SHA in findings. python -m unittest tests.test_supply_chain
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from unittest import mock

from prism import laws
from prism.adapters import run_cppcheck
from prism.config import (
    VENDOR_DIR,
    Config,
    pinned_commit,
    stamp_tool_sha,
    tool_identity,
    tools_home,
)
from prism.models import Finding

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
MANIFEST = ROOT / "third_party" / "MANIFEST.toml"
MINED = (
    "AFLplusplus", "Frama-C", "FuSeBMC", "Fuzz4All", "cbmc", "coccinelle", "codeql",
    "cppcheck", "dafny", "esbmc", "infer", "klee", "rapidcheck", "semgrep", "strix",
)
LINKED = ("z3", "pcre2", "xsimd", "nlohmann_json", "doctest", "llama.cpp")
HAVE_GIT = shutil.which("git") is not None


def _load(name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f"{name}.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


fetch_deps = _load("fetch_deps")
licence_check = _load("licence_check")
sbom = _load("sbom")


def _git(args: list[str], cwd: Path) -> str:
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@t", GIT_AUTHOR_DATE="2026-01-01T00:00:00Z",
               GIT_COMMITTER_DATE="2026-01-01T00:00:00Z")
    r = subprocess.run(["git", *args], cwd=cwd, env=env, capture_output=True, text=True, check=True)
    return r.stdout.strip()


def _local_tool_repo(td: Path, configure: str | None = None) -> tuple[str, str]:
    """A tiny upstream: returns (url, commit)."""
    repo = td / "upstream"
    repo.mkdir()
    _git(["init", "-q"], repo)
    (repo / "README").write_text("tool\n", encoding="utf-8")
    if configure is not None:
        (repo / "configure").write_text(configure, encoding="utf-8")
        (repo / "configure").chmod(0o755)
        (repo / "makefile").write_text("all:\n\t@true\n", encoding="utf-8")
    _git(["add", "-A"], repo)
    _git(["commit", "-q", "-m", "x"], repo)
    # uploadpack.allowReachableSHA1InWant lets `git fetch <url> <sha>` work locally.
    _git(["config", "uploadpack.allowReachableSHA1InWant", "true"], repo)
    return repo.as_uri(), _git(["rev-parse", "HEAD"], repo)


class TestManifest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.data = fetch_deps.load_manifest(MANIFEST)
        cls.rows = {c["name"]: c for c in cls.data["component"]}

    def test_every_component_is_well_formed(self):
        for c in self.data["component"]:
            fetch_deps.validate_component(c)  # raises on a bad row

    def test_six_linked_libraries_with_tree_digests(self):
        linked = [c["name"] for c in self.data["component"] if c["kind"] == "linked"]
        self.assertEqual(sorted(linked), sorted(LINKED))
        for name in LINKED:
            row = self.rows[name]
            self.assertTrue((ROOT / row["path"]).is_dir(), msg=name)
            self.assertRegex(row["tree_sha256"], r"^[0-9a-f]{64}$")
            self.assertTrue(row["tag"], msg=name)

    def test_external_tools_invoked_by_prism_are_pinned(self):
        want = {"esbmc", "cbmc", "klee", "cppcheck", "frama-c", "infer", "semgrep",
                "coccinelle", "aflplusplus", "dafny", "strix", "cadical", "kissat", "cake_lpr", "bitwuzla",
                # proof re-checkers run by CI (roadmap 8.5), not by the prism binary
                "lean4export", "nanoda"}
        external = {c["name"] for c in self.data["component"] if c["kind"] == "external"}
        self.assertEqual(want, external)
        self.assertEqual(self.rows["clang-tidy"]["kind"], "system")
        for name in ("cadical", "kissat", "cake_lpr", "bitwuzla", "lean4export", "nanoda"):
            self.assertTrue(self.rows[name].get("recipe"), msg=f"{name} needs a build recipe")

    def test_mined_trees_are_gone(self):
        for name in MINED:
            self.assertFalse((ROOT / "third_party" / name).exists(), msg=name)
        tracked = subprocess.run(["git", "ls-files", "third_party"], cwd=ROOT,
                                 capture_output=True, text=True).stdout
        if tracked:
            tops = {line.split("/")[1] for line in tracked.splitlines() if line.count("/") >= 1}
            self.assertFalse(tops & set(MINED), msg=sorted(tops & set(MINED)))
        self.assertFalse((ROOT / "third_party" / "SOURCES.md").exists())
        self.assertFalse((ROOT / "third_party" / "vendor.log").exists())
        # The provenance stays recorded in the manifest.
        text = MANIFEST.read_text(encoding="utf-8")
        self.assertIn("vendor.log recorded clone URLs and timestamps", text)

    def test_linked_tree_matches_manifest_offline(self):
        for name in LINKED:
            self.assertEqual(fetch_deps.check_linked(self.rows[name]), [], msg=name)

    def test_modified_linked_tree_fails_closed(self):
        row = dict(self.rows["nlohmann_json"])
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            dst = root / row["path"]
            shutil.copytree(ROOT / row["path"], dst)
            self.assertEqual(fetch_deps.check_linked(row, root), [])
            with open(dst / "json.hpp", "a", encoding="utf-8") as fh:
                fh.write("\n// tampered\n")
            errs = fetch_deps.check_linked(row, root)
            self.assertTrue(any("tree_sha256 mismatch" in e for e in errs), errs)
            bumped = dict(row, tree_version="9.9.9")
            errs = fetch_deps.check_linked(bumped, root)
            self.assertTrue(any("in-tree version" in e for e in errs), errs)

    def test_cmake_bakes_the_same_pins(self):
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("third_party/MANIFEST.toml", cmake)
        self.assertIn("manifest_pins.hpp", cmake)
        self.assertIn("CMAKE_CONFIGURE_DEPENDS", cmake)
        for bad in ("OneDrive", "/mnt/", "CMAKE_SUPPRESS_REGENERATION", "mined"):
            self.assertNotIn(bad, cmake)

    def test_stage_table_matches_cpp(self):
        cpp = (ROOT / "src" / "prism" / "config.cpp").read_text(encoding="utf-8")
        body = cpp[cpp.index("static const char* vendor_dir_for("):cpp.index("std::string adapter_install(")]
        pairs = dict(re.findall(r'\{"([^"]+)", "([^"]+)"\}', body))
        self.assertEqual(pairs, VENDOR_DIR)
        for stage, comp in VENDOR_DIR.items():
            self.assertEqual(pinned_commit(comp), self.rows[comp]["commit"], msg=stage)


@unittest.skipUnless(HAVE_GIT, "git not on PATH")
class TestFetchDeps(unittest.TestCase):
    def _comp(self, url: str, commit: str, sha: str, **kw) -> dict:
        row = {"name": "demo", "kind": "external", "version": "1", "url": url,
               "commit": commit, "archive_sha256": sha, "spdx": "MIT", "bins": ["demo"]}
        row.update(kw)
        fetch_deps.validate_component(row)
        return row

    def _real_sha(self, url: str, commit: str) -> str:
        with tempfile.TemporaryDirectory() as td:
            tar = Path(td) / "a.tar"
            with self.assertRaises(fetch_deps.HashMismatch):
                fetch_deps.fetch_archive(self._comp(url, commit, "0" * 64), tar)
            self.assertFalse(tar.exists(), "a mismatching archive must be deleted")
            # Recompute the expected value the way fetch_deps does.
            repo = Path(td) / "r"
            repo.mkdir()
            _git(["init", "-q", "--bare"], repo)
            _git(["fetch", "-q", "--depth", "1", url, commit], repo)
            data = subprocess.run(["git", "archive", "--format=tar", f"--prefix=demo-{commit}/", commit],
                                  cwd=repo, capture_output=True, check=True).stdout
            return hashlib.sha256(data).hexdigest()

    def test_tampered_hash_fails_closed_and_installs_nothing(self):
        with tempfile.TemporaryDirectory() as td:
            url, commit = _local_tool_repo(Path(td))
            good = self._real_sha(url, commit)
            tampered = ("f" if good[0] != "f" else "e") + good[1:]
            tools = Path(td) / "tools"
            with self.assertRaises(fetch_deps.HashMismatch) as cm:
                fetch_deps.fetch_tool(self._comp(url, commit, tampered), tools)
            self.assertIn("fail closed", str(cm.exception))
            self.assertFalse((tools / "demo" / commit).exists())
            leftovers = list((tools / "demo").iterdir()) if (tools / "demo").exists() else []
            self.assertEqual(leftovers, [], "no staging dir may survive a failed fetch")
            # CLI: exit code 3 on a hash mismatch.
            man = Path(td) / "MANIFEST.toml"
            man.write_text(
                f'[[component]]\nname = "demo"\nkind = "external"\nversion = "1"\nurl = "{url}"\n'
                f'commit = "{commit}"\narchive_sha256 = "{tampered}"\nspdx = "MIT"\n',
                encoding="utf-8")
            rc = fetch_deps.main(["--manifest", str(man), "--tools-dir", str(tools), "--tool", "demo"])
            self.assertEqual(rc, 3)
            self.assertFalse((tools / "demo" / commit).exists())

    def test_good_hash_installs_verified_source_and_stamp(self):
        with tempfile.TemporaryDirectory() as td:
            url, commit = _local_tool_repo(Path(td))
            good = self._real_sha(url, commit)
            tools = Path(td) / "tools"
            final = fetch_deps.fetch_tool(self._comp(url, commit, good), tools)
            self.assertEqual(final, tools / "demo" / commit)
            self.assertTrue((final / "src" / "README").is_file())
            stamp = json.loads((final / fetch_deps.STAMP).read_text(encoding="utf-8"))
            self.assertEqual(stamp["commit"], commit)
            self.assertEqual(stamp["archive_sha256"], good)
            self.assertFalse(stamp["built"])  # no recipe: source only, never guessed

    @unittest.skipIf(sys.platform == "win32", "sh recipe")
    def test_recipe_build_installs_bin_and_identity_is_commit(self):
        configure = "#!/bin/sh\nmkdir -p build\nprintf '#!/bin/sh\\necho demo 1\\n' > build/demo\nchmod +x build/demo\n"
        with tempfile.TemporaryDirectory() as td:
            url, commit = _local_tool_repo(Path(td), configure=configure)
            good = self._real_sha(url, commit)
            tools = Path(td) / "tools"
            comp = self._comp(url, commit, good, recipe="configure-make", recipe_out=["build/demo"])
            final = fetch_deps.fetch_tool(comp, tools)
            exe = final / "bin" / "demo"
            self.assertTrue(os.access(exe, os.X_OK))
            self.assertEqual(subprocess.run([str(exe)], capture_output=True, text=True).stdout, "demo 1\n")
            with mock.patch.dict(os.environ, {"PRISM_TOOLS_DIR": str(tools)}):
                self.assertEqual(tools_home(), tools)
                self.assertEqual(tool_identity(exe), commit)

    def test_unknown_commit_fails(self):
        with tempfile.TemporaryDirectory() as td:
            url, _commit = _local_tool_repo(Path(td))
            with self.assertRaises(fetch_deps.FetchError):
                fetch_deps.fetch_tool(self._comp(url, "1" * 40, "0" * 64), Path(td) / "tools")

    def test_bad_manifest_rows_are_rejected(self):
        base = {"name": "x", "kind": "external", "version": "1", "url": "u", "spdx": "MIT",
                "commit": "a" * 40, "archive_sha256": "b" * 64}
        fetch_deps.validate_component(base)
        for bad in ({"commit": "abc"}, {"archive_sha256": "zz"}, {"kind": "vendored"}, {"spdx": ""}):
            with self.assertRaises(fetch_deps.FetchError, msg=str(bad)):
                fetch_deps.validate_component(dict(base, **bad))


class TestLicenceFirewall(unittest.TestCase):
    def test_repository_manifest_passes(self):
        self.assertEqual(licence_check.main(["--manifest", str(MANIFEST)]), 0)

    def test_copyleft_linked_component_fails(self):
        for spdx in ("GPL-3.0-or-later", "LGPL-2.1-only", "AGPL-3.0-or-later", "MPL-2.0",
                     "MIT AND GPL-2.0-only", "EPL-2.0", "NOASSERTION"):
            data = {"component": [{"name": "bad", "kind": "linked", "spdx": spdx}]}
            self.assertTrue(licence_check.check(data), msg=spdx)

    def test_permissive_and_external_copyleft_pass(self):
        ok = [
            {"name": "a", "kind": "linked", "spdx": "MIT"},
            {"name": "b", "kind": "linked", "spdx": "BSD-3-Clause WITH PCRE2-exception"},
            {"name": "c", "kind": "linked", "spdx": "MIT OR GPL-2.0-only"},
            {"name": "d", "kind": "linked", "spdx": "Apache-2.0 WITH LLVM-exception"},
            {"name": "e", "kind": "external", "spdx": "GPL-3.0-or-later"},
            {"name": "f", "kind": "external", "spdx": "AGPL-3.0-or-later"},
        ]
        self.assertEqual(licence_check.check({"component": ok}), [])

    def test_ci_runs_the_firewall_and_sbom(self):
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        self.assertIn("scripts/licence_check.py", ci)
        self.assertIn("scripts/fetch_deps.py --linked", ci)
        self.assertIn("scripts/sbom.py", ci)
        rel = (ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
        self.assertIn("id-token: write", rel)
        self.assertIn("sigstore/cosign-installer", rel)
        self.assertIn("SOURCE_DATE_EPOCH", rel)

    def test_self_check_workflow_installs_pyyaml(self):
        wf = (ROOT / ".github" / "workflows" / "self-check.yml").read_text(encoding="utf-8")
        pip_lines = [ln for ln in wf.splitlines() if "pip install" in ln]
        self.assertTrue(pip_lines, "self-check.yml has no pip install line")
        self.assertTrue(any("pyyaml" in ln.lower() for ln in pip_lines))

    def test_self_check_workflow_limits_selfscan_stages(self):
        wf = (ROOT / ".github" / "workflows" / "self-check.yml").read_text(encoding="utf-8")
        self.assertIn(
            "--stage inventory,lints,polyglot",
            wf,
            "self-check self-scan must match ci.yml stage limit (full tree exceeds job timeout)",
        )

    def test_ci_smokes_svcomp_package_archive(self):
        pack = ROOT / "tools" / "svcomp" / "package_archive.py"
        self.assertTrue(pack.is_file(), "SV-COMP submission packager must exist")
        ci = (ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
        self.assertIn("package_archive.py", ci)

    def test_pir_stage_reads_function_budget_env(self):
        stage = (ROOT / "src" / "prism" / "pir" / "stage.cpp").read_text(encoding="utf-8")
        self.assertIn("PRISM_FUNCTION_BUDGET", stage)


class TestSbom(unittest.TestCase):
    def test_cyclonedx_1_5_from_manifest(self):
        bom = sbom.build(MANIFEST.read_bytes(), "1.2.3")
        self.assertEqual(bom["bomFormat"], "CycloneDX")
        self.assertEqual(bom["specVersion"], "1.5")
        self.assertRegex(bom["serialNumber"], r"^urn:uuid:[0-9a-f-]{36}$")
        refs = {c["bom-ref"]: c for c in bom["components"]}
        z3 = refs["linked:z3"]
        self.assertEqual(z3["type"], "library")
        self.assertEqual(z3["scope"], "required")
        self.assertEqual(z3["licenses"], [{"license": {"id": "MIT"}}])
        self.assertTrue(z3["purl"].startswith("pkg:github/z3prover/z3@6f24123f"))
        self.assertEqual(z3["hashes"][0]["alg"], "SHA-256")
        pcre = refs["linked:pcre2"]
        self.assertEqual(pcre["licenses"], [{"expression": "BSD-3-Clause WITH PCRE2-exception"}])
        self.assertEqual(refs["external:cppcheck"]["scope"], "optional")
        dep = bom["dependencies"][0]
        self.assertEqual(dep["ref"], "prism")
        self.assertEqual(sorted(dep["dependsOn"]), sorted(f"linked:{n}" for n in LINKED))

    def test_deterministic(self):
        with mock.patch.dict(os.environ, {"SOURCE_DATE_EPOCH": "1700000000"}):
            a = json.dumps(sbom.build(MANIFEST.read_bytes(), "v1"))
            b = json.dumps(sbom.build(MANIFEST.read_bytes(), "v1"))
        self.assertEqual(a, b)
        self.assertIn('"timestamp": "2023-11-14T22:13:20Z"', a)


class TestToolShaInFindings(unittest.TestCase):
    def test_identity_of_an_unpinned_binary_is_path_and_sha256(self):
        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "tool"
            exe.write_bytes(b"#!/bin/sh\nexit 0\n")
            want = hashlib.sha256(exe.read_bytes()).hexdigest()
            with mock.patch.dict(os.environ, {"PRISM_TOOLS_DIR": str(Path(td) / "tools")}):
                ident = tool_identity(exe)
            self.assertEqual(ident, f"path:{exe.resolve()};sha256:{want}")
            f = Finding(stage="x", status=laws.UNKNOWN, file="", function=None, line=None,
                        cls="", message="m", strength=laws.STRENGTH_FINDS)
            stamp_tool_sha([f], exe)
            self.assertEqual(f.extra["tool_sha"], ident)

    @unittest.skipIf(sys.platform == "win32", "sh stub")
    def test_cppcheck_finding_carries_the_manifest_commit(self):
        commit = pinned_commit("cppcheck")
        self.assertIsNotNone(commit)
        src = ROOT / "testdata" / "abs_ok.c"
        with tempfile.TemporaryDirectory() as td:
            tools = Path(td) / "tools"
            exe = tools / "cppcheck" / str(commit) / "bin" / "cppcheck"
            exe.parent.mkdir(parents=True)
            exe.write_text(
                "#!/bin/sh\n"
                "echo '<error id=\"nullPointer\" severity=\"error\" msg=\"Null pointer\">"
                "<location file=\"abs_ok.c\" line=\"3\"/>' >&2\nexit 0\n",
                encoding="utf-8")
            exe.chmod(0o755)
            with mock.patch.dict(os.environ, {"PRISM_TOOLS_DIR": str(tools)}), \
                 mock.patch("prism.config.shutil.which", return_value=None):
                out = run_cppcheck([src], Config(root=ROOT / "testdata"))
        self.assertEqual(len(out), 1, [f.message for f in out])
        self.assertEqual(out[0].status, laws.FAILED)
        self.assertEqual(out[0].extra.get("tool_sha"), commit)

    def test_optional_tools_stamp_every_finding(self):
        src = (ROOT / "prism" / "adapters_extra.py").read_text(encoding="utf-8")
        self.assertIn("stamp_tool_sha(got, exe)", src)
        cpp = (ROOT / "src" / "prism" / "adapters.cpp").read_text(encoding="utf-8")
        self.assertIn("stamp_tool_sha(more, *exe);", cpp)
        for tool in ("cppcheck", "esbmc", "dafny"):
            self.assertIn(f"auto out = run_{tool}_unstamped(paths, cfg);", cpp)


class TestDocsAndNotices(unittest.TestCase):
    def test_notice_licence_and_history_script(self):
        notice = (ROOT / "NOTICE").read_text(encoding="utf-8")
        self.assertIn("CC-BY-4.0", notice)
        self.assertIn("Fuzz4All", notice)
        lic = (ROOT / "LICENSE").read_text(encoding="utf-8")
        self.assertIn("GNU AFFERO GENERAL PUBLIC LICENSE", lic.splitlines()[0])
        self.assertIn("Version 3, 19 November 2007", lic)
        script = (ROOT / "scripts" / "rewrite_history.sh").read_text(encoding="utf-8")
        for name in MINED:
            self.assertIn(f"third_party/{name}/", script)
        self.assertIn("git filter-repo", script)
        self.assertIn("I UNDERSTAND", script)
        self.assertTrue((ROOT / "docs" / "SUPPLY_CHAIN.md").is_file())


if __name__ == "__main__":
    unittest.main()
