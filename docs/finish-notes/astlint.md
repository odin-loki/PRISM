# AST lint slice

Branch `finish/astlint` from `e59a8b3df`. Landed the unmerged astlint rules by checking out only `include/prism/astlint.hpp`, `src/prism/astlint_checks.cpp`, and `tests/cpp/test_astlint.cpp` from `origin/cursor/astlint-port-rules-c5c7` (commits `20596a937`, `1ebd02dbe`, `dc15f2344`). No new taxonomy ids. `checkers_core.cpp` is unchanged: the regex rules still run, and an AST hit on the same file, line, and class supersedes them.

## Bugs fixed in the landed checks

- `unlock_of`, `lock_of`, and `is_unlock_name` used `std::regex` with `(?i)`. That pattern throws, and the catch in `astlint.cpp` turns the whole translation unit into NOTRUN. They now use `prism::Regex`, the same patterns as `checkers_core.cpp`.
- Lock events are `std::stable_sort`ed so same-line calls keep source order. A release still keys the mutex by the unlock name and reports when that key is already false. An acquire keys the mutex by the lock name for `LOCK-DOUBLE-LOCK`, and also sets the unlock-name key so `lock` / `unlock` / `lock` / `unlock` is not `LOCK-DOUBLE-UNLOCK`.
- The member null-test (`STR-NULL-MEMBER`) stays on PCRE2. The pattern uses fixed-length lookbehind, which `std::regex` rejects. A null PCRE2 code searches as no match, so a bad pattern cannot throw the unit into NOTRUN.

`STR-SNPRINTF` and `STR-WCSCPY` stay C-only (`!unit.cxx`), matching the doctest that those regex classes are C-only. `STR-STRNCPY-NUL` stays on every C-family file. That doctest says the AST layer reports it on any C-family file.

## Classes landed from the branch

`MEM-NOWAIT`, `MEM-ALLOCA`, `STR-SPRINTF`, `STR-SNPRINTF`, `STR-WCSCPY`, `CRYPTO-MISUSE`, `CRYPTO-SRAND`, `PTR-CHAIN-NULL`, `STR-NULL-ARG`, `STR-NULL-MEMBER`, `LOCK-IMBALANCE`, `LOCK-DOUBLE-UNLOCK`, `LOCK-DOUBLE-LOCK`, `LOCK-MISSING-INIT`, `INT-ATOI`, `INT-WRAP-ALLOC`, `UNINIT-SWITCH`, `CTRL-MISSING-RETURN`, `FUNC-NORETURN`.

`STR-MISSING-NUL` was already on `e59a8b3df`.

## Classes newly ported

Fourteen regex classes, each with a true-positive plant and a false-positive guard in `kPortedC` (`tests/cpp/test_astlint.cpp`, `// expect: CLASS`). The testdata `TEST_CASE` also checks the existing plants. No planted file was rewritten and no new testdata file was added.

| class | true positive | silent guard |
|---|---|---|
| `MEM-REALLOC-ZERO` | `realloc_zero.c` `realloc_zero_bad` line 7 | `realloc_zero_ok` |
| `MEM-PTR-ARITH` | `ptr_arith.c` `arith_bad` line 2 | `arith_ok` (both `if` guards) |
| `MEM-VLA-SIZE` | `vla.c` `vla_bad` line 2 | `vla_ok`, plus `BUFSIZ` and `sizeof(int)` in `kPortedC` |
| `MEM-FLEX-ARRAY` | `flex_array.c` `flex_bad` 14, `flex_star_bad` 19 | `flex_ok`, `flex_plain_ok` |
| `INFOLEAK-PAD` | `infoleak_pad.c` `leak_bad` line 9 | `leak_ok` |
| `MEM-ONESIDED-INDEX` | `onesided.c` `onesided_index` line 5 | `two_sided_index`, `unsigned_index` |
| `MEM-LEAK` | `mem_leak.c` `mem_leak_bad` line 6 | `mem_leak_ok`, and a one-unguarded-return guard in `kPortedC` |
| `RES-FD-LEAK` | `fd_leak.c` `fd_leak_bad` line 6 | `fd_leak_ok`, same one-return guard |
| `API-POPEN` | `popen.c` `popen_bad` line 8 | `popen_ok` |
| `API-CHROOT` | `chroot.c` `chroot_bad` line 6 | `chroot_ok` |
| `CONC-TOCTOU` | `toctou.c` `toctou_bad` line 7 | `toctou_ok` |
| `TRUST-UNVALIDATED-INPUT` | `unvalidated.c` `unval_bad` 7, `unval_div_bad` 24 | `unval_ok` |
| `INT-CLZ-ZERO` | `clz_zero.c` `clz_zero_bad` line 2 | `clz_zero_ok`; `(unsigned)0` is silent in `kPortedC` |
| `CONC-ATOMICITY` | `atomicity.c` `atom_bad` line 7 | `atom_ok` |

The batch replays the `checkers_core.cpp` patterns on the same padded body the regex stage uses (comments blanked, string literals blanked, leading newlines so body index `i` is file line `span.first + i`). A hit is attached to the AST node that begins on that line, so macros and dependent templates are dropped. One-line signatures therefore land on the regex line and supersede. `INT-CLZ-ZERO` is C-only by file extension (`.C` counts as C), matching `checkers_core`, and it is not gated on `unit.cxx`.

`atoi` used as an index is both `INT-ATOI` and `TRUST-UNVALIDATED-INPUT`. Division is `TRUST-UNVALIDATED-INPUT` only. `INT-ATOI` stays silent only when the body has both a lower and an upper test. `realloc(p, 0)` into a different pointer, then `p[0]`, is `MEM-REALLOC-ZERO` and `PTR-UNCHECKED-ALLOC` on the use. A self-assign stays `MEM-REALLOC-SELF`.

## Deliberately not ported

- `CTRL-SIBLING-ASYMMETRY`: compares sibling branches.
- `LOCK-ORDER`: pairs the first two locks across functions.
- `MEM-CAPACITY-FIRST`: multi-line window plus failure-arm leave / dead / release heuristics. `free_owner.c` in `tests/cpp/test_checkers_c.cpp` is the existing false-positive guard.
- `INTENT` and `API-PRECONDITION`: comment preamble, not a call or memory pattern.
- Exit-block replay (`exit_blocks`) used by the regex use-after-free and `lock_double_unlock` paths. The AST lock checks stay straight-line; the flow checks follow both arms of an `if` and join them (2026-10-30), but loops and `switch` bodies are still havocked. A release that only happens on a branch that later rejoins can still disagree with the regex replay.
- Dependent template expressions stay dropped in `Checker::add`.
- A `static` local VLA is skipped by the regex and by this port. It is not planted: `static int c[n]` may fail to parse.

## Follow-up for the parent

No CMake change. No `checkers_core.cpp` or taxonomy edit.

Run the astlint doctests on WSL ext4 after `scripts/wsl_sync_from_windows.sh` (`~/PRISM`, then `scripts/wsl_build.sh`). Do not run `cmake`, `ninja`, or `prism_tests` on the OneDrive tree. The cases are `astlint:` in `tests/cpp/test_astlint.cpp`. They skip when clang is missing. `chroot.c` and `toctou.c` include `unistd.h` / `fcntl.h`, so they need a Linux clang. `infoleak_pad.c` calls `memcpy` with no header; the C unit flags already keep that implicit declaration from failing the parse.
