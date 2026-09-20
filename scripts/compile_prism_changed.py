#!/usr/bin/env python3
"""Compile changed prism sources using compile_commands.json (no CMake reconfigure).

Objects stay on /var/tmp/prism-wsl (ext4; survives WSL session better than /tmp).
Linking copies objs/libs onto /tmp/prism-link so libz3.a is never linked from
/mnt/c. Binaries are copied to build_wsl/.
Never invokes cmake configure. If another ninja owns /var/tmp/prism-wsl, skip ninja
and link with the recorded clang++ command instead.

compile_commands.json lookup (first hit wins):
  1. /var/tmp/prism-wsl/compile_commands.json
  2. build_wsl/compile_commands.json (copy persisted by wsl_configure_build.py;
     directory fields still point at /var/tmp/prism-wsl after the next configure)
  3. else exit and tell the user to run scripts/wsl_configure_build.py

If the live /var/tmp/prism-wsl tree is gone, exit NEED_RECONFIGURE —
do not wait for libs, do not no-op, do not pretend a compile happened.
The object tree is never moved onto /mnt/c (OneDrive).
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path("/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis")
BUILD = Path("/var/tmp/prism-wsl")
LINK_TMP = Path("/tmp/prism-link")
COPY_DEST = ROOT / "build_wsl"
CC_PERSIST = COPY_DEST / "compile_commands.json"
STAMP_PERSIST = COPY_DEST / "prism_wsl_stamp.json"
RECONFIGURE_HINT = (
    "run: wsl -e python3 '" + str(ROOT / "scripts/wsl_configure_build.py") + "'"
)

CHANGED_DEFAULT = (
    "stages_rest.cpp",
    "pipeline.cpp",
    "taxonomy.cpp",
    "havoc.cpp",
    "checkers_core.cpp",
    "adapters.cpp",
    "interval.cpp",
    "config.cpp",
    "journal.cpp",
    "models.cpp",
)

PRISM_CORE_CPP = (
    "adapters.cpp",
    "bmc.cpp",
    "capi.cpp",
    "checkers_api.cpp",
    "checkers_core.cpp",
    "checkers_cxx.cpp",
    "checkers_dispatch.cpp",
    "config.cpp",
    "cparse.cpp",
    "hash.cpp",
    "havoc.cpp",
    "inline.cpp",
    "interval.cpp",
    "journal.cpp",
    "laws.cpp",
    "models.cpp",
    "pipeline.cpp",
    "regex.cpp",
    "stages_rest.cpp",
    "taxonomy.cpp",
)

LIB_Z3 = BUILD / "third_party/z3/libz3.a"
LIB_LLAMA = BUILD / "third_party/llama.cpp/src/libllama.a"
LIB_GGML = BUILD / "third_party/llama.cpp/ggml/src/libggml.a"
LIB_GGML_CPU = BUILD / "third_party/llama.cpp/ggml/src/libggml-cpu.a"
LIB_GGML_BASE = BUILD / "third_party/llama.cpp/ggml/src/libggml-base.a"
LIB_PCRE = BUILD / "third_party/pcre2/libpcre2-8.a"
LIB_CORE = BUILD / "libprism_core.a"


def log(msg: str) -> None:
    print(msg, flush=True)


def ninja_busy() -> bool:
    p = subprocess.run(
        ["pgrep", "-af", "ninja"],
        capture_output=True,
        text=True,
    )
    for line in p.stdout.splitlines():
        if "pgrep" in line:
            continue
        if "/var/tmp/prism-wsl" in line or line.strip().startswith("ninja"):
            log("NINJA_BUSY " + line.strip())
            return True
    return False


def patch_ninja_skip_regen(ninja: Path) -> None:
    if not ninja.is_file():
        return
    text = ninja.read_text(encoding="utf-8", errors="replace")
    text2 = text.replace("--regenerate-during-build", "--version # skipped regenerate-during-build")
    if text2 != text:
        ninja.write_text(text2, encoding="utf-8")
        log("patched RERUN_CMAKE to no-op")


def load_compile_commands() -> list[dict]:
    """Prefer the live /var/tmp tree, then the persisted copy under build_wsl/."""
    live = BUILD / "compile_commands.json"
    if live.is_file():
        log("COMPILE_COMMANDS " + str(live))
        return json.loads(live.read_text(encoding="utf-8"))
    if CC_PERSIST.is_file():
        log("COMPILE_COMMANDS_PERSISTED " + str(CC_PERSIST))
        log("note: entries still use directory " + str(BUILD))
        return json.loads(CC_PERSIST.read_text(encoding="utf-8"))
    raise SystemExit(
        "NEED_RECONFIGURE missing compile_commands.json at "
        + str(live)
        + " and "
        + str(CC_PERSIST)
        + "\n/var/tmp/prism-wsl is not a live compile_commands.json; incremental compile cannot invent commands.\n"
        + RECONFIGURE_HINT
    )


def live_build_tree() -> bool:
    """True only if /var/tmp/prism-wsl still has a CMake/Ninja tree we can compile into."""
    return (
        BUILD.is_dir()
        and (BUILD / "CMakeCache.txt").is_file()
        and (BUILD / "CMakeFiles").is_dir()
    )


def require_live_build_tree() -> None:
    if live_build_tree():
        return
    reasons: list[str] = []
    if not BUILD.exists():
        reasons.append(str(BUILD) + " is missing (run scripts/wsl_configure_build.py once)")
    else:
        if not (BUILD / "CMakeCache.txt").is_file():
            reasons.append("missing " + str(BUILD / "CMakeCache.txt"))
        if not (BUILD / "CMakeFiles").is_dir():
            reasons.append("missing " + str(BUILD / "CMakeFiles"))
    extra = ""
    if CC_PERSIST.is_file():
        extra = (
            "\nFound persisted "
            + str(CC_PERSIST)
            + " but its directory paths still point at "
            + str(BUILD)
            + ", which is not a usable build tree."
        )
    if STAMP_PERSIST.is_file():
        try:
            stamp = json.loads(STAMP_PERSIST.read_text(encoding="utf-8"))
            extra += (
                "\nStamp build_dir="
                + str(stamp.get("build_dir", ""))
                + " compiler="
                + str(stamp.get("compiler", ""))
                + " compiler_id="
                + str(stamp.get("compiler_id", ""))
            )
        except (OSError, json.JSONDecodeError):
            extra += "\nStamp present but unreadable: " + str(STAMP_PERSIST)
    raise SystemExit(
        "NEED_RECONFIGURE /var/tmp/prism-wsl is not a live CMake build.\n"
        + "\n".join(reasons)
        + extra
        + "\nIncremental compile will not run (no silent no-op).\n"
        + RECONFIGURE_HINT
    )


def obj_from_command(cmd: str) -> Path | None:
    m = re.search(r" -o (\S+)", cmd)
    if not m:
        return None
    p = Path(m.group(1))
    if not p.is_absolute():
        p = BUILD / p
    return p


def compile_entry(e: dict) -> tuple[int, str, str]:
    f = e["file"].replace("\\", "/")
    cmd = e["command"]
    name = Path(f).name
    if not live_build_tree():
        log("NEED_RECONFIGURE not compiling " + name + ": " + str(BUILD) + " is gone")
        return 1, name, cmd
    out = obj_from_command(cmd)
    if out:
        out.parent.mkdir(parents=True, exist_ok=True)
    log("COMPILE " + name)
    log("CMD " + cmd)
    r = subprocess.run(["bash", "-lc", cmd], cwd=e.get("directory", str(BUILD)))
    return r.returncode, name, cmd


def matching_entries(cc: list[dict], want: tuple[str, ...]) -> list[dict]:
    by_key: dict[str, dict] = {}
    want_set = set(want)
    for e in cc:
        f = e["file"].replace("\\", "/")
        cmd = e.get("command", "")
        if "prism_native" in cmd:
            continue
        name = Path(f).name
        matched = None
        for w in want:
            if f.endswith("/" + w) or name == w:
                matched = w
                break
        if matched is None and name not in want_set:
            continue
        # Keep distinct full paths (ggml quants.c vs arch/x86/quants.c).
        key = f
        prev = by_key.get(key)
        if prev is None:
            by_key[key] = e
            continue
        prev_cmd = prev.get("command", "")
        if "prism_core.dir" in cmd and "prism_core.dir" not in prev_cmd:
            by_key[key] = e
        elif "prism.dir" in cmd and name == "main.cpp":
            by_key[key] = e
        elif "prism_tests.dir" in cmd and name == "test_main.cpp":
            by_key[key] = e
    return list(by_key.values())


def compile_want(cc: list[dict], want: tuple[str, ...], jobs: int = 4) -> list[str]:
    entries = matching_entries(cc, want)
    if not entries:
        raise SystemExit("no matching compile commands for " + " ".join(want))
    cmds: list[str] = []
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = [ex.submit(compile_entry, e) for e in entries]
        for fut in as_completed(futs):
            rc, name, cmd = fut.result()
            cmds.append(cmd)
            if rc != 0:
                raise SystemExit("compile failed: " + name + " rc=" + str(rc))
    return cmds


def wait_for(path: Path, timeout: float = 3600.0) -> bool:
    if path.is_file() and path.stat().st_size > 0:
        log("HAVE " + str(path) + " size=" + str(path.stat().st_size))
        return True
    if not live_build_tree():
        log("NEED_RECONFIGURE " + str(BUILD) + " vanished while waiting for " + str(path))
        return False
    if not ninja_busy():
        log("NEED_RECONFIGURE missing " + str(path) + " and ninja is not building /var/tmp/prism-wsl")
        log(RECONFIGURE_HINT)
        return False
    t0 = time.time()
    while time.time() - t0 < timeout:
        if path.is_file() and path.stat().st_size > 0:
            log("HAVE " + str(path) + " size=" + str(path.stat().st_size))
            return True
        if not live_build_tree():
            log("NEED_RECONFIGURE " + str(BUILD) + " vanished while waiting for " + str(path))
            return False
        if not ninja_busy():
            log("NEED_RECONFIGURE missing " + str(path) + " and ninja stopped")
            log(RECONFIGURE_HINT)
            return False
        time.sleep(5)
    log("MISSING after wait " + str(path))
    return False


def ninja_commands(target: str) -> str:
    p = subprocess.run(
        ["ninja", "-C", str(BUILD), "-t", "commands", target],
        capture_output=True,
        text=True,
    )
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        raise SystemExit("ninja -t commands failed for " + target)
    return p.stdout


def last_link_line(target: str) -> str:
    lines = [ln for ln in ninja_commands(target).splitlines() if ln.strip()]
    for ln in reversed(lines):
        if "clang++" in ln and " -o " in ln:
            return ln
    raise SystemExit("no clang++ link line for " + target)


def copy_link_inputs(cmd: str) -> str:
    """Copy .a/.o inputs onto ext4 and rewrite the link command to use them."""
    LINK_TMP.mkdir(parents=True, exist_ok=True)
    parts = cmd.split()
    out: list[str] = []
    i = 0
    while i < len(parts):
        tok = parts[i]
        if tok in ("-o",) and i + 1 < len(parts):
            out.append(tok)
            i += 1
            continue
        cand = tok
        p = Path(cand)
        if not p.is_absolute():
            p = BUILD / cand
        if p.suffix in {".a", ".o"} and p.is_file():
            dest = LINK_TMP / p.name
            if p.resolve() != dest.resolve():
                shutil.copy2(p, dest)
            out.append(str(dest))
        else:
            out.append(tok)
        i += 1
    rewritten = " ".join(out)
    rewritten, n = re.subn(r" -o \S+", " -o " + str(LINK_TMP / "OUTBIN"), rewritten, count=1)
    if n != 1:
        log("could not rewrite -o")
    return rewritten


def archive_objs(lib: Path, objs: list[Path]) -> None:
    if not objs:
        raise SystemExit("no objects for " + str(lib))
    missing = [str(o) for o in objs if not o.is_file()]
    if missing:
        raise SystemExit("missing objects for " + str(lib) + ": " + " ".join(missing[:8]))
    lib.parent.mkdir(parents=True, exist_ok=True)
    if lib.exists():
        lib.unlink()
    cmd = ["llvm-ar", "qc", str(lib), *[str(o) for o in objs]]
    log("AR " + str(lib) + " n=" + str(len(objs)))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        raise SystemExit("llvm-ar failed " + str(lib))
    subprocess.run(["llvm-ranlib", str(lib)], check=True)


def ninja_explicit_objs(target_rel: str) -> list[Path]:
    text = (BUILD / "build.ninja").read_text(encoding="utf-8", errors="replace")
    key = "build " + target_rel + ":"
    i = text.find(key)
    if i < 0:
        raise SystemExit("ninja target missing: " + target_rel)
    line = text[i : text.find("\n", i)]
    rest = line.split(":", 1)[1].strip()
    toks = rest.split()
    if toks:
        toks = toks[1:]  # drop rule name
    objs: list[Path] = []
    for tok in toks:
        if tok in {"|", "||"}:
            break
        if tok.endswith(".o"):
            p = Path(tok)
            objs.append(p if p.is_absolute() else BUILD / p)
    return objs


def archive_core() -> None:
    objs = ninja_explicit_objs("libprism_core.a")
    archive_objs(LIB_CORE, objs)


def archive_llama_libs() -> None:
    archive_objs(LIB_LLAMA, ninja_explicit_objs("third_party/llama.cpp/src/libllama.a"))
    archive_objs(LIB_GGML, ninja_explicit_objs("third_party/llama.cpp/ggml/src/libggml.a"))
    archive_objs(LIB_GGML_CPU, ninja_explicit_objs("third_party/llama.cpp/ggml/src/libggml-cpu.a"))
    archive_objs(LIB_GGML_BASE, ninja_explicit_objs("third_party/llama.cpp/ggml/src/libggml-base.a"))


def link_direct(outfile: str, main_obj: Path) -> int:
    LINK_TMP.mkdir(parents=True, exist_ok=True)
    libs = (LIB_CORE, LIB_PCRE, LIB_Z3, LIB_LLAMA, LIB_GGML, LIB_GGML_CPU, LIB_GGML_BASE)
    for lib in libs:
        if not lib.is_file():
            log("MISSING_LIB " + str(lib))
            return 1
        shutil.copy2(lib, LINK_TMP / lib.name)
    main_dest = LINK_TMP / main_obj.name
    shutil.copy2(main_obj, main_dest)
    out = LINK_TMP / outfile
    cmd = [
        "/usr/bin/clang++",
        "-O3",
        "-DNDEBUG",
        str(main_dest),
        "-o",
        str(out),
        str(LINK_TMP / "libprism_core.a"),
        str(LINK_TMP / "libpcre2-8.a"),
        str(LINK_TMP / "libz3.a"),
        str(LINK_TMP / "libllama.a"),
        str(LINK_TMP / "libggml.a"),
        "-ldl",
        str(LINK_TMP / "libggml-cpu.a"),
        str(LINK_TMP / "libggml-base.a"),
        "-lm",
        "-pthread",
    ]
    log("LINK_TMP " + " ".join(cmd))
    r = subprocess.run(cmd, cwd=str(LINK_TMP))
    if r.returncode == 0:
        dest = BUILD / outfile
        shutil.copy2(out, dest)
        log("COPIED " + str(out) + " -> " + str(dest))
    return r.returncode


def link_on_tmp(target: str, outfile: str) -> int:
    cmd = last_link_line(target)
    cmd2 = copy_link_inputs(cmd)
    cmd2 = cmd2.replace(str(LINK_TMP / "OUTBIN"), str(LINK_TMP / outfile))
    log("LINK_TMP " + cmd2)
    r = subprocess.run(["bash", "-lc", cmd2], cwd=str(BUILD))
    if r.returncode == 0:
        dest = BUILD / outfile
        shutil.copy2(LINK_TMP / outfile, dest)
        log("COPIED " + str(LINK_TMP / outfile) + " -> " + str(dest))
    return r.returncode


def copy_bins() -> None:
    COPY_DEST.mkdir(parents=True, exist_ok=True)
    for name in ("prism", "prism_tests", "prism_gui"):
        src = BUILD / name
        if not src.is_file():
            alt = BUILD / "bin" / name
            src = alt if alt.is_file() else src
        if src.is_file():
            shutil.copy2(src, COPY_DEST / name)
            log("COPIED " + str(src) + " -> " + str(COPY_DEST / name))
        else:
            log("MISSING " + name)
    for src in sorted(BUILD.glob("libprism_cuda.so*")) + sorted(BUILD.glob("libprism_native.so*")):
        dest = COPY_DEST / src.name
        shutil.copy2(src, dest, follow_symlinks=True)
        log("COPIED " + str(src) + " -> " + str(dest))


def main() -> int:
    os.environ["CMAKE_SKIP_PACKAGE_REGISTRY"] = "ON"
    flags = set(a for a in sys.argv[1:] if a.startswith("--"))
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    compile_only = "--compile-only" in flags
    cc = load_compile_commands()
    require_live_build_tree()
    want = tuple(args) or CHANGED_DEFAULT
    if "--archive-libs" in flags:
        archive_core()
        archive_llama_libs()
        log("archive-libs done")
        return 0
    if "--link-only" in flags:
        if not wait_for(LIB_Z3, timeout=3600.0):
            return 1
        compile_want(cc, want, jobs=4)
        archive_core()
        if not LIB_LLAMA.is_file():
            archive_llama_libs()
        rc = link_direct("prism", BUILD / "CMakeFiles/prism.dir/src/prism/main.cpp.o")
        if rc != 0:
            return rc
        rc = link_direct("prism_tests", BUILD / "CMakeFiles/prism_tests.dir/tests/cpp/test_main.cpp.o")
        if rc != 0:
            return rc
        copy_bins()
        return 0
    if "--compile-llama" in flags:
        llama_want: list[str] = []
        for e in cc:
            f = e["file"].replace("\\", "/")
            if "/llama.cpp/" in f or "/ggml/" in f:
                llama_want.append(Path(f).name)
        if not llama_want:
            raise SystemExit("no llama/ggml compile commands")
        compile_want(cc, tuple(dict.fromkeys(llama_want)), jobs=4)
        log("compile-llama done")
        return 0
    compile_want(cc, want, jobs=4)
    if compile_only:
        log("compile-only done")
        return 0

    extra = list(PRISM_CORE_CPP) + ["main.cpp", "test_main.cpp"]
    missing = []
    for name in extra:
        obj = BUILD / "CMakeFiles/prism_core.dir/src/prism" / (name + ".o")
        if name == "main.cpp":
            obj = BUILD / "CMakeFiles/prism.dir/src/prism/main.cpp.o"
        elif name == "test_main.cpp":
            obj = BUILD / "CMakeFiles/prism_tests.dir/tests/cpp/test_main.cpp.o"
        if not obj.is_file():
            missing.append(name)
    if missing:
        log("COMPILE_MISSING " + " ".join(missing))
        compile_want(cc, tuple(missing), jobs=4)

    patch_ninja_skip_regen(BUILD / "build.ninja")

    if not ninja_busy():
        ninja_txt = (BUILD / "build.ninja").read_text(encoding="utf-8", errors="replace")
        targets = ["prism", "prism_tests"]
        if "build prism_gui:" in ninja_txt:
            targets.append("prism_gui")
        log("NINJA " + " ".join(targets))
        r = subprocess.run(["ninja", "-j", "8", *targets], cwd=BUILD)
        if r.returncode == 0:
            copy_bins()
            return 0
        log("ninja failed rc=" + str(r.returncode) + "; falling back to /tmp link")

    log("waiting for static libs on /var/tmp/prism-wsl")
    for lib in (LIB_PCRE, LIB_Z3, LIB_LLAMA, LIB_GGML, LIB_GGML_CPU, LIB_GGML_BASE):
        if not wait_for(lib, timeout=3600.0):
            if lib in (LIB_LLAMA, LIB_GGML, LIB_GGML_CPU, LIB_GGML_BASE):
                log("LLAMA_LIB_NOTRUN " + str(lib))
                continue
            return 1

    # Recompile changed files after ninja may have overwritten objects.
    compile_want(cc, want, jobs=4)
    archive_core()

    ninja_txt = (BUILD / "build.ninja").read_text(encoding="utf-8", errors="replace")
    targets = [("prism", "prism"), ("prism_tests", "prism_tests")]
    if "build prism_gui:" in ninja_txt:
        targets.append(("prism_gui", "prism_gui"))
    for tgt, name in targets:
        rc = link_on_tmp(tgt, name)
        if rc != 0:
            log("tmp link failed for " + tgt)
            return rc
    copy_bins()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
