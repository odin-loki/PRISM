# Witness YAML scalars

`to_yaml` / `scalar` (`src/prism/svcomp/witness.cpp`) writes invariant and counterexample strings as YAML that stays one document. `build_correctness_witness` only serializes the invariants it is given.

## Quoting

A scalar that is not a plain identifier (`[A-Za-z_][A-Za-z0-9_.\-/]*`, and not `true` / `false` / `null` / `yes` / `no` / `on` / `off` / `~` / `y` / `n`) is double-quoted. That covers:

- a colon (`a:b`, `a: b`), left as a literal colon inside the quotes
- a double quote, escaped as `\"`
- a newline, escaped as `\n` (same spelling as before)
- a leading YAML indicator, or any other first byte that is not a letter or `_`. The indicators are `-`, `?`, `:`, `,`, `[`, `]`, `{`, `}`, `#`, `&`, `*`, `!`, `|`, `>`, `'`, `"`, `%`, `@`, and a backtick. A leading digit, `.`, `+`, `/`, or space is quoted too

`\` is `\\`. `\r` is `\r`. `\t` is `\t`. Other bytes below 0x20, and 0x7F, are `\xHH`.

## Bytes that are not printable ASCII

A raw break would fold a double-quoted scalar and split the witness line. Well-formed UTF-8 is written as UTF-8 inside the quotes. The YAML line breaks U+0085, U+2028 and U+2029 are `\u0085`, `\u2028` and `\u2029`. A byte that is not a well-formed UTF-8 sequence (overlong, surrogate, or a stray continuation) is `\xHH`, one byte at a time. The witness document is still emitted; `\xFF` reads back as U+00FF through the task YAML reader.

## What stayed

`correctness_invariants` still exports a loop invariant only for `PROVED`, `PROVED-UNBOUNDED` or `PROVED-CERTIFIED` with `k_induction` `closed-invariants`. `BOUNDED` and `HYPOTHESIS` are outside that set. `WitnessMeta::data_model` still defaults to `LP64`. The proof gate, replay, the property parser, the SV-COMP CLI, the PIR stage and the BMC encoder were not changed.

## Tests

`tests/cpp/test_svcomp_witness.cpp`, one line in `_prism_test_sources` next to `test_svcomp.cpp`.
