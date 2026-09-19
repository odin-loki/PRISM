# Native Helix build (CPU / MinGW)

This machine has **no Visual Studio**. The CPU library is built with
Strawberry MinGW `g++` and vendored **xsimd 13.2.0** headers. CUDA,
llama.cpp, and Qt C++ are forced **OFF** (nvcc needs an MSVC host;
the Qt C++ SDK is not installed).

## Command that produced `helix_native.dll`

From the repository root:

```
cmake -B native/build -DHELIX_CUDA=OFF -DHELIX_LLAMA=OFF -DHELIX_QT=OFF -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=g++
cmake --build native/build
```

Outputs:

- `native/build/helix_native.dll` — SIMD coverage hash + havoc (xsimd AVX2)
- `native/build/helix_cli.exe` — smoke test (`helix_cli.exe helix`)

Compiler used: `C:\Strawberry\c\bin\g++.exe` (MinGW-W64 13.2.0).

xsimd is **not** fetched at configure time when
`third_party/xsimd/include/xsimd/xsimd.hpp` exists (shallow clone of
https://github.com/xtensor-stack/xsimd.git tag `13.2.0`). CMake falls
back to `FetchContent` only if that tree is missing.

## What this build does not compile

| target | why |
|---|---|
| `helix_cuda` (`native/cuda/mutate.cu`) | `nvcc` on Windows needs MSVC as the host compiler |
| llama.cpp | same MSVC+CUDA requirement; Python talks to Ollama instead |
| `helix_gui` (Qt C++) | Qt6 C++ SDK not installed; use `python -m helix --gui` (PySide6) |

Re-enable those on a machine with VS Build Tools + CUDA + Qt:

```
cmake -B build -DHELIX_CUDA=ON -DHELIX_LLAMA=ON -DHELIX_QT=ON
cmake --build build --config Release
```
