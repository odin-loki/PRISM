# PRISM

**PRISM** = Performance, Regression, Integration and Security Module.

> The name is **PRISM** (an earlier name is retired) — see [CLAUDE.md](CLAUDE.md).
> PRISM ships two engines with the same stages and laws: the C++23 engine
> (`src/prism`, binary `prism`) and the Python engine (package `prism/`,
> `python -m prism PATH`).

Hybrid C/C++ testing pipeline in **C++23**: deterministic instruments first,
then bounded proofs, then fuzzing, then Qwen 3.5 9B. GUI is Qt. Inference is
llama.cpp (Ollama fallback). SIMD is vendored xsimd. GPU mutation is CUDA.
SMT is vendored Z3. Regex is vendored PCRE2. No pip/vcpkg packages.

```
# WSL (clang++, ISO C++ std::jthread — not MinGW)
cmake -B build_wsl -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DPRISM_CUDA=OFF -DPRISM_LLAMA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON
cmake --build build_wsl
./build_wsl/prism testdata --no-llm --jobs 8
./build_wsl/prism testdata --resume
./build_wsl/prism_tests

# Native MSVC (cl), from a VS/x64 Native Tools prompt
cmake -B build -G Ninja -DPRISM_CUDA=OFF -DPRISM_LLAMA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON
cmake --build build
```

A missing tool is `NOTRUN`. It is never a clean result. `PROVED` and
`BOUNDED` are never merged. A fuzzer `CLEAN` is not a proof. LLM output
is `HYPOTHESIS`. A Dafny-style proof under `requires` is `PROVED-ASSUMING`.

See [docs/PLAN.md](docs/PLAN.md) and [docs/MINED.md](docs/MINED.md).

Third-party sources live in `third_party/` (xsimd, Z3, PCRE2, nlohmann/json,
doctest, llama.cpp, plus the mined adapter projects listed in
[`third_party/SOURCES.md`](third_party/SOURCES.md)). They are trees copied
into this repo, not package downloads at build time and not git submodules.
