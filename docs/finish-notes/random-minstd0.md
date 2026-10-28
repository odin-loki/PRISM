# minstd_rand0

Branch `finish/cxx-random-minstd0`, base `079b279295e732b434335ce5791f4ba54a4b8f9e`.

`std::minstd_rand0` is declared on the existing header
`src/prism/pir/models/cxx/random`. That path is one extensionless file. A
directory of that name would make `#include <random>` fail with EISDIR. The
include guard is still `_GLIBCXX_RANDOM`. `#pragma GCC system_header`.
Namespace `std`.

This slice did not run cmake, ninja, `prism_tests`, or pir. The solver was
not run. This header is not a proof.

## What minstd_rand0 does

`minstd_rand0` is the Park-Miller recurrence with multiplier 16807, increment
0, and modulus 2147483647. `default_seed` is 1. `operator()` returns the next
state of `(16807 * x) mod 2147483647`. That is not a nondeterministic value.
After `seed(1)` the first output is 16807, and the one after that is
282475249.

`seed(s)` sets the state. The stored state is `s` modulo the modulus. A
multiple of the modulus, including 0, is stored as 1, because the increment
is 0 and a zero state would stay 0. `seed(0)`, `seed(modulus)`, and
`seed(2 * modulus)` each yield the same first output as `seed(1)`.
`discard(z)` calls `operator()` `z` times. `discard(0)` does not change the
state, so the next output matches a twin engine seeded the same way.
`discard(1)` leaves the following output at 282475249. `min()` is 1 and
`max()` is `modulus - 1`.

## What stayed

`minstd_rand` is unchanged: multiplier 48271, increment 0, modulus
2147483647, default seed 1. After `seed(1)` the first output is 48271 and
the next is 182605794. `seed(0)` still stores 1, so the next output is
48271. `discard(0)` still does not step.

`random_device::operator()` still returns `__VERIFIER_nondet_uint` (C
linkage, so the name is not mangled). The comment still says it is not a
measure of entropy, that `random_device` is not a proof, and that a program
that only calls it is not PROVED and not CLEAN. The function does not return
0 from every call.

## What still falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and `lower_to_ir` lowers that translation unit again with libstdc++
(`extra.cxx_models`). They are not stubbed:

- `uniform_int_distribution`
- `shuffle`
- the other engines (`mt19937`, `ranlux`, `knuth_b`, `default_random_engine`)
  and the rest of `<random>`

The header does not include `<vector>`, `<functional>`, or `<memory>`.

clang++ 18.1.3 rejects a translation unit that names `std::mt19937`,
`std::ranlux24`, `std::knuth_b`, `std::default_random_engine`, and
`std::uniform_int_distribution` when this header is first on the include
path (`no type named 'mt19937' in namespace 'std'`, and the same for the
other names). That translation unit compiles against libstdc++.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `CMakeLists.txt` was not edited.
`_prism_test_sources` already lists `tests/cpp/test_cxx_random.cpp`. The
conformance header array in `tests/cpp/test_qa_conformance.cpp` was not
edited. `random` stays in that list. No conformance yml was added.
`testdata/conformance/prism/cxx/cxx_random_true.cpp` and
`testdata/conformance/libc-models/random_contracts.cpp` were not edited.
They still cover `minstd_rand` only. `src/prism/bmc.cpp` was not edited.

## Tests

`tests/cpp/test_cxx_random.cpp` includes the model and reads the header
text. The `minstd_rand` checks are unchanged: the first output after
`seed(1)` is 48271, and `discard(0)` matches a twin. The new case checks
`minstd_rand0`: multiplier 16807, modulus 2147483647, default seed 1, the
first output after `seed(1)` is 16807, the second is 282475249, `discard(0)`
does not step, `discard(1)` reaches 282475249, and `seed(0)`,
`seed(modulus)`, and `seed(2 * modulus)` each return 16807 on the next
call. The omitted-engines parenthetical still names `mt19937`, `ranlux`,
`knuth_b`, and `default_random_engine`, and it does not name `minstd_rand0`.
The text still says `random_device` is not a proof, not PROVED, and not
CLEAN. cmake, `prism_tests`, and pir were not run, so that doctest was not
executed.

`tests/cxx_models/random_trace.cpp` includes `<random>`. clang++ 18.1.3
(`-std=c++23 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined`)
printed the same lines with the model directory as `-isystem` and without
it. g++ 13.3.0 did the same without the sanitizers:

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
```
