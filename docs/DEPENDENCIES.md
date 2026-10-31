# Dependencies (branch prism-forge)

Every dependency is pinned to an exact version with a checksum. FORGE is the one deliberate exception.

| Dependency | Kind | Pin | Where | Vendored? |
|---|---|---|---|---|
| FORGE (odin-loki/FORGE) | linked, **mandatory** | **unpinned**, tracks `main` | `FORGE_SOURCE_DIR` or FetchContent `main` (cmake/forge_mind.cmake) | no (by design) |
| Z3 4.13.4 | linked | commit + archive/tree SHA-256 | third_party/MANIFEST.toml | yes, third_party/z3 |
| PCRE2 | linked | commit + SHA-256 | MANIFEST.toml | yes |
| xsimd | linked | commit + SHA-256 | MANIFEST.toml | yes |
| nlohmann/json | linked | commit + SHA-256 | MANIFEST.toml | yes |
| doctest 2.4.11 | linked (tests) | commit + SHA-256 | MANIFEST.toml | yes |
| ~~llama.cpp b6875~~ | **retired** | removed | replaced by FORGE mind | deleted |
| LLVM/clang 23.1.4 | toolchain | per-platform SHA-256 | third_party/llvm/PIN.toml (branch prism-llvm) | pinned fetch (too large) |
| CMake 3.31.6 | toolchain | SHA-256 per platform | third_party/TOOLCHAIN_PINS.toml | pinned fetch |
| Qt 6.8.3 (qtbase, Widgets) | GUI, optional | source tarball SHA-256 | TOOLCHAIN_PINS.toml | pinned fetch |
| CUDA 12.6.3 (nvcc 12.6.85, cudart/cccl 12.6.77) | kernels, optional | SHA-256 per platform | TOOLCHAIN_PINS.toml | pinned fetch |
| Lean 4 v4.34.0 + Mathlib | proofs | lean-toolchain + lake-manifest.json git revs | proofs/ | fetched by lake |
| External analysers (esbmc, cbmc, klee, cppcheck, frama-c, infer, semgrep, coccinelle, AFL++, dafny, strix, cadical, kissat, bitwuzla, cake_lpr, lean4export, nanoda) | separate processes | commit + SHA-256 | MANIFEST.toml `kind = "external"` | fetched by `prism-deps tool NAME` |
| Docker base | build image | ubuntu:24.04 by digest | Dockerfile | n/a |
| Lint tools (clang-tidy, ruff, mypy, eslint, shellcheck, ...) | `kind = "system"` | recorded by path + file SHA-256 at run time | MANIFEST.toml | no |
| Python | scripts only | none required beyond stdlib (3.11+, tomllib) | — | — |

Fetch a pinned toolchain: `python3 scripts/fetch_toolchain.py cmake` (or `qtbase`, `cuda_nvcc`, ...).
A download whose SHA-256 differs from the pin is refused (exit 2).

## Models
PRISM's AI runs on FORGE mind in process. Models are FORGE `.fpk` files, never committed:
- `PRISM_FPK` — assistant model, default `~/.prism/models/qwen3.5-9b.fpk` (Qwen 3.5 9B converted with FORGE).
- `PRISM_PROVER_FPK` — Lean prover (e.g. Goedel-Prover 8B `.fpk`); else the first `*prover*`/`*goedel*` `.fpk` in `~/.prism/models`.
- `PRISM_EMBED_FPK` — optional one-shot embedding graph for triage; else TF-IDF.
- `PRISM_DEVICE` / `FORGE_DEVICE` — `cpu`, `cuda`, `vulkan`, ...
llama-server, Ollama and GGUF are no longer supported.
