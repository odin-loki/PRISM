#!/usr/bin/env python3
import sys
from pathlib import Path

sys.path.insert(0, str(Path("/mnt/c/Users/odinl/OneDrive/Desktop/Code Analysis")))
from prism import simdmut

print("HAS_NATIVE", simdmut.HAS_NATIVE)
print("hash", simdmut.coverage_hash(b"abc"))
print("havoc", simdmut.havoc(b"abc\x00def")[:8])
