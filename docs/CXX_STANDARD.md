# C++ programming standard (PRISM engine)

**Owner intent (2026-10-24):** PRISM’s own C++ (`src/prism/`, `src/gui/`,
`src/tools/`, `include/prism/`, `tests/cpp/`) should be **memory-safe ISO C++23**
— the strictest practical rules for **safety** and **performance**. This document
records that goal and what is **not** enforced yet.

`third_party/` is pinned vendored code; do not edit it. Rules below apply to
PRISM sources only.

## Baseline today

| Item | Status |
|------|--------|
| Language | **C++23** — `CMAKE_CXX_STANDARD 23` / `CMAKE_CXX_STANDARD_REQUIRED ON` in root `CMakeLists.txt`; MSVC uses `/std:c++latest` where applicable. |
| Product direction | C++23-only engine — [CPP_PORT_PLAN.md](CPP_PORT_PLAN.md). |
| Optional ASan+UBSan | `PRISM_SANITIZE=ON` adds `-fsanitize=address,undefined` (Clang/GCC). |
| Fuzzing | `PRISM_FUZZ` + libFuzzer on supported Clang builds. |
| External lint | cppcheck / clang-tidy via polyglot when installed (`NOTRUN` if missing — Law 1). |
| **Written coding standard** | **This file only** — not yet wired into CI as hard gates. |

Assurance: [DO-333.md](assurance/DO-333.md) points here; enforcement items below
are still in progress.

## Target standard (to enforce)

### Language and library

- **ISO C++23** only for new code; no C++17/20 downgrade in `src/prism/`.
- Prefer **standard library** facilities (`std::span`, `std::string_view`, containers,
  `std::optional`, `std::expected` where appropriate) over ad-hoc buffers.
- **No owning raw pointers** in interfaces; no `new`/`delete` except inside vetted
  low-level adapters (document exceptions in code review).
- **No unchecked undefined behaviour** in PRISM code paths; use sanitizers and tests
  to catch slips; `PRISM_HAS_INT128` / portable wide-int paths must stay explicit.

### Memory safety

- Treat **memory safety** as a design requirement: bounds-checked access, no
  unbounded `memcpy` without proof, no `reinterpret_cast` except at documented FFI
  boundaries (Z3, process, CUDA).
- **Law 9 / sandbox:** executing scanned code stays behind `--allow-exec`; engine
  code must not introduce silent execution paths.

### Safety (compile-time and runtime)

- Aim for **warnings as errors** on Clang CI (`-Werror` with a curated allowlist).
- Add a **root `.clang-tidy`** profile (e.g. `bugprone-*`, `cppcoreguidelines-*`,
  `modernize-*`, `performance-*`) with incremental cleanup — not `-warnings-as-errors`
  on day one.
- **MSVC:** subset builds use `/W4` or stricter where green; document divergences
  in [MSVC_PORT.md](MSVC_PORT.md).

### Performance

- **Release** builds stay optimized (`-O2` / MSVC `/O2`); avoid pessimizing
  “safety” patterns (e.g. redundant copies in hot paths) — profile before micro-opts.
- Hot paths (`pir/`, `solver/`, `proc/`) favor **contiguous data**, minimal
  allocations per unit of work, and **no accidental sync** in parallel stages.

### Process

- New stages and tools follow [CLAUDE.md](../CLAUDE.md) laws and status vocabulary.
- Python engine is reference/oracle only per port plan; **new behaviour lands in C++ first**.

## Tooling

| Tool | Use |
|------|-----|
| `.clang-tidy` | Engine code only (`HeaderFilterRegex`); run with `compile_commands.json`. |
| `PRISM_WERROR=ON` | CMake: `-Werror` on Clang/GCC for `prism_core` (and tests when wired). |
| MSVC | `/W4 /permissive-` on `prism_core` via `prism_engine_warning_flags()`. |

## Next engineering work (tracked)

- Wire `prism_engine_warning_flags` on `prism_tests`, `prism`, `prism-qa`.
- Optional CI job: `PRISM_WERROR=ON` on Linux (fix fallout incrementally).
- Expand clang-tidy in polyglot / a dedicated CI step with `compile_commands.json`.
