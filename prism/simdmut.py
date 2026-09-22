"""xsimd-shaped mutation and coverage hashing.

The C++ engine (src/prism, built by the top-level CMakeLists.txt) uses xsimd
+ optional CUDA. This Python module is the same algorithm so a machine
without a loadable native library still fuzzes: 8-wide byte havoc,
xxhash-ish mixing.

Load order (never a proof):
1. ctypes-load prism_native from PRISM_NATIVE_DLL, then build_wsl/ and
   build/. C ABI is prism_havoc / prism_coverage_hash (src/prism/capi.cpp).
2. Missing library is a Python fallback, never CLEAN/PROVED.
CUDA (build_wsl/libprism_cuda.so) is not required at import; GPU
failure is a CPU fallback.
"""

from __future__ import annotations

import os
import struct
from pathlib import Path

HAS_NATIVE = False
_native_havoc = _native_hash = None

_CPU_LIB_NAMES = (
    "libprism_native.so",
    "prism_native.so",
    "prism_native.dll",
    "libprism_native.dll",
    "libprism_native.dylib",
)

# Optional. Preload only; GPU failure must not block CPU or Python.
_CUDA_LIB_NAMES = (
    "libprism_cuda.so",
    "prism_cuda.so",
    "prism_cuda.dll",
    "libprism_cuda.dll",
)

# C ABI prefix of src/prism/capi.cpp (prism_havoc, prism_coverage_hash).
_CAPI_PREFIXES = ("prism",)


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _native_search_dirs() -> list[Path]:
    root = _repo_root()
    return [
        root / "build_wsl",
        root / "build",
        root / "build" / "Release",
        root / "build" / "Debug",
        Path(__file__).resolve().parent,
        Path.cwd(),
    ]


def _native_lib_candidates() -> list[Path]:
    """Ordered ctypes paths: PRISM_NATIVE_DLL, then search dirs × CPU names."""
    env = os.environ.get("PRISM_NATIVE_DLL")
    candidates = [Path(env)] if env else []
    for d in _native_search_dirs():
        candidates.extend(d / n for n in _CPU_LIB_NAMES)
    return candidates


def _cuda_lib_candidates() -> list[Path]:
    """Optional GPU libs (build_wsl/libprism_cuda.so, build/). Not required."""
    return [d / n for d in _native_search_dirs() for n in _CUDA_LIB_NAMES]


def _cdll(path: Path):
    import ctypes

    kwargs = {}
    rtld = getattr(ctypes, "RTLD_GLOBAL", None)
    if rtld is not None and os.name != "nt":
        kwargs["mode"] = rtld
    return ctypes.CDLL(str(path), **kwargs)


def _bind_native_symbols(lib):
    """Bind the prism_* C ABI. Missing symbols → (None, None)."""
    import ctypes

    for prefix in _CAPI_PREFIXES:
        try:
            hash_sym = getattr(lib, f"{prefix}_coverage_hash")
            havoc_sym = getattr(lib, f"{prefix}_havoc")
        except (AttributeError, OSError):
            continue
        hash_sym.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        hash_sym.restype = ctypes.c_ulonglong
        havoc_sym.argtypes = [
            ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulonglong,
        ]
        havoc_sym.restype = None

        def _hash(data: bytes, _fn=hash_sym) -> int:
            n = len(data)
            buf = ctypes.create_string_buffer(data, n) if n else ctypes.create_string_buffer(1)
            return int(_fn(buf if n else None, n))

        def _havoc(data: bytes, _fn=havoc_sym) -> bytes:
            raw = data or b"\x00"
            buf = ctypes.create_string_buffer(raw, len(raw))
            seed = int.from_bytes(os.urandom(8), "little")
            _fn(buf, len(raw), seed)
            return buf.raw[: len(raw)]

        return _hash, _havoc
    return None, None


def _try_preload_cuda() -> bool:
    """Load libprism_cuda.so if present. Failure is CPU fallback, not CLEAN."""
    for path in _cuda_lib_candidates():
        try:
            if not path.is_file():
                continue
            _cdll(path)
            return True
        except Exception:
            continue
    return False


def _try_load_dll():
    """ctypes-load prism_native. Failures fall back to Python."""
    try:
        _try_preload_cuda()
    except Exception:
        pass
    for path in _native_lib_candidates():
        try:
            if not path.is_file():
                continue
            lib = _cdll(path)
            bound = _bind_native_symbols(lib)
            if bound[0] is not None and bound[1] is not None:
                return bound
        except Exception:
            continue
    return None, None


if not HAS_NATIVE:
    try:
        _h, _v = _try_load_dll()
        if _h is not None and _v is not None:
            _native_hash, _native_havoc = _h, _v
            HAS_NATIVE = True
    except Exception:
        HAS_NATIVE = False
        _native_havoc = _native_hash = None


def _py_hash(data: bytes) -> int:
    # xxhash64-ish mix, 8-byte lanes (the xsimd width the C++ engine uses)
    h = 0x9E3779B97F4A7C15
    n = len(data)
    i = 0
    while i + 8 <= n:
        lane = struct.unpack_from("<Q", data, i)[0]
        h ^= lane * 0xBF58476D1CE4E5B9
        h = ((h << 13) | (h >> 51)) & 0xFFFFFFFFFFFFFFFF
        h = (h * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        i += 8
    tail = data[i:] + b"\x00" * 8
    lane = struct.unpack_from("<Q", tail)[0]
    h ^= lane
    h ^= n
    h ^= h >> 33
    h = (h * 0xFF51AFD7ED558CCD) & 0xFFFFFFFFFFFFFFFF
    h ^= h >> 33
    return h


def coverage_hash(data: bytes) -> int:
    if HAS_NATIVE:
        return int(_native_hash(data))
    return _py_hash(data)


# AFL++ include/config.h INTERESTING_8 / INTERESTING_16 / INTERESTING_32.
# Signed values stored little-endian; byte overlay uses the low 8 bits of
# INTERESTING_8. CLEAN from havoc is never a proof.
INTERESTING_8 = (-128, -1, 0, 1, 16, 32, 64, 100, 127)
INTERESTING_16 = (-32768, -129, 128, 255, 256, 512, 1000, 1024, 4096, 32767)
INTERESTING_32 = (
    -2147483648,
    -100663046,
    -32769,
    32768,
    65535,
    65536,
    100663045,
    2139095040,
    2147483647,
)


def _overlay_le(buf: bytearray, i: int, value: int, width: int) -> None:
    n = len(buf)
    raw = value & ((1 << (8 * width)) - 1)
    for k in range(width):
        if i + k >= n:
            break
        buf[i + k] = (raw >> (8 * k)) & 0xFF


def havoc(data: bytes) -> bytes:
    if HAS_NATIVE:
        return bytes(_native_havoc(data))
    b = bytearray(data or b"\x00")
    n = len(b)
    ops = os.urandom(1)[0] % 8 + 1
    for _ in range(ops):
        kind = os.urandom(1)[0] % 8
        i = os.urandom(1)[0] % n
        if kind == 0:
            b[i] ^= os.urandom(1)[0]
        elif kind == 1:
            b[i] = os.urandom(1)[0]
        elif kind == 2:
            b[i] = 0xFF
        elif kind == 3:
            b[i] = 0x00
        elif kind == 4:
            v = INTERESTING_8[os.urandom(1)[0] % len(INTERESTING_8)]
            _overlay_le(b, i, v, 1)
        elif kind == 5:
            v = INTERESTING_16[os.urandom(1)[0] % len(INTERESTING_16)]
            _overlay_le(b, i, v, 2)
        elif kind == 6:
            v = INTERESTING_32[os.urandom(1)[0] % len(INTERESTING_32)]
            _overlay_le(b, i, v, 4)
        else:
            j = os.urandom(1)[0] % n
            b[i], b[j] = b[j], b[i]
    return bytes(b)
