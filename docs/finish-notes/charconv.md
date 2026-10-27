# charconv slice

Branch `finish/cxx-charconv`, base `b58ed8da4042896363fbbe540d29692207d1a1ea`.
`<charconv>` is a header file, `src/prism/pir/models/cxx/charconv`, the same
shape as `vector`. A directory of that name would make `#include <charconv>`
fail with EISDIR and every such unit would fall back to libstdc++.

The solver was not run. The yml files record the contract each harness asks
for. `expected` values are YAML booleans (`true` / `false`, unquoted). Nothing
here is a proof.

## What the model covers

Integer `from_chars` and `to_chars` for `char`, `signed char`, `unsigned char`,
`short`, `unsigned short`, `int`, `unsigned int`, `long`, `unsigned long`,
`long long`, and `unsigned long long`. `bool` overloads are deleted.

On success, `ptr` is the first unparsed character (`from_chars`) or one past
the last character written (`to_chars`), the out-parameter or the buffer
receives the value, and `errc{}` is returned. A value that does not fit in
the destination type returns `errc::result_out_of_range` and does not write a
wrapped or zero success value; the destination is left unchanged. Remaining
digits are still consumed, so `ptr` is the first non-digit. No digits returns
`errc::invalid_argument` and does not write; `ptr` stays at the original
`first`. A leading `+` is not a sign. A leading `-` is accepted only for a
signed type. `INT_MIN` is stored as that value, not as a negation of a
positive that does not fit.

A null `first` or `last`, a `last` before `first`, or a base outside 2..36 is
a precondition failure via `__prism_check` (`PTR-NULL-DEREF` or
`FUNC-CONTRACT`), not a successful zero. The destination is not written on
that path.

`to_chars` builds the characters in a local buffer and copies them only when
`last - first` is large enough. A buffer that is too small returns
`errc::value_too_large`, does not claim success, and does not write past
`last`. The header does not include `<string>` or `<iostream>`.

## What falls through to libstdc++

`std::chars_format` and the floating-point overloads of `from_chars` and
`to_chars` are not declared. Omitted APIs fail to compile, so pir falls back
to libstdc++. They are omitted so this model does not invent a rounded value.

## Wiring

`src/prism/pir/stage.cpp` already writes every embedded `cxx/` model and
passes that directory as `-isystem`. It was not edited. `PRISM_PIR_MODELS`
lists `src/prism/pir/models/cxx/charconv`, so the embedded name is
`cxx/charconv`. `tests/cpp/test_qa_conformance.cpp` names `charconv` in the
header array. The iostream file and the `deque/deque` skip in that test are
unchanged.

`src/prism/bmc.cpp` and `src/prism/bmc_unenc.inc` still return "is not a
charconv model" from the source text, before the header is inlined. Those
exits are still in place. `tests/cpp/test_concolic.cpp` still expects
`from_chars_unenc_bad` and `to_chars_unenc_bad` to be `NEEDS-HARNESS` until
they go.

## Tests

`testdata/conformance/libc-models/charconv_contracts.cpp` is the libc-contract
harness (`#include <charconv>`). `charconv_small_true` parses `"42"`.
`charconv_overflow_false` parses `"2147483648"` into an `int` left at
`INT_MAX` and returns `v + 1` (`INT-SIGNED-OVF`). `charconv_null_false` is
`from_chars(nullptr, nullptr, v)` (`PTR-NULL-DEREF`).

`testdata/conformance/prism/cxx/cxx_charconv_true.cpp` parses `"42"` and
checks the value, the end pointer, and `errc{}` (`no-overflow`, expected
`true`). `cxx_charconv_false.cpp` parses `"2147483648"` into an `int` left at
`INT_MAX` and returns `v + 1` (`no-overflow`, expected `false`, witness
`[0]`).

Differential, clang++ 18, libstdc++ 14, `-std=c++23 -D_GLIBCXX_ASSERTIONS
-fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`.
`tests/cxx_models/charconv_trace.cpp` compiled twice (`-isystem` the model
directory, and without it). Safe-path stdout matches, including
`small ec=1 v=42 off=2`, `over ec=2 v=12345 off=10` (the sentinel is not a
wrapped `int`), `over0 ec=2 v=77 off=10` (not a zero from `2^32`), and
`short ec=4 success=0 b0=Q b1=Q z2=Z z3=Z` (too small, canary unchanged).
The model binary with `null` aborts on `CHECK PTR-NULL-DEREF:` and does not
print `fake=`. With `invert` it aborts on `CHECK FUNC-CONTRACT:` (`last
before first`). `tests/cpp/test_cxx_charconv.cpp` is that comparison.
`prism_tests` was not executed. No engine configure, no pir run.
