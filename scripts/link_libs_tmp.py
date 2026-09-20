#!/usr/bin/env python3
"""Compile remaining PRISM/llama/Qt objects, archive/link on /tmp (ext4)."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path("/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis")
BUILD = ROOT / "build_wsl"
TMP = Path("/tmp/prism-link")


def run(cmd: list[str], cwd: Path | None = None, timeout: float | None = None) -> int:
    print("RUN " + " ".join(cmd), flush=True)
    p = subprocess.run(cmd, cwd=str(cwd) if cwd else None, timeout=timeout)
    return p.returncode


def ninja_cmd(target: str) -> str:
    p = subprocess.run(
        ["ninja", "-C", str(BUILD), "-t", "commands", target],
        capture_output=True,
        text=True,
    )
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        raise SystemExit("ninja -t commands failed for " + target)
    lines = [ln for ln in p.stdout.splitlines() if ln.strip()]
    return lines[-1] if lines else ""


def bash(cmd: str, cwd: Path) -> int:
    print("BASH", cmd[:240], "...", flush=True)
    return subprocess.run(["bash", "-lc", cmd], cwd=str(cwd)).returncode


def main() -> int:
    os.environ["CMAKE_SKIP_PACKAGE_REGISTRY"] = "ON"
    TMP.mkdir(parents=True, exist_ok=True)
    ninja = BUILD / "build.ninja"
    text = ninja.read_text(encoding="utf-8", errors="replace")
    text2 = text.replace("--regenerate-during-build", "--version # skipped regenerate-during-build")
    if text2 != text:
        ninja.write_text(text2, encoding="utf-8")
        print("patched RERUN_CMAKE", flush=True)

    # Archive libz3 on ext4 if ninja wants a rebuild.
    z3_a = BUILD / "third_party/z3/libz3.a"
    z3_cmd = ninja_cmd("third_party/z3/libz3.a")
    if z3_cmd:
        z3_tmp = TMP / "libz3.a"
        z3_cmd2 = z3_cmd.replace("third_party/z3/libz3.a", str(z3_tmp))
        rc = bash(z3_cmd2, BUILD)
        if rc != 0:
            return rc
        shutil.copy2(z3_tmp, z3_a)
        print("COPIED libz3.a", flush=True)

    objs = [
        "CMakeFiles/prism_core.dir/src/prism/stages_rest.cpp.o",
        "CMakeFiles/prism_core.dir/src/prism/pipeline.cpp.o",
        "CMakeFiles/prism.dir/src/prism/main.cpp.o",
        "CMakeFiles/prism_tests.dir/tests/cpp/test_main.cpp.o",
        "CMakeFiles/prism_gui.dir/src/gui/main.cpp.o",
        "CMakeFiles/prism_gui.dir/src/gui/MainWindow.cpp.o",
        "prism_gui_autogen",
        "third_party/llama.cpp/src/libllama.a",
        "third_party/llama.cpp/ggml/src/libggml.a",
        "third_party/llama.cpp/ggml/src/libggml-cpu.a",
        "third_party/llama.cpp/ggml/src/libggml-base.a",
    ]
    rc = run(["ninja", "-j", "8", *objs], BUILD, timeout=900)
    if rc != 0:
        return rc

    # Archive prism_core on /tmp
    core_cmd = ninja_cmd("libprism_core.a")
    core_tmp = TMP / "libprism_core.a"
    core_cmd2 = core_cmd.replace("libprism_core.a", str(core_tmp))
    rc = bash(core_cmd2, BUILD)
    if rc != 0:
        return rc
    shutil.copy2(core_tmp, BUILD / "libprism_core.a")
    print("COPIED libprism_core.a", flush=True)

    for tgt, name in (("prism", "prism"), ("prism_tests", "prism_tests"), ("prism_gui", "prism_gui")):
        cmd = ninja_cmd(tgt)
        cmd2, n = re.subn(r" -o \S+", " -o " + str(TMP / name), cmd, count=1)
        if n != 1:
            print("failed to rewrite -o for", tgt, flush=True)
            cmd2 = cmd
        rc = bash(cmd2, BUILD)
        if rc != 0:
            return rc
        shutil.copy2(TMP / name, BUILD / name)
        print("COPIED", name, "size", (TMP / name).stat().st_size, flush=True)

    for name in ("prism", "prism_tests", "prism_gui"):
        p = BUILD / name
        print("EXISTS" if p.is_file() else "MISSING", p, flush=True)
    llama = BUILD / "third_party/llama.cpp/src/libllama.a"
    print("LLAMA", "yes" if llama.is_file() else "no", llama, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
