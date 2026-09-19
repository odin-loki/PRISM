"""xsimd-shaped mutation and coverage hashing.

The C++ engine (native/) uses xsimd + CUDA. This Python module is the
same algorithm so a machine without MSVC still fuzzes: 8-wide byte
havoc, xxhash-ish mixing. When helix_native.pyd is importable it is
used instead.
"""

from __future__ import annotations

import os
import struct
from pathlib import Path

try:
    from helix_native import havoc as _native_havoc, coverage_hash as _native_hash
    HAS_NATIVE = True
except Exception:
    HAS_NATIVE = False
    _native_havoc = _native_hash = None


def _try_load_dll():
    """Load helix_native.dll via ctypes. Failures fall back to Python."""
    import ctypes

    root = Path(__file__).resolve().parents[1]
    names = (
        "helix_native.dll",
        "libhelix_native.dll",
        "libhelix_native.so",
        "helix_native.so",
        "libhelix_native.dylib",
    )
    dirs = [
        root / "native" / "build",
        root / "native" / "build" / "Release",
        root / "native" / "build" / "Debug",
        Path(__file__).resolve().parent,
        Path.cwd(),
    ]
    env = os.environ.get("HELIX_NATIVE_DLL")
    candidates = [Path(env)] if env else []
    for d in dirs:
        candidates.extend(d / n for n in names)

    for path in candidates:
        try:
            if not path.is_file():
                continue
            lib = ctypes.CDLL(str(path))
            lib.helix_coverage_hash.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            lib.helix_coverage_hash.restype = ctypes.c_ulonglong
            lib.helix_havoc.argtypes = [
                ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulonglong,
            ]
            lib.helix_havoc.restype = None

            def _hash(data: bytes, _lib=lib) -> int:
                n = len(data)
                buf = ctypes.create_string_buffer(data, n) if n else ctypes.create_string_buffer(1)
                return int(_lib.helix_coverage_hash(buf if n else None, n))

            def _havoc(data: bytes, _lib=lib) -> bytes:
                raw = data or b"\x00"
                buf = ctypes.create_string_buffer(raw, len(raw))
                seed = int.from_bytes(os.urandom(8), "little")
                _lib.helix_havoc(buf, len(raw), seed)
                return buf.raw[: len(raw)]

            return _hash, _havoc
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
    # xxhash64-ish mix, 8-byte lanes (the xsimd width we use in native/)
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


def havoc(data: bytes) -> bytes:
    if HAS_NATIVE:
        return bytes(_native_havoc(data))
    b = bytearray(data or b"\x00")
    n = len(b)
    ops = os.urandom(1)[0] % 8 + 1
    for _ in range(ops):
        kind = os.urandom(1)[0] % 6
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
            # interesting ints
            interesting = [0, 1, 0x7F, 0x80, 0xFF, 0x7FFFFFFF]
            v = interesting[os.urandom(1)[0] % len(interesting)]
            b[i] = v & 0xFF
        else:
            j = os.urandom(1)[0] % n
            b[i], b[j] = b[j], b[i]
    return bytes(b)
