#!/usr/bin/env python3
"""Configure + build PRISM on /var/tmp/prism-wsl (WSL ext4), copy binaries to build_wsl/.

After a successful configure (and again after a successful build) copies
compile_commands.json plus prism_wsl_stamp.json into build_wsl/. Directory
paths in that JSON still point at /var/tmp/prism-wsl. The object tree stays on
ext4 — it is never moved onto /mnt/c.

Never put this behind `wsl -e bash -lc "..."` with `$` — invoke:
  wsl -e python3 '/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis/scripts/wsl_configure_build.py'
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path("/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis")
BUILD = Path("/var/tmp/prism-wsl")
COPY_DEST = ROOT / "build_wsl"
LOG = BUILD / "wsl_configure_build.log"
CC_PERSIST = COPY_DEST / "compile_commands.json"
STAMP_PERSIST = COPY_DEST / "prism_wsl_stamp.json"

CMAKE = [
    "cmake",
    "-B",
    str(BUILD),
    "-G",
    "Ninja",
    "-DCMAKE_C_COMPILER=clang",
    "-DCMAKE_CXX_COMPILER=clang++",
    "-DCMAKE_BUILD_TYPE=Release",
    "-S",
    str(ROOT),
    "-DPRISM_LLAMA=ON",
    "-DPRISM_QT=ON",
    "-DPRISM_Z3=ON",
    "-DPRISM_CUDA=ON",
]


def log(msg: str) -> None:
    print(msg, flush=True)
    LOG.parent.mkdir(parents=True, exist_ok=True)
    with LOG.open("a", encoding="utf-8") as f:
        f.write(msg + "\n")


def run(cmd: list[str], cwd: Path | None = None) -> int:
    log("RUN " + " ".join(cmd))
    env = os.environ.copy()
    env["CMAKE_SKIP_PACKAGE_REGISTRY"] = "ON"
    proc = subprocess.run(cmd, cwd=str(cwd) if cwd else None, env=env)
    log("exit " + str(proc.returncode))
    return proc.returncode


def patch_ninja_skip_regen(ninja: Path) -> None:
    if not ninja.is_file():
        return
    text = ninja.read_text(encoding="utf-8", errors="replace")
    text2 = text.replace("--regenerate-during-build", "--version # skipped regenerate-during-build")
    if text2 != text:
        ninja.write_text(text2, encoding="utf-8")
        log("patched ninja regenerate-during-build to no-op")


def copy_bins() -> list[str]:
    COPY_DEST.mkdir(parents=True, exist_ok=True)
    copied: list[str] = []
    for name in ("prism", "prism_tests", "prism_gui"):
        src = BUILD / name
        if not src.is_file():
            alt = BUILD / "bin" / name
            src = alt if alt.is_file() else src
        if src.is_file():
            dest = COPY_DEST / name
            shutil.copy2(src, dest)
            copied.append(str(dest))
            log("COPIED " + str(src) + " -> " + str(dest))
        else:
            log("MISSING " + name)
    for src in sorted(BUILD.glob("libprism_cuda.so*")) + sorted(BUILD.glob("libprism_native.so*")):
        if not src.is_file():
            continue
        dest = COPY_DEST / src.name
        shutil.copy2(src, dest, follow_symlinks=True)
        copied.append(str(dest))
        log("COPIED " + str(src) + " -> " + str(dest))
    return copied


def _cache_get(cache: str, key: str) -> str:
    for line in cache.splitlines():
        if line.startswith(key + ":") and "=" in line:
            return line.split("=", 1)[1].strip()
    return ""


def persist_compile_metadata() -> None:
    """Copy compile_commands.json + a small stamp into build_wsl/. Objects stay on /var/tmp."""
    COPY_DEST.mkdir(parents=True, exist_ok=True)
    cc_src = BUILD / "compile_commands.json"
    if not cc_src.is_file():
        log("MISSING " + str(cc_src) + " (not persisted)")
        return
    shutil.copy2(cc_src, CC_PERSIST)
    log("COPIED " + str(cc_src) + " -> " + str(CC_PERSIST))

    ninja_src = BUILD / "build.ninja"
    if ninja_src.is_file():
        ninja_dest = COPY_DEST / "prism-wsl-build.ninja"
        shutil.copy2(ninja_src, ninja_dest)
        log("COPIED " + str(ninja_src) + " -> " + str(ninja_dest))

    compiler = ""
    compiler_id = ""
    compiler_version = ""
    cache_path = BUILD / "CMakeCache.txt"
    if cache_path.is_file():
        cache = cache_path.read_text(encoding="utf-8", errors="replace")
        compiler = _cache_get(cache, "CMAKE_CXX_COMPILER")
        compiler_id = _cache_get(cache, "CMAKE_CXX_COMPILER_ID")
        compiler_version = _cache_get(cache, "CMAKE_CXX_COMPILER_VERSION")
    if not compiler:
        compiler = "/usr/bin/clang++"

    files: list[str] = []
    try:
        entries = json.loads(cc_src.read_text(encoding="utf-8"))
        files = sorted({str(e.get("file", "")).replace("\\", "/") for e in entries if e.get("file")})
    except (OSError, json.JSONDecodeError) as exc:
        log("WARN could not parse compile_commands.json for stamp: " + str(exc))

    digest = hashlib.sha256()
    digest.update("\n".join(files).encode("utf-8"))
    stamp = {
        "schema": 1,
        "build_dir": str(BUILD),
        "persist_dir": str(COPY_DEST),
        "compiler": compiler,
        "compiler_id": compiler_id,
        "compiler_version": compiler_version,
        "source_count": len(files),
        "source_list_sha256": digest.hexdigest(),
        "compile_commands_sha256": hashlib.sha256(cc_src.read_bytes()).hexdigest(),
        "note": (
            "compile_commands.json directories still point at "
            + str(BUILD)
            + "; objects stay there. A missing /var/tmp/prism-wsl needs scripts/wsl_configure_build.py."
        ),
    }
    STAMP_PERSIST.write_text(json.dumps(stamp, indent=2) + "\n", encoding="utf-8")
    log("WROTE " + str(STAMP_PERSIST) + " compiler_id=" + compiler_id + " sources=" + str(len(files)))


def main() -> int:
    BUILD.mkdir(parents=True, exist_ok=True)
    LOG.write_text("", encoding="utf-8")
    log("ROOT=" + str(ROOT))
    log("BUILD=" + str(BUILD))

    if run(CMAKE) != 0:
        log("CMAKE_CONFIGURE_FAILED")
        sys.stderr.write(LOG.read_text(encoding="utf-8")[-8000:])
        return 1

    persist_compile_metadata()
    patch_ninja_skip_regen(BUILD / "build.ninja")

    cache = (BUILD / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    log("CACHE_PRISM_LLAMA=" + ("ON" if "PRISM_LLAMA:BOOL=ON" in cache else "OFF"))
    log("CACHE_PRISM_QT=" + ("ON" if "PRISM_QT:BOOL=ON" in cache else "OFF"))
    log("CACHE_PRISM_Z3=" + ("ON" if "PRISM_Z3:BOOL=ON" in cache else "OFF"))
    log("CACHE_PRISM_CUDA=" + ("ON" if "PRISM_CUDA:BOOL=ON" in cache else "OFF"))

    ninja_txt = (BUILD / "build.ninja").read_text(encoding="utf-8", errors="replace")
    has_gui = "\nbuild prism_gui:" in ninja_txt or "build prism_gui:" in ninja_txt
    has_llama = "third_party/llama.cpp" in ninja_txt
    has_cuda = "src/cuda/mutate.cu" in ninja_txt or "prism_cuda" in ninja_txt
    log("NINJA_HAS_PRISM_GUI=" + str(has_gui))
    log("NINJA_HAS_LLAMA=" + str(has_llama))
    log("NINJA_HAS_PRISM_CUDA=" + str(has_cuda))

    targets = ["prism", "prism_tests", "prism_native"]
    if has_gui:
        targets.append("prism_gui")
    if has_cuda:
        targets.append("prism_cuda")
    build_cmd = ["ninja", "-C", str(BUILD), "-j", "8", *targets]
    if run(build_cmd) != 0:
        log("CMAKE_BUILD_FAILED")
        sys.stderr.write(LOG.read_text(encoding="utf-8")[-8000:])
        return 1

    copied = copy_bins()
    persist_compile_metadata()
    log("DONE copied=" + ",".join(copied))
    print(LOG.read_text(encoding="utf-8")[-4000:])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
