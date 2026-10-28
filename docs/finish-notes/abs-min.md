# abs of the type minimum

Slice `finish/libc-abs-min`, base `f81f9a6a4a23eeb665368af993a3476f99293476`.

`abs`, `labs`, and `llabs` in `src/prism/pir/models/libc/stdlib.cpp` take the
minimum of the argument type from `<limits.h>`: `INT_MIN`, `LONG_MIN`, and
`LLONG_MIN`. That value is

`__prism_check(0, "FUNC-CONTRACT", "abs(INT_MIN) is undefined")`

(and the same shape for `labs(LONG_MIN)` and `llabs(LLONG_MIN)`), then
`__builtin_trap`. The arm does not negate the argument and does not return.
It does not return the minimum as a positive value and it does not return
zero.

A negative value other than that minimum is in the `else if (x < 0)` arm and
returns `-x`. Zero and a positive value are in the remaining `else` and
return `x`, so zero stays zero and a positive value stays that value.

The comment above the three functions says they are not PROVED and not
CLEAN. This is a model of the C rule that the minimum has no representable
negation. It is not a proof, and it is not a fuzzer `CLEAN` result.

`malloc`, `calloc`, `free`, `realloc`, `qsort`, `bsearch`, `atoi`, `atol`,
`strtol`, `strtoul`, `rand`, `srand`, `getenv`, and `operator new` / `delete`
are unchanged. The comment on `rand` still says the seed is not stored. On a
non-empty `bsearch` range the model still reaches `__prism_assume(0)` and
returns NULL. `src/prism/bmc_encoder.inc` was not edited. Its call model of
`abs` still flags the minimum as `INT-SIGNED-OVF`. `docs/PIR.md` was not
edited.

## Tests

`tests/cpp/test_qsort_null.cpp` still locks the other function texts and the
qsort and bsearch checks. The locked `abs`, `labs`, and `llabs` strings are
the new bodies.

`tests/cpp/test_abs_min.cpp` reads `stdlib.cpp` from `PRISM_SOURCE_DIR`. For
each function it checks that the minimum arm is `FUNC-CONTRACT` then
`__builtin_trap`, that the arm has no negation and no return, that a later
negative value returns `-x`, and that zero and a positive value return `x`.
The comment says not PROVED and not CLEAN. The seed sentence and the
`bsearch` `__prism_assume(0)` path are still in the file.

`CMakeLists.txt` lists `tests/cpp/test_abs_min.cpp` as one new line in
`_prism_test_sources`. No existing test source line was removed.

`prism_tests` was not executed. This slice does not configure or build.
