# Conformance suite sources

Roadmap Part 2.7. Run with `python tools/conformance.py` (see
`docs/CONFORMANCE.md` for the metric definitions and current numbers).

## `prism/` — in-house tasks (this repository)

252 single-function tasks written for PRISM, one `.c`/`.cpp` file plus a
`.yml` sidecar each, grouped by feature/UB class:

| directory | what it covers |
|---|---|
| `overflow/` | signed `+ - * / %`, unary minus, `abs`, `++`/`--`, compound assignment, `long`/`long long`, integer promotion of `short`/`char`/`unsigned short`, mixed signedness, casts/truncation, ternary, nested `if`, `switch` (+ fall-through), `for`/`while`/`do` loops, nested loops, `break`, recursion, helper calls, constants |
| `unsigned/` | unsigned wrap-around (defined, never UB) next to signed UB hidden in unsigned-looking code |
| `div/` | division/remainder by zero, guarded divisors, `LLONG_MIN / -1` |
| `shift/` | shift exponent out of range or negative, `1 << 31`, left shift of negative values, left-shift overflow, promotion of `unsigned char` before shifting, `int >> unsigned` |
| `array/` | constant-bound local, 2-D and global arrays; reads, writes, off-by-one loops, negative indices |
| `pointer/` | local pointers (`&v`, array decay) and a NULL dereference on a path |
| `ptrparam/` | pointer parameters: Law 6 demands `NEEDS-HARNESS` (`expect_status`), never a proof or a refutation |
| `macro/` | `INT_MAX`/`INT_MIN`, `#define` bounds and function-like macros (the engine has no preprocessor) |
| `c23/` | `bool`, binary literals, digit separators, `typeof`, `[[attributes]]`, one-argument `static_assert`, `ckd_add`, `_BitInt`, empty initialisers, `nullptr`, enums with a fixed underlying type |
| `cxx/` | C++23: `constexpr`, trailing return types, templates, lambdas, `std::abs`, `if consteval`, `std::array`, C++20 shift semantics, `std::midpoint`, reference parameters, range-for, structured bindings, `[[assume]]` |
| `regress/` | one `_true`/`_false` pair per encoder soundness bug fixed after the first measurement (docs/CONFORMANCE.md "Known issues"): call arguments, `INT_MIN` constant, promoted comparisons, helper early return, loop early exit / break / continue, dangling else, code after a loop under k-induction, uncomputable enumerators, cast aliasing, side effects under `?:`/`&&`, downward `+` overflow, `long long` width, mixed widths |

Every feature has a `_true` variant (no undefined behaviour for any input)
and a `_false` variant (a violation exists). `false` tasks carry a `witness`
(concrete arguments that trigger the UB); `python tools/conformance.py
--self-check` compiles every task with `-fsanitize=undefined,address`,
requires each witness to trip the sanitizer and each `true` function to
survive an edge-value grid plus 2000 random inputs.

Licence: same as this repository.

## `sv-comp/` — pinned SV-COMP subset

- Upstream: <https://gitlab.com/sosy-lab/benchmarking/sv-benchmarks>
- Commit: `07b00127ac57f773f9de6eee95821b6947948dbe` (2026-09-11, `main`)
- Files copied unmodified (`.c`/`.i` input plus the task-definition `.yml`),
  45 tasks, all with a `no-overflow.prp` verdict (the property PRISM checks):

| directory | tasks | licence |
|---|---|---|
| `c/signedintegeroverflow-regression` | 15 (all tasks with a `no-overflow` verdict) | BSD-style, University of Freiburg (`signedintegeroverflow-regression/LICENSE.txt`) |
| `c/bitvector` | 23 (`byte_add*`, `gcd_1/2`, `interleave_bits`, `jain_*`, `modulus-1/2`, `num_conversion_1`, `parity`, `sum02-1`) | Apache-2.0 (SPDX headers, `bitvector/README.md`) |
| `c/loop-simple` | 3 (`nested_1`, `nested_1b`, `nested_2`) | Apache-2.0 (SPDX headers) |
| `c/loop-invgen` | 4 (`half_2`, `large_const`, `nested6`, `id_trans`) | Apache-2.0 (upstream `LICENSE.txt` links to `LICENSE.Apache-2.0.txt`) |

`LICENSE.Apache-2.0.txt` is the upstream Apache-2.0 text; `UPSTREAM-README.md`
is the upstream README (licensing and attribution rules).
`properties/` holds `no-overflow.prp`, `unreach-call.prp` and
`valid-memsafety.prp` from `c/properties/` at the same commit, copied
unmodified so the tasks' `../properties/*.prp` references resolve (used by
`tools/svcomp/`, see `docs/SVCOMP.md`).

SV-COMP tasks are whole programs; their verdict applies to `main`, and only
to the named property (a FAILED of another class is reported separately, not
as a false alarm). Their counterexamples are not replayed (inputs come from
`__VERIFIER_nondet_*`), so SV-COMP detection is reported as "refuted, not
replayed".

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
  GCC or `dg-` directives (`ESBMC_FOREIGN_TEXT` in `tools/conformance.py`).
- Files: the test's single source file, renamed
  `<dir>__<test>.cpp` (unmodified content), plus a generated `.yml` sidecar
  recording `upstream: esbmc@653926f91580 regression/<path>`, ESBMC's
  options, the language standard and the label.

Fetch and convert the whole set (not in git):

```
python tools/conformance.py --fetch-esbmc /tmp/esbmc     # sparse, blob-filtered fetch of the pinned commit
PRISM_BIN=build/prism python tools/conformance.py --esbmc /tmp/esbmc --suite /tmp/none
python tools/conformance.py --self-check --suite /tmp/esbmc/esbmc-cpp   # native label check
python tools/conformance.py --curate-esbmc /tmp/esbmc    # regenerate esbmc-cpp/ (the committed subset)
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
`--function`, `--data-races-check`, ...), and FAILED labels that come from a
limit of ESBMC's operational model ("capacity exceeded", "forgotten
memory"). At the pinned commit: 1919 tasks converted, 1044 THOROUGH, 113
intrinsics, 55 multi-file, 51 without a single verdict line, 24 KNOWNBUG,
59 property-changing options, 14 XML descriptions, 6 model limits skipped.

What a label means: ESBMC checks, from `main`, assertions, array bounds,
pointer safety and division by zero (not signed overflow). The property is
`esbmc-cpp` and is property-scoped like SV-COMP: a PRISM FAILED of another
class (overflow, shift, uninitialised read) on a SUCCESSFUL task is "other
property", not a false alarm; any proof of `main` on a FAILED task is a wrong
proof. A SUCCESSFUL under `--no-unwinding-assertions` only speaks up to
ESBMC's bound (`label_bound:` in the sidecar).

The committed subset (64 tasks: 32 SUCCESSFUL, 32 FAILED; 19 categories, at
most 3 per category and label) is chosen by `curate_esbmc`: deterministic
programs only (no `nondet_*`, `rand`, input, threads, time), licence-clean,
ordered by SHA-256 of the upstream path (not by PRISM's results), and kept
only when one native run (clang++, `-fsanitize=undefined,address`,
assertions on) agrees with ESBMC's label. For a deterministic program that
single run is its only behaviour, so the label is checked, not just
inherited; `--self-check` repeats it.

## `libc-models/` — contract harnesses for the pir libc models (this repository)

Roadmap 8.2. Harnesses that `#include` the model sources of
`src/prism/pir/models/libc/` and state the C standard's contract of each
model with `assert()` (`harness.h` explains the scheme). `_true` functions:
the model meets its contract; `_false` functions: a wrong contract or a
precondition violation, each naming the class that must refute it
(`expect_class:`; a refutation for another reason is not a detection).
Results: `docs/PIR.md` "Library models verified by PRISM". Licence: same as
this repository.

## Juliet (not in git)

NIST SARD Juliet C/C++ test suite 1.3, fetched on demand:

```
python tools/conformance.py --fetch-juliet /tmp/juliet     # download + verify + extract
PRISM_BIN=build/prism python tools/conformance.py --juliet /tmp/juliet
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
