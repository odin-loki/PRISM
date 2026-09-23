# Conformance suite sources

Roadmap Part 2.7. Run with `python tools/conformance.py` (see
`docs/CONFORMANCE.md` for the metric definitions and current numbers).

## `prism/` — in-house tasks (this repository)

218 single-function tasks written for PRISM, one `.c`/`.cpp` file plus a
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

SV-COMP tasks are whole programs; their verdict applies to `main`, and only
to the named property (a FAILED of another class is reported separately, not
as a false alarm). Their counterexamples are not replayed (inputs come from
`__VERIFIER_nondet_*`), so SV-COMP detection is reported as "refuted, not
replayed".

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
