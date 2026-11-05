# C++ coding standard

PRISM's own C++ (`src/prism/`, `src/gui/`, `src/tools/`, `include/prism/`,
`tests/cpp/`) is ISO C++23 and aims to be memory-safe. This page lists the
rules and says, for each one, what enforces it today. "Review" means only
code review enforces it.

`third_party/` is pinned upstream code. These rules do not apply to it;
never edit it.

## Build settings

| Setting | Where | Value |
|---|---|---|
| Language | `CMakeLists.txt` | `CMAKE_CXX_STANDARD 23`, `CMAKE_CXX_STANDARD_REQUIRED ON`, `CMAKE_CXX_EXTENSIONS OFF` (ISO, not `gnu++23`) |
| Compiler | `CMakeLists.txt` | clang (pinned 23.1.4), clang-cl on Windows; MSVC `cl` 19.40+ on Windows x64 ([MSVC_PORT.md](MSVC_PORT.md)). Anything else, and MinGW, fails configure. |
| Warnings | `prism_engine_warning_flags()` | clang/GCC: `-Wall -Wextra -Wpedantic`; `cl` and clang-cl: `/W4 /permissive-` |
| Warnings as errors | `-DPRISM_WERROR=ON` | adds `-Werror` (clang/GCC only). Default `OFF`. No CI job sets it. |
| Optimisation | `prism_core` | `-O3 -mavx2` (clang); `/O2 /arch:AVX2 /utf-8` (MSVC-style drivers). Default build type `Release`. |
| ASan + UBSan | `-DPRISM_SANITIZE=ON` | `-fsanitize=address,undefined -fno-sanitize-recover=undefined`. clang/GCC only. |
| compile database | `CMAKE_EXPORT_COMPILE_COMMANDS ON` | `build/compile_commands.json`, for clang-tidy |

The warning flags apply to `prism_core`, `prism`, `prism_qa`, `prism-qa`
and `prism_tests`. `src/prism/taxonomy.cpp` and `src/prism/checkers_cxx.cpp`
are compiled at `-O0`.

## Rules

### Language and library

| Rule | Enforced by |
|---|---|
| ISO C++23 only; no compiler extensions in engine code. | Compiler (`CMAKE_CXX_EXTENSIONS OFF`, `-Wpedantic`) |
| Prefer the standard library: `std::span`, `std::string_view`, `std::optional`, `std::expected`, containers, `std::jthread`. | Review; clang-tidy `modernize-*` (run by hand) |
| No `__int128` in engine code. Use `prism::i128` / `prism::u128` (`include/prism/wide_int.hpp`), which builds on MSVC `cl`. | Review |
| Processes start through `prism::exec` (`include/prism/exec.hpp`, FORGE spawn), not `system`, `popen` or `fork`. | Review |

### Memory safety

| Rule | Enforced by |
|---|---|
| No owning raw pointers in interfaces; no bare `new`/`delete` outside low-level adapters. | Review. clang-tidy `cppcoreguidelines-owning-memory` is disabled in `.clang-tidy`. |
| Bounds-checked access; no `memcpy` without a size the code has checked. | Review; ASan in the nightly job |
| `reinterpret_cast` only at FFI boundaries (Z3, process, CUDA, libclang, fuzz entry). | Review. clang-tidy `cppcoreguidelines-pro-type-reinterpret-cast` is disabled. |
| No undefined behaviour. | UBSan (`PRISM_SANITIZE`) in the nightly job |

`.github/workflows/self-check.yml` runs nightly: it builds PRISM with
`PRISM_SANITIZE=ON` and runs `prism_tests` and `prism-qa conformance`
under ASan + UBSan. It is not a gate on pull requests.

### Laws

Engine code follows the laws in [CLAUDE.md](../CLAUDE.md). For C++ this
means in particular:

- A step that cannot run returns a `NOTRUN` finding with the reason; it
  never returns an empty result (Laws 1 and 7).
- Code from the scanned tree runs only behind `--allow-exec` and the
  sandbox gate (Law 9, `include/prism/sandbox.hpp`).
- No flag that disables a check is passed to a tool (Law 8).
  `prism::exec::refuse_disabled_checks` throws on any `--no-…-check`
  argument.

### Performance

| Rule | Enforced by |
|---|---|
| Release builds stay optimised; measure before micro-optimising. | Build settings above |
| Hot paths (`src/prism/pir/`, `src/prism/solver/`) keep data contiguous and allocate little per unit of work. | Review |
| Parallel stages share no mutable state without a lock. | Review; TSan is not run in CI |

## clang-tidy

The root `.clang-tidy` enables `bugprone-*`, `cppcoreguidelines-*` and
`modernize-*`, with these groups disabled:

- `bugprone-`: easily-swappable-parameters, implicit-widening-of-multiplication-result,
  misplaced-widening-cast, narrowing-conversions, reserved-identifier,
  too-small-loop-variable.
- `cppcoreguidelines-`: avoid-c-arrays, avoid-do-while, avoid-magic-numbers,
  macro-usage, non-private-member-variables-in-classes, owning-memory,
  pro-bounds-array-to-pointer-decay, pro-bounds-constant-array-index,
  pro-bounds-pointer-arithmetic, pro-type-const-cast,
  pro-type-reinterpret-cast, pro-type-union-access, pro-type-vararg.
- `modernize-`: avoid-c-arrays, concat-nested-namespaces, loop-convert,
  pass-by-value, use-nodiscard, use-trailing-return-type, use-using.

`HeaderFilterRegex` is `(src/|include/prism/).*`. There is no
`WarningsAsErrors`. Run it by hand with the compile database:

```
clang-tidy -p build src/prism/<file>.cpp
```

No CI job runs clang-tidy on PRISM's own sources.

## Not enforced yet

- `-Werror` in CI (`PRISM_WERROR=ON` exists; no job sets it).
- clang-tidy in CI.
- The memory-safety rules that clang-tidy has disabled (owning memory,
  pointer arithmetic, `reinterpret_cast`) are review-only.
