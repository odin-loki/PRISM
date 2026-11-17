# Inline-asm and .S census (W4)

Mechanical count by script over the source trees, no model judgement. Date 2026-11-16.

Trees: Linux 6.18.60 at `/home/odin/pbsd-ref/linux` (WSL); FreeBSD (HardenedBSD 15.0) at
`hbsd/src` of the dreamy-mahavira worktree. Script: the census scanner used for W4 (kept
outside the repo; method below).

## Method

- Inline asm: every `asm`, `__asm__`, `__asm`, `asm_inline` call (with optional `volatile`,
  `__volatile__`, `goto`, `inline`) in `.c`, `.h`, `.inc`, `.cc` files. The template is the
  concatenation of the string literals of the first argument, up to the first `:`, `)`, `,` or
  `;` at depth 0. A leading macro (`ALTERNATIVE`, `LOCK_PREFIX`, `_ASM_*`, `__HYPERCALL`, ...) is
  recorded as `MACRO:<name>` and not expanded. Commented-out call sites are skipped.
  `Documentation/`, `tools/`, `scripts/` are skipped for Linux.
- `.S` routines: `SYM_FUNC_START*`, `SYM_CODE_START*`, `ENTRY`, `ALTENTRY` (Linux) and
  `ENTRY`, `ALTENTRY`, `ENTRY_NOPROFILE` (FreeBSD). Labels and `END`/`ENDPROC` are not counted.
- ISA: from the path (`arch/<isa>/` on Linux, `sys/<isa>/` on FreeBSD). Files outside those
  directories are `common` (their asm is not ISA-tagged here; many `common` templates are x86
  SIMD helpers, see below).

Cross-check with plain `grep` (same tree, same file types): Linux `arch/x86` open-paren asm
lines 680 vs 694 census sites; whole Linux tree 11,066 grep lines vs 6,814 census sites (the
gap is the skipped directories, comments and multi-line keyword lines). FreeBSD `sys/` keyword
lines 1,511 vs 2,873 census sites over `sys/` plus `contrib/`/`cddl/`, which the census
counts and the grep above does not.

## Totals by ISA

Linux (C/H: 6,814 sites, 58,772 files; `.S`: 1,286 files)

| ISA | sites | distinct templates | macro-built sites | `.S` files | `.S` routines (distinct) |
|---|---|---|---|---|---|
| x86-64 (arch/x86) | 694 | 472 | 123 | 150 | 482 (474) |
| arm64 | 227 | 120 | 76 | 73 | 216 (215) |
| riscv | 182 | 108 | 71 | 64 | 102 (102) |
| arm (32) | 491 | 323 | 22 | 233 | 765 (757) |
| powerpc | 708 | 305 | 115 | 138 | 71 (71) |
| sparc | 481 | 256 | 2 | 133 | 363 (360) |
| mips | 410 | 262 | 24 | 54 | 1 (1) |
| m68k | 370 | 283 | 1 | 75 | 34 (34) |
| s390 | 303 | 258 | 24 | 29 | 56 (53) |
| common (generic dirs) | 1,919 | 1,000 | 14 | 71 | 106 (104) |

FreeBSD (C/H: 2,873 sites, 39,692 files; `.S`: 850 files)

| ISA | sites | distinct templates | macro-built sites | `.S` files | `.S` routines (distinct) |
|---|---|---|---|---|---|
| x86-64 (amd64) | 268 | 205 | 20 | 17 | 107 (107) |
| i386 | 173 | 137 | 15 | 13 | 50 (50) |
| arm64 | 127 | 83 | 9 | 21 | 104 (104) |
| arm32 | 83 | 76 | 0 | 23 | 66 (66) |
| riscv | 80 | 68 | 6 | 9 | 40 (39) |
| powerpc | 307 | 179 | 8 | 20 | 22 (22) |
| common | 1,175 | 664 | 37 | 511 | 243 (243) |
| contrib (third-party) | 590 | 330 | 54 | 45 | 14 (14) |
| crypto `.S` (common) | - | - | - | 170 | 16 (16) |
| cddl (dtrace) | 6 | 4 | 0 | 6 | 80 (80) |

## Top x86-64 templates (source text, counts of sites)

Linux arch/x86 (the top entries are mostly macros: the expansion is ISA-level, not the
template text):

| sites | template |
|---|---|
| 23 | `MACRO:LOCK_PREFIX` |
| 17 | `MACRO:ALTERNATIVE` |
| 12 | `MACRO:lock` |
| 10 | `vxorps %0, %%ymm` |
| 7 | `syscall` |
| 6 | `hlt` (also `MACRO:VMWARE_HYPERCALL`, `MACRO:__HYPERCALL`, 6 each) |
| 6 | `int $0x80` |
| 4 | `mfence` |
| 3 | `cpuid`, `pause`, `vmmcall`, `fninit` |
| 2 | `rdmsr`, `wrmsr` (inside `_ASM_EXTABLE_TYPE` wrappers) |

FreeBSD amd64/x86 (top entries):

| sites | template |
|---|---|
| 14 | `MACRO:__STRING` |
| 7 | `int $0x80` |
| 6 | `MACRO:HYPERCALL_STR`, `syscall` (6 each) |
| 4 | `rdtscp` |
| 3 | `cpuid`, `int $18` |
| 2 | `rdmsr`, `rdtsc`, `lock;cmpxchg16b %1`, `fnclex`, `fnstcw`, `ldmxcsr`, `stmxcsr`, `sgdt`, `sidt`, `str`, `out %al,(%dx)` |

The per-template tail is long: most x86-64 templates occur once or twice. About 18% of the
Linux x86-64 sites (123 of 694) use a macro whose expansion this census does not see
(`LOCK_PREFIX`, `ALTERNATIVE`, `_ASM_EXTABLE`, hypercall wrappers); on FreeBSD amd64/x86 it is 20 of 268. A model keyed on macro-expanded text
must expand them first; the W4 table below keys on the expanded LLVM text.

## Modelled in this change (W4 slice 1, x86-64)

Source: `src/prism/pir/models/asm/x86_64_asm.{h,cpp}`. Gate: `tests/asm/x86_64_asm_validate.cpp`
(native execution on this host vs model, 4,096 random vectors per instruction, seed 20261116).

| template | class | validated | result |
|---|---|---|---|
| `bswap %0` | Pure | native vs model | 4096 vectors, 0 mismatches |
| `lock; xaddq %0, %1` | Memory (atomic RMW) | native vs model | 0 mismatches |
| `lock; cmpxchgq %2, %1` | Memory (atomic RMW), ZF | native vs model | 0 mismatches (half the vectors equal) |
| `xchgq %0, %1` | Memory (implicit lock) | native vs model | 0 mismatches |
| `lock; addq %1, %0` | Memory (atomic RMW) | native vs model | 0 mismatches |
| `pause`, `mfence`, `lfence`, `sfence` | Fence | weak: memory word unchanged | 0 mismatches; ordering itself not observable single-threaded |
| `rdtsc` | Nondet | property: consecutive reads never decrease | 0 violations |
| `cpuid` | Nondet | not compared (machine-specific values) | UNVALIDATED |
| `rdmsr`, `wrmsr` | Privileged | not executed (faults in user mode) | UNVALIDATED |

Not modelled (stay havoc, listed): `in`/`out` port I/O, `cli`/`sti`/`hlt`, `invlpg`, `lgdt`/`lidt`,
`syscall`, `int`, `vmcall`/`vmmcall`, `cmpxchg16b`, string ops (`rep movs`/`stos`), x87/SSE
state, the `.S` routines (entry, context switch, boot). `QEMU` user-mode is not installed in
WSL; the native host run replaces it for the non-privileged set.

Next: arm64 and riscv templates; the translator hook (asm_havoc in
`src/prism/pir/translate_kernel.inc`) is not wired in this change.
