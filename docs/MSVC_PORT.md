# PRISM on Windows

PRISM builds and runs natively on Windows x64. This page covers the
Windows host build, what runs there and what is `NOTRUN`, how scanned code
is run (`job-object`), and the Windows x64 MSVC analysis target
(`--target x86_64-pc-windows-msvc`).

It describes `main` plus two branches landing on 2026-11-05:
`port/winmsvc` (MSVC `cl` builds, `--target`) and `port/harden` (the
sandbox gate). Linux is the primary platform; SV-COMP runs on Linux only.

## 1. Compilers

| Compiler | Where | Notes |
|---|---|---|
| clang-cl from the pinned LLVM 23.1.4 | Windows x64 | The CI compiler. `third_party/llvm/PIN.toml`, fetched by `scripts/fetch_llvm.py`. |
| MSVC `cl` 19.40 or newer (VS 2022 17.10+) | Windows x64 only | `port/winmsvc`. Older `cl` fails configure (C++23 needs 19.40). |
| MinGW | — | Rejected by configure. |

`CMakeLists.txt` refuses any other compiler. A clang other than 23.1.4 is
a configure warning, not an error.

Under `cl`, `prism_tests` force-includes `tests/cpp/msvc_builtins.hpp`
(`__builtin_trap` and the bit builtins of the C23 `stdbit` model) and
builds with `/utf-8 /bigobj`. The MSVC 19.51 build (VS 18 Build Tools,
Ninja, `PRISM_Z3=ON`) compiled and linked `prism_core`, `prism`,
`prism-qa` and `prism_tests` (2026-11-05, commit af2c47e91). No CI job
builds with `cl`.

Both MSVC-style drivers (`cl` and clang-cl) get `/W4 /permissive-` on the
engine targets and `/O2 /arch:AVX2 /utf-8` on `prism_core`. `WIN32` adds
`NOMINMAX` everywhere and links `winhttp`. See
[CXX_STANDARD.md](CXX_STANDARD.md) for the warning policy.

`PRISM_SANITIZE=ON` stops configure on any MSVC-style driver.
`PRISM_FUZZ` needs a clang that accepts `-fsanitize=fuzzer`, and the
harness `tests/fuzz/fuzz_cparse.cpp` includes `<unistd.h>`, so self-fuzzing
is a Linux job ([FUZZ_SELF.md](FUZZ_SELF.md)).

## 2. Building on Windows

Build under `C:\Temp\prism-build-*`, never in the OneDrive clone. OneDrive
sync makes builds slow and makes the linker fail with `LNK1104`. Edit in
OneDrive if you like; build elsewhere.

Inside a Visual Studio x64 developer prompt (`vcvars64.bat`):

```
cmake -S <repo> -B C:\Temp\prism-build-win -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=%PRISM_LLVM_BIN%\clang-cl.exe -DCMAKE_CXX_COMPILER=%PRISM_LLVM_BIN%\clang-cl.exe ^
  -DPRISM_CUDA=OFF -DPRISM_QT=OFF -DPRISM_Z3=ON ^
  -DPRISM_TESTS=ON -DPRISM_TESTS_WIN_SUBSET=ON -DPRISM_LIBCLANG=OFF
cmake --build C:\Temp\prism-build-win
C:\Temp\prism-build-win\prism_tests.exe
```

- Use `-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl` for a `cl` build.
- Keep the default `CMAKE_BUILD_TYPE=Release` with `cl`.
- `-DPRISM_TESTS_WIN_SUBSET=ON` builds the `fake_*` stand-in executables
  (`fake_tool_launcher`, `fake_cc`, `fake_session_probe`, `fake_prism`,
  `fake_cake_lpr`, `fake_mem_hog`, …) that the subprocess tests need on
  Windows. The test sources are the same list as on Linux; without the
  flag those cases cannot find their stand-ins. The name is historical.
- `FORGE_SOURCE_DIR` points at a FORGE checkout; when it is empty
  `cmake/forge.cmake` fetches FORGE (at the SHA in `cmake/FORGE_PIN` once
  `port/cipin` lands).
- The vendored Z3 build is slow. Run one cmake/ninja process per build
  tree.
- Use the CMake from `C:\Program Files\CMake` (or the pinned one from
  `scripts/fetch_toolchain.py cmake`). Strawberry Perl's CMake breaks the
  Z3 configure.

Helper scripts (`scripts/`):

| Script | Does |
|---|---|
| `win_msbuild_smoke.ps1 [-BuildDir D] [-Z3]` | Configures with `cl` (default `C:\Temp\prism-build-win-cl`) and builds `prism_tests`; with `-Z3` it calls `win_z3_one.ps1`. Holds a lock file in the build dir. |
| `win_z3_one.ps1 [-BuildDir D] [-Jobs N] [-RunPirE2e]` | One worker on `C:\Temp\prism-build-win-z3`: `libz3`, then `prism_tests`, then optionally the pir e2e cases. Lock: `.win_z3_build.lock`. |
| `win_z3_lock.ps1` | The shared lock functions. |
| `win_z3_build_libz3.ps1`, `win_z3_followup.ps1` | Thin wrappers; prefer `win_z3_one.ps1`. |
| `win_z3_pipeline.ps1` | Runs `prism_tests -tc="pir e2e*"`. |

The scripts expect VS 18 Build Tools at the default path.

## 3. Windows CI jobs

`.github/workflows/ci.yml` has three Windows jobs. All use clang-cl from the
pinned LLVM, fetch the pinned Linux sysroot (`scripts/fetch_toolchain.py
linux-sysroot`) and set `PRISM_LIBCLANG=OFF`.

| Job | Configure | Runs |
|---|---|---|
| `windows-compile-smoke` | `PRISM_Z3=OFF` | builds `prism_core`, `prism`, `prism-qa`, `prism_tests`; runs `prism_tests.exe` |
| `windows-compile-smoke-z3` | `PRISM_Z3=ON` | the same with Z3 (solver, `leanbb`, certified, pir cases) |
| `windows-gui-qt` | `PRISM_QT=ON`, Qt 6.8.3 `win64_msvc2022_64` | `prism_gui_tests.exe` and the `gui model` suite offscreen, plus a GUI smoke run with `--smoke-screenshot` |

The Z3 job caches its build directory. On an exact cache hit it backdates
`third_party/z3` and `cmake/z3-generated` so Ninja does not rebuild
`libz3` from fresh checkout timestamps; a `restore-keys` fallback is not
backdated. Both smoke jobs put the clang ASan runtime DLL directory on
`PATH` so the conformance label self-check can link UBSan + ASan.

## 4. Running `prism` on Windows

`prism.exe PATH` runs the same stage list as on Linux
(`include/prism/pipeline.hpp` `STAGE_ORDER`). The exit codes are the same
on every host (`include/prism/exit.hpp`): 0 ok, 1 a finding at
`--fail-on`, 2 usage, 3 internal or environment error.
`tests/cpp/test_cli.cpp` runs `prism.exe` on Windows and checks them.

| Part | On Windows |
|---|---|
| Source stages (inventory, classify, lints, taint, thread, interval, contracts parsing, ltl, …) | Run. Portable C++. |
| Clang-AST lints | Run through `clang -ast-dump=json` when PRISM is built without libclang (`PRISM_LIBCLANG=OFF`, as in CI). No clang: the layer is `NOTRUN` and the regex lints still run. |
| `pir` | Runs when clang and opt are found (`PRISM_LLVM_BIN`) and the target's headers are there (section 6 and [§7](#pir-on-windows)). |
| External tools (cppcheck, esbmc, dafny, clang-tidy, frama-c, …) | Run when found by `--tool`, the `prism-deps tool` home or `PATH`. On Windows the lookup follows the `SearchPath` directory order, and a bare name takes `.exe` before `.bat` before `.cmd` (`src/prism/config.cpp`). Missing is `NOTRUN` (Law 1). |
| Steps that execute scanned code | Need `--allow-exec`, and on Windows also `--allow-exec=unsandboxed` (section 5). |
| AI (FORGE, `.fpk`) | Runs in process. Default model path `%USERPROFILE%\.prism\models\`. |

Windows details the engine handles:

- `Config::which` passes over the Microsoft Store `python.exe` stub under
  `\WindowsApps\` (it exits 9009); with no other python the tool is
  missing (`NOTRUN`).
- A crashing child does not hang on a Windows error dialog
  (`SetErrorMode` in `src/prism/stages/platform.cpp`).
- Command lines are quoted for `CommandLineToArgvW` / the MSVC CRT
  (`sandbox::quote_windows_arg`, `windows_command_line`). A `.bat`/`.cmd`
  target runs through `cmd.exe /d /s /c`; an argument holding `%`, `!`,
  `"`, CR or LF is refused rather than guessed (`batch_command_line`).
- 128-bit arithmetic goes through `prism::i128` / `prism::u128`
  (`include/prism/wide_int.hpp`): native `__int128` when the compiler has
  it and is not MSVC, a `{hi, lo}` pair otherwise.
- The lowered pir model cache lives under
  `%LOCALAPPDATA%\prism\pir-models`. Each entry is keyed and checked per
  target; an entry for another target is kept, a broken one is removed.

## 5. Running scanned code: the `job-object` sandbox

Law 9: code from the scanned tree runs only with `--allow-exec`
(`Config.allow_exec`, default off). The exec stages are listed in
`exec_stages()` (`src/prism/pipeline.cpp`); the full table is
[ARCHITECTURE.md](ARCHITECTURE.md#running-on-untrusted-code-law-9).

`sandbox::kind()` (`include/prism/sandbox.hpp`) names the sandbox:

| Kind | Host | Isolates file system and network |
|---|---|---|
| `bwrap` | Linux with a working bubblewrap | yes |
| `job-object` | Windows | no |
| `rlimits-only` | other POSIX | no |

Only `bwrap` isolates (`sandbox::isolates`). So on Windows:

- `--allow-exec` alone is held back. Every step that would execute scanned
  code is `NOTRUN` with the hint "install bubblewrap, or re-run with
  `--allow-exec=unsandboxed`", and the report notes say so.
- `--allow-exec=unsandboxed` (`Config.allow_unsandboxed`) runs those steps
  with resource limits only. The report notes that scanned code ran
  without file-system or network isolation.

With `job-object`, FORGE's spawn (`forge::os::spawn_wait`, wrapped by
`include/prism/exec.hpp`) puts every child in a kill-on-close Job Object
with the memory and CPU caps of `sandbox::limits_for`: a 2 GiB address
space (`LIMIT_AS_BYTES`; off for ASan builds) and CPU time of the timeout
plus one second. A timeout or a stop kills the whole job, so children the
program started die with it. `Spec::watch_memory_bytes` kills the tree once
it passes a resident-memory cap; the certified checkers use it for
`PRISM_CHECKER_MEM`.

The child still sees the user's files and the network. Use
`--allow-exec=unsandboxed` only on code you trust.

## 6. Analysis target: Windows x64 MSVC

`--target TRIPLE` (else `PRISM_TARGET`, else the host) names the platform
the scanned code is built for (`include/prism/abi_model.hpp`,
`parse_target`):

| Triple | Data model | C / C++ headers |
|---|---|---|
| `x86_64-unknown-linux-gnu` | LP64 | glibc, libstdc++ (PRISM's C++ models) |
| `i386-unknown-linux-gnu` | ILP32 | glibc |
| `x86_64-pc-windows-msvc` | LLP64: 32-bit `long`, 16-bit unsigned `wchar_t`, 64-bit pointers and `size_t` | UCRT, MSVC STL |

- The default on a Windows x64 host is `x86_64-pc-windows-msvc`; on every
  other host it is `x86_64-unknown-linux-gnu`.
- Common spellings are accepted (`x86_64-windows-msvc`,
  `x86_64-pc-windows-msvc19.44…`, `amd64-pc-windows-msvc`).
- An unsupported triple is exit 2, never a scan for another ABI.
- An SV-COMP data model of ILP32 wins over the triple. `prism svcomp`
  always uses a Linux target.

What the target changes:

- **Source-level widths.** `interval`, the concrete interpreter, the fuse
  and diff argument packing and the AST lints read `active_target()`:
  `long` is 4 bytes and `wchar_t` 2 bytes on LLP64.
- **pir lowering.** clang gets `--target=x86_64-pc-windows-msvc`,
  `-fno-wrapv`, `-D_NO_CRT_STDIO_INLINE`, `-D_CRT_SECURE_NO_WARNINGS` and
  `-D_CRT_NONSTDC_NO_WARNINGS`. `-fno-wrapv` is there because clang's MSVC
  driver adds `-fwrapv`, which would drop every signed-overflow check
  (Law 8). `_NO_CRT_STDIO_INLINE` keeps `printf` and friends modelled
  library calls.
- **Headers.** On Windows clang finds Visual Studio and the Windows SDK by
  itself. Off Windows, `PRISM_MSVC_SYSROOT` names a root holding both
  (clang `-Xmicrosoft-windows-sys-root`). A probe compiles a file that
  includes `<stdio.h>`, `<stdlib.h>` and `<string.h>`; if it fails, `pir`
  is `NOTRUN` with clang's first error line.
- **C library models.** Lowered and cached per target. `time.cpp` models
  the UCRT's 64-bit `time_t` and the `_time64` family.
- **C++.** A C++ unit for the MSVC target is lowered against the MSVC STL
  headers, never PRISM's libstdc++ models. The STL code is encoded like
  user code; the library precondition checks (`_GLIBCXX_ASSERTIONS` on
  libstdc++) are `NOTRUN`, and `extra.cxx_models` says so.
- **Findings.** pir findings for a non-default target carry
  `extra.target`.

`testdata/conformance/prism/datamodel/` holds four true/false pairs whose
answer flips between LP64 and LLP64 (`long + 1` at 2^31 − 1, `1L << 32`,
`a[sizeof(long)]`, `memcpy` of a `wchar_t`). Conformance tasks take a
`target:` key. `tests/cpp/test_target.cpp` checks the triples, the CLI,
the front-end flags and the pir verdict of each pair on both targets.

## 7. What is NOTRUN on Windows

Tests and source comments cite this section (`tests/cpp/test_svcomp.cpp`,
`include/prism/pir/frontend.hpp`). Each item below prints its own `NOTRUN`
reason; none is a silent skip (Laws 1 and 7).

### PIR on Windows

`frontend_usable` (`include/prism/pir/frontend.hpp`) decides whether pir
can lower on this host. It is `NOTRUN` with the reason when:

- clang or opt is missing (install the pinned LLVM: `scripts/fetch_llvm.py`);
- the target is `x86_64-pc-windows-msvc` and the UCRT / Windows SDK headers
  are not found;
- the target is a Linux one and the pinned Linux sysroot is missing
  (`scripts/fetch_toolchain.py linux-sysroot`, under
  `%USERPROFILE%\.prism\toolchain\linux-sysroot`, or `PRISM_LINUX_SYSROOT`).

The probe compiles through temporary files, because Windows clang given
stdin `-` writes `-.ll` to disk instead of IR to stdout.

### SV-COMP

SV-COMP tasks are Linux programs. On Windows, `tests/cpp/test_svcomp.cpp`
(`svcomp_exec_portable()` is false) reports `NOTRUN` for:

- replay with a real compiler and UBSan: the replay stub is POSIX C
  (`<unistd.h>`, `_exit`) and no MSVC-target clang here has a usable UBSan;
- `svcomp solve` on the pinned conformance tasks and `fuzz_replay` with a
  real compiler;
- the archive executable-bit check in `svcomp pack` (NTFS has no mode bits).

What does run on Windows: the replay decision logic against `fake_cc`
(replayed / not-replayed, the UBSan message filter, a build failure,
`reach_error`), the Law 9 gate, and `svcomp pack` with a `.cmd` stand-in.

### Other parts

| Part | Why |
|---|---|
| Steps that execute scanned code without `--allow-exec=unsandboxed` | No isolating sandbox on Windows (section 5). |
| AFL++ | No native Windows build: `afl-fuzz` is not found, so it is `NOTRUN`. The `PRISM_AFL=1` test case says "NOTRUN on Windows". |
| Sanitizer runs, label self-check | Need a clang that links UBSan + ASan (`sanitizers_usable()`); otherwise `NOTRUN`. CI puts the clang ASan DLLs on `PATH`. |
| `prism-bitblast --eval` in `test_leanbb` | `NOTRUN` when the file is not a Windows PE build (for example a Linux binary left by WSL). |
| `kill -9` exit status in `test_qa` | POSIX signals only. The Job Object timeout, background-grandchild and memory-cap cases run instead. |
| `prism_gui` without a display | POSIX only; Windows always has a window station. |

## 8. Writing portable engine code

- No `__int128` in engine code: use `prism::i128` / `prism::u128`.
- Spawn processes through `prism::exec` (FORGE), not `fork`/`popen`.
- Use `sandbox::private_temp_dir` for scratch directories, not fixed
  `/tmp` paths.
- Put Windows-only code behind `#ifdef _WIN32` with a POSIX branch next to
  it, and give a test that cannot run on Windows a `NOTRUN` message
  (`prism_test::notrun(reason)` from `tests/cpp/test_support.hpp`, branch
  `port/testharness`).
- New tests go in the shared `prism_tests` source list; a stand-in program
  goes behind `PRISM_TESTS_WIN_SUBSET`.
