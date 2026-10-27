# random slice

Branch `finish/cxx-random`, base `d036e09c7c29d24e411c1375a227a69189d819b1`.
`std::minstd_rand` lives in one extensionless file,
`src/prism/pir/models/cxx/random`. A directory of that name would make
`#include <random>` fail with EISDIR. The include guard is `_GLIBCXX_RANDOM`.
`#pragma GCC system_header`. Namespace `std`.

The solver was not run. The yml records the contract the harness asks for.
`expected` is the boolean `true`. This header is not a proof.

## What the model is

`minstd_rand` is the Park-Miller engine: multiplier 48271, increment 0,
modulus 2147483647. `operator()` returns the next state of
`(48271 * x) mod 2147483647`. That is not a nondeterministic value. After
`seed(1)` the first output is 48271, and the one after that is 182605794.

`seed(s)` sets the state. The stored state is `s` modulo the modulus. A
multiple of the modulus, including 0, is stored as 1, because the increment
is 0 and a zero state would stay 0. `seed(0)` then yields the same first
output as `seed(1)`. `discard(z)` calls `operator()` `z` times. `discard(0)`
does not change the state, so the next output matches a twin engine seeded
the same way. `min()` is 1 and `max()` is `modulus - 1`.

`random_device::operator()` may be nondeterministic. The body is a call to
`__VERIFIER_nondet_uint` (C linkage, so the name is not mangled). It is not
a measure of entropy. `random_device` is not a proof. A program that only
calls it is not PROVED and not CLEAN. The function does not return 0 from
every call.

## What falls through to libstdc++

Not declared, so a unit that names one fails to compile against this header
and `lower_to_ir` lowers that translation unit again with libstdc++
(`extra.cxx_models`). They are not stubbed:

- `uniform_int_distribution` (kept out so a value cannot land on `a - 1` or
  `b + 1`)
- `shuffle`
- the other engines (`minstd_rand0`, `mt19937`, `ranlux`, `knuth_b`,
  `default_random_engine`) and the rest of `<random>`

The header does not include `<vector>`, `<functional>`, or `<memory>`.

## Wiring

`src/prism/pir/stage.cpp` was not edited. `PRISM_PIR_MODELS` gained

`src/prism/pir/models/cxx/random`

so the existing embed writes `cxx/random` and the existing `-isystem`
directory is enough for `#include <random>`. `_prism_test_sources` gained
`tests/cpp/test_cxx_random.cpp`. `tests/cpp/test_qa_conformance.cpp` lists
`random` in the header-name array. The skips for
`iostream/prism_ostream.hpp` and `deque/deque` are unchanged.

`testdata/conformance/libc-models/random_contracts.cpp` is the
`#include <random>` that case searches for. It is not a scored task: there
is no false twin. A nondeterministic `random_device` call is not a defect.

## Tests

`tests/cxx_models/random_trace.cpp` includes `<random>`. clang++ 18.1.3
against libstdc++, `-std=c++23 -Wall -Wextra -Wpedantic -Werror
-fsanitize=address,undefined`, printed the same lines with the model
directory as `-isystem` and without it. g++ 13.3 did the same without the
sanitizers:

```
first=48271
discard0=48271 twin=48271
second=182605794
after_discard1=182605794 same_first=1
seed0=48271 min=1 max=2147483646
```

`uniform_int_distribution` fails to compile against this header.
`random_device::operator()` references `__VERIFIER_nondet_uint` and does not
link as a body that returns 0. `tests/cpp/test_cxx_random.cpp` includes the
model and reads the header text: the first output after `seed(1)` is 48271,
and the text says `random_device` is not a proof. cmake, `prism_tests`, and
pir were not run.

`testdata/conformance/prism/cxx/cxx_random_true.cpp` seeds `minstd_rand`
with 1, requires the first output to be 48271, and requires `discard(0)` to
leave the next output unchanged relative to a twin engine (`no-overflow`,
true). There is no false task. Treating nondeterministic `random_device` as
a defect would be the wrong claim.

## Still in front of the model

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` were not edited. They still
report unconstrained `getrandom` and `arc4random` as not a proof. They do
not name `minstd_rand`. `docs/PIR.md` was not edited.
