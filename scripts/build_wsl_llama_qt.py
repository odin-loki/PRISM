#!/usr/bin/env python3
"""Build llama/Qt/prism in build_wsl, linking on /tmp if the OneDrive link stalls."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path("/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis")
BUILD = ROOT / "build_wsl"
TMP = Path("/tmp/prism-link")
NINJA = BUILD / "build.ninja"


def patch_regen() -> None:
    text = NINJA.read_text(encoding="utf-8", errors="replace")
    text2 = text.replace("--regenerate-during-build", "--version # skipped regenerate-during-build")
    if text2 != text:
        NINJA.write_text(text2, encoding="utf-8")
        print("patched RERUN_CMAKE", flush=True)
    else:
        print("RERUN_CMAKE already patched or absent", flush=True)


def run(cmd: list[str], cwd: Path, timeout: float | None = None) -> int:
    print("RUN " + " ".join(cmd), flush=True)
    try:
        p = subprocess.run(cmd, cwd=str(cwd), timeout=timeout)
        return p.returncode
    except subprocess.TimeoutExpired:
        print("TIMEOUT " + " ".join(cmd), flush=True)
        return 124


def ninja_commands(target: str) -> str:
    p = subprocess.run(
        ["ninja", "-C", str(BUILD), "-t", "commands", target],
        capture_output=True,
        text=True,
    )
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        raise SystemExit(p.returncode)
    return p.stdout


def last_link_line(target: str) -> str:
    lines = [ln for ln in ninja_commands(target).splitlines() if ln.strip()]
    for ln in reversed(lines):
        if "clang++" in ln and " -o " in ln:
            return ln
    raise SystemExit("no clang++ link line for " + target)


def link_on_tmp(target: str, outfile: str) -> int:
    TMP.mkdir(parents=True, exist_ok=True)
    cmd = last_link_line(target)
    # force output onto ext4
    import re

    cmd2, n = re.subn(r" -o \S+", " -o " + str(TMP / outfile), cmd, count=1)
    if n != 1:
        print("could not rewrite -o for", target, flush=True)
        cmd2 = cmd
    print("LINK_TMP", cmd2[:220], "...", flush=True)
    r = subprocess.run(["bash", "-lc", cmd2], cwd=str(BUILD))
    if r.returncode == 0:
        dest = BUILD / outfile
        shutil.copy2(TMP / outfile, dest)
        print("COPIED", TMP / outfile, "->", dest, flush=True)
    return r.returncode


def main() -> int:
    os.environ["CMAKE_SKIP_PACKAGE_REGISTRY"] = "ON"
    patch_regen()

    # Compile llama + prism objects; skip relinking huge libz3.a.
    compile_targets = [
        "third_party/llama.cpp/src/libllama.a",
        "third_party/llama.cpp/ggml/src/libggml.a",
        "third_party/llama.cpp/ggml/src/libggml-cpu.a",
        "third_party/llama.cpp/ggml/src/libggml-base.a",
        "libprism_core.a",
        "CMakeFiles/prism.dir/src/prism/main.cpp.o",
        "CMakeFiles/prism_tests.dir/tests/cpp/test_main.cpp.o",
        "CMakeFiles/prism_gui.dir/src/gui/main.cpp.o",
        "CMakeFiles/prism_gui.dir/src/gui/MainWindow.cpp.o",
        "prism_gui_autogen",
    ]
    rc = run(["ninja", "-j", "8", *compile_targets], BUILD, timeout=900)
    if rc != 0:
        return rc

    for tgt, name in (("prism", "prism"), ("prism_tests", "prism_tests"), ("prism_gui", "prism_gui")):
        rc = link_on_tmp(tgt, name)
        if rc != 0:
            print("tmp link failed for", tgt, "; falling back to ninja", flush=True)
            rc = run(["ninja", "-j", "2", tgt], BUILD, timeout=300)
            if rc != 0:
                return rc

    for name in ("prism", "prism_tests", "prism_gui"):
        p = BUILD / name
        print("EXISTS" if p.is_file() else "MISSING", p, "size=" + (str(p.stat().st_size) if p.is_file() else "-"), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
