#!/usr/bin/env python3
from pathlib import Path
import shutil

src = Path("/var/tmp/prism-wsl")
dst = Path("/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis/build_wsl")
dst.mkdir(parents=True, exist_ok=True)
for p in list(src.glob("libprism_cuda.so*")) + list(src.glob("libprism_native.so*")):
    shutil.copy2(p, dst / p.name, follow_symlinks=True)
    print("COPIED", p, "->", dst / p.name)
