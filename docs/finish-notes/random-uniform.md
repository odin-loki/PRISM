# uniform_int_distribution

Branch `finish/cxx-random-uniform`, base `7b92c6083b057788a82a3ce741bdf194edc5cc6b`.

`std::uniform_int_distribution<int>` is declared on the existing header
`src/prism/pir/models/cxx/random`. That path is one extensionless file. A
directory of that name would make `#include <random>` fail with EISDIR. The
include guard is still `_GLIBCXX_RANDOM`. `#pragma GCC system_header`.
Namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The solver was
not run. This header is not a proof. The distribution is not a proof of
uniformity. It is not PROVED and not CLEAN.

## What uniform_int_distribution<int> does

Only the `int` specialization is defined. `uniform_int_distribution<long>`
is an incomplete specialization of the forward declaration, so naming it
fails to compile against this header.

The constructor stores `a` and `b`. The defaults are `0` and `INT_MAX`.
`a > b` calls

`__prism_check(0, "FUNC-CONTRACT", "uniform_int_distribution range is inverted")`

and then `__builtin_trap`. The assignments of `_M_a` and `_M_b` are in the
`else`. That path does not store the inverted pair. `operator()` repeats the
same check and then `__builtin_trap`, and that arm has no `return`. A
constructed inverted object is not observed: the constructor traps first.

`a == b` returns `a` and does not call `__VERIFIER_nondet_uint`.

Otherwise the span is `b - a + 1`, formed in `long long` and held as
`unsigned long long`. If that span does not fit in an unsigned 32-bit count
(`span > 0xffffffff`), `operator()` calls

`__prism_check(0, "FUNC-CONTRACT", "uniform_int_distribution span does not fit in an unsigned 32-bit count")`

and then `__builtin_trap`. That arm has no `return`, no `%`, and no call to
`__VERIFIER_nondet_uint`. It does not return a wrapped value. The full
`int` range `[INT_MIN, INT_MAX]` has span `4294967296`, so it takes this
path. `[INT_MIN, INT_MAX - 1]` has span `4294967295` and fits.

Otherwise the result is `a + (nondet % span)`, with `nondet` the existing
`__VERIFIER_nondet_uint`. The sum is formed in `long long` and cast back to
`int`. It is inside `[a, b]`. The engine argument is not the source of the
value. This is not a proof of uniformity.

`__prism_check` is declared
`extern "C" void __prism_check(int, const char*, const char*);` and is not
defined in the header. On cl, `__builtin_trap` is the same `abort` stand-in
other models use. clang provides the builtin.

## What stayed

`minstd_rand` is unchanged: multiplier 48271, increment 0, modulus
2147483647, default seed 1. After `seed(1)` the first output is 48271 and
the next is 182605794. `discard(0)` still does not step.

`minstd_rand0` is unchanged: multiplier 16807, increment 0, modulus
2147483647, default seed 1. After `seed(1)` the first output is 16807 and
the next is 282475249. `seed(0)`, `seed(modulus)`, and `seed(2 * modulus)`
each still yield 16807.

`random_device::operator()` still returns `__VERIFIER_nondet_uint` (C
linkage, so the name is not mangled). The comment still says it is not a
measure of entropy, that `random_device` is not a proof, and that a program
that only calls it is not PROVED and not CLEAN. The function does not return
0 from every call.

## What still falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and `lower_to_ir` lowers that translation unit again with libstdc++
(`extra.cxx_models`). They are not stubbed:

- `uniform_real_distribution`
- `bernoulli_distribution`
- `normal_distribution`
- `shuffle`
- the other engines (`mt19937`, `ranlux`, `knuth_b`, `default_random_engine`)
  and the rest of `<random>`

The header does not include `<vector>`, `<functional>`, or `<memory>`.

clang++ 18.1.3 rejects a translation unit that names `std::mt19937`,
`std::uniform_real_distribution<double>`, `std::bernoulli_distribution`,
`std::normal_distribution<double>`, and `std::uniform_int_distribution<long>`
when this header is first on the include path (`no type named 'mt19937' in
namespace 'std'`, and `uniform_int_distribution<long>` is an undefined
template). That translation unit compiles against libstdc++.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`_prism_test_sources` already lists `tests/cpp/test_cxx_random.cpp`. No new
tests/cpp file was added. `tests/cxx_models/random_trace.cpp` is not listed
in `CMakeLists.txt`. `tests/cpp/test_qa_conformance.cpp` was not edited.
`random` stays in that list. No conformance yml was added.
`testdata/conformance/prism/cxx/cxx_random_true.cpp` and
`testdata/conformance/libc-models/random_contracts.cpp` were not edited.
They still cover `minstd_rand` only. `src/prism/bmc.cpp` was not edited.
`docs/PIR.md` was not edited.

## Tests

`tests/cpp/test_cxx_random.cpp` includes the model and reads the header
text. The `minstd_rand` and `minstd_rand0` checks are unchanged. The new
case checks that the inverted `__prism_check` and `__builtin_trap` in the
constructor are before the `else` that stores `_M_a` and `_M_b`, and that
the failing arm does not assign those members and does not return.
`operator()` repeats that check and trap before `a == b`, and that arm does
not return and does not call `__VERIFIER_nondet_uint`. `a == b` returns
`_M_a` before the nondet call. The span check traps before `% span`, and
the sum return is in the `else`. The failing span arm does not return, does
not use `%`, and does not call the nondet function. The omitted-engines
parenthetical still names `mt19937` and does not name `minstd_rand0`.
`mt19937`, `uniform_real_distribution`, `bernoulli_distribution`, and
`normal_distribution` are not declared as classes.
`uniform_int_distribution<long>` is not specialized. The text says this is
not a proof of uniformity, not PROVED, and not CLEAN. The header is still a
regular file and still does not include `<vector>`, `<functional>`, or
`<memory>`. `__prism_check` is not defined. cmake, `prism_tests`, and pir
were not run, so that doctest was not executed.

A separate translation unit that includes `<random>` with the model
directory as `-isystem` was compiled with clang++ 18.1.3
(`-std=c++23 -Wall -Wextra -Wpedantic -Werror`) at `-O0` and at `-O1` with
`-fsanitize=address,undefined -fno-sanitize-recover=undefined`. Equal bounds
`4`, `-7`, and `0` each returned that bound and did not call
`__VERIFIER_nondet_uint` (the call count stayed 0 at `-O0`). Draws with a
stub nondet stayed inside `[a, b]` for `[1, 6]`, `[-3, 2]`, `[0, INT_MAX]`,
`[-1, INT_MAX]`, and `[INT_MIN, INT_MAX - 1]`, including nondet values `0`,
`2147483647`, `4294967294`, and `4294967295`. The default bounds were `0`
and `2147483647`.

`tests/cxx_models/random_trace.cpp` includes `<random>`. clang++ 18.1.3
(`-std=c++23 -Wall -Wextra -Wpedantic -Werror -O1
-fsanitize=address,undefined -fno-sanitize-recover=undefined`) printed the
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
```

`argv` `invert` on the model aborted with
`CHECK FUNC-CONTRACT: uniform_int_distribution range is inverted` and did
not print `wrapped=`. `argv` `span` aborted with
`CHECK FUNC-CONTRACT: uniform_int_distribution span does not fit in an unsigned 32-bit count`
and did not print `wrapped=`.

A driver whose `__prism_check` returns dies with SIGILL from
`__builtin_trap`. The inverted constructor printed no `stored=` line and no
`wrapped=` line. The span that does not fit stored `-2147483648,2147483647`
and then trapped; it printed no `wrapped=` line.
