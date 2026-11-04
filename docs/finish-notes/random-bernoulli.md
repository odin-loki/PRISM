# bernoulli_distribution

Branch `finish/cxx-random-bernoulli`, base `2e9979546458c5bd050803b9c5a918fe172df5e8`.

`std::bernoulli_distribution` is declared on the existing header
`src/prism/pir/models/cxx/random`. That path is one extensionless file. A
directory of that name would make `#include <random>` fail with EISDIR. The
include guard is still `_GLIBCXX_RANDOM`. `#pragma GCC system_header`.
Namespace `std`. It is not a template. The probability is a `double`.

This slice did not run cmake, ninja, or the `prism_tests` binary. The solver
was not run. This header is not a proof. The distribution is not a proof of
the probability. It is not PROVED and not CLEAN.

## What bernoulli_distribution does

`result_type` is `bool`. The default constructor delegates to
`bernoulli_distribution(0.5)`. The explicit constructor takes a `double`.
`p()` returns the stored probability. `min()` is `false` and `max()` is
`true`. `param` and `reset` are not declared.

`p < 0`, `p > 1`, or a NaN `p` calls

`__prism_check(0, "FUNC-CONTRACT", "bernoulli_distribution probability is outside [0, 1]")`

and then `__builtin_trap`. The constructor's assignments of `_M_p` are in
the `else`. That path does not store `p`. `operator()` repeats the same
check and then `__builtin_trap`, and that arm has no `return`. It returns
no bool. NaN is `__builtin_isnan`. On cl, `__builtin_isnan` is `p != p`,
next to the existing `__builtin_trap` stand-in. clang provides the builtin.

`p == 0` returns `false` and does not call `__VERIFIER_nondet_int`.
`-0.0` compares equal to `0.0`, so it takes the same path. `p == 1` returns
`true` and does not call nondet.

Otherwise the result is `__VERIFIER_nondet_int() ? true : false`. A nonzero
value, including a negative one, is `true`. Zero is `false`. The engine
argument is not the source of the value. The probability is not used. Two
probabilities in `(0, 1)` with the same nondet bit return the same bool.
That is not a proof of the probability.

`__VERIFIER_nondet_int` is declared `extern "C"` and is not defined in the
header, so the call is not a mangled name. `__prism_check` stays declared
and not defined.

## What stayed

`minstd_rand` is unchanged: multiplier 48271, increment 0, modulus
2147483647, default seed 1. After `seed(1)` the first output is 48271 and
the next is 182605794. `discard(0)` still does not step.

`minstd_rand0` is unchanged: multiplier 16807, increment 0, modulus
2147483647, default seed 1. After `seed(1)` the first output is 16807 and
the next is 282475249. `seed(0)`, `seed(modulus)`, and `seed(2 * modulus)`
each still yield 16807.

`random_device::operator()` still returns `__VERIFIER_nondet_uint` (C
linkage). The comment still says it is not a measure of entropy, that
`random_device` is not a proof, and that a program that only calls it is
not PROVED and not CLEAN.

`uniform_int_distribution<int>` is unchanged. `a > b` is still
`FUNC-CONTRACT` and then `__builtin_trap`, and that path does not store the
pair. `a == b` still returns `a` without `__VERIFIER_nondet_uint`. A span
that does not fit in an unsigned 32-bit count still traps and does not
return a wrapped value. A span that fits is still `a + (nondet % span)`.

## What still falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and `lower_to_ir` lowers that translation unit again with libstdc++
(`extra.cxx_models`). They are not stubbed:

- `uniform_real_distribution`
- `normal_distribution`
- `shuffle`
- `param` and `reset` on `bernoulli_distribution`
- the other engines (`mt19937`, `ranlux`, `knuth_b`, `default_random_engine`)
  and the rest of `<random>`

`uniform_int_distribution` is still only the `int` specialization. The
header does not include `<vector>`, `<functional>`, or `<memory>`.

clang++ 18.1.3 rejects a translation unit that names `std::mt19937`,
`std::uniform_real_distribution<double>`, `std::normal_distribution<double>`,
and `std::uniform_int_distribution<long>` when this header is first on the
include path (`no type named 'mt19937' in namespace 'std'`,
`no template named 'uniform_real_distribution'`,
`no member named 'normal_distribution'`, and
`uniform_int_distribution<long>` is an undefined template). That translation
unit compiles against libstdc++.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`_prism_test_sources` already lists `tests/cpp/test_cxx_random.cpp`. No new
tests/cpp file was added. `tests/cxx_models/random_trace.cpp` is still not
listed in `CMakeLists.txt`. `tests/cpp/test_qa_conformance.cpp` was not
edited. `random` stays in that list. No conformance yml was added.
`testdata/conformance/prism/cxx/cxx_random_true.cpp` and
`testdata/conformance/libc-models/random_contracts.cpp` were not edited.
They still cover `minstd_rand` only. `src/prism/bmc.cpp` was not edited.
`docs/PIR.md` was not edited.

## Tests

`tests/cpp/test_cxx_random.cpp` includes the model and reads the header
text. The `minstd_rand` and `minstd_rand0` checks are unchanged. The
`uniform_int_distribution<int>` checks are unchanged. The new case checks
that the constructor's `__prism_check` and `__builtin_trap` are before the
`else` that stores `_M_p`, and that the failing arm does not assign `_M_p`,
does not return, and does not call `__VERIFIER_nondet_int`. `operator()`
repeats that check and trap before `p == 0`, and that arm does not return
and does not call nondet. `p == 0` returns `false` before the nondet call.
`p == 1` returns `true` and that arm does not call nondet. The other arm
returns `__VERIFIER_nondet_int() ? true : false`. The omitted-distribution
parenthetical still names `uniform_real_distribution` and
`normal_distribution` and does not name `bernoulli_distribution`. The
omitted-engines parenthetical still names `mt19937` and does not name
`minstd_rand0`. The text says this is not a proof of the probability, not
PROVED, and not CLEAN. The header is still a regular file and still does
not include `<vector>`, `<functional>`, or `<memory>`.

g++ 13.3.0 compiled that file with a standalone doctest main
(`-std=c++23 -Wall -Wextra -Wpedantic -Werror`) at `-O0` and at `-O1` with
`-fsanitize=address,undefined -fno-sanitize-recover=undefined`. The
bernoulli case passed, including under the sanitizers (106 assertions).
`p == 0` and `-0.0` returned false with a nondet-int call count of 0 while
the stub value was 1. `p == 1` returned true with a call count of 0 while
the stub value was 0. The default probability is 0.5 and its draws followed
the nondet bit. Draws at 0.25 and 0.75 did the same, so the probability was
not the result. The two `minstd_rand` cases passed. cmake was not run, so
this was not `prism_tests`.

One check in the uniform case failed, and it failed on the base commit as
well. The header comment already contains `uniform_int_distribution<long>`,
and the case still requires that string to be absent. This slice did not
change that comment or that check. The other assertions in that case passed.

A separate translation unit that includes `<random>` with the model
directory as `-isystem` was compiled with clang++ 18.1.3
(`-std=c++23 -Wall -Wextra -Wpedantic -Werror`) at `-O0` and at `-O1` with
`-fsanitize=address,undefined -fno-sanitize-recover=undefined`. `p == 0` and
`-0.0` returned false and did not call `__VERIFIER_nondet_int`. `p == 1`
returned true and did not call it. Other probabilities returned the truth
of the nondet int. The same driver passed under the sanitizers.

`tests/cxx_models/random_trace.cpp` includes `<random>`. clang++ 18.1.3 at
`-O0`, and again at `-O1` with
`-fsanitize=address,undefined -fno-sanitize-recover=undefined`, printed the
same lines with the model directory as `-isystem` and without it. g++ 13.3.0
did the same without the sanitizers:

```
first=48271
discard0=48271 twin=48271
second=182605794
after_discard1=182605794 same_first=1
seed0=48271 min=1 max=2147483646
m0first=16807
m0discard0=16807 m0twin=16807
m0second=282475249
m0seed0=16807 m0mod=16807 m0twice=16807 m0min=1 m0max=2147483646 mul=16807
same=4 neg=-7 defa=0 defb=2147483647 inside=1 lo=1 hi=6
bern0=0 bern1=1 defp=0.5
```

`argv` `lo`, `hi`, and `nan` on the model aborted with
`CHECK FUNC-CONTRACT: bernoulli_distribution probability is outside [0, 1]`
and did not print `drew=`. `argv` `invert` and `argv` `span` still aborted
with the uniform messages and did not print `wrapped=`.

A driver whose `__prism_check` returns dies with SIGILL from
`__builtin_trap` for a negative probability, a probability above 1, and a
NaN. It printed no `stored=` line and no `drew=` line.
