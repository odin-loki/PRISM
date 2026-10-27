# confidence

Law 5 is unchanged: confidence = visibility × answer × resolution, and a factor with no data is 0. `score_counts` still returns that exact product. The published report fields are that product rounded to 4 decimal places, half to even, the same rule as Python `round(x, 4)`.

## Where rounding lives

`src/prism/verdict/verdict.cpp` `score_counts` (the ratio helper) does not round. It was not given a round step: the Lean grid in `tests/cpp/test_main.cpp` ("verdict module equals the Lean model: confidence grid") compares the raw product to the proved fractions within `1e-12`.

The published 4-decimal rounding is `prism::round4` in `src/prism/pipeline.cpp`, lines 151–158, applied to visibility, answer, resolution, and confidence in `apply_confidence` at lines 163–166. That file is owned by another slice and was not edited. The old `std::round(x * 10000.0) / 10000.0` (half away from zero) is already gone; `round4` formats the exact binary value with `{:.4f}` and reads that decimal back. On MSVC 14.51 and libstdc++ 13 that format is half to even, and it rounds the decimal expansion of the double, not the scaled product `x * 1e4`.

`docs/CPP_PORT_PLAN.md` still describes the old `std::round` path. That sentence is stale relative to `round4`.

## Tie cases

`1.00005` is not an exact tie. The double is slightly above `1.00005` (`x * 1e4` is `10000.500000000001819`), so half-to-even and `std::round` both publish `1.0001`.

Values that do separate the rules:

| value | exact decimal rule (published) | `std::round(x * 1e4) / 1e4` |
|---|---|---|
| `1/32` (`0.03125`) | `0.0312` | `0.0313` |
| `3.5/10000` (just under `0.00035`, but `x * 1e4 == 3.5`) | `0.0003` | `0.0004` |

`0.00005` is stored just above `5e-5`. Both the decimal rule and `std::round` publish `0.0001`. A half-to-even round of the scaled product would publish `0.0000`, which the existing check rejects.

## Empty oracle

`score_counts` does not read `extra`. It cannot treat `extra.contains("oracle")` as resolving. The resolve check is `confidence_score` in `src/prism/pipeline.cpp` lines 110–119: a `FAILED` row resolves only with a non-empty counterexample, a non-empty `oracle`, or a non-empty `read`. An empty string does not count. That was already true, and `tests/cpp/test_journal_confidence.cpp` ("FAILED resolves only with a counterexample or a non-empty oracle") already locks it. No verdict change.

## Golden numbers

No existing expectation depended on half-away, so none were edited:

- `round4(1/3) == 0.3333`, `round4(2/3) == 0.6667`, `round4(0) == 0`, `round4(1) == 1`
- `round4(0.00005) == 0.0001`, `round4(0.00015) == 0.0001`
- one answered function of three publishes answer and confidence `0.3333`
- empty scope stays 0 on every factor
- the Lean confidence grid stays the unrounded fractions

New expectations, in "an exact 4th-decimal tie rounds half to even":

- `round4(1/32) == 0.0312` (and `std::round` of the same value is `0.0313`)
- `round4(1.00005) == 1.0001`
- `round4(3.5/10000) == 0.0003` (and `std::round` of the same value is `0.0004`)
- 1 proved function out of 32 publishes answer and confidence `0.0312`; `confidence_score` before rounding is still `1/32`
