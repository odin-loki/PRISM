#!/usr/bin/env python3
"""Link prism_tests and prism_gui using archives already on /tmp."""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path("/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis")
BUILD = ROOT / "build_wsl"
TMP = Path("/tmp/prism-link")

LIBS = {
    "libprism_core.a": BUILD / "libprism_core.a",
    "libpcre2-8.a": BUILD / "third_party/pcre2/libpcre2-8.a",
    "libz3.a": BUILD / "third_party/z3/libz3.a",
    "libllama.a": BUILD / "third_party/llama.cpp/src/libllama.a",
    "libggml.a": BUILD / "third_party/llama.cpp/ggml/src/libggml.a",
    "libggml-cpu.a": BUILD / "third_party/llama.cpp/ggml/src/libggml-cpu.a",
    "libggml-base.a": BUILD / "third_party/llama.cpp/ggml/src/libggml-base.a",
}


def ninja_cmd(target: str) -> str:
    p = subprocess.run(
        ["ninja", "-C", str(BUILD), "-t", "commands", target],
        capture_output=True,
        text=True,
    )
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        raise SystemExit("ninja -t commands failed for " + target)
    for ln in reversed(p.stdout.splitlines()):
        if "clang++" in ln and " -o " in ln:
            return ln
    raise SystemExit("no link line for " + target)


def rewrite(cmd: str) -> str:
    # Prefer /tmp copies of fat archives.
    repl = [
        ("libprism_core.a", str(TMP / "libprism_core.a")),
        ("third_party/pcre2/libpcre2-8.a", str(TMP / "libpcre2-8.a")),
        ("third_party/z3/libz3.a", str(TMP / "libz3.a")),
        ("third_party/llama.cpp/src/libllama.a", str(TMP / "libllama.a")),
        ("third_party/llama.cpp/ggml/src/libggml.a", str(TMP / "libggml.a")),
        ("third_party/llama.cpp/ggml/src/libggml-cpu.a", str(TMP / "libggml-cpu.a")),
        ("third_party/llama.cpp/ggml/src/libggml-base.a", str(TMP / "libggml-base.a")),
    ]
    for old, new in repl:
        cmd = cmd.replace(old, new)
    return cmd


def main() -> int:
    TMP.mkdir(parents=True, exist_ok=True)
    for name, src in LIBS.items():
        dest = TMP / name
        if src.is_file():
            if not dest.is_file() or src.stat().st_mtime > dest.stat().st_mtime or dest.stat().st_size == 0:
                print("COPY", src, "->", dest, flush=True)
                shutil.copy2(src, dest)
        else:
            print("MISSING LIB", src, flush=True)
            return 1

    for tgt, name in (("prism_tests", "prism_tests"), ("prism_gui", "prism_gui")):
        cmd = rewrite(ninja_cmd(tgt))
        cmd, n = re.subn(r" -o \S+", " -o " + str(TMP / name), cmd, count=1)
        print("LINK", name, flush=True)
        print(cmd[:300], "...", flush=True)
        r = subprocess.run(["bash", "-lc", cmd], cwd=str(BUILD))
        if r.returncode != 0:
            return r.returncode
        shutil.copy2(TMP / name, BUILD / name)
        print("COPIED", name, (TMP / name).stat().st_size, flush=True)

    for name in ("prism", "prism_tests", "prism_gui"):
        p = BUILD / name
        t = TMP / name
        print(
            name,
            "build_wsl" if p.is_file() else "NO-build_wsl",
            p.stat().st_size if p.is_file() else 0,
            "tmp" if t.is_file() else "NO-tmp",
            t.stat().st_size if t.is_file() else 0,
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
