# Conformance suite sources

Where every task set of `build/prism-qa conformance` comes from, with its
pin and licence. The task format, the scoring and the release gate are in
`docs/CONFORMANCE.md`.

## `prism/` — in-house tasks (this repository)

Tasks written for PRISM: one `.c`/`.cpp` file plus a `.yml` sidecar each,
grouped by construct or UB class:

| directory | what it covers |
|---|---|
| `overflow/` | signed `+ - * / %`, unary minus, `abs`, `++`/`--`, compound assignment, `long`/`long long`, integer promotion, mixed signedness, casts, ternary, `switch`, loops, recursion, helper calls, constants |
| `unsigned/` | unsigned wrap-around (defined) next to signed UB hidden in unsigned-looking code |
| `div/` | division/remainder by zero, guarded divisors, `LLONG_MIN / -1` |
| `shift/` | shift count out of range or negative, `1 << 31`, left shift of negative values, left-shift overflow, promotion before shifting, `int >> unsigned` |
| `array/` | local, 2-D and global arrays; reads, writes, off-by-one loops, negative indices |
| `pointer/` | local pointers (`&v`, array decay) and a NULL dereference on a path |
| `ptrparam/` | pointer parameters: Law 6 demands `NEEDS-HARNESS` (`expect_status`), never a proof or a refutation |
| `macro/` | `INT_MAX`/`INT_MIN`, `#define` bounds, function-like macros |
| `memory/` | heap and stack bounds, NULL, use after free, double and invalid free, `realloc`, misalignment, `memcpy` overlap, string functions, uninitialised reads |
| `kindmem/` | k-induction with memory: loops that write through pointers, aliasing, allocation, globals, structs |
| `c99/`, `c11/`, `c17/`, `c23/` | constructs of each C standard (designated initialisers, flexible arrays, `_Generic`, `_Atomic`, `aligned_alloc`, `typeof`, `_BitInt`, `ckd_add`, `nullptr`, ...) |
| `cxx/` | C++ language constructs and standard-library use (containers, strings, `optional`, `span`, `mdspan`, `format`, ranges, threads, C++20 shift semantics, ...) |
| `fp/` | floating point: conversion to integer out of range (`no-fp-cast`), division, rounding |
| `eh/` | C++ exceptions: catch, cleanups, `noexcept`, exception types (`no-uncaught`) |
| `coro/` | C++ coroutines |
| `sjlj/` | `setjmp`/`longjmp` |
| `virt/` | virtual dispatch and function pointers |
| `asm/` | inline assembly under a contract |
| `datamodel/` | pairs whose answer flips between LP64 and LLP64; each sidecar names its `target:` |
| `regress/` | one `_true`/`_false` pair per wrong proof or false alarm fixed (docs/CONFORMANCE.md "Known issues") |

Every construct has a `_true` variant (no undefined behaviour for any input)
and a `_false` variant (a violation exists). `false` tasks carry a `witness`
(arguments that trigger the violation). `build/prism-qa conformance
--self-check` compiles every task with `-fsanitize=undefined,address`,
requires each witness to trip a sanitizer and each `true` function to run
clean on an edge-value grid plus 2000 random inputs.

Licence: same as this repository.

## `sv-comp/` — pinned SV-COMP subset

- Upstream: <https://gitlab.com/sosy-lab/benchmarking/sv-benchmarks>
- Commit: `07b00127ac57f773f9de6eee95821b6947948dbe` (2026-09-11, `main`)
- Files copied unmodified (`.c`/`.i` input plus the task-definition `.yml`),
  174 tasks: every task of the directories below that has an
  `expected_verdict` for `no-overflow.prp` (117: 75 true, 42 false) or
  `unreach-call.prp` (127: 98 true, 29 false). Whole directories were
  taken, not a selection by PRISM's results.

| directory | tasks (no-overflow t/f, unreach-call t/f) | licence |
|---|---|---|
| `c/signedintegeroverflow-regression` | 15 (5/10, –) | BSD-style, University of Freiburg (`signedintegeroverflow-regression/LICENSE.txt`) |
| `c/bitvector` | 68 (36/32, 30/6) | Apache-2.0 (SPDX headers, `bitvector/README.md`) |
| `c/loop-simple` | 8 (6/0, 6/2) | Apache-2.0 (SPDX headers, `loop-simple/README.md`) |
| `c/loop-invgen` | 29 (16/0, 28/1), including the `*-reducer` variants | Apache-2.0 (upstream `LICENSE.txt` links to `LICENSE.Apache-2.0.txt`; copied as is) |
| `c/loop-new` | 11 (4/0, 11/0), including the `*-reducer` variants | Apache-2.0 (upstream `LICENSE.txt` links to `LICENSE.Apache-2.0.txt`; copied as is) |
| `c/loop-acceleration` | 43 (8/0, 23/20) | BSD-2-Clause, Matt Lewis 2014 (`loop-acceleration/LICENSE`, copied with the upstream `README`) |

Each directory also holds the upstream `README`/`LICENSE` files as they are
at the commit. Every licence above is permissive.
`LICENSE.Apache-2.0.txt` is the upstream Apache-2.0 text; `UPSTREAM-README.md`
is the upstream README (licensing and attribution rules).
`properties/` holds `no-overflow.prp`, `unreach-call.prp` and
`valid-memsafety.prp` from `c/properties/` at the same commit, copied
unmodified so the tasks' `../properties/*.prp` references resolve.

Two tools read this subset. `prism-qa conformance` scores each task for
`no-overflow` (else `valid-memsafety`); a task with only an `unreach-call`
verdict is not one of its tasks. `prism svcomp score` scores both
`no-overflow` and `unreach-call` with SV-COMP points (docs/SVCOMP.md).

SV-COMP tasks are whole programs: the verdict applies to `main`, and only to
the named property (a FAILED of another class is reported separately, not
as a false alarm). In `prism-qa conformance` their counterexamples are not
replayed (inputs come from `__VERIFIER_nondet_*`), so detection is
"refuted, not replayed".

## `esbmc-cpp/` — ESBMC C++ regression tests (curated subset; full set fetched)

- Upstream: <https://github.com/esbmc/esbmc>, directories
  `regression/esbmc-cpp`, `esbmc-cpp11`, `esbmc-cpp14`, `esbmc-cpp17`,
  `esbmc-cpp20`, `esbmc-cpp23`
- Commit: `653926f91580d8d67858d42db814f09d9a9fa257` (2026-09-23, `master`)
- Licence: ESBMC's own code and tests are Apache-2.0 (ESBMC `COPYING`: "The
  ESBMC code, authored by us and our modifications to the CBMC codebase, is
  distributed under the terms of the Apache License 2.0"; copyright holders
  Lucas Cordeiro, Jeremy Morse, Bernd Fischer, Mikhail Ramalho).
  `LICENSE.Apache-2.0.txt` is the licence text. The subset leaves out every
  directory or file that may carry another licence: `esbmc-cpp/cbmc` (CBMC,
  BSD-4-clause), `esbmc-cpp/gcc-template-tests` (GCC testsuite, GPL),
  `esbmc-cpp/qt`, `esbmc-cpp/esbmc-systemc`, and any file mentioning a
  copyright, licence, a textbook listing ("Fig. N", Deitel/Pearson), LLBMC,
  GCC or `dg-` directives (`ESBMC_FOREIGN_TEXT` in `src/tools/qa/support/fetch.cpp`).
- Files: the test's single source file, renamed
  `<dir>__<test>.cpp` (unmodified content), plus a generated `.yml` sidecar
  recording `upstream: esbmc@653926f91580 regression/<path>`, ESBMC's
  options, the language standard and the label.

Fetch and convert the whole set (not in git):

```
build/prism-qa conformance --fetch-esbmc /tmp/esbmc     # sparse, blob-filtered fetch of the pinned commit
build/prism-qa conformance --esbmc /tmp/esbmc --suite /tmp/none
build/prism-qa conformance --self-check --suite /tmp/esbmc/esbmc-cpp   # native label check
build/prism-qa conformance --curate-esbmc /tmp/esbmc    # regenerate esbmc-cpp/ (the committed subset)
```

GitHub archive downloads are not used (some proxies refuse them): the
fetcher runs `git fetch --depth 1 --filter=blob:none <commit>` with a sparse
checkout of the six directories and refuses any other `HEAD`.

Conversion (`esbmc_convert`): `test.desc` line 1 is the level (CORE only by
default; THOROUGH with `--esbmc-thorough`; KNOWNBUG skipped), line 2 the
input, line 3 ESBMC's options, then output regexes, exactly one of which must
be `VERIFICATION SUCCESSFUL` (→ `main: true`) or `VERIFICATION FAILED`
(→ `main: false`). Skipped, with a count printed per reason: multi-file
tests, tests using `__ESBMC_*` intrinsics, options that change the checked
property set (`--overflow-check`, `--memory-leak-check`, `--no-*-check`,
`--function`, `--data-races-check`, `--error-label`, ...), FAILED labels that
come from a limit of ESBMC's operational model ("capacity exceeded",
"forgotten memory"), and labels that contradict the C++ standard
(`ESBMC_DISPUTED`, each with its reason). At the pinned commit: 1906 tasks
converted, 1044 THOROUGH, 113 intrinsics, 55 multi-file, 51 without a
single verdict line, 24 KNOWNBUG, 70 property-changing options (11 of them
`--error-label`: a reachable `ERROR:` label is ESBMC's violation there, and
PRISM checks no labels), 14 XML descriptions, 6 model limits and 2 disputed
labels skipped.

What a label means: ESBMC checks, from `main`, assertions, array bounds,
pointer safety and division by zero (not signed overflow). The property is
`esbmc-cpp` and is property-scoped like SV-COMP: a PRISM FAILED of another
class (overflow, shift, uninitialised read) on a SUCCESSFUL task is "other
property", not a false alarm; any proof of `main` on a FAILED task is a wrong
proof. A SUCCESSFUL under `--no-unwinding-assertions` only speaks up to
ESBMC's bound (`label_bound:` in the sidecar).

The committed subset (64 tasks: 32 SUCCESSFUL, 32 FAILED; at most 3 per
category and label) is chosen by `curate_esbmc`: deterministic programs only
(no `nondet_*`, `rand`, input, threads, time), licence-clean, ordered by
SHA-256 of the upstream path (not by PRISM's results), and kept only when
one native run (clang++, `-fsanitize=undefined,address`, assertions on)
agrees with ESBMC's label. For a deterministic program that single run is
its only behaviour, so the label is checked, not just inherited;
`--self-check` repeats it.

## `concurrency/` — thread programs (this repository)

Whole programs that create threads (`pthread_create` or `thrd_create`),
in true/false pairs. Each program has one sidecar per property it is
labelled for (`<name>-norace.yml`, `-noassert.yml`, `-nodeadlock.yml`); the
verdict applies to `main`. Scored by the `conc` stage only
(docs/CONCURRENCY.md). Licence: same as this repository.

## `libc-models/` — contract harnesses for PRISM's library models (this repository)

Harnesses that state the contract of PRISM's own library models with
`assert()`, so the `pir` stage checks the repository's model code, not the
platform library. The C harnesses `#include` the model sources of
`src/prism/pir/models/libc/` (`harness.h` explains the scheme); the C++
harnesses call the C++ model headers. `_true` functions: the model meets its
contract for every input. `_false` functions: a wrong contract or a
precondition violation, each naming the class that must refute it
(`expect_class:`; a refutation for another reason is not a detection). The
property is `libc-contract`. Results: `docs/PIR.md` "Library models verified
by PRISM". Licence: same as this repository.

## Juliet (not in git)

NIST SARD Juliet C/C++ test suite 1.3, fetched on demand:

```
build/prism-qa conformance --fetch-juliet /tmp/juliet     # download + verify + extract
build/prism-qa conformance --juliet /tmp/juliet
```

- URL: <https://samate.nist.gov/SARD/downloads/test-suites/2017-10-01-juliet-test-suite-for-c-cplusplus-v1-3.zip>
- Size: 152957342 bytes
- SHA-256: `ada9d7e1c323d283446df3f55bdee0d00bda1fed786785fe98764d58688f38eb`
  (recorded 2026-09-23; the fetcher refuses any other archive)
- Extracted: single-file C tests of CWE190/CWE191 (`int`, `int64_t` only:
  unsigned wrap and `char`/`short` arithmetic are not undefined behaviour),
  CWE369 (`int`), CWE476 and CWE680, flow variant `_01` by default
  (`--juliet-flows 01,02,...`). `*_bad` functions are `false`, `good*`
  functions are `true`.
- Licence: Juliet is a work of the US Government (NIST SARD), public domain in
  the United States.
