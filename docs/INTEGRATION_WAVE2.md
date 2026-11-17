# integration-wave2: merge record

Branch `integration-wave2`, cut from `integration-wave1` (e2251b7). Open PRs merged with `git merge --no-ff`, in this order:
#57 pir-w2-translator-s, #58 pir-w1-kernel-specs, #54 pir-w3-memory-model, #56 pir-w5-ops-tables, #59 pir-w4-asm-semantics,
#51 pir-w7-concurrency, #55 pir-w6-lean-mirror, #52 pir-device-model, #53 fix-windows-ci. Nothing is merged on GitHub; each
original PR stays open and reviewable on its own.

## Conflicts and resolutions

1. #59 (W4 asm semantics) vs the earlier merges, `tests/cpp/test_pir_kernel.cpp` ("inline asm, atomics, barriers and volatile").
   W1/W2 added the cases `asm_memclobber_*`, `asm_goto_memclobber_bad`, `asm_nomem_escaped_ok`, and expected `asm-havoc` for the
   no-memory-clobber `nop` cases; W4 models `nop` exactly (`translate_asm.inc`, tag `asm-exact`) and expects `asm-exact` for `asm_nomem_ok`.
   Resolution: keep every case from both sides; `asm_nomem_ok` and `asm_nomem_escaped_ok` (both a plain `nop`) now expect `asm-exact`;
   `asm_ud2_ok` keeps W4's `asm-privileged`. The translator changes merged without conflict (translate.cpp, translate_ctl.inc,
   translate_kernel.inc auto-merged; `asm_havoc` keeps both W1/W2's memory-clobber `extra` text and W4's `tag` parameter).
2. #51 (W7 concurrency): `include/prism/config.hpp`, `include/prism/pir.hpp`, `src/prism/cli.cpp`, `src/prism/pir/translate.cpp`
   (includes and declarations). W5 added `--ops-table` / `OpsField` / `translate_asm.inc`; W7 adds `--lock-contracts` / `LockContract` /
   `translate_lock.inc`. The two are independent: both options, both structs, both help lines and both `#include`s are kept.
3. #51, `tests/cpp/test_pir_kernel.cpp`: W1/W2 inserted the Linux/FreeBSD W1/W2 spec cases at the same place as W7's lock cases.
   All cases of both sides are kept (W7's added after the W1/W2 ones; W7 changed no existing case).
4. #52 (device model), `src/prism/pir/stage_mem.cpp`: includes (`<fstream>`, `<sstream>`, ... from earlier merges plus `<cstdlib>`) are
   all kept; the `PRISM_PIR_INLINE_DEPTH` block sits after the harness options already merged.
5. #55, #53: no conflicts.

## Integration fixes

(see the commits after the merges)
